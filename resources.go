// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender

/*
#include "bridge.h"
*/
import "C"

import (
	"embed"
	"io/fs"
)

// Fonts lay out typed text with the bundled open fonts: Noto Sans in four
// weights, EB Garamond, Noto Sans for Arabic, Devanagari, Hebrew, Japanese,
// Korean and Thai, and, optionally, Noto Emoji. They are never registered
// with the system and no other font is substituted for a missing one.
//
// A Fonts serves one call at a time: calls that share one take turns, and a
// call made while another uses the fonts waits for it, until its context
// ends; a call that takes no context waits as long as it takes. Close it to
// release its native fonts; it has no finalizer.
type Fonts struct {
	owner owner
	// c is read only by a call that has borrowed it from owner.
	c *C.rmrgo_fonts
}

// NewFonts loads the fonts from fsys, which holds them as <name>.ttf at its
// root, the way package [github.com/altessa-s/rmrender-go/fonts] does:
//
//	f, err := rmrender.NewFonts(fonts.FS)
//
// Every font is read and handed to the backend before NewFonts returns;
// fsys is not kept.
//
// Resource reads run on a separate goroutine, synchronously with this call.
// A panic or runtime.Goexit stops C; after its memory and callback state are
// released, the calling goroutine panics or exits, running its defers.
//
// NewFonts borrows no Document and no Fonts, so fsys may call into this
// package, Close included, and draw anything. Called itself from a callback
// of another call, as from a template read, NewFonts is bound by what that
// callback may do, and its fsys with it.
func NewFonts(fsys fs.FS) (*Fonts, error) {
	if fsys == nil {
		return nil, errInvalid("no font files")
	}
	c := &call{fonts: fsys}
	limits := C.rmr_limits_default()
	var out *C.rmrgo_fonts
	var cerr C.rmr_error
	status := c.run(func(handle C.uintptr_t) C.rmr_status {
		return C.rmrgo_fonts_create(handle, &limits, &out, &cerr)
	})
	if err := c.errorOf(status, &cerr); err != nil {
		return nil, err
	}
	f := &Fonts{c: out}
	liveFonts.Add(1)
	f.owner.hold(func() {
		C.rmrgo_fonts_free(f.c)
		f.c = nil
		liveFonts.Add(-1)
	})
	return f, nil
}

// Close releases the native fonts. It returns at once, even while a call
// uses them, and from a callback of that call too: the fonts are then
// released when that call finishes, after it has released its callback
// state, so their memory may outlive Close. A closed Fonts makes a call fail
// with StatusInvalidArgument, a call that was waiting for its turn too;
// closing it again does nothing.
func (f *Fonts) Close() error {
	f.owner.close()
	return nil
}

//go:embed internal/clib/rmrender/resources/templates
var bundled embed.FS

// bundledTemplates is the template directory of bundled, at its root.
var bundledTemplates = func() fs.FS {
	sub, err := fs.Sub(bundled, "internal/clib/rmrender/resources/templates")
	if err != nil {
		panic(err)
	}
	return sub
}()

// BundledTemplates answers rmrender-lib's page templates, as name.template
// files at the root: the tablet's templates redrawn as vector models, such
// as "P Lines medium", "P Grid small" and "LS Dots top".
func BundledTemplates() fs.FS {
	return bundledTemplates
}

// Templates resolve a page's template by its name: the first layer holding
// name.template wins, and the bundled templates come last. A template no
// layer holds leaves the page blank behind its ink; a malformed one fails
// the call. A Templates is read only, and may be shared.
type Templates struct {
	search []fs.FS
}

// NewTemplates answers templates looked up in layers, in order, before the
// bundled ones. Each layer holds name.template files at its root. Reads run
// on a separate goroutine, synchronously with the render or export using
// them; a panic or runtime.Goexit stops C and is raised again on the calling
// goroutine after cleanup.
//
// A read may close the Document or the Fonts of the call it serves, or any
// other, and may draw a different document, or a lone page, without that
// call's Fonts. It must not draw the call's own document, nor with the
// call's Fonts: such a call waits for the call it serves, which waits for
// the read, so it ends only when its own context does, with that context's
// error, and one that takes no context only when what it waits for is
// closed.
func NewTemplates(layers ...fs.FS) *Templates {
	t := &Templates{}
	for _, l := range layers {
		if l != nil {
			t.search = append(t.search, l)
		}
	}
	t.search = append(t.search, bundledTemplates)
	return t
}

// NoTemplates answers templates that hold no name, not even the bundled
// ones: every page, of a lone .rm or of a document, is drawn without its
// template, ink and imported PDF pages only.
func NoTemplates() *Templates {
	return &Templates{search: []fs.FS{}}
}

// layers answers where a name is looked up, in order; only the bundled
// templates for a nil t.
func (t *Templates) layers() []fs.FS {
	if t == nil {
		return []fs.FS{bundledTemplates}
	}
	return t.search
}
