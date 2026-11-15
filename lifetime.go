// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender

import (
	"context"
	"sync"
	"sync/atomic"
)

// owner holds one C object, a document or a set of fonts, for the calls that
// borrow it and for Close.
//
// Calls take turns: one at a time borrows the object, and the next waits for
// its turn as long as its context lets it, so that a call queued behind a
// slow one, or behind the very call whose callback made it, ends when its
// context does. Close takes no turn. It marks the object closed and returns:
// the object is released at once when no call borrows it, or else by the
// call that does, when it gives the object back. Calls waiting for a turn
// then find it closed.
type owner struct {
	setup sync.Once
	// turn holds a token while a call borrows the object; gone is closed by
	// Close, so that a waiting call stops waiting.
	turn chan struct{}
	gone chan struct{}

	mu     sync.Mutex
	closed bool
	busy   bool
	// free releases the object; nil once it is released, or for none.
	free func()
}

// init readies o; the zero owner holds no object.
func (o *owner) init() {
	o.setup.Do(func() {
		o.turn = make(chan struct{}, 1)
		o.gone = make(chan struct{})
	})
}

// hold gives o the object free releases.
func (o *owner) hold(free func()) {
	o.init()
	o.free = free
}

// acquire waits for o's turn, until ctx ends, and borrows the object. It
// fails with closed when the object is closed, or released, or is closed
// while the call waits, and with ctx's error when ctx ends first. A nil ctx
// waits as long as it takes. The caller gives the object back with release.
func (o *owner) acquire(ctx context.Context, closed string) error {
	o.init()
	if o.isClosed() {
		return errInvalid(closed)
	}
	var done <-chan struct{}
	if ctx != nil {
		done = ctx.Done()
	}
	select {
	case o.turn <- struct{}{}:
	default:
		select {
		case o.turn <- struct{}{}:
		case <-o.gone:
			return errInvalid(closed)
		case <-done:
			return cancelled(ctx)
		}
	}
	o.mu.Lock()
	if o.closed || o.free == nil {
		o.mu.Unlock()
		<-o.turn
		return errInvalid(closed)
	}
	o.busy = true
	o.mu.Unlock()
	return nil
}

// release gives back the object acquire borrowed, releasing it if Close was
// called meanwhile, and passes the turn on.
func (o *owner) release() {
	o.mu.Lock()
	o.busy = false
	var free func()
	if o.closed {
		free, o.free = o.free, nil
	}
	o.mu.Unlock()
	if free != nil {
		free()
	}
	<-o.turn
}

// close marks the object closed and releases it now, unless a call borrows
// it, which then releases it. Closing again does nothing.
func (o *owner) close() {
	o.init()
	o.mu.Lock()
	if o.closed {
		o.mu.Unlock()
		return
	}
	o.closed = true
	close(o.gone)
	var free func()
	if !o.busy {
		free, o.free = o.free, nil
	}
	o.mu.Unlock()
	if free != nil {
		free()
	}
}

// isClosed reports whether Close has been called.
func (o *owner) isClosed() bool {
	o.mu.Lock()
	defer o.mu.Unlock()
	return o.closed
}

// liveDocuments and liveFonts count the C documents and font sets not yet
// released, for the tests.
var liveDocuments, liveFonts atomic.Int64
