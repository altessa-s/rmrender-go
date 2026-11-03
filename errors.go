// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender

/*
#include "bridge.h"
*/
import "C"

import (
	"strings"
)

// Status says what went wrong. The numbers are the C library's.
type Status int

// The statuses a call can answer with.
const (
	// StatusOK: no error. No *Error carries it.
	StatusOK Status = iota
	// StatusInvalidArgument: an argument the call cannot be made with, or
	// input that does not hold together: an archive that is not one, a page
	// index out of range, a closed Fonts or Document.
	StatusInvalidArgument
	// StatusOutOfMemory: an allocation failed, or a size is larger than a Go
	// int can hold.
	StatusOutOfMemory
	// StatusLimit: a [Limits] bound would be exceeded.
	StatusLimit
	// StatusUnsupported: the input needs something the library or the
	// backend does not provide, such as a page of a newer format, a font the
	// [Fonts] lack, or an encrypted PDF.
	StatusUnsupported
	// StatusBackend: the native backend failed, for example on an
	// undecodable image or a page that does not parse; or a font, template
	// or image could not be read.
	StatusBackend
	// StatusCancelled: the context ended. [errors.Unwrap] answers its cause.
	StatusCancelled
)

// String answers the status as a bare English phrase, and a phrase of its own
// for a number the C library does not name.
func (s Status) String() string {
	if s < 0 || int64(s) > int64(^uint32(0)) {
		return "unknown status"
	}
	return C.GoString(C.rmr_status_string(C.rmr_status(s)))
}

// Error is what every call of this package fails with.
type Error struct {
	// Status says which failure this is.
	Status Status
	// Message is what went wrong in words: the C library's phrase, or what
	// the Go side refused.
	Message string
	// Index is the position in the input the failure concerns, where the C
	// library names one, such as the draw command a backend failed on.
	Index int
	// Err is the cause: what reading a font, template or image failed with,
	// or the cause of a cancelled context; nil otherwise.
	Err error
}

// Sentinels for [errors.Is]: an *Error matches the sentinel of its Status,
// whatever its other fields hold.
var (
	ErrInvalidArgument = &Error{Status: StatusInvalidArgument}
	ErrOutOfMemory     = &Error{Status: StatusOutOfMemory}
	ErrLimit           = &Error{Status: StatusLimit}
	ErrUnsupported     = &Error{Status: StatusUnsupported}
	ErrBackend         = &Error{Status: StatusBackend}
	ErrCancelled       = &Error{Status: StatusCancelled}
)

// Error spells the status, then the message when it says more, and the
// cause.
func (e *Error) Error() string {
	if e == nil {
		return "rmrender: <nil>"
	}
	var b strings.Builder
	b.WriteString("rmrender: ")
	status := e.Status.String()
	b.WriteString(status)
	if e.Message != "" && e.Message != status {
		b.WriteString(": ")
		b.WriteString(e.Message)
	}
	if e.Err != nil {
		b.WriteString(": ")
		b.WriteString(e.Err.Error())
	}
	return b.String()
}

// Is reports whether target is an *Error of the same Status. A nil *Error,
// on either side, matches nothing.
func (e *Error) Is(target error) bool {
	t, ok := target.(*Error)
	return ok && t != nil && e != nil && t.Status == e.Status
}

// Unwrap answers the cause, nil when there is none.
func (e *Error) Unwrap() error {
	if e == nil {
		return nil
	}
	return e.Err
}

// cError turns a failed C status and the error record beside it into an
// *Error.
func cError(status C.rmr_status, cerr *C.rmr_error) *Error {
	e := &Error{
		Status:  Status(status),
		Message: C.GoString(&cerr.message[0]),
	}
	if uint64(cerr.index) <= uint64(maxInt) {
		e.Index = int(cerr.index)
	}
	return e
}

// errInvalid answers an argument the Go side refuses before C sees it.
func errInvalid(message string) error {
	return &Error{Status: StatusInvalidArgument, Message: message}
}

// errTooLarge answers a size C states and a Go int cannot hold. No Go value
// of that size can be allocated, so it is reported the way a failed
// allocation is.
func errTooLarge(what string) error {
	return &Error{Status: StatusOutOfMemory, Message: what + " too large for a Go int"}
}
