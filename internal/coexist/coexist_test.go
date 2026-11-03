// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

// Package coexist links rmrender-go into one program with rmlines-go,
// rmdoc-go and rmmarks-go, which compile the same C libraries under their own
// names. It is a module of its own, so that the main module does not depend
// on them: that the test binary links at all is the test, and each package
// then reads or draws the same page.
package coexist

import (
	"context"
	"os"
	"testing"

	"github.com/altessa-s/rmdoc-go"
	"github.com/altessa-s/rmlines-go"
	"github.com/altessa-s/rmmarks-go"
	"github.com/altessa-s/rmrender-go"
	"github.com/altessa-s/rmrender-go/fonts"
)

func TestOneProgram(t *testing.T) {
	page, err := os.ReadFile("../../testdata/pages/Wikipedia_highlighted_p1.rm")
	if err != nil {
		t.Fatal(err)
	}
	parsed, err := rmlines.Parse(page)
	if err != nil {
		t.Fatal(err)
	}
	if len(parsed.Highlights) == 0 {
		t.Fatal("rmlines-go read no highlights")
	}

	archive, err := rmdoc.Pack([]rmdoc.File{
		{Name: "a1b2.content", Data: []byte(`{"cPages":{"pages":[{"id":"p1","idx":{"value":"ba"}}]}}`)},
		{Name: "a1b2/p1.rm", Data: page},
	})
	if err != nil {
		t.Fatal(err)
	}

	f, err := rmrender.NewFonts(fonts.FS)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	doc, err := rmrender.OpenDocument(archive, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer doc.Close()
	marks, err := doc.PageMarks(0, &rmrender.Options{Fonts: f})
	if err != nil {
		t.Fatal(err)
	}
	if len(marks.Highlights) != len(parsed.Highlights) {
		t.Fatalf("rmrender-go found %d highlights, rmlines-go %d", len(marks.Highlights), len(parsed.Highlights))
	}
	if _, _, err := doc.PDF(context.Background(), rmrender.PDFFresh, &rmrender.Options{Fonts: f}); err != nil {
		t.Fatal(err)
	}

	var boxes []rmmarks.PageBox
	for _, h := range marks.Highlights {
		boxes = append(boxes, rmmarks.PageBox{X0: h.Band.Box.X0, Y0: h.Band.Box.Y0, X1: h.Band.Box.X1, Y1: h.Band.Box.Y1})
	}
	if _, err := rmmarks.PassagesFromReading(boxes, nil); err != nil {
		t.Fatal(err)
	}
}
