// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"bytes"
	"context"
	"errors"
	"image"
	"testing"

	"github.com/altessa-s/rmrender-go"
)

// junkPage is a .rm file that does not parse.
var junkPage = []byte("not a page")

// newerPage is the header of a .rm version this package does not read.
var newerPage = []byte("reMarkable .lines file, version=9          ")

// wantUnreadable fails the test unless err is the failure of page p2, the
// second, under UnreadableStrict.
func wantUnreadable(t testing.TB, err error, status rmrender.Status) {
	t.Helper()
	wantStatus(t, err, status)
	var page *rmrender.UnreadablePageError
	if !errors.As(err, &page) || page.Page != 1 || page.ID != "p2" {
		t.Fatalf("got %v, want page 1 (p2) unreadable", err)
	}
	var e *rmrender.Error
	if errors.As(err, &e); e.Index != 1 {
		t.Fatalf("got index %d, want 1", e.Index)
	}
}

// A page whose .rm file does not parse fails every call that reaches it by
// default, naming it, and leaves the others to be drawn.
func TestUnreadableStrict(t *testing.T) {
	ctx := context.Background()
	strokes := loadPage(t, strokesPage)
	d := openDocument(t, notebook(t, "P Grid small", strokes, junkPage, strokes))
	o := withFonts(t, rmrender.Options{})
	for _, mode := range []rmrender.PDFMode{rmrender.PDFFresh, rmrender.PDFPreserving, rmrender.PDFRecomposed} {
		_, _, err := d.PDF(ctx, mode, o)
		wantUnreadable(t, err, rmrender.StatusBackend)
		_, err = d.WritePDF(ctx, &bytes.Buffer{}, mode, o)
		wantUnreadable(t, err, rmrender.StatusBackend)
	}
	_, _, err := d.SVG(ctx, o)
	wantUnreadable(t, err, rmrender.StatusBackend)
	_, err = d.PageImage(ctx, 1, 0.25, o)
	wantUnreadable(t, err, rmrender.StatusBackend)
	_, err = d.PageSVG(ctx, 1, o)
	wantUnreadable(t, err, rmrender.StatusBackend)
	_, err = d.PageMarks(1, o)
	wantUnreadable(t, err, rmrender.StatusBackend)
	wantUnreadable(t, d.CheckPage(1), rmrender.StatusBackend)
	if !errors.Is(d.CheckPage(1), rmrender.ErrBackend) {
		t.Fatal("an unreadable page is not a backend failure")
	}
	for _, i := range []int{0, 2} {
		if err := d.CheckPage(i); err != nil {
			t.Fatalf("page %d: %v", i, err)
		}
		if _, err := d.PageImage(ctx, i, 0.25, o); err != nil {
			t.Fatalf("page %d: %v", i, err)
		}
	}
	first := *o
	first.MaximumPages = 1
	if _, _, err := d.PDF(ctx, rmrender.PDFFresh, &first); err != nil {
		t.Fatalf("a page not reached failed the PDF: %v", err)
	}

	newer := openDocument(t, notebook(t, "", strokes, newerPage))
	_, _, err = newer.PDF(ctx, rmrender.PDFFresh, o)
	wantUnreadable(t, err, rmrender.StatusUnsupported)

	unknown := *o
	unknown.Unreadable = rmrender.UnreadablePlaceholder + 1
	_, _, err = d.PDF(ctx, rmrender.PDFFresh, &unknown)
	wantStatus(t, err, rmrender.StatusInvalidArgument)
}

// A missing .rm file is a page nobody wrote on, never an unreadable one.
func TestMissingPageIsBlank(t *testing.T) {
	strokes := loadPage(t, strokesPage)
	d := openDocument(t, notebook(t, "P Grid small", strokes, nil))
	if err := d.CheckPage(1); err != nil {
		t.Fatal(err)
	}
	_, report, err := d.PDF(context.Background(), rmrender.PDFFresh, withFonts(t, rmrender.Options{}))
	if err != nil || report.UnreadablePages != 0 {
		t.Fatalf("report %+v, %v", report, err)
	}
}

// UnreadableBlank draws the page as a page without a .rm file, and counts it.
func TestUnreadableBlank(t *testing.T) {
	ctx := context.Background()
	strokes := loadPage(t, strokesPage)
	d := openDocument(t, notebook(t, "P Grid small", strokes, junkPage, strokes))
	without := openDocument(t, notebook(t, "P Grid small", strokes, nil, strokes))
	o := withFonts(t, rmrender.Options{Unreadable: rmrender.UnreadableBlank})
	got, err := d.PageImage(ctx, 1, 0.25, o)
	if err != nil {
		t.Fatal(err)
	}
	want, err := without.PageImage(ctx, 1, 0.25, o)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got.Pix, want.Pix) || inked(got) == 0 {
		t.Fatal("the unreadable page is not the blank page on its template")
	}
	for _, mode := range []rmrender.PDFMode{rmrender.PDFFresh, rmrender.PDFPreserving, rmrender.PDFRecomposed} {
		pdf, report, err := d.PDF(ctx, mode, o)
		if err != nil {
			t.Fatal(err)
		}
		wantPDF(t, pdf, 3)
		if report.UnreadablePages != 1 || report.RenderedPages != 3 {
			t.Fatalf("report %+v", report)
		}
		report, err = d.WritePDF(ctx, &bytes.Buffer{}, mode, o)
		if err != nil || report.UnreadablePages != 1 {
			t.Fatalf("report %+v, %v", report, err)
		}
	}
	svg, report, err := d.SVG(ctx, o)
	if err != nil || len(svg) != 3 || report.UnreadablePages != 1 {
		t.Fatalf("%d pages, report %+v, %v", len(svg), report, err)
	}
	if _, err := d.PageMarks(1, o); err != nil {
		t.Fatal(err)
	}
}

// greys counts the pixels of img near the grey of the label's box and of
// its text, and those inked outside the middle half of the page.
func greys(img *image.RGBA) (box, text, outside int) {
	b := img.Bounds()
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			c := img.RGBAAt(x, y)
			switch {
			case c.A == 0xff && c.R == c.G && c.G == c.B && c.R >= 238 && c.R <= 246:
				box++
			case c.A == 0xff && c.R == c.G && c.G == c.B && c.R >= 56 && c.R <= 72:
				text++
			}
			middle := x > b.Dx()/4 && x < b.Dx()*3/4 && y > b.Dy()/4 && y < b.Dy()*3/4
			if c.A != 0 && !middle {
				outside++
			}
		}
	}
	return box, text, outside
}

// UnreadablePlaceholder draws the label, in the bundled face, on a sheet
// without its template, in every output, and counts the page.
func TestUnreadablePlaceholder(t *testing.T) {
	ctx := context.Background()
	strokes := loadPage(t, strokesPage)
	d := openDocument(t, notebook(t, "P Grid small", strokes, junkPage, strokes))
	o := withFonts(t, rmrender.Options{Unreadable: rmrender.UnreadablePlaceholder})
	img, err := d.PageImage(ctx, 1, 0.5, o)
	if err != nil {
		t.Fatal(err)
	}
	if box, text, outside := greys(img); box < 1000 || text < 100 || outside != 0 {
		t.Fatalf("label: %d box pixels, %d text pixels, %d pixels outside the middle", box, text, outside)
	}
	for _, mode := range []rmrender.PDFMode{rmrender.PDFFresh, rmrender.PDFPreserving, rmrender.PDFRecomposed} {
		pdf, report, err := d.PDF(ctx, mode, o)
		if err != nil {
			t.Fatal(err)
		}
		wantPDF(t, pdf, 3)
		if report.UnreadablePages != 1 || !bytes.Contains(pdf, []byte("NotoSans")) {
			t.Fatalf("report %+v; label face embedded: %v", report, bytes.Contains(pdf, []byte("NotoSans")))
		}
	}
	page, err := d.PageSVG(ctx, 1, o)
	if err != nil || !bytes.Contains(page, []byte("#404040")) {
		t.Fatalf("SVG page without the label: %v", err)
	}
	svg, report, err := d.SVG(ctx, o)
	if err != nil || report.UnreadablePages != 1 || !bytes.Equal(svg[1], page) {
		t.Fatalf("SVG pages: report %+v, %v", report, err)
	}

	bare := *o
	bare.Fonts = nil
	_, err = d.PageImage(ctx, 1, 0.5, &bare)
	wantStatus(t, err, rmrender.StatusUnsupported)
}

// Over an imported PDF, the label is drawn on the PDF page in every mode,
// the preserving export still appending to the PDF's own bytes.
func TestUnreadableOverImportedPDF(t *testing.T) {
	ctx := context.Background()
	o := withFonts(t, rmrender.Options{Unreadable: rmrender.UnreadablePlaceholder})
	source, err := rmrender.PagePDF(ctx, loadPage(t, linesPage), o)
	if err != nil {
		t.Fatal(err)
	}
	d := openDocument(t, imported(t, source, junkPage))
	for _, mode := range []rmrender.PDFMode{rmrender.PDFFresh, rmrender.PDFPreserving, rmrender.PDFRecomposed} {
		pdf, report, err := d.PDF(ctx, mode, o)
		if err != nil {
			t.Fatal(err)
		}
		wantPDF(t, pdf, 1)
		if report.UnreadablePages != 1 || !bytes.Contains(pdf, []byte("NotoSans")) {
			t.Fatalf("report %+v", report)
		}
		if mode == rmrender.PDFPreserving && !bytes.HasPrefix(pdf, source) {
			t.Fatal("the preserving export did not append to the source PDF")
		}
	}
	labelled, err := d.PageImage(ctx, 0, 0.25, o)
	if err != nil {
		t.Fatal(err)
	}
	o.Unreadable = rmrender.UnreadableBlank
	blank, err := d.PageImage(ctx, 0, 0.25, o)
	if err != nil {
		t.Fatal(err)
	}
	if inked(blank) == 0 || bytes.Equal(labelled.Pix, blank.Pix) {
		t.Fatal("the imported page or the label was not drawn")
	}
}
