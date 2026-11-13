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
	for _, which := range []string{"font", "writer"} {
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
				if which == "font" {
					_, _ = rmrender.NewFonts(callbackFS{next})
				} else {
					_, _ = d.WritePDF(context.Background(), callbackWriter{next}, rmrender.PDFPreserving, options)
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
			if i%2 == 0 {
				_, _ = rmrender.NewFonts(callbackFS{next})
			} else {
				_, _ = d.WritePDF(context.Background(), callbackWriter{next}, rmrender.PDFFresh, nil)
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
			if i%2 == 0 {
				_, _ = rmrender.NewFonts(callbackFS{runtime.Goexit})
			} else {
				_, _ = d.WritePDF(context.Background(), exitingWriter{}, rmrender.PDFFresh, nil)
			}
		}()
		<-done
		if got := rmrender.LiveHandles(); got != before {
			t.Fatalf("live handles = %d, want %d", got, before)
		}
	}
	runtime.GC()
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
