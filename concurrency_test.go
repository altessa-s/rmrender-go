// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"sync"
	"testing"

	"github.com/altessa-s/rmrender-go"
	"github.com/altessa-s/rmrender-go/fonts"
)

// Many goroutines draw at once: lone pages and pages of one shared document,
// with one shared Fonts, and every call answers what it answers alone. Under
// -race this also shows the shared values are not written.
func TestDrawingIsSharedAcrossGoroutines(t *testing.T) {
	o := withFonts(t, rmrender.Options{Template: "P Dots S"})
	pages := [][]byte{loadPage(t, strokesPage), loadPage(t, typedPage), loadPage(t, colorPage)}
	d := openDocument(t, notebook(t, "P Grid small", pages...))

	want := make([][]byte, len(pages))
	for i, p := range pages {
		img, err := rmrender.PageImage(context.Background(), p, 0.2, o)
		if err != nil {
			t.Fatal(err)
		}
		want[i] = img.Pix
	}
	wantDoc, _, err := d.PDF(context.Background(), rmrender.PDFFresh, o)
	if err != nil {
		t.Fatal(err)
	}

	const goroutines, rounds = 8, 4
	var wg sync.WaitGroup
	errs := make(chan error, goroutines)
	for g := range goroutines {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for r := range rounds {
				i := (g + r) % len(pages)
				img, err := rmrender.PageImage(context.Background(), pages[i], 0.2, o)
				if err != nil {
					errs <- err
					return
				}
				if !bytes.Equal(img.Pix, want[i]) {
					errs <- fmt.Errorf("page %d drew differently at once", i)
					return
				}
				if _, err := d.PageImage(context.Background(), i, 0.2, o); err != nil {
					errs <- err
					return
				}
				if _, err := rmrender.PageMarks(pages[i], "", o); err != nil {
					errs <- err
					return
				}
				if g == 0 && r == 0 {
					pdf, _, err := d.PDF(context.Background(), rmrender.PDFFresh, o)
					if err != nil {
						errs <- err
						return
					}
					if len(pdf) != len(wantDoc) {
						errs <- fmt.Errorf("the document PDF came out differently at once")
						return
					}
				}
			}
		}()
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Error(err)
	}
}

// Close races calls in flight: each either finishes whole or is refused as
// closed, and the fonts and the document are released once, after the last
// of them.
func TestCloseRacesCalls(t *testing.T) {
	page := loadPage(t, strokesPage)
	archive := notebook(t, "", page)
	for range 3 {
		f, err := rmrender.NewFonts(fonts.FS)
		if err != nil {
			t.Fatal(err)
		}
		d, err := rmrender.OpenDocument(archive, nil)
		if err != nil {
			t.Fatal(err)
		}
		o := &rmrender.Options{Fonts: f}
		var wg sync.WaitGroup
		errs := make(chan error, 16)
		for i := range 8 {
			wg.Add(1)
			go func() {
				defer wg.Done()
				var err error
				if i%2 == 0 {
					_, err = rmrender.PagePDF(context.Background(), page, o)
				} else {
					_, err = d.PageImage(context.Background(), 0, 0.1, o)
				}
				if err != nil && !errors.Is(err, rmrender.ErrInvalidArgument) {
					errs <- err
				}
			}()
		}
		wg.Add(2)
		go func() {
			defer wg.Done()
			_ = f.Close()
		}()
		go func() {
			defer wg.Done()
			_ = d.Close()
		}()
		wg.Wait()
		close(errs)
		for err := range errs {
			t.Error(err)
		}
	}
}
