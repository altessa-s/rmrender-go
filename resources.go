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
	"sync"
)

// Fonts lay out typed text with the bundled open fonts: Noto Sans in four
// weights, EB Garamond, Noto Sans for Arabic, Devanagari, Hebrew, Japanese,
// Korean and Thai, and, optionally, Noto Emoji. They are never registered
// with the system and no other font is substituted for a missing one.
//
// A Fonts serves one call at a time: calls that share one take turns. Close
// it to release its native fonts; it has no finalizer.
type Fonts struct {
	mu sync.Mutex
	c  *C.rmrgo_fonts
}

// NewFonts loads the fonts from fsys, which holds them as <name>.ttf at its
// root, the way package [github.com/altessa-s/rmrender-go/fonts] does:
//
//	f, err := rmrender.NewFonts(fonts.FS)
//
// Every font is read and handed to the backend before NewFonts returns;
// fsys is not kept.
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
	return &Fonts{c: out}, nil
}

// Close releases the native fonts, after any call using them has finished.
// A closed Fonts makes a call fail with StatusInvalidArgument; closing it
// again does nothing.
func (f *Fonts) Close() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	C.rmrgo_fonts_free(f.c)
	f.c = nil
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
// bundled ones. Each layer holds name.template files at its root.
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
