// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"errors"
	"fmt"
	"testing"

	"github.com/altessa-s/rmrender-go"
)

// The statuses are the C library's, in its order and with its words.
func TestStatusNames(t *testing.T) {
	want := []string{"ok", "invalid argument", "out of memory", "limit exceeded", "unsupported feature", "backend failed", "cancelled"}
	statuses := []rmrender.Status{rmrender.StatusOK, rmrender.StatusInvalidArgument, rmrender.StatusOutOfMemory,
		rmrender.StatusLimit, rmrender.StatusUnsupported, rmrender.StatusBackend, rmrender.StatusCancelled}
	for i, s := range statuses {
		if int(s) != i || s.String() != want[i] {
			t.Errorf("status %d is %q, want %d %q", s, s, i, want[i])
		}
	}
	for _, s := range []rmrender.Status{-1, 7, 1 << 40} {
		if s.String() != "unknown status" {
			t.Errorf("status %d is %q", int(s), s)
		}
	}
}

func TestErrorText(t *testing.T) {
	cause := errors.New("disk on fire")
	for _, c := range []struct {
		err  *rmrender.Error
		want string
	}{
		{&rmrender.Error{Status: rmrender.StatusLimit}, "rmrender: limit exceeded"},
		{&rmrender.Error{Status: rmrender.StatusLimit, Message: "limit exceeded"}, "rmrender: limit exceeded"},
		{&rmrender.Error{Status: rmrender.StatusBackend, Message: "font resources could not be loaded", Err: cause},
			"rmrender: backend failed: font resources could not be loaded: disk on fire"},
		{nil, "rmrender: <nil>"},
	} {
		if got := c.err.Error(); got != c.want {
			t.Errorf("got %q, want %q", got, c.want)
		}
	}
}

func TestErrorsIs(t *testing.T) {
	cause := errors.New("cause")
	err := fmt.Errorf("drawing: %w", &rmrender.Error{Status: rmrender.StatusUnsupported, Message: "x", Err: cause})
	if !errors.Is(err, rmrender.ErrUnsupported) || errors.Is(err, rmrender.ErrLimit) || !errors.Is(err, cause) {
		t.Fatal("errors.Is does not match by status and cause")
	}
	var nilError *rmrender.Error
	if nilError.Is(rmrender.ErrLimit) || rmrender.ErrLimit.Is(nilError) || nilError.Unwrap() != nil {
		t.Fatal("a nil *Error matches")
	}
}
