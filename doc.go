// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

// Package rmrender draws reMarkable pages and documents: a .rm page to a PNG,
// a JPEG, a one-page PDF or an SVG document, a .rmdoc document page by page
// or whole to a PDF or to SVG, with handwriting and its pen textures, typed
// text, highlights, images and page templates; and it reads the highlights
// of a page as Go values.
//
// The package binds rmrender-lib, a C23 library, together with the libraries
// it reads its input with: rmlines-lib for .rm pages, rmdoc-lib for .rmdoc
// archives and rmmarks-lib for highlights. All four are vendored into this
// module and compiled by cgo. The drawing backend is native: CoreGraphics
// and CoreText on darwin, Cairo, Pango and PDFium on linux and windows (and
// on darwin with the rmrender_cairo build tag). Building needs a C23
// compiler; the Cairo backend also needs the Cairo stack and PDFium, on
// windows from MSYS2 CLANG64, as the README says.
//
// # Pages
//
// [PageImage], [PagePNG], [PageJPEG], [PagePDF] and [PageSVG] draw the bytes
// of one .rm page on a [Paper], with a template named in [Options] and the
// page grown with its ink as [Edge] says. [PageMarks] answers its highlights.
//
// # Documents
//
// [OpenDocument] unpacks a .rmdoc archive. A [Document] draws any of its pages
// as the document's own PDF lays them out, imported PDF pages beneath their
// marks, and the whole document as a PDF in each [PDFMode] or as one SVG
// document per page, with a [Report] of what could not be kept. SVG holds an
// imported PDF page as a raster, at [Options].PDFRasterDPI. A PDF draws
// grained ink as [Options].PDFGrain says.
//
// # Fonts and templates
//
// A page with a text block needs [Fonts], made from the files package
// [github.com/altessa-s/rmrender-go/fonts] embeds; the tablet writes a text
// block on most pages. Templates come from [BundledTemplates], after any
// layers given to [NewTemplates]. Nothing is read from the file system
// unless an [io/fs.FS] given here reads it.
//
// # Errors
//
// Every failure is an [*Error]. Its [Status] says which, and the sentinel
// values (ErrInvalidArgument, ErrLimit and the others) match it through
// [errors.Is]. A failure to read a font, template or image unwraps to what
// the read failed with; a cancelled call unwraps to its context's cause.
//
// # Ownership and concurrency
//
// Results are Go values, copied out of C memory before a call returns. A
// [Fonts] and a [Document] own C memory until they are closed, and have no
// finalizer. A Fonts serves one call at a time, as the C library requires,
// so calls that share one take turns; so do the calls of one Document.
// [Templates] are read only and may be shared freely. The Cairo backend's
// PDFium is process-wide; the C library serializes its use and keeps it for
// the life of the process.
package rmrender
