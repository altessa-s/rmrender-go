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
	"sync/atomic"
	"unsafe"
)

// call is what one call into C hands its callbacks, through a cgo.Handle:
// where fonts, templates and page images are read from, and where an export
// writes its file. Caller code runs on a helper goroutine, one callback
// at a time. Fields it writes are read only after the wait, so they need no
// lock.
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

	// unreadable holds the pages the call was told do not parse, in the
	// order it was told of them.
	unreadable []int

	// err is what the first failed callback failed with.
	err error
	// panicked and panicValue keep a callback's panic, recovered before it
	// can unwind through C and raised again once C has returned.
	panicked   bool
	panicValue any
	// exited keeps Goexit until the same cleanup has run.
	exited    bool
	callbacks callbackRunner
}

// liveHandles counts calls whose callback state has not yet been released.
var liveHandles atomic.Int64

// fail records the first failure of a callback.
func (c *call) fail(err error) {
	if c.err == nil && !c.panicked && !c.exited {
		c.err = err
	}
}

// invoke keeps a callback's panic or exit until C has stopped and released
// its memory. Only the caller's code runs on the helper goroutine.
func (c *call) invoke(fn func()) bool {
	if c.panicked || c.exited {
		return false
	}
	returned, p := c.callbacks.run(fn)
	c.panicked = p != nil
	c.panicValue = p
	c.exited = !returned && p == nil
	return returned
}

// run calls C with a handle to c, deleting the handle and unpinning what was
// lent afterwards, and raising any panic or exit of a callback.
func (c *call) run(f func(handle C.uintptr_t) C.rmr_status) C.rmr_status {
	liveHandles.Add(1)
	h := cgo.NewHandle(c)
	status := f(C.uintptr_t(h))
	h.Delete()
	c.pinner.Unpin()
	c.lent = nil
	c.callbacks.stop()
	liveHandles.Add(-1)
	if c.panicked {
		panic(c.panicValue)
	}
	if c.exited {
		runtime.Goexit()
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
	var data []byte
	var err error
	missing := false
	if !c.invoke(func() {
		data, err = fs.ReadFile(fsys, name)
		missing = errors.Is(err, fs.ErrNotExist)
	}) {
		return nil, nil
	}
	if missing {
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
	data, err := c.read(c.fonts, C.GoString(name)+".ttf")
	if c.panicked || c.exited {
		return C.RMR_ERR_BACKEND
	}
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
	*out = C.rmr_asset_view{}
	n, ok := resourceName(name, length)
	if !ok {
		return C.RMR_ERR_INVALID_ARGUMENT
	}
	for _, layer := range c.templates.layers() {
		data, err := c.read(layer, n+".template")
		if c.panicked || c.exited {
			return C.RMR_ERR_BACKEND
		}
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
// An abnormal return is kept by invoke and checked before answering C.
func (o *output) write(p []byte, invoke func(func()) bool) error {
	for len(p) > 0 {
		n := min(len(p), outputPiece)
		if len(o.buffer) < n {
			o.buffer = make([]byte, n)
		}
		copy(o.buffer, p[:n])
		var written int
		var err error
		if !invoke(func() { written, err = o.w.Write(o.buffer[:n]) }) {
			return nil
		}
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
	if c.out == nil {
		return C.RMR_ERR_INVALID_ARGUMENT
	}
	n, err := goLength(size, "output")
	if err != nil {
		c.fail(err)
		return C.RMR_ERR_OUT_OF_MEMORY
	}
	if err := c.out.write(unsafe.Slice((*byte)(unsafe.Pointer(bytes)), n), c.invoke); err != nil {
		c.fail(err)
		return C.RMR_ERR_BACKEND
	}
	if c.panicked || c.exited {
		return C.RMR_ERR_BACKEND
	}
	return C.RMR_OK
}

// rmrgoUnreadable is told of a page of a document whose .rm file does not
// parse, before the call draws it as Options.Unreadable says or fails with
// it.
//
//export rmrgoUnreadable
func rmrgoUnreadable(handle C.uintptr_t, page C.size_t, _ C.rmr_status) {
	c := cgo.Handle(handle).Value().(*call)
	if uint64(page) <= uint64(maxInt) {
		c.unreadable = append(c.unreadable, int(page))
	}
}

// rmrgoImage resolves an image a lone page places, by its file name. A
// missing image keeps its place in the drawing order and paints nothing.
//
//export rmrgoImage
func rmrgoImage(handle C.uintptr_t, name *C.char, length C.size_t, out *C.rmr_asset_view) (status C.rmr_status) {
	c := cgo.Handle(handle).Value().(*call)
	*out = C.rmr_asset_view{}
	n, ok := resourceName(name, length)
	if !ok {
		return C.RMR_OK
	}
	data, err := c.read(c.images, n)
	if c.panicked || c.exited {
		return C.RMR_ERR_BACKEND
	}
	if err != nil {
		c.fail(err)
		return C.RMR_ERR_BACKEND
	}
	c.lend(data, out)
	return C.RMR_OK
}

// callbackRunner reuses one goroutine for a call's reads and output pieces.
// Waiting for each answer keeps caller code synchronous without allowing it
// to unwind a C frame. An abnormal answer ends the worker as well.
type callbackRunner struct {
	jobs    chan func()
	answers chan callbackResult
	stopped chan struct{}
}

type callbackResult struct {
	returned   bool
	panicValue any
}

func (r *callbackRunner) run(fn func()) (bool, any) {
	if r.jobs == nil {
		r.jobs = make(chan func())
		r.answers = make(chan callbackResult)
		r.stopped = make(chan struct{})
		go func() {
			defer close(r.stopped)
			finished := false
			defer func() {
				if !finished {
					r.answers <- callbackResult{panicValue: recover()}
				}
			}()
			for fn := range r.jobs {
				fn()
				// Drop the closure before answering so borrowed data is not kept
				// while the worker waits for its next piece.
				fn = nil
				r.answers <- callbackResult{returned: true}
			}
			finished = true
		}()
	}
	r.jobs <- fn
	answer := <-r.answers
	return answer.returned, answer.panicValue
}

func (r *callbackRunner) stop() {
	if r.jobs != nil {
		close(r.jobs)
		<-r.stopped
	}
}
