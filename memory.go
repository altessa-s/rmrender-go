// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender

/*
#include <stdlib.h>
#include "bridge.h"
*/
import "C"

import (
	"image"
	"math"
	"runtime"
	"strings"
	"unsafe"
)

const maxInt = math.MaxInt

// goLength converts a C size to a Go int, refusing one past math.MaxInt.
func goLength(n C.size_t, what string) (int, error) {
	if uint64(n) > maxInt {
		return 0, errTooLarge(what)
	}
	return int(n), nil
}

// goBytes copies a C byte range into Go memory.
func goBytes(p *C.uint8_t, size C.size_t, what string) ([]byte, error) {
	n, err := goLength(size, what)
	if err != nil {
		return nil, err
	}
	out := make([]byte, n)
	if n > 0 {
		copy(out, unsafe.Slice((*byte)(unsafe.Pointer(p)), n))
	}
	return out, nil
}

// goText copies counted C text into a Go string, by its length and not up to
// a NUL.
func goText(p *C.char, length C.size_t, what string) (string, error) {
	n, err := goLength(length, what)
	if err != nil || n == 0 || p == nil {
		return "", err
	}
	return string(unsafe.Slice((*byte)(unsafe.Pointer(p)), n)), nil
}

// pinned answers the address of b's first byte, pinned by p so that C may
// hold it for the length of a call; nil for no bytes.
func pinned(p *runtime.Pinner, b []byte) *C.uint8_t {
	if len(b) == 0 {
		return nil
	}
	p.Pin(&b[0])
	return (*C.uint8_t)(unsafe.Pointer(&b[0]))
}

// cText copies s into C memory as a NUL-terminated string, refusing one that
// holds a NUL, which C would read as its end. The caller frees it.
func cText(s, what string) (*C.char, error) {
	if strings.IndexByte(s, 0) >= 0 {
		return nil, errInvalid(what + " holds a NUL byte")
	}
	return C.CString(s), nil
}

func freeText(p *C.char) {
	C.free(unsafe.Pointer(p))
}

// takeBuffer copies a C buffer into Go memory and releases it.
func takeBuffer(b *C.rmr_buffer) ([]byte, error) {
	defer C.rmr_buffer_free(b)
	return goBytes(b.bytes, b.size, "output")
}

// takeSVGPages copies the SVG documents of C into Go memory, one slice
// each, and releases them.
func takeSVGPages(p *C.rmr_svg_pages) ([][]byte, error) {
	defer C.rmr_svg_pages_free(p)
	count, err := goLength(p.count, "page count")
	if err != nil {
		return nil, err
	}
	if count == 0 {
		return [][]byte{}, nil
	}
	buffers := unsafe.Slice(p.pages, count)
	out := make([][]byte, count)
	for i := range buffers {
		if out[i], err = goBytes(buffers[i].bytes, buffers[i].size, "output"); err != nil {
			return nil, err
		}
	}
	return out, nil
}

// takeBitmap copies a C bitmap, premultiplied RGBA with the first row at the
// top, into an image of the same layout and releases it.
func takeBitmap(b *C.rmr_bitmap) (*image.RGBA, error) {
	defer C.rmr_bitmap_free(b)
	width, err := goLength(b.width, "bitmap width")
	if err != nil {
		return nil, err
	}
	height, err := goLength(b.height, "bitmap height")
	if err != nil {
		return nil, err
	}
	stride, err := goLength(b.stride, "bitmap stride")
	if err != nil {
		return nil, err
	}
	if width > maxInt/4 || stride < 4*width || (height > 0 && stride > maxInt/height) {
		return nil, errTooLarge("bitmap")
	}
	img := image.NewRGBA(image.Rect(0, 0, width, height))
	if width == 0 || height == 0 {
		return img, nil
	}
	pixels := unsafe.Slice((*byte)(unsafe.Pointer(b.pixels)), stride*height)
	for y := range height {
		copy(img.Pix[y*img.Stride:y*img.Stride+4*width], pixels[y*stride:y*stride+4*width])
	}
	return img, nil
}
