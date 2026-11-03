// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"strings"
	"testing"

	"github.com/altessa-s/rmrender-go"
)

// wantBox fails the test unless b lies within the page and is not inverted.
func wantBox(t testing.TB, b rmrender.Box) {
	t.Helper()
	for _, v := range []float64{b.X0, b.Y0, b.X1, b.Y1} {
		if v < 0 || v > 1 {
			t.Fatalf("box %+v leaves the page", b)
		}
	}
	if b.X0 > b.X1 || b.Y0 > b.Y1 {
		t.Fatalf("box %+v is inverted", b)
	}
}

// The highlighted text of an imported PDF page comes with its words.
func TestHighlightedText(t *testing.T) {
	m, err := rmrender.PageMarks(loadPage(t, highlightedPage), "page-1", withFonts(t, rmrender.Options{}))
	if err != nil {
		t.Fatal(err)
	}
	if m.PageID != "page-1" {
		t.Fatalf("page ID %q", m.PageID)
	}
	if len(m.Highlights) == 0 || m.Empty() {
		t.Fatal("no highlights")
	}
	for _, h := range m.Highlights {
		if strings.TrimSpace(h.Text) == "" {
			t.Fatalf("a highlight without text: %+v", h)
		}
		if h.Band.Item == "" || h.Band.Color == 0 {
			t.Fatalf("a highlight without its band: %+v", h)
		}
		wantBox(t, h.Band.Box)
	}
}

// Highlighter strokes become bands, each in its own colour.
func TestHighlighterBands(t *testing.T) {
	m, err := rmrender.PageMarks(loadPage(t, colorPage), "", withFonts(t, rmrender.Options{}))
	if err != nil {
		t.Fatal(err)
	}
	if len(m.Bands) == 0 {
		t.Fatal("no bands")
	}
	colors := map[uint32]bool{}
	for _, b := range m.Bands {
		wantBox(t, b.Box)
		colors[b.Color] = true
	}
	if len(colors) < 2 {
		t.Fatalf("bands in %d colours", len(colors))
	}
	if m.PageID != "" {
		t.Fatalf("page ID %q", m.PageID)
	}
}

func TestNoMarks(t *testing.T) {
	m, err := rmrender.PageMarks(loadPage(t, strokesPage), "x", withFonts(t, rmrender.Options{}))
	if err != nil {
		t.Fatal(err)
	}
	if !m.Empty() {
		t.Fatalf("marks on a page of handwriting: %+v", m)
	}
}
