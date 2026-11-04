// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/xml"
	"errors"
	"fmt"
	"image/png"
	"io"
	"math"
	"strings"
	"testing"

	"github.com/altessa-s/rmrender-go"
)

// svgDoc is what the tests read of an SVG document.
type svgDoc struct {
	// The root's attributes.
	width, height, viewBox string
	// How many elements of each name it holds, the root included.
	elements map[string]int
	// The d attribute of every path, and the href of every image.
	paths, images []string
}

// parseSVG decodes data to its end, failing the test unless it is one
// well-formed XML document with an svg root.
func parseSVG(t testing.TB, data []byte) svgDoc {
	t.Helper()
	doc := svgDoc{elements: map[string]int{}}
	d := xml.NewDecoder(bytes.NewReader(data))
	depth, roots := 0, 0
	for {
		token, err := d.Token()
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			t.Fatalf("not well-formed XML: %v", err)
		}
		switch e := token.(type) {
		case xml.StartElement:
			if depth == 0 {
				roots++
				if e.Name.Local != "svg" || e.Name.Space != "http://www.w3.org/2000/svg" {
					t.Fatalf("root is %v", e.Name)
				}
			}
			depth++
			doc.elements[e.Name.Local]++
			for _, a := range e.Attr {
				switch {
				case depth == 1 && a.Name.Local == "width":
					doc.width = a.Value
				case depth == 1 && a.Name.Local == "height":
					doc.height = a.Value
				case depth == 1 && a.Name.Local == "viewBox":
					doc.viewBox = a.Value
				case e.Name.Local == "path" && a.Name.Local == "d":
					doc.paths = append(doc.paths, a.Value)
				case e.Name.Local == "image" && a.Name.Local == "href":
					doc.images = append(doc.images, a.Value)
				}
			}
		case xml.EndElement:
			depth--
		}
	}
	if roots != 1 || depth != 0 {
		t.Fatalf("%d root elements, depth %d at the end", roots, depth)
	}
	return doc
}

// wantSheet fails the test unless doc is width by height points, with a
// viewBox spanning them.
func wantSheet(t testing.TB, doc svgDoc, width, height int) {
	t.Helper()
	if doc.width != fmt.Sprintf("%dpt", width) || doc.height != fmt.Sprintf("%dpt", height) ||
		doc.viewBox != fmt.Sprintf("0 0 %d %d", width, height) {
		t.Fatalf("SVG is %s by %s, viewBox %q; want %dpt by %dpt", doc.width, doc.height, doc.viewBox, width, height)
	}
}

// The sheet of an SVG page is its PDF page's, in points: A4 and Letter
// rounded to whole points, Letter grown to hold the canvas, the tablet's
// own paper at PointsPerUnit.
func TestPageSVGSheet(t *testing.T) {
	data := loadPage(t, strokesPage)
	tablet := func(units float64) int { return int(math.Round(units * rmrender.PointsPerUnit)) }
	for _, c := range []struct {
		paper         rmrender.Paper
		width, height int
	}{
		{rmrender.PaperA4, 595, 842},
		{rmrender.PaperLetter, 612, int(math.Round(canvasHeight * 612.0 / canvasWidth))},
		{rmrender.PaperTablet, tablet(canvasWidth), tablet(canvasHeight)},
	} {
		o := withFonts(t, rmrender.Options{Paper: c.paper})
		out, err := rmrender.PageSVG(context.Background(), data, o)
		if err != nil {
			t.Fatal(err)
		}
		doc := parseSVG(t, out)
		wantSheet(t, doc, c.width, c.height)
		if len(doc.paths) == 0 {
			t.Errorf("paper %d: no strokes drawn", c.paper)
		}
		again, err := rmrender.PageSVG(context.Background(), data, o)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(again, out) {
			t.Errorf("paper %d: two calls differ", c.paper)
		}
	}
}

// Every fixture page draws as well-formed SVG, also with the pages' growth
// and their highlighter blend modes.
func TestPageSVGFixtures(t *testing.T) {
	for _, name := range []string{strokesPage, linesPage, colorPage, highlightedPage, typedPage} {
		for _, edge := range []rmrender.Edge{rmrender.EdgeRibbon, rmrender.EdgeUncut} {
			out, err := rmrender.PageSVG(context.Background(), loadPage(t, name), withFonts(t, rmrender.Options{Edge: edge}))
			if err != nil {
				t.Fatalf("%s: %v", name, err)
			}
			parseSVG(t, out)
		}
	}
}

// Typed text becomes the filled outlines of its glyphs: curves, and neither
// text elements nor a raster of it.
func TestPageSVGTypedTextIsOutlines(t *testing.T) {
	out, err := rmrender.PageSVG(context.Background(), loadPage(t, typedPage), withFonts(t, rmrender.Options{}))
	if err != nil {
		t.Fatal(err)
	}
	doc := parseSVG(t, out)
	if doc.elements["text"] != 0 || doc.elements["image"] != 0 {
		t.Fatalf("text drawn as %d text and %d image elements", doc.elements["text"], doc.elements["image"])
	}
	curves := 0
	for _, d := range doc.paths {
		curves += strings.Count(d, "C")
	}
	if curves < 100 {
		t.Fatalf("%d paths with %d curves: no glyph outlines", len(doc.paths), curves)
	}
	_, err = rmrender.PageSVG(context.Background(), loadPage(t, typedPage), nil)
	wantStatus(t, err, rmrender.StatusUnsupported)
}

// A lone page, the same page of a notebook and the notebook's SVG export
// are one and the same document; NoTemplates leaves the template out of
// each.
func TestSVGPipelinesAgree(t *testing.T) {
	page := loadPage(t, strokesPage)
	o := withFonts(t, rmrender.Options{Template: "P Lines medium"})
	lone, err := rmrender.PageSVG(context.Background(), page, o)
	if err != nil {
		t.Fatal(err)
	}
	d := openDocument(t, notebook(t, "P Lines medium", page, loadPage(t, typedPage)))
	single, err := d.PageSVG(context.Background(), 0, o)
	if err != nil {
		t.Fatal(err)
	}
	whole, _, err := d.SVG(context.Background(), o)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(lone, single) || !bytes.Equal(single, whole[0]) {
		t.Fatal("a lone page, a document page and the document export draw the page differently")
	}

	o.Templates = rmrender.NoTemplates()
	bare, err := rmrender.PageSVG(context.Background(), page, o)
	if err != nil {
		t.Fatal(err)
	}
	plain, err := d.PageSVG(context.Background(), 0, o)
	if err != nil {
		t.Fatal(err)
	}
	plainWhole, _, err := d.SVG(context.Background(), o)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(bare, plain) || !bytes.Equal(plain, plainWhole[0]) {
		t.Fatal("NoTemplates draws the pipelines differently")
	}
	if n, m := len(parseSVG(t, plain).paths), len(parseSVG(t, single).paths); n >= m {
		t.Fatalf("NoTemplates kept the template: %d paths, %d with it", n, m)
	}
	untemplated, err := rmrender.PageSVG(context.Background(), page, withFonts(t, rmrender.Options{}))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(bare, untemplated) {
		t.Fatal("NoTemplates drew a template")
	}
}

func TestDocumentSVG(t *testing.T) {
	d := openDocument(t, notebook(t, "P Grid small", loadPage(t, strokesPage), loadPage(t, typedPage), loadPage(t, highlightedPage)))
	o := withFonts(t, rmrender.Options{Paper: rmrender.PaperLetter})
	pages, report, err := d.SVG(context.Background(), o)
	if err != nil {
		t.Fatal(err)
	}
	if len(pages) != 3 || report.DocumentPages != 3 || report.RenderedPages != 3 {
		t.Fatalf("%d SVG pages, report %+v", len(pages), report)
	}
	for i, p := range pages {
		wantSheet(t, parseSVG(t, p), 612, int(math.Round(canvasHeight*612.0/canvasWidth)))
		one, err := d.PageSVG(context.Background(), i, o)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(one, p) {
			t.Fatalf("page %d differs from the export's", i)
		}
	}
	again, _, err := d.SVG(context.Background(), o)
	if err != nil {
		t.Fatal(err)
	}
	for i := range pages {
		if !bytes.Equal(again[i], pages[i]) {
			t.Fatalf("page %d: two exports differ", i)
		}
	}

	o.MaximumPages = 2
	pages, report, err = d.SVG(context.Background(), o)
	if err != nil {
		t.Fatal(err)
	}
	if len(pages) != 2 || report.DocumentPages != 3 || report.RenderedPages != 2 {
		t.Fatalf("%d SVG pages, report %+v", len(pages), report)
	}
	o.MaximumPages = -1
	_, _, err = d.SVG(context.Background(), o)
	wantStatus(t, err, rmrender.StatusInvalidArgument)
	o.MaximumPages = 0

	_, err = d.PageSVG(context.Background(), 3, o)
	wantStatus(t, err, rmrender.StatusInvalidArgument)
	_, err = d.PageSVG(context.Background(), -1, o)
	wantStatus(t, err, rmrender.StatusInvalidArgument)
}

// pngImages decodes the PNG data URIs of doc's images, failing the test on
// any other.
func pngImages(t testing.TB, doc svgDoc) [][2]int {
	t.Helper()
	var sizes [][2]int
	for _, href := range doc.images {
		data, ok := strings.CutPrefix(href, "data:image/png;base64,")
		if !ok {
			t.Fatalf("image is not an embedded PNG: %.40q", href)
		}
		raw, err := base64.StdEncoding.DecodeString(data)
		if err != nil {
			t.Fatal(err)
		}
		config, err := png.DecodeConfig(bytes.NewReader(raw))
		if err != nil {
			t.Fatal(err)
		}
		sizes = append(sizes, [2]int{config.Width, config.Height})
	}
	return sizes
}

// An imported PDF page is rasterized beneath the marks at PDFRasterDPI
// pixels per inch of the sheet, 150 by default, and embedded as a PNG.
func TestImportedPDFSVG(t *testing.T) {
	o := withFonts(t, rmrender.Options{})
	source, err := rmrender.PagePDF(context.Background(), loadPage(t, linesPage), o)
	if err != nil {
		t.Fatal(err)
	}
	d := openDocument(t, imported(t, source, loadPage(t, strokesPage)))
	for _, c := range []struct {
		dpi, want float64
	}{{0, 150}, {72, 72}, {300, 300}} {
		o.PDFRasterDPI = c.dpi
		out, err := d.PageSVG(context.Background(), 0, o)
		if err != nil {
			t.Fatal(err)
		}
		doc := parseSVG(t, out)
		wantSheet(t, doc, 595, 842)
		sizes := pngImages(t, doc)
		if len(sizes) != 1 {
			t.Fatalf("at %v DPI: %d images", c.dpi, len(sizes))
		}
		if w := math.Round(595 * c.want / 72); math.Abs(float64(sizes[0][0])-w) > 1 {
			t.Fatalf("at %v DPI the page is %d pixels wide, want %v", c.dpi, sizes[0][0], w)
		}
		if len(doc.paths) == 0 {
			t.Fatal("the marks were not drawn")
		}
		whole, report, err := d.SVG(context.Background(), o)
		if err != nil {
			t.Fatal(err)
		}
		if len(whole) != 1 || report.RenderedPages != 1 || !bytes.Equal(whole[0], out) {
			t.Fatalf("at %v DPI the export differs from the page: %d pages, report %+v", c.dpi, len(whole), report)
		}
	}
	for _, dpi := range []float64{-1, math.NaN(), math.Inf(1), 10001} {
		o.PDFRasterDPI = dpi
		_, err := d.PageSVG(context.Background(), 0, o)
		wantStatus(t, err, rmrender.StatusInvalidArgument)
		_, _, err = d.SVG(context.Background(), o)
		wantStatus(t, err, rmrender.StatusInvalidArgument)
		_, err = rmrender.PageSVG(context.Background(), loadPage(t, strokesPage), o)
		wantStatus(t, err, rmrender.StatusInvalidArgument)
	}
}

// MaximumPDFBytes bounds SVG output, all pages of a document together.
func TestSVGLimits(t *testing.T) {
	o := withFonts(t, rmrender.Options{})
	out, err := rmrender.PageSVG(context.Background(), loadPage(t, strokesPage), o)
	if err != nil {
		t.Fatal(err)
	}
	limits := rmrender.DefaultLimits()
	limits.MaximumPDFBytes = len(out) - 1
	o.Limits = &limits
	_, err = rmrender.PageSVG(context.Background(), loadPage(t, strokesPage), o)
	wantStatus(t, err, rmrender.StatusLimit)

	d := openDocument(t, notebook(t, "", loadPage(t, strokesPage), loadPage(t, strokesPage)))
	limits.MaximumPDFBytes = 2*len(out) - 1
	_, _, err = d.SVG(context.Background(), o)
	wantStatus(t, err, rmrender.StatusLimit)
	limits.MaximumPDFBytes = 2 * len(out)
	pages, _, err := d.SVG(context.Background(), o)
	if err != nil {
		t.Fatal(err)
	}
	if len(pages) != 2 {
		t.Fatalf("%d pages", len(pages))
	}
}

func TestSVGCancelled(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	o := withFonts(t, rmrender.Options{})
	_, err := rmrender.PageSVG(ctx, loadPage(t, strokesPage), o)
	wantStatus(t, err, rmrender.StatusCancelled)
	d := openDocument(t, notebook(t, "", loadPage(t, strokesPage)))
	_, err = d.PageSVG(ctx, 0, o)
	wantStatus(t, err, rmrender.StatusCancelled)
	_, _, err = d.SVG(ctx, o)
	wantStatus(t, err, rmrender.StatusCancelled)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("%v does not unwrap to the context's cause", err)
	}

	// Ended during the call, while a template is read: the lone page names
	// it in the options, the notebook's page in the document.
	templated := openDocument(t, notebook(t, "P Lines medium", loadPage(t, strokesPage)))
	for name, draw := range map[string]func(context.Context, *rmrender.Options) error{
		"page": func(ctx context.Context, o *rmrender.Options) error {
			_, err := rmrender.PageSVG(ctx, loadPage(t, strokesPage), o)
			return err
		},
		"document page": func(ctx context.Context, o *rmrender.Options) error {
			_, err := templated.PageSVG(ctx, 0, o)
			return err
		},
		"document": func(ctx context.Context, o *rmrender.Options) error {
			_, _, err := templated.SVG(ctx, o)
			return err
		},
	} {
		ctx, cancel := context.WithCancelCause(context.Background())
		errStop := errors.New("stop")
		layer := cancellingFS{cancel: func() { cancel(errStop) }}
		err := draw(ctx, withFonts(t, rmrender.Options{Templates: rmrender.NewTemplates(layer), Template: "P Lines medium"}))
		wantStatus(t, err, rmrender.StatusCancelled)
		if !errors.Is(err, errStop) {
			t.Fatalf("%s: %v does not unwrap to the cause", name, err)
		}
	}
}

func TestSVGClosedDocument(t *testing.T) {
	d, err := rmrender.OpenDocument(notebook(t, "", loadPage(t, strokesPage)), nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := d.Close(); err != nil {
		t.Fatal(err)
	}
	_, err = d.PageSVG(context.Background(), 0, withFonts(t, rmrender.Options{}))
	wantStatus(t, err, rmrender.StatusInvalidArgument)
	_, _, err = d.SVG(context.Background(), withFonts(t, rmrender.Options{}))
	wantStatus(t, err, rmrender.StatusInvalidArgument)
}
