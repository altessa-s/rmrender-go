// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"bytes"
	"context"
	"errors"
	"image"
	"io"
	"io/fs"
	"runtime"
	"strings"
	"sync"
	"testing"
	"testing/fstest"
	"time"

	"github.com/altessa-s/rmrender-go"
	"github.com/altessa-s/rmrender-go/fonts"
)

// prompt is how long a call that must not wait may take, and deadline the
// context of a call that must end with it. Both are far below the minutes a
// deadlock would hang for, and far above what the calls take.
const (
	prompt   = 5 * time.Second
	deadline = 100 * time.Millisecond
)

// writerFunc is an io.Writer of a function.
type writerFunc func(p []byte) (int, error)

func (f writerFunc) Write(p []byte) (int, error) { return f(p) }

// hookFS runs hook on its first read of a file whose name ends in suffix, then
// answers from fsys.
type hookFS struct {
	fsys   fs.FS
	suffix string
	once   sync.Once
	hook   func()
}

func (h *hookFS) Open(name string) (fs.File, error) {
	if strings.HasSuffix(name, h.suffix) {
		h.once.Do(h.hook)
	}
	return h.fsys.Open(name)
}

// within runs f on a goroutine of its own, failing the test unless it returns
// before limit, so that a deadlock fails the test instead of hanging it. f
// reports with t.Error, which may be called from any goroutine.
func within(t *testing.T, limit time.Duration, f func()) {
	t.Helper()
	done := make(chan struct{})
	go func() {
		defer close(done)
		f()
	}()
	select {
	case <-done:
	case <-time.After(limit):
		t.Fatalf("still running after %v", limit)
	}
}

// closeAtEnd closes c when the test ends, without waiting past prompt: were
// Close to wait for a call stuck in a deadlock, the test would fail instead
// of hanging in its cleanup.
func closeAtEnd(t *testing.T, c io.Closer) {
	t.Cleanup(func() {
		done := make(chan struct{})
		go func() {
			defer close(done)
			_ = c.Close()
		}()
		select {
		case <-done:
		case <-time.After(prompt):
			t.Errorf("Close still blocked after %v", prompt)
		}
	})
}

// openFonts loads the bundled fonts, closed when the test ends.
func openFonts(t *testing.T) *rmrender.Fonts {
	t.Helper()
	f, err := rmrender.NewFonts(fonts.FS)
	if err != nil {
		t.Fatal(err)
	}
	closeAtEnd(t, f)
	return f
}

// openLoose opens data, closed when the test ends as closeAtEnd closes.
func openLoose(t *testing.T, data []byte) *rmrender.Document {
	t.Helper()
	d, err := rmrender.OpenDocument(data, nil)
	if err != nil {
		t.Fatal(err)
	}
	closeAtEnd(t, d)
	return d
}

// counts are the C objects and callback handles not yet released.
type counts struct{ documents, fonts, handles int64 }

func live() counts {
	return counts{rmrender.LiveDocuments(), rmrender.LiveFonts(), rmrender.LiveHandles()}
}

// wantLive fails the test unless the counts are back at before.
func wantLive(t *testing.T, before counts) {
	t.Helper()
	if got := live(); got != before {
		t.Fatalf("live documents, fonts and handles = %+v, want %+v", got, before)
	}
}

// wantClosed fails the test unless err is the refusal of a closed object.
func wantClosed(t *testing.T, err error) {
	t.Helper()
	if !errors.Is(err, rmrender.ErrInvalidArgument) {
		t.Fatalf("got %v, want a refusal as closed", err)
	}
}

// wantDeadline fails the test unless err is a call ended by its deadline.
func wantDeadline(t *testing.T, err error) {
	t.Helper()
	if !errors.Is(err, rmrender.ErrCancelled) || !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("got %v, want the deadline's cancellation", err)
	}
}

// Close returns at once from a callback of the very call using what it
// closes, and that call still finishes whole; the C memory is released when
// it does, not before.
func TestCloseFromCallback(t *testing.T) {
	page := loadPage(t, linesPage)
	imaged := loadAuthored(t, imagePage)
	archive := notebook(t, "P Lines medium", page, page)
	for _, c := range []struct {
		name string
		// run draws with d and f, closeIt called from a callback, and
		// answers what it drew, checked once the call has returned.
		run   func(d *rmrender.Document, f *rmrender.Fonts, closeIt func()) (any, error)
		check func(t *testing.T, drawn any)
	}{
		{"writer", func(d *rmrender.Document, f *rmrender.Fonts, closeIt func()) (any, error) {
			var out bytes.Buffer
			_, err := d.WritePDF(context.Background(), writerFunc(func(p []byte) (int, error) {
				closeIt()
				return out.Write(p)
			}), rmrender.PDFFresh, &rmrender.Options{Fonts: f})
			return out.Bytes(), err
		}, func(t *testing.T, drawn any) { wantPDF(t, drawn.([]byte), 2) }},
		{"document template read", func(d *rmrender.Document, f *rmrender.Fonts, closeIt func()) (any, error) {
			h := &hookFS{fsys: fstest.MapFS{}, suffix: ".template", hook: closeIt}
			return d.PageImage(context.Background(), 0, 0.1, &rmrender.Options{Fonts: f, Templates: rmrender.NewTemplates(h)})
		}, func(t *testing.T, drawn any) {
			if inked(drawn.(*image.RGBA)) == 0 {
				t.Fatal("the page drew nothing")
			}
		}},
		{"lone page template read", func(_ *rmrender.Document, f *rmrender.Fonts, closeIt func()) (any, error) {
			h := &hookFS{fsys: fstest.MapFS{}, suffix: ".template", hook: closeIt}
			return rmrender.PagePDF(context.Background(), page, &rmrender.Options{
				Fonts: f, Template: "P Lines medium", Templates: rmrender.NewTemplates(h),
			})
		}, func(t *testing.T, drawn any) { wantPDF(t, drawn.([]byte), 1) }},
		{"image read", func(_ *rmrender.Document, f *rmrender.Fonts, closeIt func()) (any, error) {
			h := &hookFS{fsys: pictureFS(), suffix: ".png", hook: closeIt}
			return rmrender.PageImage(context.Background(), imaged, 0.25, &rmrender.Options{Fonts: f, Images: h})
		}, func(t *testing.T, drawn any) {
			if reds(drawn.(*image.RGBA)) == 0 {
				t.Fatal("the image was not drawn")
			}
		}},
	} {
		for _, which := range []string{"document", "fonts", "both"} {
			t.Run(c.name+"/"+which, func(t *testing.T) {
				before := live()
				d, err := rmrender.OpenDocument(archive, nil)
				if err != nil {
					t.Fatal(err)
				}
				f, err := rmrender.NewFonts(fonts.FS)
				if err != nil {
					t.Fatal(err)
				}
				// Close is called from the callback, while the call holds
				// what it closes; were it to wait for the call, within would
				// fail the test.
				closed := false
				closeIt := func() {
					if which != "fonts" {
						_ = d.Close()
					}
					if which != "document" {
						_ = f.Close()
					}
					closed = true
				}
				var drawn any
				within(t, prompt, func() {
					drawn, err = c.run(d, f, closeIt)
				})
				if err != nil {
					t.Fatal(err)
				}
				if !closed {
					t.Fatal("the callback did not run")
				}
				c.check(t, drawn)
				_ = d.Close()
				_ = f.Close()
				wantLive(t, before)
				_, err = d.PageImage(context.Background(), 0, 0.1, nil)
				wantClosed(t, err)
				_, err = rmrender.PagePNG(context.Background(), page, 0.1, &rmrender.Options{Fonts: f})
				wantClosed(t, err)
			})
		}
	}
}

// A callback that draws the document of its own call, or with its call's
// Fonts, waits for that call, which waits for the callback: the inner call
// ends with its context, and the outer call then finishes. A draw that shares
// neither runs.
func TestRenderFromCallback(t *testing.T) {
	page := loadPage(t, strokesPage)
	archive := notebook(t, "P Lines medium", page)
	for _, c := range []struct {
		name string
		// inner draws from the callback of a call on d with Fonts f.
		inner func(ctx context.Context, d, other *rmrender.Document, f *rmrender.Fonts) error
		// waits says the inner call shares d or f and so must wait.
		waits bool
	}{
		{"same document", func(ctx context.Context, d, _ *rmrender.Document, _ *rmrender.Fonts) error {
			_, err := d.PageImage(ctx, 0, 0.1, nil)
			return err
		}, true},
		{"other document, same Fonts", func(ctx context.Context, _, other *rmrender.Document, f *rmrender.Fonts) error {
			_, err := other.PageImage(ctx, 0, 0.1, &rmrender.Options{Fonts: f})
			return err
		}, true},
		{"lone page, same Fonts", func(ctx context.Context, _, _ *rmrender.Document, f *rmrender.Fonts) error {
			_, err := rmrender.PagePNG(ctx, page, 0.1, &rmrender.Options{Fonts: f})
			return err
		}, true},
		{"other document, no Fonts", func(ctx context.Context, _, other *rmrender.Document, _ *rmrender.Fonts) error {
			_, err := other.PageImage(ctx, 0, 0.1, nil)
			return err
		}, false},
		{"other document, other Fonts", func(ctx context.Context, _, other *rmrender.Document, _ *rmrender.Fonts) error {
			_, err := other.PageImage(ctx, 0, 0.1, &rmrender.Options{Fonts: bundledFonts(t)})
			return err
		}, false},
	} {
		for _, callback := range []string{"writer", "template"} {
			t.Run(c.name+"/"+callback, func(t *testing.T) {
				d := openLoose(t, archive)
				// A blank sheet, which needs no Fonts to draw.
				other := openLoose(t, notebook(t, "", nil))
				f := openFonts(t)
				var inner error
				ran := false
				hook := func() {
					ctx, cancel := context.WithTimeout(context.Background(), deadline)
					defer cancel()
					start := time.Now()
					inner = c.inner(ctx, d, other, f)
					ran = true
					if took := time.Since(start); c.waits && took > 5*deadline {
						t.Errorf("the inner call returned after %v for a deadline of %v", took, deadline)
					}
				}
				within(t, prompt, func() {
					var err error
					if callback == "writer" {
						var once sync.Once
						_, err = d.WritePDF(context.Background(), writerFunc(func(p []byte) (int, error) {
							once.Do(hook)
							return len(p), nil
						}), rmrender.PDFFresh, &rmrender.Options{Fonts: f})
					} else {
						h := &hookFS{fsys: fstest.MapFS{}, suffix: ".template", hook: hook}
						_, err = d.PageImage(context.Background(), 0, 0.1, &rmrender.Options{
							Fonts: f, Templates: rmrender.NewTemplates(h),
						})
					}
					if err != nil {
						t.Error(err)
					}
				})
				if !ran {
					t.Fatal("the callback did not run")
				}
				if c.waits {
					wantDeadline(t, inner)
				} else if inner != nil {
					t.Fatal(inner)
				}
			})
		}
	}
}

// A call queued behind another, here behind a slow writer, stops waiting when
// its context ends instead of when the call ahead of it finishes; one that
// takes no context waits, and a Close ends its wait.
func TestQueuedCallHonoursContext(t *testing.T) {
	page := loadPage(t, strokesPage)
	f := openFonts(t)
	o := &rmrender.Options{Fonts: f}
	a := openLoose(t, notebook(t, "", page))
	b := openLoose(t, notebook(t, "", page))
	release := make(chan struct{})
	var releasing sync.Once
	letGo := func() { releasing.Do(func() { close(release) }) }
	// The writer is let go at the latest after prompt, so that a call that
	// waits for it instead of for its context fails the test instead of
	// hanging it.
	safety := time.AfterFunc(prompt, letGo)
	defer safety.Stop()
	started := make(chan struct{})
	finished := make(chan error, 1)
	go func() {
		var once sync.Once
		_, err := a.WritePDF(context.Background(), writerFunc(func(p []byte) (int, error) {
			once.Do(func() { close(started) })
			<-release
			return len(p), nil
		}), rmrender.PDFFresh, o)
		finished <- err
	}()
	<-started
	for _, c := range []struct {
		name string
		run  func(ctx context.Context) error
	}{
		{"other document, same Fonts", func(ctx context.Context) error {
			_, err := b.PageImage(ctx, 0, 0.1, o)
			return err
		}},
		{"other document's export, same Fonts", func(ctx context.Context) error {
			_, _, err := b.PDF(ctx, rmrender.PDFFresh, o)
			return err
		}},
		{"lone page, same Fonts", func(ctx context.Context) error {
			_, err := rmrender.PagePNG(ctx, page, 0.1, o)
			return err
		}},
		{"same document, no Fonts", func(ctx context.Context) error {
			_, err := a.PageImage(ctx, 0, 0.1, nil)
			return err
		}},
		{"same document's SVG", func(ctx context.Context) error {
			_, _, err := a.SVG(ctx, nil)
			return err
		}},
	} {
		t.Run(c.name, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), deadline)
			defer cancel()
			start := time.Now()
			err := c.run(ctx)
			took := time.Since(start)
			wantDeadline(t, err)
			// A small multiple of the deadline, far below the time the
			// writer ahead of it is held for.
			if took > 5*deadline {
				t.Fatalf("returned after %v for a deadline of %v", took, deadline)
			}
		})
	}

	// A call with no context waits for its turn, and a Close ends the wait.
	waiting := make(chan error, 1)
	go func() { waiting <- a.CheckPage(0) }()
	select {
	case err := <-waiting:
		t.Fatalf("CheckPage did not wait: %v", err)
	case <-time.After(deadline):
	}
	within(t, prompt, func() { _ = a.Close() })
	select {
	case err := <-waiting:
		wantClosed(t, err)
	case <-time.After(prompt):
		t.Fatal("a closed document left its waiting call waiting")
	}

	letGo()
	select {
	case err := <-finished:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(prompt):
		t.Fatal("the slow export did not finish")
	}
	if _, err := b.PageImage(context.Background(), 0, 0.1, o); err != nil {
		t.Fatal(err)
	}
}

// Close races calls in flight and is made twice and three times at once:
// each call finishes whole or is refused as closed, and every C object is
// released once, after the last call using it.
func TestCloseRacesRenders(t *testing.T) {
	page := loadPage(t, linesPage)
	archive := notebook(t, "P Lines medium", page, page)
	before := live()
	for round := range 20 {
		f, err := rmrender.NewFonts(fonts.FS)
		if err != nil {
			t.Fatal(err)
		}
		d, err := rmrender.OpenDocument(archive, nil)
		if err != nil {
			t.Fatal(err)
		}
		o := &rmrender.Options{Fonts: f}
		ctx, cancel := context.WithCancel(context.Background())
		var wg sync.WaitGroup
		start := make(chan struct{})
		for g := range 6 {
			wg.Add(1)
			go func() {
				defer wg.Done()
				<-start
				for i := range 4 {
					var err error
					switch (g + i) % 6 {
					case 0:
						_, err = d.PageImage(ctx, i%d.Len(), 0.1, o)
					case 1:
						_, err = d.WritePDF(ctx, io.Discard, rmrender.PDFFresh, o)
					case 2:
						_, _, err = d.SVG(ctx, o)
					case 3:
						_, err = rmrender.PagePNG(ctx, page, 0.1, o)
					case 4:
						err = d.CheckPage(i % d.Len())
					case 5:
						_, err = d.PageMarks(i%d.Len(), o)
					}
					if err != nil && !errors.Is(err, rmrender.ErrInvalidArgument) && !errors.Is(err, rmrender.ErrCancelled) {
						t.Errorf("round %d: %v", round, err)
					}
				}
			}()
		}
		close(start)
		time.Sleep(time.Duration(round%4) * time.Millisecond)
		if round%3 == 0 {
			cancel()
		}
		for range 3 {
			wg.Add(2)
			go func() {
				defer wg.Done()
				_ = d.Close()
			}()
			go func() {
				defer wg.Done()
				_ = f.Close()
			}()
		}
		wg.Wait()
		cancel()
		_, err = d.PageImage(context.Background(), 0, 0.1, nil)
		wantClosed(t, err)
		_, err = rmrender.PagePNG(context.Background(), page, 0.1, o)
		wantClosed(t, err)
		wantClosed(t, d.CheckPage(0))
	}
	wantLive(t, before)
}

// Calls that fail, refused, cancelled before or while they wait, or failing in
// C, release everything they took, goroutines included.
func TestFailingCallsLeakNothing(t *testing.T) {
	page := loadPage(t, strokesPage)
	archive := notebook(t, "", page)
	settle()
	goroutines := runtime.NumGoroutine()
	before := live()

	f, err := rmrender.NewFonts(fonts.FS)
	if err != nil {
		t.Fatal(err)
	}
	d, err := rmrender.OpenDocument(archive, nil)
	if err != nil {
		t.Fatal(err)
	}
	o := &rmrender.Options{Fonts: f}
	cancelled, cancel := context.WithCancel(context.Background())
	cancel()
	bad := &rmrender.Options{Fonts: f, Limits: &rmrender.Limits{MaximumPoints: 1}}
	failing := errors.New("writer failed")
	for range 1000 {
		_, _ = d.PageImage(cancelled, 0, 0.1, o)
		_, _, _ = d.PDF(cancelled, rmrender.PDFFresh, o)
		_, _ = d.WritePDF(cancelled, io.Discard, rmrender.PDFFresh, o)
		_, _ = d.WritePDF(context.Background(), writerFunc(func([]byte) (int, error) { return 0, failing }), rmrender.PDFFresh, o)
		_, _ = d.PageImage(context.Background(), 5, 0.1, o)
		_, _ = d.PageImage(context.Background(), 0, 0.1, bad)
		_, _ = rmrender.PagePNG(cancelled, page, 0.1, o)
		_, _ = rmrender.PagePNG(context.Background(), page, 0.1, bad)
	}
	_ = d.Close()
	_ = f.Close()
	for range 1000 {
		_, _ = d.PageImage(context.Background(), 0, 0.1, o)
		_, _ = rmrender.PagePNG(context.Background(), page, 0.1, o)
		_ = d.CheckPage(0)
	}
	wantLive(t, before)
	settle()
	if got := runtime.NumGoroutine(); got > goroutines {
		t.Fatalf("%d goroutines, %d before", got, goroutines)
	}
}

// settle lets goroutines that have finished their work exit.
func settle() {
	for range 10 {
		runtime.GC()
		time.Sleep(10 * time.Millisecond)
	}
}

// A callback that closes the document and the fonts of its call and then
// panics or exits keeps the guarantees of either alone: the panic or exit
// reaches the caller after cleanup, and the closed objects are released
// once.
func TestAbnormalExitAfterCloseFromCallback(t *testing.T) {
	page := loadPage(t, linesPage)
	imaged := loadAuthored(t, imagePage)
	archive := notebook(t, "P Lines medium", page)
	for _, callback := range []string{"writer", "template", "image"} {
		for _, exit := range []string{"panic", "goexit"} {
			t.Run(callback+"/"+exit, func(t *testing.T) {
				before := live()
				d, err := rmrender.OpenDocument(archive, nil)
				if err != nil {
					t.Fatal(err)
				}
				f, err := rmrender.NewFonts(fonts.FS)
				if err != nil {
					t.Fatal(err)
				}
				stop := func() {
					_ = d.Close()
					_ = f.Close()
					if exit == "panic" {
						panic("stop")
					}
					runtime.Goexit()
				}
				o := &rmrender.Options{Fonts: f}
				var recovered any
				returned := false
				within(t, prompt, func() {
					done := make(chan struct{})
					go func() {
						defer close(done)
						defer func() { recovered = recover() }()
						switch callback {
						case "writer":
							_, _ = d.WritePDF(context.Background(), callbackWriter{stop}, rmrender.PDFFresh, o)
						case "template":
							o.Templates = rmrender.NewTemplates(&hookFS{fsys: fstest.MapFS{}, suffix: ".template", hook: stop})
							_, _ = d.PageImage(context.Background(), 0, 0.1, o)
						case "image":
							o.Images = &hookFS{fsys: pictureFS(), suffix: ".png", hook: stop}
							_, _ = rmrender.PageImage(context.Background(), imaged, 0.1, o)
						}
						returned = true
					}()
					<-done
				})
				if returned {
					t.Fatal("the call returned after the callback's abnormal exit")
				}
				if exit == "panic" && recovered != "stop" {
					t.Fatalf("recovered %v, want the callback's panic", recovered)
				}
				if exit == "goexit" && recovered != nil {
					t.Fatalf("recovered %v after Goexit", recovered)
				}
				wantLive(t, before)
				_, err = d.PageImage(context.Background(), 0, 0.1, nil)
				wantClosed(t, err)
			})
		}
	}
}

// WritePDF checks its context while pages are drawn: one that ends while the
// writer receives the end of the file leaves the file whole, and no error.
func TestWritePDFCancelledDuringTheLastWrite(t *testing.T) {
	d := openDocument(t, notebook(t, "", loadPage(t, strokesPage)))
	o := withFonts(t, rmrender.Options{})
	want, _, err := d.PDF(context.Background(), rmrender.PDFFresh, o)
	if err != nil {
		t.Fatal(err)
	}
	if len(want) >= rmrender.OutputPiece {
		t.Fatalf("the fixture's PDF is %d bytes, not one piece", len(want))
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	var got bytes.Buffer
	_, err = d.WritePDF(ctx, writerFunc(func(p []byte) (int, error) {
		cancel()
		return got.Write(p)
	}), rmrender.PDFFresh, o)
	if err != nil {
		t.Fatalf("got %v, want the whole file and no error", err)
	}
	if ctx.Err() == nil {
		t.Fatal("the writer was not called")
	}
	if !bytes.Equal(blankVolatile(got.Bytes()), blankVolatile(want)) {
		t.Fatal("the file written differs from PDF's")
	}
}
