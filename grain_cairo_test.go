// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

//go:build linux || windows || (darwin && rmrender_cairo)

package rmrender_test

import (
	"testing"

	"github.com/altessa-s/rmrender-go"
)

// On Cairo a transparency group per grained stroke costs bytes that drawing
// the stencil straight onto the page does not.
func TestPDFGrainGroupIsLargerOnCairo(t *testing.T) {
	maskPage, maskDocument := grainPDFs(t, rmrender.PDFGrainMask)
	groupPage, groupDocument := grainPDFs(t, rmrender.PDFGrainGroup)
	if len(groupPage) <= len(maskPage) || len(groupDocument) <= len(maskDocument) {
		t.Fatalf("group %d and %d bytes, mask %d and %d", len(groupPage), len(groupDocument), len(maskPage), len(maskDocument))
	}
}
