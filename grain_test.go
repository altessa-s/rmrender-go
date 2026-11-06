// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"context"
	"testing"

	"github.com/altessa-s/rmrender-go"
)

// A fixture page with grained strokes.
const grainPage = linesPage

// grainPDFs answers the PDF of the grained page, alone and as a document,
// drawn with grain.
func grainPDFs(t *testing.T, grain rmrender.PDFGrain) (page, document []byte) {
	t.Helper()
	data := loadPage(t, grainPage)
	o := withFonts(t, rmrender.Options{PDFGrain: grain})
	page, err := rmrender.PagePDF(context.Background(), data, o)
	if err != nil {
		t.Fatalf("grain %d: %v", grain, err)
	}
	wantPDF(t, page, 1)
	d := openDocument(t, notebook(t, "", data))
	document, _, err = d.PDF(context.Background(), rmrender.PDFFresh, o)
	if err != nil {
		t.Fatalf("grain %d: %v", grain, err)
	}
	wantPDF(t, document, 1)
	return page, document
}

func TestUnknownPDFGrain(t *testing.T) {
	data := loadPage(t, grainPage)
	for _, grain := range []rmrender.PDFGrain{-1, rmrender.PDFGrainGroup + 1} {
		o := withFonts(t, rmrender.Options{PDFGrain: grain})
		_, err := rmrender.PagePDF(context.Background(), data, o)
		wantStatus(t, err, rmrender.StatusInvalidArgument)
		d := openDocument(t, notebook(t, "", data))
		_, _, err = d.PDF(context.Background(), rmrender.PDFFresh, o)
		wantStatus(t, err, rmrender.StatusInvalidArgument)
	}
}
