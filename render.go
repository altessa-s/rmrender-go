// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender

/*
#include "bridge.h"
*/
import "C"

import (
	"bytes"
	"context"
	"image"
	"image/png"
	"unsafe"
)

// cancelled answers the error of a context that ended before or during a
// call.
func cancelled(ctx context.Context) error {
	return &Error{Status: StatusCancelled, Message: "context ended", Err: context.Cause(ctx)}
}

// live answers the error of a context that has ended, nil for one that has
// not or for none.
func live(ctx context.Context) error {
	if ctx != nil && ctx.Err() != nil {
		return cancelled(ctx)
	}
	return nil
}

// watch answers a C flag raised when ctx ends, and the function that stops
// watching and releases it. A context that cannot end needs no flag.
func watch(ctx context.Context) (unsafe.Pointer, func()) {
	if ctx.Done() == nil {
		return nil, func() {}
	}
	flag := C.rmrgo_flag_new()
	if flag == nil {
		return nil, func() {}
	}
	raised := make(chan struct{})
	stop := context.AfterFunc(ctx, func() {
		C.rmrgo_flag_raise(flag)
		close(raised)
	})
	return flag, func() {
		if !stop() {
			// Raising has begun: the flag is freed only after it.
			<-raised
		}
		C.rmrgo_flag_free(flag)
	}
}

// render makes one call into C for o: with o's fonts locked and in the
// request, the callbacks reading o's templates and images, and ctx able to
// cancel the drawing.
func render(ctx context.Context, o *Options, f func(r *C.rmrgo_request, cerr *C.rmr_error) C.rmr_status) error {
	if ctx == nil {
		ctx = context.Background()
	}
	if err := live(ctx); err != nil {
		return err
	}
	r, err := o.request()
	if err != nil {
		return err
	}
	c := &call{}
	if o != nil {
		c.templates = o.Templates
		c.images = o.Images
		if o.Fonts != nil {
			o.Fonts.mu.Lock()
			defer o.Fonts.mu.Unlock()
			if o.Fonts.c == nil {
				return errInvalid("fonts are closed")
			}
			r.fonts = o.Fonts.c
		}
	}
	flag, stop := watch(ctx)
	defer stop()
	r.cancel = flag
	var cerr C.rmr_error
	status := c.run(func(handle C.uintptr_t) C.rmr_status {
		r.handle = handle
		return f(&r, &cerr)
	})
	if status == C.RMR_ERR_CANCELLED && ctx.Err() != nil {
		e := cError(status, &cerr)
		e.Err = context.Cause(ctx)
		return e
	}
	return c.errorOf(status, &cerr)
}

// encodePNG answers img as PNG bytes.
func encodePNG(img image.Image) ([]byte, error) {
	var b bytes.Buffer
	if err := png.Encode(&b, img); err != nil {
		return nil, err
	}
	return b.Bytes(), nil
}
