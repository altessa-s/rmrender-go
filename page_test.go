// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"bytes"
	"context"
	"errors"
	"image/color"
	"io/fs"
	"math"
	"testing"
	"testing/fstest"
	"time"

	"github.com/altessa-s/rmrender-go"
	"github.com/altessa-s/rmrender-go/fonts"
)

func TestVersionIsTheCLibrarys(t *testing.T) {
	if got := rmrender.CVersion(); got != rmrender.Version {
		t.Fatalf("C library is %s, Version says %s", got, rmrender.Version)
	}
}

// At scale 1 a tablet page on A4 is as wide as its canvas, and as tall as A4
// is at that width; each scale multiplies both. US Letter at that width is
// shorter than the canvas, which the sheet always holds.
func TestPagePNGSizes(t *testing.T) {
	data := loadPage(t, strokesPage)
	a4 := int(math.Round(canvasWidth * 841.89 / 595.276))
	letter := max(int(math.Round(canvasWidth*792.0/612)), canvasHeight)
	for _, c := range []struct {
		paper         rmrender.Paper
		scale         float64
		width, height int
	}{
		{rmrender.PaperA4, 1, canvasWidth, a4},
		{rmrender.PaperA4, 0.5, canvasWidth / 2, int(math.Round(float64(a4) / 2))},
		{rmrender.PaperLetter, 1, canvasWidth, letter},
		{rmrender.PaperTablet, 1, canvasWidth, canvasHeight},
		{rmrender.PaperTablet, 0.25, canvasWidth / 4, canvasHeight / 4},
	} {
		out, err := rmrender.PagePNG(context.Background(), data, c.scale, withFonts(t, rmrender.Options{Paper: c.paper}))
		if err != nil {
			t.Fatal(err)
		}
		img := decodePNG(t, out)
		wantSize(t, img, c.width, c.height)
		if inked(img) == 0 {
			t.Errorf("paper %d at %v: nothing drawn", c.paper, c.scale)
		}
	}
}

func TestPagePDFHasOnePage(t *testing.T) {
	for _, name := range []string{strokesPage, linesPage, colorPage, highlightedPage} {
		out, err := rmrender.PagePDF(context.Background(), loadPage(t, name), withFonts(t, rmrender.Options{}))
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		wantPDF(t, out, 1)
	}
}

// A background fills the whole sheet; without one, only ink is opaque.
func TestBackground(t *testing.T) {
	data := loadPage(t, strokesPage)
	plain, err := rmrender.PageImage(context.Background(), data, 0.25, withFonts(t, rmrender.Options{Paper: rmrender.PaperTablet}))
	if err != nil {
		t.Fatal(err)
	}
	white, err := rmrender.PageImage(context.Background(), data, 0.25, withFonts(t, rmrender.Options{Paper: rmrender.PaperTablet, Background: color.White}))
	if err != nil {
		t.Fatal(err)
	}
	size := white.Bounds().Dx() * white.Bounds().Dy()
	if n := inked(white); n != size {
		t.Fatalf("%d of %d pixels filled", n, size)
	}
	if n := inked(plain); n == 0 || n >= size/2 {
		t.Fatalf("%d of %d pixels inked without a background", n, size)
	}
	if got := white.RGBAAt(0, 0); got != (color.RGBA{255, 255, 255, 255}) {
		t.Fatalf("corner is %v", got)
	}
}

// A template draws behind the ink; the bundled ones are found by name.
func TestTemplate(t *testing.T) {
	data := loadPage(t, strokesPage)
	bare, err := rmrender.PageImage(context.Background(), data, 0.25, withFonts(t, rmrender.Options{}))
	if err != nil {
		t.Fatal(err)
	}
	lined, err := rmrender.PageImage(context.Background(), data, 0.25, withFonts(t, rmrender.Options{Template: "P Lines medium"}))
	if err != nil {
		t.Fatal(err)
	}
	if inked(lined) <= inked(bare) {
		t.Fatalf("the template drew nothing: %d inked pixels, %d without", inked(lined), inked(bare))
	}
	missing, err := rmrender.PageImage(context.Background(), data, 0.25, withFonts(t, rmrender.Options{Template: "No such template"}))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(missing.Pix, bare.Pix) {
		t.Fatal("a missing template drew something")
	}
}

// A layer of templates comes before the bundled ones, and may add names.
func TestTemplateLayers(t *testing.T) {
	data := loadPage(t, strokesPage)
	grid, err := fs.ReadFile(rmrender.BundledTemplates(), "P Grid small.template")
	if err != nil {
		t.Fatal(err)
	}
	lines, err := fs.ReadFile(rmrender.BundledTemplates(), "P Lines medium.template")
	if err != nil {
		t.Fatal(err)
	}
	layer := fstest.MapFS{
		"Mine.template":           {Data: grid},
		"P Lines medium.template": {Data: grid},
	}
	draw := func(templates *rmrender.Templates, name string) []byte {
		img, err := rmrender.PageImage(context.Background(), data, 0.25, withFonts(t, rmrender.Options{Templates: templates, Template: name}))
		if err != nil {
			t.Fatal(err)
		}
		return img.Pix
	}
	gridded := draw(nil, "P Grid small")
	if !bytes.Equal(draw(rmrender.NewTemplates(layer), "Mine"), gridded) {
		t.Fatal("a template of the layer was not used")
	}
	if !bytes.Equal(draw(rmrender.NewTemplates(layer), "P Lines medium"), gridded) {
		t.Fatal("the layer did not take precedence")
	}
	if !bytes.Equal(draw(rmrender.NewTemplates(fstest.MapFS{"P Lines medium.template": {Data: lines}}), "P Grid small"), gridded) {
		t.Fatal("the bundled templates are not the last layer")
	}

	broken := rmrender.NewTemplates(fstest.MapFS{"Broken.template": {Data: []byte("{")}})
	_, err = rmrender.PageImage(context.Background(), data, 0.25, withFonts(t, rmrender.Options{Templates: broken, Template: "Broken"}))
	if err == nil {
		t.Fatal("a malformed template was accepted")
	}
}

// What reading a template fails with is the cause of the error.
func TestTemplateReadFailure(t *testing.T) {
	errDisk := errors.New("disk on fire")
	templates := rmrender.NewTemplates(failingFS{errDisk})
	_, err := rmrender.PagePDF(context.Background(), loadPage(t, strokesPage), withFonts(t, rmrender.Options{Templates: templates, Template: "P Lines medium"}))
	wantStatus(t, err, rmrender.StatusBackend)
	if !errors.Is(err, errDisk) {
		t.Fatalf("%v does not unwrap to the read failure", err)
	}
}

type failingFS struct{ err error }

func (f failingFS) Open(string) (fs.File, error) { return nil, f.err }

// A panic while reading a template unwinds no C frame: it comes back from
// the call that made it.
func TestTemplatePanicIsRaisedAgain(t *testing.T) {
	defer func() {
		if p := recover(); p != "boom" {
			t.Fatalf("recovered %v", p)
		}
	}()
	_, _ = rmrender.PagePDF(context.Background(), loadPage(t, strokesPage),
		withFonts(t, rmrender.Options{Templates: rmrender.NewTemplates(panickingFS{}), Template: "P Lines medium"}))
	t.Fatal("no panic")
}

type panickingFS struct{}

func (panickingFS) Open(string) (fs.File, error) { panic("boom") }

func TestEdgeModesGrowAlike(t *testing.T) {
	data := loadPage(t, colorPage)
	for _, edge := range []rmrender.Edge{rmrender.EdgeRibbon, rmrender.EdgeUncut, rmrender.EdgeCentreline} {
		img, err := rmrender.PageImage(context.Background(), data, 0.25, withFonts(t, rmrender.Options{Edge: edge, Paper: rmrender.PaperTablet}))
		if err != nil {
			t.Fatalf("edge %d: %v", edge, err)
		}
		if b := img.Bounds(); b.Dx() < canvasWidth/4 || b.Dy() < canvasHeight/4 {
			t.Fatalf("edge %d: %v is smaller than the canvas", edge, b)
		}
	}
	_, err := rmrender.PageImage(context.Background(), data, 0.25, withFonts(t, rmrender.Options{Edge: rmrender.EdgeRibbon, Margin: 10}))
	wantStatus(t, err, rmrender.StatusInvalidArgument)
}

// A page with a text block, typed or empty, needs fonts, and is laid out
// with the bundled ones.
func TestTextNeedsFonts(t *testing.T) {
	for _, name := range []string{typedPage, strokesPage} {
		_, err := rmrender.PagePDF(context.Background(), loadPage(t, name), nil)
		wantStatus(t, err, rmrender.StatusUnsupported)
	}
	data := loadPage(t, typedPage)
	out, err := rmrender.PagePNG(context.Background(), data, 0.5, withFonts(t, rmrender.Options{}))
	if err != nil {
		t.Fatal(err)
	}
	if inked(decodePNG(t, out)) == 0 {
		t.Fatal("no text drawn")
	}
	pdf, err := rmrender.PagePDF(context.Background(), data, withFonts(t, rmrender.Options{}))
	if err != nil {
		t.Fatal(err)
	}
	wantPDF(t, pdf, 1)
}

func TestFontsMustBeComplete(t *testing.T) {
	_, err := rmrender.NewFonts(fstest.MapFS{})
	wantStatus(t, err, rmrender.StatusUnsupported)
	_, err = rmrender.NewFonts(fstest.MapFS{"NotoSans-Regular.ttf": {Data: []byte("not a font")}})
	if err == nil {
		t.Fatal("a broken font was accepted")
	}
}

func TestClosedFonts(t *testing.T) {
	f, err := rmrender.NewFonts(fonts.FS)
	if err != nil {
		t.Fatal(err)
	}
	if err := f.Close(); err != nil {
		t.Fatal(err)
	}
	if err := f.Close(); err != nil {
		t.Fatal(err)
	}
	_, err = rmrender.PagePDF(context.Background(), loadPage(t, strokesPage), &rmrender.Options{Fonts: f})
	wantStatus(t, err, rmrender.StatusInvalidArgument)
}

func TestBadPages(t *testing.T) {
	for _, data := range [][]byte{nil, []byte("not a page"), []byte("reMarkable .lines file, version=3          ")} {
		_, err := rmrender.PagePDF(context.Background(), data, withFonts(t, rmrender.Options{}))
		var e *rmrender.Error
		if !errors.As(err, &e) {
			t.Fatalf("%q: %v", data, err)
		}
	}
	_, err := rmrender.PagePDF(context.Background(), loadPage(t, strokesPage), withFonts(t, rmrender.Options{Template: "a\x00b"}))
	wantStatus(t, err, rmrender.StatusInvalidArgument)
	_, err = rmrender.PageImage(context.Background(), loadPage(t, strokesPage), 0, withFonts(t, rmrender.Options{}))
	wantStatus(t, err, rmrender.StatusInvalidArgument)
	_, err = rmrender.PageImage(context.Background(), loadPage(t, strokesPage), math.NaN(), withFonts(t, rmrender.Options{}))
	wantStatus(t, err, rmrender.StatusInvalidArgument)
	_, err = rmrender.PagePDF(context.Background(), loadPage(t, strokesPage), withFonts(t, rmrender.Options{Paper: 7}))
	wantStatus(t, err, rmrender.StatusInvalidArgument)
}

func TestLimits(t *testing.T) {
	limits := rmrender.DefaultLimits()
	limits.MaximumBitmapSide = 100
	_, err := rmrender.PageImage(context.Background(), loadPage(t, strokesPage), 1, withFonts(t, rmrender.Options{Limits: &limits}))
	wantStatus(t, err, rmrender.StatusLimit)
	limits = rmrender.DefaultLimits()
	limits.MaximumDraws = -1
	_, err = rmrender.PagePDF(context.Background(), loadPage(t, strokesPage), withFonts(t, rmrender.Options{Limits: &limits}))
	wantStatus(t, err, rmrender.StatusInvalidArgument)
}

func TestCancelledBeforeTheCall(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	_, err := rmrender.PagePDF(ctx, loadPage(t, strokesPage), withFonts(t, rmrender.Options{}))
	wantStatus(t, err, rmrender.StatusCancelled)
	if !errors.Is(err, context.Canceled) || !errors.Is(err, rmrender.ErrCancelled) {
		t.Fatalf("%v", err)
	}
}

// A context that ends during the call stops the drawing.
func TestCancelledDuringTheCall(t *testing.T) {
	ctx, cancel := context.WithCancelCause(context.Background())
	errStop := errors.New("stop")
	layer := cancellingFS{cancel: func() { cancel(errStop) }}
	_, err := rmrender.PageImage(ctx, loadPage(t, strokesPage), 0.25,
		withFonts(t, rmrender.Options{Templates: rmrender.NewTemplates(layer), Template: "P Lines medium"}))
	wantStatus(t, err, rmrender.StatusCancelled)
	if !errors.Is(err, errStop) {
		t.Fatalf("%v does not unwrap to the cause", err)
	}
}

// cancellingFS ends a context when a file is opened, and gives the context
// time to say so before answering that the file is not there.
type cancellingFS struct{ cancel func() }

func (c cancellingFS) Open(string) (fs.File, error) {
	c.cancel()
	time.Sleep(100 * time.Millisecond)
	return nil, fs.ErrNotExist
}
