// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender

/*
#include "bridge.h"
*/
import "C"

import (
	"errors"
	"io"
	"io/fs"
	"runtime"
	"runtime/cgo"
	"strings"
	"unsafe"
)

// call is what one call into C hands its callbacks, through a cgo.Handle:
// where fonts, templates and page images are read from, and where an export
// writes its file. The callbacks run on the calling goroutine, one at a
// time, so its fields need no lock.
type call struct {
	fonts     fs.FS
	templates *Templates
	images    fs.FS

	// pinner keeps the bytes lent to C in place until the call returns;
	// lent keeps them alive.
	pinner runtime.Pinner
	lent   [][]byte

	// out receives the file of an export written as it is produced.
	out *output

	// err is what the first failed callback failed with.
	err error
	// panicked and panicValue keep a callback's panic, recovered before it
	// can unwind through C and raised again once C has returned.
	panicked   bool
	panicValue any
}

// fail records the first failure of a callback.
func (c *call) fail(err error) {
	if c.err == nil && !c.panicked {
		c.err = err
	}
}

// recovered keeps a panic of a callback, which then answers failure.
func (c *call) recovered(status *C.rmr_status) {
	if p := recover(); p != nil {
		c.panicked = true
		c.panicValue = p
		*status = C.RMR_ERR_BACKEND
	}
}

// run calls C with a handle to c, deleting the handle and unpinning what was
// lent afterwards, and raising any panic of a callback.
func (c *call) run(f func(handle C.uintptr_t) C.rmr_status) C.rmr_status {
	h := cgo.NewHandle(c)
	status := f(C.uintptr_t(h))
	h.Delete()
	c.pinner.Unpin()
	c.lent = nil
	if c.panicked {
		panic(c.panicValue)
	}
	return status
}

// errorOf turns a C status into a Go error, nil for success, with the
// failure of a callback as its cause.
func (c *call) errorOf(status C.rmr_status, cerr *C.rmr_error) error {
	if status == C.RMR_OK {
		return nil
	}
	e := cError(status, cerr)
	e.Err = c.err
	return e
}

// lend hands data to C for the rest of the call, pinned in place.
func (c *call) lend(data []byte, out *C.rmr_asset_view) {
	if len(data) == 0 {
		*out = C.rmr_asset_view{}
		return
	}
	c.lent = append(c.lent, data)
	out.bytes = pinned(&c.pinner, data)
	out.size = C.size_t(len(data))
}

// read answers the file name of fsys, nil when it is not there.
func (c *call) read(fsys fs.FS, name string) ([]byte, error) {
	if fsys == nil || !fs.ValidPath(name) {
		return nil, nil
	}
	data, err := fs.ReadFile(fsys, name)
	if errors.Is(err, fs.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	return data, nil
}

// resourceName answers a name a template or image is looked up by, false
// for one that could reach outside the directory it is looked up in.
func resourceName(p *C.char, length C.size_t) (string, bool) {
	name, err := goText(p, length, "name")
	if err != nil || name == "" || name[0] == '.' || strings.ContainsAny(name, "/\\\x00") {
		return "", false
	}
	return name, true
}

// rmrgoFont resolves a bundled font by its resource name, without the .ttf
// extension. A missing font answers unsupported, as the library asks.
//
//export rmrgoFont
func rmrgoFont(handle C.uintptr_t, name *C.char, out *C.rmr_asset_view) (status C.rmr_status) {
	c := cgo.Handle(handle).Value().(*call)
	defer c.recovered(&status)
	data, err := c.read(c.fonts, C.GoString(name)+".ttf")
	if err != nil {
		c.fail(err)
		return C.RMR_ERR_BACKEND
	}
	if len(data) == 0 {
		return C.RMR_ERR_UNSUPPORTED
	}
	c.lend(data, out)
	return C.RMR_OK
}

// rmrgoTemplate resolves a page template by name: the first layer of the
// call's Templates that holds name.template. None holding it leaves the page
// without a template.
//
//export rmrgoTemplate
func rmrgoTemplate(handle C.uintptr_t, name *C.char, length C.size_t, out *C.rmr_asset_view) (status C.rmr_status) {
	c := cgo.Handle(handle).Value().(*call)
	defer c.recovered(&status)
	*out = C.rmr_asset_view{}
	n, ok := resourceName(name, length)
	if !ok {
		return C.RMR_ERR_INVALID_ARGUMENT
	}
	for _, layer := range c.templates.layers() {
		data, err := c.read(layer, n+".template")
		if err != nil {
			c.fail(err)
			return C.RMR_ERR_BACKEND
		}
		if data != nil {
			c.lend(data, out)
			return C.RMR_OK
		}
	}
	return C.RMR_OK
}

// outputPiece is the most an export's writer is handed at once.
const outputPiece = 4 << 20

// output is where an export writes its file: w, through a buffer of Go
// memory each piece C hands over is copied into, at most outputPiece bytes
// at a time, so that w never sees C memory and nothing holds the whole file.
type output struct {
	w      io.Writer
	buffer []byte
}

// write passes C's bytes p on to w, failing as w fails, or with
// [io.ErrShortWrite] when w takes less than it is given without saying why.
func (o *output) write(p []byte) error {
	for len(p) > 0 {
		n := min(len(p), outputPiece)
		if len(o.buffer) < n {
			o.buffer = make([]byte, n)
		}
		copy(o.buffer, p[:n])
		written, err := o.w.Write(o.buffer[:n])
		if err != nil {
			return err
		}
		if written != n {
			return io.ErrShortWrite
		}
		p = p[n:]
	}
	return nil
}

// rmrgoWrite hands the next size bytes of an export's file to its writer.
// The writer's failure stops the export.
//
//export rmrgoWrite
func rmrgoWrite(handle C.uintptr_t, bytes *C.uint8_t, size C.size_t) (status C.rmr_status) {
	c := cgo.Handle(handle).Value().(*call)
	defer c.recovered(&status)
	if c.out == nil {
		return C.RMR_ERR_INVALID_ARGUMENT
	}
	n, err := goLength(size, "output")
	if err != nil {
		c.fail(err)
		return C.RMR_ERR_OUT_OF_MEMORY
	}
	if err := c.out.write(unsafe.Slice((*byte)(unsafe.Pointer(bytes)), n)); err != nil {
		c.fail(err)
		return C.RMR_ERR_BACKEND
	}
	return C.RMR_OK
}

// rmrgoImage resolves an image a lone page places, by its file name. A
// missing image keeps its place in the drawing order and paints nothing.
//
//export rmrgoImage
func rmrgoImage(handle C.uintptr_t, name *C.char, length C.size_t, out *C.rmr_asset_view) (status C.rmr_status) {
	c := cgo.Handle(handle).Value().(*call)
	defer c.recovered(&status)
	*out = C.rmr_asset_view{}
	n, ok := resourceName(name, length)
	if !ok {
		return C.RMR_OK
	}
	data, err := c.read(c.images, n)
	if err != nil {
		c.fail(err)
		return C.RMR_ERR_BACKEND
	}
	c.lend(data, out)
	return C.RMR_OK
}
