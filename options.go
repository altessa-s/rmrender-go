// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender

/*
#include "bridge.h"
*/
import "C"

import (
	"image/color"
	"io/fs"
	"math"
)

// PointsPerUnit is the size of one .rm unit in PDF points: the scale of a
// page drawn at the tablet's own size.
const PointsPerUnit = 0.317180616

// Paper is the sheet a notebook page is laid out on.
type Paper int

// The papers. A landscape canvas turns A4 and US Letter.
const (
	// PaperA4 is A4, 595.276 by 841.89 points.
	PaperA4 Paper = iota
	// PaperLetter is US Letter, 612 by 792 points.
	PaperLetter
	// PaperTablet is the canvas itself at [PointsPerUnit].
	PaperTablet
)

// Edge is how a page grows with ink that goes past its canvas.
type Edge int

// The page growth modes.
const (
	// EdgeRibbon follows the tablet's notebook export: overhang is trimmed
	// while the stroke's centreline stays inside an edge, and the page grows
	// down only past a small foot.
	EdgeRibbon Edge = iota
	// EdgeUncut keeps the overhanging width of strokes.
	EdgeUncut
	// EdgeCentreline follows stroke centrelines, adding [Options.Margin]
	// along the edges that grew.
	EdgeCentreline
)

// PDFMode says how a document becomes a PDF.
type PDFMode int

// The document PDF modes. They differ only for a document with an imported
// PDF; a notebook renders the same in each.
const (
	// PDFFresh draws a new PDF: imported pages as vectors, with their links
	// and outline, but without forms, signatures or other annotations.
	PDFFresh PDFMode = iota
	// PDFPreserving appends the marks to the imported PDF as an incremental
	// update, leaving its bytes, annotations, forms and signed revisions as
	// they were. A changed page order, count or visibility is refused.
	PDFPreserving
	// PDFRecomposed writes a new single-revision PDF from the imported one's
	// objects, allowing pages to be reordered, removed or repeated, and
	// keeping forms and annotations. Existing signatures stop verifying.
	PDFRecomposed
)

// Limits bounds what one call may build: counts of points, path elements and
// draw commands, the bytes of plans, bitmaps and PDFs, the side of a bitmap,
// and the magnitude of coordinates and widths. They cover memory the C
// library owns; allocations of the native frameworks are outside them.
type Limits struct {
	MaximumPoints      int
	MaximumElements    int
	MaximumDraws       int
	MaximumPlanBytes   int
	MaximumBitmapBytes int
	MaximumPDFBytes    int
	MaximumBitmapSide  int
	MaximumCoordinate  float64
	MaximumWidth       float64
}

// DefaultLimits answers the C library's defaults, used wherever no Limits
// are given: a million points, four million elements, 65,535 draws,
// 256 MiB of plan, bitmap and PDF each, 32,768 pixels a side.
func DefaultLimits() Limits {
	l := C.rmr_limits_default()
	return Limits{
		MaximumPoints:      int(l.maximum_points),
		MaximumElements:    int(l.maximum_elements),
		MaximumDraws:       int(l.maximum_draws),
		MaximumPlanBytes:   int(l.maximum_plan_bytes),
		MaximumBitmapBytes: int(l.maximum_bitmap_bytes),
		MaximumPDFBytes:    int(l.maximum_pdf_bytes),
		MaximumBitmapSide:  int(l.maximum_bitmap_side),
		MaximumCoordinate:  float64(l.maximum_coordinate),
		MaximumWidth:       float64(l.maximum_width),
	}
}

// cLimits answers l for C, the defaults for nil.
func cLimits(l *Limits) (C.rmr_limits, error) {
	if l == nil {
		return C.rmr_limits_default(), nil
	}
	for _, n := range []int{l.MaximumPoints, l.MaximumElements, l.MaximumDraws, l.MaximumPlanBytes,
		l.MaximumBitmapBytes, l.MaximumPDFBytes, l.MaximumBitmapSide} {
		if n < 0 {
			return C.rmr_limits{}, errInvalid("negative limit")
		}
	}
	if uint64(l.MaximumBitmapSide) > math.MaxUint32 {
		return C.rmr_limits{}, errInvalid("bitmap side limit too large")
	}
	return C.rmr_limits{
		maximum_points:       C.size_t(l.MaximumPoints),
		maximum_elements:     C.size_t(l.MaximumElements),
		maximum_draws:        C.size_t(l.MaximumDraws),
		maximum_plan_bytes:   C.size_t(l.MaximumPlanBytes),
		maximum_bitmap_bytes: C.size_t(l.MaximumBitmapBytes),
		maximum_pdf_bytes:    C.size_t(l.MaximumPDFBytes),
		maximum_bitmap_side:  C.uint32_t(l.MaximumBitmapSide),
		maximum_coordinate:   C.double(l.MaximumCoordinate),
		maximum_width:        C.double(l.MaximumWidth),
	}, nil
}

// Options says how a page or a document is drawn. The zero value, or nil,
// draws on A4 with the bundled templates, no fonts and a transparent
// background. Each field names the calls that read it.
type Options struct {
	// Fonts lay out typed text, and anchor the handwriting written on it.
	// A page with a text block needs them, typed or empty, and the tablet
	// writes one on most pages: without Fonts such a page fails with
	// StatusUnsupported.
	Fonts *Fonts
	// Templates resolve page templates by name; nil means
	// [BundledTemplates].
	Templates *Templates
	// Paper is the sheet a notebook page is laid out on. A page over an
	// imported PDF takes the size of its PDF page instead.
	Paper Paper
	// Edge is how a lone .rm page grows with overhanging ink, and Margin
	// the margin EdgeCentreline adds, in .rm units. A document's pages grow
	// as the document export grows them.
	Edge   Edge
	Margin float64
	// Template names the template a lone .rm page is drawn on, such as
	// "P Lines medium". A .rm file does not name its own; a document's pages
	// take theirs from the document.
	Template string
	// Images resolve the images a lone .rm page places, by file name; nil
	// leaves them blank in their place. A document's pages take theirs from
	// the archive.
	Images fs.FS
	// Background fills a bitmap before drawing; nil leaves it transparent.
	Background color.Color
	// MaximumPages caps the pages of a document PDF; zero means all.
	MaximumPages int
	// Limits bound the work; nil means [DefaultLimits].
	Limits *Limits
}

// request is the C side of opts, for a call that holds the fonts' lock.
func (o *Options) request() (C.rmrgo_request, error) {
	var r C.rmrgo_request
	if o == nil {
		o = &Options{}
	}
	limits, err := cLimits(o.Limits)
	if err != nil {
		return r, err
	}
	if o.Paper < PaperA4 || o.Paper > PaperTablet {
		return r, errInvalid("unknown paper")
	}
	if o.Edge < EdgeRibbon || o.Edge > EdgeCentreline {
		return r, errInvalid("unknown edge")
	}
	r.paper = C.rmr_paper(o.Paper)
	r.edge = C.rmr_ink_edge(o.Edge)
	r.margin = C.double(o.Margin)
	r.limits = limits
	return r, nil
}

// cColor answers c as the straight, unpremultiplied colour C takes;
// transparent for nil.
func cColor(c color.Color) C.rmr_color {
	if c == nil {
		return C.rmr_color{}
	}
	n := color.NRGBA64Model.Convert(c).(color.NRGBA64)
	return C.rmr_color{
		red:   C.double(float64(n.R) / 0xffff),
		green: C.double(float64(n.G) / 0xffff),
		blue:  C.double(float64(n.B) / 0xffff),
		alpha: C.double(float64(n.A) / 0xffff),
	}
}
