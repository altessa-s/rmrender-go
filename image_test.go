// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"bytes"
	"context"
	"errors"
	"image"
	"image/color"
	"image/png"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"testing"
	"testing/fstest"

	"github.com/altessa-s/rmrender-go"
)

// imagePage places the image picture.png over one stroke; see
// testdata/authored/README.md.
const imagePage = "Image_placed.rm"

// loadAuthored answers a page written for these tests.
func loadAuthored(t testing.TB, name string) []byte {
	t.Helper()
	data, err := os.ReadFile(filepath.Join("testdata", "authored", name))
	if err != nil {
		t.Fatal(err)
	}
	return data
}

// pictureFS holds the image imagePage places: opaque red.
func pictureFS() fstest.MapFS {
	img := image.NewRGBA(image.Rect(0, 0, 40, 30))
	for i := 0; i < len(img.Pix); i += 4 {
		copy(img.Pix[i:], []byte{0xff, 0, 0, 0xff})
	}
	var b bytes.Buffer
	if err := png.Encode(&b, img); err != nil {
		panic(err)
	}
	return fstest.MapFS{"picture.png": {Data: b.Bytes()}}
}

// reds counts the opaque red pixels of img, which only the picture draws.
func reds(img image.Image) int {
	n := 0
	b := img.Bounds()
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			c := color.RGBAModel.Convert(img.At(x, y)).(color.RGBA)
			if c.A == 0xff && c.R > 0xf0 && c.G < 0x10 && c.B < 0x10 {
				n++
			}
		}
	}
	return n
}

// The images a lone page places are read through Options.Images, by the name
// the page gives: drawn when there, left blank when not, and a failed read
// fails the call with the read's error as its cause.
func TestPageImages(t *testing.T) {
	page := loadAuthored(t, imagePage)
	o := withFonts(t, rmrender.Options{Images: pictureFS()})
	img, err := rmrender.PageImage(context.Background(), page, 0.25, o)
	if err != nil {
		t.Fatal(err)
	}
	// 400 by 300 units at a quarter scale, less the antialiased edge.
	if n := reds(img); n < 90*65 {
		t.Fatalf("%d red pixels, want the picture's 100 by 75", n)
	}
	svg, err := rmrender.PageSVG(context.Background(), page, o)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Contains(svg, []byte("data:image/png;base64,")) {
		t.Fatal("the SVG holds no PNG")
	}

	for _, images := range []fs.FS{nil, fstest.MapFS{}} {
		img, err := rmrender.PageImage(context.Background(), page, 0.25, withFonts(t, rmrender.Options{Images: images}))
		if err != nil {
			t.Fatal(err)
		}
		if n := reds(img); n != 0 {
			t.Fatalf("%d red pixels without the picture", n)
		}
		if inked(img) == 0 {
			t.Fatal("the stroke was not drawn")
		}
	}

	failure := errors.New("no pictures today")
	_, err = rmrender.PageImage(context.Background(), page, 0.25, withFonts(t, rmrender.Options{Images: failingFS{failure}}))
	wantStatus(t, err, rmrender.StatusBackend)
	if !errors.Is(err, failure) {
		t.Fatalf("got %v, want the read's error as the cause", err)
	}
}

// A panic or Goexit in an image read is raised again on the calling
// goroutine once C has returned and the call's callback state is released.
func TestPageImageAbnormalExit(t *testing.T) {
	page := loadAuthored(t, imagePage)
	before := rmrender.LiveHandles()
	func() {
		defer func() {
			if p := recover(); p != "boom" {
				t.Fatalf("recovered %v, want the read's panic", p)
			}
		}()
		_, _ = rmrender.PageImage(context.Background(), page, 0.25, withFonts(t, rmrender.Options{Images: panickingFS{}}))
		t.Fatal("PageImage returned after its image read panicked")
	}()
	done := make(chan bool)
	go func() {
		returned := false
		defer func() { done <- returned }()
		_, _ = rmrender.PageImage(context.Background(), page, 0.25, withFonts(t, rmrender.Options{
			Images: callbackFS{runtime.Goexit},
		}))
		returned = true
	}()
	if <-done {
		t.Fatal("PageImage returned after its image read exited")
	}
	if got := rmrender.LiveHandles(); got != before {
		t.Fatalf("live handles = %d, want %d", got, before)
	}
}
