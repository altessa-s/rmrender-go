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
