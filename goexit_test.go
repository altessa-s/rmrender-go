// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"bytes"
	"context"
	"fmt"
	"github.com/altessa-s/rmrender-go"
	"github.com/altessa-s/rmrender-go/fonts"
	"io/fs"
	"math/rand/v2"
	"os"
	"os/exec"
	"runtime"
	"sync"
	"testing"
)

type exitingWriter struct{}

func (exitingWriter) Write([]byte) (int, error) { runtime.Goexit(); return 0, nil }
func TestCallbackGoexitCleansUp(t *testing.T) {
	d := openDocument(t, notebook(t, "", nil))
	before := rmrender.LiveHandles()
	done := make(chan struct{})
	go func() {
		defer close(done)
		_, _ = d.WritePDF(context.Background(), exitingWriter{}, rmrender.PDFFresh, nil)
		t.Error("WritePDF returned after Goexit")
	}()
	<-done
	if got := rmrender.LiveHandles(); got != before {
		t.Fatalf("live handles = %d, want %d", got, before)
	}
}

type callbackFS struct{ fn func() }

func (f callbackFS) Open(name string) (fs.File, error) { f.fn(); return fonts.FS.Open(name) }

type callbackWriter struct{ fn func() }

func (w callbackWriter) Write(p []byte) (int, error) { w.fn(); return len(p), nil }

func TestResourceExitCleansUp(t *testing.T) {
	page := loadPage(t, strokesPage)
	fontSet := bundledFonts(t)
	for _, which := range []string{"font", "template"} {
		t.Run(which, func(t *testing.T) {
			before := rmrender.LiveHandles()
			calls := 0
			fsys := callbackFS{func() { calls++; runtime.Goexit() }}
			done := make(chan struct{})
			go func() {
				defer close(done)
				switch which {
				case "font":
					_, _ = rmrender.NewFonts(fsys)
				case "template":
					_, _ = rmrender.PageSVG(context.Background(), page, &rmrender.Options{Fonts: fontSet, Template: "P Lines medium", Templates: rmrender.NewTemplates(fsys)})

				}
				t.Error("resource call returned after exit")
			}()
			<-done
			if calls != 1 || rmrender.LiveHandles() != before {
				t.Fatalf("calls = %d, live handles = %d, want 1 and %d", calls, rmrender.LiveHandles(), before)
			}
		})
	}
}

func TestCallbacksStopOnSecondExit(t *testing.T) {
	d := openDocument(t, imported(t, largePDF(9<<20), loadPage(t, strokesPage)))
	options := withFonts(t, rmrender.Options{})
	for _, which := range []string{"font", "writer", "progress"} {
		t.Run(which, func(t *testing.T) {
			before := rmrender.LiveHandles()
			calls := 0
			next := func() {
				calls++
				if calls == 2 {
					runtime.Goexit()
				}
			}
			done := make(chan struct{})
			go func() {
				defer close(done)
				switch which {
				case "font":
					_, _ = rmrender.NewFonts(callbackFS{next})
				case "writer":
					_, _ = d.WritePDF(context.Background(), callbackWriter{next}, rmrender.PDFPreserving, options)
				case "progress":
					o := *options
					o.Progress = func(int, int) { next() }
					_, _, _ = d.PDF(context.Background(), rmrender.PDFPreserving, &o)
				}
				t.Error("call returned after exit")
			}()
			<-done
			if calls != 2 || rmrender.LiveHandles() != before {
				t.Fatalf("calls = %d, live handles = %d, want 2 and %d", calls, rmrender.LiveHandles(), before)
			}
		})
	}
	runtime.GC()
}

func TestCallbackAbnormalConcurrency(t *testing.T) {
	d := openDocument(t, notebook(t, "", nil))
	before := rmrender.LiveHandles()
	random := rand.New(rand.NewPCG(1, 2))
	var wg sync.WaitGroup
	for i := range 128 {
		panics := random.IntN(2) == 0
		wg.Add(1)
		go func() {
			defer wg.Done()
			defer func() {
				if p := recover(); (panics && p != "stop") || (!panics && p != nil) {
					t.Errorf("panic = %v, panics = %v", p, panics)
				}
			}()
			next := func() {
				if panics {
					panic("stop")
				}
				runtime.Goexit()
			}
			switch i % 3 {
			case 0:
				_, _ = rmrender.NewFonts(callbackFS{next})
			case 1:
				_, _ = d.WritePDF(context.Background(), callbackWriter{next}, rmrender.PDFFresh, nil)
			case 2:
				_, _, _ = d.PDF(context.Background(), rmrender.PDFFresh, &rmrender.Options{Progress: func(int, int) { next() }})
			}
			t.Error("abnormal callback returned")
		}()
	}
	wg.Wait()
	if got := rmrender.LiveHandles(); got != before {
		t.Fatalf("live handles = %d, want %d", got, before)
	}
}

func TestRepeatedCallbackExit(t *testing.T) {
	d := openDocument(t, notebook(t, "", nil))
	before := rmrender.LiveHandles()
	for i := range 2000 {
		done := make(chan struct{})
		go func() {
			defer close(done)
			switch i % 3 {
			case 0:
				_, _ = rmrender.NewFonts(callbackFS{runtime.Goexit})
			case 1:
				_, _ = d.WritePDF(context.Background(), exitingWriter{}, rmrender.PDFFresh, nil)
			case 2:
				_, _, _ = d.PDF(context.Background(), rmrender.PDFFresh, &rmrender.Options{Progress: func(int, int) { runtime.Goexit() }})
			}
		}()
		<-done
		if got := rmrender.LiveHandles(); got != before {
			t.Fatalf("live handles = %d, want %d", got, before)
		}
	}
	runtime.GC()
}

func TestDocumentProgressAbnormal(t *testing.T) {
	d := progressDocuments(t)[1]
	for _, export := range documentExports(d) {
		for _, at := range []int{0, 1, d.Len()} {
			for _, panics := range []bool{false, true} {
				t.Run(fmt.Sprintf("%s/at%d/panic%t", export.name, at, panics), func(t *testing.T) {
					before := rmrender.LiveHandles()
					calls := 0
					done := make(chan struct{})
					o := withFonts(t, rmrender.Options{PDFRasterDPI: 36})
					o.Progress = func(completed, _ int) {
						calls++
						if completed == at {
							if panics {
								panic("progress stop")
							}
							runtime.Goexit()
						}
					}
					go func() {
						defer close(done)
						defer func() {
							p := recover()
							if panics && p != "progress stop" || !panics && p != nil {
								t.Errorf("panic = %v, want panic = %t", p, panics)
							}
						}()
						_ = export.run(context.Background(), o)
						t.Error("abnormal progress callback returned")
					}()
					<-done
					if calls != at+1 || rmrender.LiveHandles() != before {
						t.Fatalf("calls = %d, handles = %d, want %d and %d", calls, rmrender.LiveHandles(), at+1, before)
					}
					o.Progress = nil
					if err := export.run(context.Background(), o); err != nil {
						t.Fatal(err)
					}
				})
			}
		}
	}
}

func TestDocumentProgressAbnormalStopsNative(t *testing.T) {
	// Keep exports serial: the native report counter is package-wide.
	const pages = 48
	content := make([][]byte, pages)
	page := loadPage(t, strokesPage)
	for i := range content {
		content[i] = page
	}
	notes := openDocument(t, notebook(t, "", content...))
	pdf, _, err := notes.PDF(context.Background(), rmrender.PDFFresh, withFonts(t, rmrender.Options{}))
	if err != nil {
		t.Fatal(err)
	}
	documents := []*rmrender.Document{notes, openDocument(t, imported(t, pdf, content[0]))}
	for i, d := range documents {
		if d.Len() != pages {
			t.Fatalf("document has %d pages, want %d", d.Len(), pages)
		}
		for _, export := range documentExports(d) {
			for _, cancellable := range []bool{false, true} {
				for _, panics := range []bool{false, true} {
					t.Run(fmt.Sprintf("document%d/%s/cancellable%t/panic%t", i, export.name, cancellable, panics), func(t *testing.T) {
						ctx := context.Background()
						if cancellable {
							var cancel context.CancelFunc
							ctx, cancel = context.WithCancel(ctx)
							defer cancel()
						}
						before := rmrender.NativeProgressReports()
						handles := rmrender.LiveHandles()
						var atFailure int64
						calls := 0
						o := withFonts(t, rmrender.Options{PDFRasterDPI: 36, Progress: func(completed, total int) {
							if completed != calls || total != pages {
								t.Errorf("progress = (%d, %d), want (%d, %d)", completed, total, calls, pages)
							}
							calls++
							if completed == 1 {
								atFailure = rmrender.NativeProgressReports()
								if panics {
									panic("progress stop")
								}
								runtime.Goexit()
							}
						}})
						done := make(chan struct{})
						go func() {
							defer close(done)
							defer func() {
								p := recover()
								if panics && p != "progress stop" || !panics && p != nil {
									t.Errorf("panic = %v, want panic = %t", p, panics)
								}
							}()
							_ = export.run(ctx, o)
							t.Error("abnormal progress callback returned")
						}()
						<-done
						if calls != 2 || atFailure-before != 2 {
							t.Fatalf("caller reports = %d, native reports at failure = %d, want 2 each", calls, atFailure-before)
						}
						// Native PDF writers use at most eight workers and a
						// window of 2*workers+2. Allow that window to settle,
						// but never the rest of this much larger document.
						const window = 2*8 + 2
						after := rmrender.NativeProgressReports() - atFailure
						if after > window {
							t.Errorf("native reports after abnormal Progress = %d, want <= %d (document has %d pages)", after, window, pages)
						}
						t.Logf("native reports after abnormal Progress = %d (limit %d)", after, window)
						if got := rmrender.LiveHandles(); got != handles {
							t.Errorf("live handles = %d, want %d", got, handles)
						}
					})
				}
			}
		}
	}
}

// A failed subtest also fails its parent, so exercise Fatal in a child test
// process and let the outer test check both its failure and its cleanup.
func TestCallbackFatal(t *testing.T) {
	if os.Getenv("RMRENDER_CALLBACK_FATAL") == "1" {
		d := openDocument(t, notebook(t, "", nil))
		before := rmrender.LiveHandles()
		deferred := false
		ok := t.Run("callback", func(t *testing.T) {
			defer func() { deferred = true }()
			_, _ = d.WritePDF(context.Background(), callbackWriter{func() { t.Fatal("callback fatal") }}, rmrender.PDFFresh, nil)
		})
		if !ok && deferred && rmrender.LiveHandles() == before {
			fmt.Println("fatal failed and cleaned up")
		}
		return
	}
	cmd := exec.Command(os.Args[0], "-test.run=^TestCallbackFatal$")
	cmd.Env = append(os.Environ(), "RMRENDER_CALLBACK_FATAL=1")
	out, err := cmd.CombinedOutput()
	if err == nil || !bytes.Contains(out, []byte("fatal failed and cleaned up")) {
		t.Fatalf("child = %v: %s", err, out)
	}
}
