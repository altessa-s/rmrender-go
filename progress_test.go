// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"context"
	"errors"
	"fmt"
	"io"
	"slices"
	"strings"
	"testing"

	"github.com/altessa-s/rmrender-go"
)

type documentExport struct {
	name string
	run  func(context.Context, *rmrender.Options) error
}

func documentExports(d *rmrender.Document) []documentExport {
	var exports []documentExport
	for _, mode := range []struct {
		name string
		mode rmrender.PDFMode
	}{
		{"fresh", rmrender.PDFFresh},
		{"preserving", rmrender.PDFPreserving},
		{"recomposed", rmrender.PDFRecomposed},
	} {
		exports = append(exports,
			documentExport{mode.name + "/buffer", func(ctx context.Context, o *rmrender.Options) error {
				_, _, err := d.PDF(ctx, mode.mode, o)
				return err
			}},
			documentExport{mode.name + "/writer", func(ctx context.Context, o *rmrender.Options) error {
				_, err := d.WritePDF(ctx, io.Discard, mode.mode, o)
				return err
			}},
		)
	}
	return append(exports, documentExport{"svg", func(ctx context.Context, o *rmrender.Options) error {
		_, _, err := d.SVG(ctx, o)
		return err
	}})
}

func TestOptionsProgressWiring(t *testing.T) {
	o := &rmrender.Options{}
	for _, step := range []struct {
		name string
		opts *rmrender.Options
		on   bool
	}{
		{"nil options", nil, false},
		{"nil progress", o, false},
		{"enabled", o, true},
		{"disabled again", o, false},
	} {
		t.Run(step.name, func(t *testing.T) {
			if step.on {
				o.Progress = func(int, int) { t.Error("inspection invoked Progress") }
			} else {
				o.Progress = nil
			}
			request, callback, err := rmrender.OptionsProgress(step.opts)
			if err != nil {
				t.Fatal(err)
			}
			if request != step.on || callback != step.on {
				t.Fatalf("request.progress = %t, C options.progress != NULL = %t, want both %t", request, callback, step.on)
			}
		})
	}
}

// Exercise both the notebook path and actual imported-PDF paths (including
// pages without marks, which must settle too).
func progressDocuments(t *testing.T) []*rmrender.Document {
	t.Helper()
	page := loadPage(t, strokesPage)
	notes := openDocument(t, notebook(t, "", page, nil, page, nil, page))
	pdf, _, err := notes.PDF(context.Background(), rmrender.PDFFresh, withFonts(t, rmrender.Options{}))
	if err != nil {
		t.Fatal(err)
	}
	return []*rmrender.Document{notes, openDocument(t, imported(t, pdf, page))}
}

func TestDocumentProgress(t *testing.T) {
	for i, d := range progressDocuments(t) {
		for _, export := range documentExports(d) {
			for _, maximum := range []int{0, 3} {
				t.Run(fmt.Sprintf("document%d/%s/maximum%d", i, export.name, maximum), func(t *testing.T) {
					total := d.Len()
					if maximum != 0 {
						total = maximum
					}
					var got []int
					o := withFonts(t, rmrender.Options{MaximumPages: maximum, PDFRasterDPI: 36, Progress: func(completed, n int) {
						if n != total {
							t.Errorf("total = %d, want %d", n, total)
						}
						got = append(got, completed)
					}})
					err := export.run(context.Background(), o)
					if i == 1 && maximum != 0 && strings.HasPrefix(export.name, "preserving/") {
						// Preserving an imported PDF cannot change its page count.
						if !errors.Is(err, rmrender.ErrUnsupported) || !slices.Equal(got, []int{0}) {
							t.Fatalf("preserving refusal: progress = %v, error = %v", got, err)
						}
						return
					}
					if err != nil {
						t.Fatal(err)
					}
					want := make([]int, total+1)
					for n := range want {
						want[n] = n
					}
					if !slices.Equal(got, want) {
						t.Fatalf("progress = %v, want %v", got, want)
					}
					// Reusing the options with nil must install no callback.
					o.Progress = nil
					if err := export.run(context.Background(), o); err != nil {
						t.Fatal(err)
					}
					if !slices.Equal(got, want) {
						t.Fatalf("nil Progress made calls: %v", got)
					}
				})
			}
		}
	}
}

func TestDocumentProgressCancellation(t *testing.T) {
	for i, d := range progressDocuments(t) {
		for _, export := range documentExports(d) {
			t.Run(fmt.Sprintf("document%d/%s", i, export.name), func(t *testing.T) {
				ctx, cancel := context.WithCancel(context.Background())
				defer cancel()
				var got []int
				o := withFonts(t, rmrender.Options{PDFRasterDPI: 36, Progress: func(completed, total int) {
					if total != d.Len() {
						t.Errorf("total = %d, want %d", total, d.Len())
					}
					got = append(got, completed)
					if completed == 1 {
						cancel()
					}
				}})
				err := export.run(ctx, o)
				if !errors.Is(err, context.Canceled) || !errors.Is(err, rmrender.ErrCancelled) {
					t.Fatalf("progress = %v, cancellation = %v", got, err)
				}
				if len(got) < 2 || len(got) > d.Len() {
					t.Fatalf("progress is not a cancelled prefix: %v", got)
				}
				for completed, n := range got {
					if n != completed {
						t.Fatalf("progress is not an ordered prefix: %v", got)
					}
				}
				// Cancellation must release the document and fonts for reuse.
				o.Progress = nil
				if err := export.run(context.Background(), o); err != nil {
					t.Fatal(err)
				}
			})
		}
	}
}

func TestDocumentProgressFailedPage(t *testing.T) {
	d := openDocument(t, notebook(t, "", []byte("bad page"), nil, nil))
	for _, export := range documentExports(d) {
		t.Run(export.name, func(t *testing.T) {
			var got []int
			o := &rmrender.Options{Progress: func(completed, total int) {
				if total != 3 {
					t.Errorf("total = %d, want 3", total)
				}
				got = append(got, completed)
			}}
			if err := export.run(context.Background(), o); !errors.Is(err, rmrender.ErrBackend) {
				t.Fatalf("failed page: %v", err)
			}
			if !slices.Equal(got, []int{0}) {
				t.Fatalf("failed page reported completion: %v", got)
			}
		})
	}
}
