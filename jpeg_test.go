// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"bytes"
	"context"
	"image"
	"image/color"
	"image/jpeg"
	"testing"

	"github.com/altessa-s/rmrender-go"
)

// decodeJPEG decodes data, failing the test unless it is a JPEG.
func decodeJPEG(t testing.TB, data []byte) image.Image {
	t.Helper()
	img, err := jpeg.Decode(bytes.NewReader(data))
	if err != nil {
		t.Fatalf("not a JPEG: %v", err)
	}
	return img
}

// wantGrey fails the test unless the corner of img is near the grey level.
func wantGrey(t testing.TB, img image.Image, level uint8) {
	t.Helper()
	r, g, b, _ := img.At(0, 0).RGBA()
	for _, c := range []uint32{r >> 8, g >> 8, b >> 8} {
		if d := int(c) - int(level); d < -3 || d > 3 {
			t.Fatalf("corner is %v, want grey %d", img.At(0, 0), level)
		}
	}
}

// A JPEG is the page image at its size, flattened onto the background, or
// onto white without one.
func TestPageJPEG(t *testing.T) {
	data := loadPage(t, strokesPage)
	o := withFonts(t, rmrender.Options{})
	want, err := rmrender.PageImage(context.Background(), data, 0.5, o)
	if err != nil {
		t.Fatal(err)
	}
	out, err := rmrender.PageJPEG(context.Background(), data, 0.5, 0, o)
	if err != nil {
		t.Fatal(err)
	}
	img := decodeJPEG(t, out)
	wantSize(t, img, want.Bounds().Dx(), want.Bounds().Dy())
	wantGrey(t, img, 0xff)
	if o.Background != nil {
		t.Fatal("PageJPEG changed the options")
	}

	o.Background = color.Black
	black, err := rmrender.PageJPEG(context.Background(), data, 0.5, 90, o)
	if err != nil {
		t.Fatal(err)
	}
	wantGrey(t, decodeJPEG(t, black), 0)

	// What a translucent background leaves transparent is flattened onto
	// white: half-transparent black is mid grey.
	o.Background = color.NRGBA{A: 0x80}
	grey, err := rmrender.PageJPEG(context.Background(), data, 0.5, 90, o)
	if err != nil {
		t.Fatal(err)
	}
	wantGrey(t, decodeJPEG(t, grey), 0x7f)
	o.Background = nil

	// The ink is there: a JPEG with ink is darker than a blank one.
	dark := 0
	b := img.Bounds()
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			if r, _, _, _ := img.At(x, y).RGBA(); r < 0x8000 {
				dark++
			}
		}
	}
	if dark == 0 {
		t.Fatal("no ink in the JPEG")
	}

	low, err := rmrender.PageJPEG(context.Background(), data, 0.5, 5, o)
	if err != nil {
		t.Fatal(err)
	}
	high, err := rmrender.PageJPEG(context.Background(), data, 0.5, 100, o)
	if err != nil {
		t.Fatal(err)
	}
	if len(low) >= len(high) {
		t.Fatalf("quality 5 is %d bytes, quality 100 %d", len(low), len(high))
	}
	for _, quality := range []int{-1, 101} {
		_, err := rmrender.PageJPEG(context.Background(), data, 0.5, quality, o)
		wantStatus(t, err, rmrender.StatusInvalidArgument)
	}
	_, err = rmrender.PageJPEG(context.Background(), data, 0, 0, o)
	wantStatus(t, err, rmrender.StatusInvalidArgument)
}

func TestDocumentPageJPEG(t *testing.T) {
	d := openDocument(t, notebook(t, "P Lines medium", loadPage(t, strokesPage), loadPage(t, typedPage)))
	o := withFonts(t, rmrender.Options{})
	for i := range d.Len() {
		want, err := d.PageImage(context.Background(), i, 0.25, o)
		if err != nil {
			t.Fatal(err)
		}
		out, err := d.PageJPEG(context.Background(), i, 0.25, 80, o)
		if err != nil {
			t.Fatal(err)
		}
		img := decodeJPEG(t, out)
		wantSize(t, img, want.Bounds().Dx(), want.Bounds().Dy())
		wantGrey(t, img, 0xff)
	}
	_, err := d.PageJPEG(context.Background(), 2, 0.25, 80, o)
	wantStatus(t, err, rmrender.StatusInvalidArgument)
	_, err = d.PageJPEG(context.Background(), 0, 0.25, 101, o)
	wantStatus(t, err, rmrender.StatusInvalidArgument)

	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	_, err = d.PageJPEG(ctx, 0, 0.25, 0, o)
	wantStatus(t, err, rmrender.StatusCancelled)
	_, err = rmrender.PageJPEG(ctx, loadPage(t, strokesPage), 0.25, 0, o)
	wantStatus(t, err, rmrender.StatusCancelled)
}
