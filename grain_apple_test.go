// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

//go:build darwin && !rmrender_cairo

package rmrender_test

import (
	"bytes"
	"testing"

	"github.com/altessa-s/rmrender-go"
)

// CoreGraphics always draws grain PDFGrainMask's way, the zero value's:
// PDFGrainGroup draws the same.
func TestPDFGrainIsIgnoredByCoreGraphics(t *testing.T) {
	zeroPage, zeroDocument := grainPDFs(t, 0)
	for _, grain := range []rmrender.PDFGrain{rmrender.PDFGrainMask, rmrender.PDFGrainGroup} {
		page, document := grainPDFs(t, grain)
		if !samePDF(page, zeroPage) || !samePDF(document, zeroDocument) {
			t.Fatalf("grain %d changed CoreGraphics output", grain)
		}
	}
}

// samePDF reports whether a and b draw the same: equal bytes but for the
// dates and the file identifier, which every call writes anew, after the
// pages.
func samePDF(a, b []byte) bool {
	if len(a) != len(b) {
		return false
	}
	end := len(a)
	for _, key := range []string{"/Producer", "/CreationDate", "/ModDate"} {
		if i := bytes.Index(a, []byte(key)); i >= 0 {
			end = min(end, i)
		}
	}
	return bytes.Equal(a[:end], b[:end])
}
