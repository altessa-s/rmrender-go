// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender

/*
#include "bridge.h"
*/
import "C"

import "unsafe"

// Box is a rectangle in fractions of the drawn page, from its top left
// corner: X0 and X1 across, Y0 and Y1 down, each within 0 and 1.
type Box struct {
	X0, Y0, X1, Y1 float64
}

// Band is where a highlighter stroke or a highlight lies, its colour as
// 0xRRGGBB, and the identifier of the stroke or highlight it came from.
type Band struct {
	Box   Box
	Color uint32
	Item  string
}

// Highlight is text an imported PDF page had highlighted, with the band the
// highlight is drawn as.
type Highlight struct {
	Text string
	Band Band
}

// Marks are the highlights of one page: the highlighter strokes drawn over
// handwriting or typed text, which an OCR pass reads the words under, and
// the highlighted text of an imported PDF, which comes with its words.
// Values only: they hold no C memory.
type Marks struct {
	PageID     string
	Bands      []Band
	Highlights []Highlight
}

// Empty reports whether the page has neither bands nor highlights.
func (m *Marks) Empty() bool {
	return m == nil || len(m.Bands) == 0 && len(m.Highlights) == 0
}

// marksOf copies the marks C answered into Go values.
func marksOf(v *C.rmm_marked_page) (*Marks, error) {
	m := &Marks{}
	if v == nil {
		return m, nil
	}
	var err error
	if m.PageID, err = goText(v.page_id.bytes, v.page_id.length, "page id"); err != nil {
		return nil, err
	}
	bands, err := goLength(v.band_count, "band count")
	if err != nil {
		return nil, err
	}
	if bands > 0 {
		m.Bands = make([]Band, bands)
		for i, b := range unsafe.Slice(v.bands, bands) {
			if m.Bands[i], err = bandOf(b); err != nil {
				return nil, err
			}
		}
	}
	highlights, err := goLength(v.highlight_count, "highlight count")
	if err != nil {
		return nil, err
	}
	if highlights > 0 {
		m.Highlights = make([]Highlight, highlights)
		for i, h := range unsafe.Slice(v.highlights, highlights) {
			if m.Highlights[i].Text, err = goText(h.text.bytes, h.text.length, "highlight text"); err != nil {
				return nil, err
			}
			if m.Highlights[i].Band, err = bandOf(h.band); err != nil {
				return nil, err
			}
		}
	}
	return m, nil
}

func bandOf(b C.rmm_mark_band) (Band, error) {
	item, err := goText(b.item.bytes, b.item.length, "band item")
	return Band{
		Box:   Box{float64(b.box.x0), float64(b.box.y0), float64(b.box.x1), float64(b.box.y1)},
		Color: uint32(b.color),
		Item:  item,
	}, err
}
