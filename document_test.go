// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"bytes"
	"context"
	"errors"
	"math"
	"testing"

	"github.com/altessa-s/rmrender-go"
)

func TestNotebook(t *testing.T) {
	data := notebook(t, "P Lines medium", loadPage(t, strokesPage), loadPage(t, typedPage), loadPage(t, highlightedPage))
	d := openDocument(t, data)
	if d.ID() != docID || d.Len() != 3 {
		t.Fatalf("document %q of %d pages", d.ID(), d.Len())
	}
	pages := d.Pages()
	if len(pages) != 3 || pages[0].ID != "p1" || pages[2].ID != "p3" || pages[1].Template != "P Lines medium" || pages[0].SourcePage != -1 {
		t.Fatalf("pages %+v", pages)
	}
	o := withFonts(t, rmrender.Options{})
	for mode, name := range map[rmrender.PDFMode]string{rmrender.PDFFresh: "fresh", rmrender.PDFPreserving: "preserving", rmrender.PDFRecomposed: "recomposed"} {
		pdf, report, err := d.PDF(context.Background(), mode, o)
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		wantPDF(t, pdf, 3)
		if report.DocumentPages != 3 || report.RenderedPages != 3 || report.RefusedSources != 0 {
			t.Fatalf("%s: report %+v", name, report)
		}
	}
	o.MaximumPages = 2
	pdf, report, err := d.PDF(context.Background(), rmrender.PDFFresh, o)
	if err != nil {
		t.Fatal(err)
	}
	wantPDF(t, pdf, 2)
	if report.DocumentPages != 3 || report.RenderedPages != 2 {
		t.Fatalf("report %+v", report)
	}
}

// A document page is drawn as the lone page is, on its template.
func TestDocumentPageImage(t *testing.T) {
	page := loadPage(t, strokesPage)
	d := openDocument(t, notebook(t, "P Grid small", page))
	o := withFonts(t, rmrender.Options{})
	got, err := d.PageImage(context.Background(), 0, 0.25, o)
	if err != nil {
		t.Fatal(err)
	}
	o.Template = "P Grid small"
	want, err := rmrender.PageImage(context.Background(), page, 0.25, o)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got.Pix, want.Pix) || got.Rect != want.Rect {
		t.Fatal("a document page draws differently from the same page alone")
	}
	png, err := d.PagePNG(context.Background(), 0, 1, o)
	if err != nil {
		t.Fatal(err)
	}
	wantSize(t, decodePNG(t, png), canvasWidth, int(math.Round(canvasWidth*841.89/595.276)))

	_, err = d.PageImage(context.Background(), 1, 1, o)
	wantStatus(t, err, rmrender.StatusInvalidArgument)
	_, err = d.PageImage(context.Background(), -1, 1, o)
	wantStatus(t, err, rmrender.StatusInvalidArgument)
}

func TestDocumentMarks(t *testing.T) {
	d := openDocument(t, notebook(t, "", loadPage(t, strokesPage), loadPage(t, highlightedPage)))
	m, err := d.PageMarks(1, withFonts(t, rmrender.Options{}))
	if err != nil {
		t.Fatal(err)
	}
	if m.PageID != "p2" || len(m.Highlights) == 0 || m.Highlights[0].Text == "" {
		t.Fatalf("marks %+v", m)
	}
}

// A document of an imported PDF: its pages are the PDF's, with marks drawn
// over them, and the preserving export appends to the PDF's own bytes.
func TestImportedPDF(t *testing.T) {
	o := withFonts(t, rmrender.Options{})
	source, err := rmrender.PagePDF(context.Background(), loadPage(t, linesPage), o)
	if err != nil {
		t.Fatal(err)
	}
	d := openDocument(t, imported(t, source, loadPage(t, strokesPage)))
	if d.Len() != 1 || d.Pages()[0].SourcePage != 0 {
		t.Fatalf("%d pages: %+v", d.Len(), d.Pages())
	}
	fresh, report, err := d.PDF(context.Background(), rmrender.PDFFresh, o)
	if err != nil {
		t.Fatal(err)
	}
	wantPDF(t, fresh, 1)
	if report.RenderedPages != 1 {
		t.Fatalf("report %+v", report)
	}
	preserved, _, err := d.PDF(context.Background(), rmrender.PDFPreserving, o)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.HasPrefix(preserved, source) || len(preserved) <= len(source) {
		t.Fatal("the preserving export did not append to the source PDF")
	}
	wantPDF(t, preserved, 1)
	recomposed, _, err := d.PDF(context.Background(), rmrender.PDFRecomposed, o)
	if err != nil {
		t.Fatal(err)
	}
	wantPDF(t, recomposed, 1)

	// The page image holds the imported page beneath the marks.
	under, err := d.PageImage(context.Background(), 0, 0.25, o)
	if err != nil {
		t.Fatal(err)
	}
	marksOnly, err := rmrender.PageImage(context.Background(), loadPage(t, strokesPage), 0.25, o)
	if err != nil {
		t.Fatal(err)
	}
	if inked(under) <= inked(marksOnly) {
		t.Fatalf("the imported page was not drawn: %d inked pixels, %d for the marks alone", inked(under), inked(marksOnly))
	}
}

// A document that lists no pages draws its imported PDF's pages.
func TestUnlistedPDFPages(t *testing.T) {
	o := withFonts(t, rmrender.Options{})
	source, _, err := openDocument(t, notebook(t, "", loadPage(t, strokesPage), loadPage(t, linesPage))).PDF(context.Background(), rmrender.PDFFresh, o)
	if err != nil {
		t.Fatal(err)
	}
	d := openDocument(t, archive(t, file{docID + ".pdf", source}))
	if d.Len() != 2 || len(d.Pages()) != 0 {
		t.Fatalf("%d pages, %d listed", d.Len(), len(d.Pages()))
	}
	img, err := d.PageImage(context.Background(), 1, 0.25, o)
	if err != nil {
		t.Fatal(err)
	}
	if inked(img) == 0 {
		t.Fatal("the imported page was not drawn")
	}
}

func TestEmptyNotebookIsOneBlankSheet(t *testing.T) {
	d := openDocument(t, archive(t, file{docID + ".content", []byte(`{"cPages":{"pages":[]}}`)}))
	if d.Len() != 1 {
		t.Fatalf("%d pages", d.Len())
	}
	pdf, _, err := d.PDF(context.Background(), rmrender.PDFFresh, nil)
	if err != nil {
		t.Fatal(err)
	}
	wantPDF(t, pdf, 1)
	img, err := d.PageImage(context.Background(), 0, 0.1, nil)
	if err != nil {
		t.Fatal(err)
	}
	if inked(img) != 0 {
		t.Fatal("a blank sheet holds ink")
	}
}

func TestChoosingTheDocument(t *testing.T) {
	data := archive(t,
		file{"c3d4.metadata", []byte(`{}`)},
		file{docID + ".content", []byte(`{"cPages":{"pages":[]}}`)},
		file{"e5f6.content", []byte(`{"cPages":{"pages":[]}}`)},
	)
	if id := openDocument(t, data).ID(); id != docID {
		t.Fatalf("opened %q", id)
	}
	d, err := rmrender.OpenDocument(data, &rmrender.DocumentOptions{ID: "e5f6"})
	if err != nil {
		t.Fatal(err)
	}
	defer d.Close()
	if d.ID() != "e5f6" {
		t.Fatalf("opened %q", d.ID())
	}
}

func TestBadArchives(t *testing.T) {
	for _, data := range [][]byte{nil, []byte("not a zip"), make([]byte, 64)} {
		_, err := rmrender.OpenDocument(data, nil)
		wantStatus(t, err, rmrender.StatusInvalidArgument)
	}
	bad := archive(t, file{docID + ".content", []byte(`{"cPages":`)})
	if _, err := rmrender.OpenDocument(bad, nil); err == nil {
		t.Fatal("malformed .content accepted")
	}
	broken := notebook(t, "", []byte("not a page"))
	d := openDocument(t, broken)
	_, _, err := d.PDF(context.Background(), rmrender.PDFFresh, withFonts(t, rmrender.Options{}))
	if err == nil {
		t.Fatal("a page that does not parse was drawn")
	}
	_, _, err = d.PDF(context.Background(), 9, nil)
	wantStatus(t, err, rmrender.StatusInvalidArgument)
}

func TestClosedDocument(t *testing.T) {
	d, err := rmrender.OpenDocument(notebook(t, "", loadPage(t, strokesPage)), nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := d.Close(); err != nil {
		t.Fatal(err)
	}
	if err := d.Close(); err != nil {
		t.Fatal(err)
	}
	_, _, err = d.PDF(context.Background(), rmrender.PDFFresh, nil)
	if !errors.Is(err, rmrender.ErrInvalidArgument) {
		t.Fatalf("%v", err)
	}
	if d.Len() != 1 || d.ID() != docID {
		t.Fatal("a closed document forgot what it held")
	}
}
