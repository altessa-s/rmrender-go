// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"testing"

	"github.com/altessa-s/rmrender-go"
)

// fuzzLimits keep one input cheap: arbitrary bytes may place ink far away
// and grow the page with it, and the template repeats over all of it.
func fuzzLimits() *rmrender.Limits {
	l := rmrender.DefaultLimits()
	l.MaximumPoints = 1 << 17
	l.MaximumElements = 1 << 18
	l.MaximumDraws = 1 << 14
	l.MaximumBitmapSide = 2048
	l.MaximumBitmapBytes = 16 << 20
	l.MaximumPDFBytes = 16 << 20
	l.MaximumPlanBytes = 64 << 20
	return &l
}

// FuzzPage draws arbitrary bytes as a .rm page, seeded with testdata/pages.
// Each call must answer a result or an *Error and never crash: an image of
// at least one pixel, a one-page PDF, and marks inside the page.
func FuzzPage(f *testing.F) {
	seeds, err := filepath.Glob(filepath.Join("testdata", "pages", "*.rm"))
	if err != nil {
		f.Fatal(err)
	}
	for _, path := range seeds {
		data, err := os.ReadFile(path)
		if err != nil {
			f.Fatal(err)
		}
		f.Add(data)
	}
	o := withFonts(f, rmrender.Options{Limits: fuzzLimits(), Template: "P Lines small"})
	f.Fuzz(func(t *testing.T, data []byte) {
		wantError := func(err error) bool {
			t.Helper()
			if err == nil {
				return false
			}
			var e *rmrender.Error
			if !errors.As(err, &e) || e.Status == rmrender.StatusOK {
				t.Fatalf("not an *Error: %v", err)
			}
			return true
		}
		img, err := rmrender.PageImage(context.Background(), data, 0.05, o)
		if !wantError(err) && img.Bounds().Empty() {
			t.Fatal("an empty image")
		}
		pdf, err := rmrender.PagePDF(context.Background(), data, o)
		if !wantError(err) {
			wantPDF(t, pdf, 1)
		}
		m, err := rmrender.PageMarks(data, "", o)
		if !wantError(err) {
			for _, b := range m.Bands {
				if b.Box.X0 < 0 || b.Box.X1 > 1 || b.Box.Y0 < 0 || b.Box.Y1 > 1 {
					t.Fatalf("a band leaves the page: %+v", b)
				}
			}
		}
	})
}
