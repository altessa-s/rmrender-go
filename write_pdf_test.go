// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"math/rand/v2"
	"testing"

	"github.com/altessa-s/rmrender-go"
)

// recorder keeps what it is written and how, failing with fail on the write
// numbered failAt (from 1) and, with short set, taking one byte less than
// it is given without saying why.
type recorder struct {
	bytes.Buffer
	writes, largest, afterFailure int
	failAt                        int
	fail                          error
	short                         bool
	failed                        bool
}

func (r *recorder) Write(p []byte) (int, error) {
	if r.failed {
		r.afterFailure++
	}
	r.writes++
	r.largest = max(r.largest, len(p))
	if r.failAt != 0 && r.writes >= r.failAt {
		r.failed = true
		if r.short {
			n, _ := r.Buffer.Write(p[:len(p)-1])
			return n, nil
		}
		return 0, r.fail
	}
	return r.Buffer.Write(p)
}

// blankVolatile answers a copy of pdf without what two exports of the same
// document may not share: the dates and the document ID, written at fixed
// widths.
func blankVolatile(pdf []byte) []byte {
	b := bytes.Clone(pdf)
	for i := 0; i+3 < len(b); i++ {
		if bytes.HasPrefix(b[i:], []byte("(D:")) || bytes.HasPrefix(b[i:], []byte("/ID")) {
			for i += 3; i < len(b) && b[i] != ')' && b[i] != ']'; i++ {
				b[i] = '0'
			}
		}
	}
	return b
}

// largePDF answers a one-page PDF over size bytes long: a page whose content
// stream is comments of random text, which deflate cannot shrink much.
func largePDF(size int) []byte {
	random := rand.New(rand.NewPCG(1, 2))
	content := make([]byte, 0, size+80)
	for len(content) < size {
		content = append(content, '%')
		for range 79 {
			content = append(content, byte('!'+random.IntN('~'-'!'+1)))
		}
		content = append(content, '\n')
	}
	var b bytes.Buffer
	offsets := make([]int, 5)
	b.WriteString("%PDF-1.7\n")
	objects := []string{
		"<< /Type /Catalog /Pages 2 0 R >>",
		"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
		"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R >>",
	}
	for i, o := range objects {
		offsets[i+1] = b.Len()
		fmt.Fprintf(&b, "%d 0 obj\n%s\nendobj\n", i+1, o)
	}
	offsets[4] = b.Len()
	fmt.Fprintf(&b, "4 0 obj\n<< /Length %d >>\nstream\n", len(content))
	b.Write(content)
	b.WriteString("\nendstream\nendobj\n")
	xref := b.Len()
	b.WriteString("xref\n0 5\n0000000000 65535 f \n")
	for _, o := range offsets[1:] {
		fmt.Fprintf(&b, "%010d 00000 n \n", o)
	}
	fmt.Fprintf(&b, "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n", xref)
	return b.Bytes()
}

var modes = []struct {
	mode rmrender.PDFMode
	name string
}{{rmrender.PDFFresh, "fresh"}, {rmrender.PDFPreserving, "preserving"}, {rmrender.PDFRecomposed, "recomposed"}}

// wantSameExport fails the test unless d written to a writer in mode is the
// PDF [Document.PDF] answers, reported the same, handed over in pieces of
// at most rmrender.OutputPiece bytes; it answers the writer.
func wantSameExport(t *testing.T, d *rmrender.Document, mode rmrender.PDFMode, o *rmrender.Options) *recorder {
	t.Helper()
	want, wantReport, err := d.PDF(context.Background(), mode, o)
	if err != nil {
		t.Fatal(err)
	}
	var r recorder
	report, err := d.WritePDF(context.Background(), &r, mode, o)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(blankVolatile(r.Bytes()), blankVolatile(want)) {
		t.Fatalf("written %d bytes, not the %d of the PDF", r.Len(), len(want))
	}
	if report != wantReport {
		t.Fatalf("report %+v, want %+v", report, wantReport)
	}
	if r.writes == 0 || r.largest > rmrender.OutputPiece {
		t.Fatalf("%d writes, the largest %d bytes", r.writes, r.largest)
	}
	return &r
}

// A document written to an io.Writer is the PDF of its mode, byte for byte,
// for a notebook and for an imported PDF, small or written in many pieces.
func TestWritePDF(t *testing.T) {
	o := withFonts(t, rmrender.Options{})
	nb := openDocument(t, notebook(t, "P Lines medium", loadPage(t, strokesPage), loadPage(t, typedPage), loadPage(t, highlightedPage)))
	source, err := rmrender.PagePDF(context.Background(), loadPage(t, linesPage), o)
	if err != nil {
		t.Fatal(err)
	}
	small := openDocument(t, imported(t, source, loadPage(t, strokesPage)))
	large := openDocument(t, imported(t, largePDF(9<<20), loadPage(t, strokesPage)))
	for _, m := range modes {
		t.Run(m.name, func(t *testing.T) {
			wantSameExport(t, nb, m.mode, o)
			wantSameExport(t, small, m.mode, o)
			// A fresh export redraws the imported page, without its
			// comments; the others copy its stream as it is.
			if r := wantSameExport(t, large, m.mode, o); m.mode != rmrender.PDFFresh && r.writes < 3 {
				t.Fatalf("a %d-byte file in %d writes", r.Len(), r.writes)
			}
		})
	}
	o.MaximumPages = 2
	var r recorder
	report, err := nb.WritePDF(context.Background(), &r, rmrender.PDFFresh, o)
	if err != nil {
		t.Fatal(err)
	}
	wantPDF(t, r.Bytes(), 2)
	if report.DocumentPages != 3 || report.RenderedPages != 2 {
		t.Fatalf("report %+v", report)
	}
}

// The writer's failure stops the export, with the writer's error as its
// cause; the writer is not called again, and what it took is the start of
// the file.
func TestWritePDFWriterFails(t *testing.T) {
	o := withFonts(t, rmrender.Options{})
	d := openDocument(t, imported(t, largePDF(9<<20), loadPage(t, strokesPage)))
	whole, _, err := d.PDF(context.Background(), rmrender.PDFPreserving, o)
	if err != nil {
		t.Fatal(err)
	}
	full := errors.New("disk full")
	for _, r := range []*recorder{
		{failAt: 1, fail: full},
		{failAt: 2, fail: full},
		{failAt: 2, short: true},
	} {
		_, err := d.WritePDF(context.Background(), r, rmrender.PDFPreserving, o)
		wantStatus(t, err, rmrender.StatusBackend)
		var e *rmrender.Error
		if !errors.As(err, &e) || e.Message != "PDF output write failed" {
			t.Fatalf("got %v", err)
		}
		if r.short && !errors.Is(err, io.ErrShortWrite) || !r.short && !errors.Is(err, full) {
			t.Fatalf("got %v, not caused by the writer", err)
		}
		if r.afterFailure != 0 || !bytes.HasPrefix(whole, r.Bytes()) {
			t.Fatalf("%d writes after the failure; %d bytes taken", r.afterFailure, r.Len())
		}
	}
}

// A writer's panic comes back out of WritePDF, once C has returned.
func TestWritePDFWriterPanics(t *testing.T) {
	d := openDocument(t, notebook(t, "", loadPage(t, strokesPage)))
	defer func() {
		if p := recover(); p != "writer panicked" {
			t.Fatalf("recovered %v", p)
		}
	}()
	_, _ = d.WritePDF(context.Background(), panicker{}, rmrender.PDFFresh, withFonts(t, rmrender.Options{}))
	t.Fatal("WritePDF returned")
}

type panicker struct{}

func (panicker) Write([]byte) (int, error) { panic("writer panicked") }

// A context that has ended, an unknown mode, a negative page cap, a nil
// writer and a closed document write nothing.
func TestWritePDFRefusals(t *testing.T) {
	o := withFonts(t, rmrender.Options{})
	d := openDocument(t, notebook(t, "", loadPage(t, strokesPage)))
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	var r recorder
	_, err := d.WritePDF(ctx, &r, rmrender.PDFFresh, o)
	wantStatus(t, err, rmrender.StatusCancelled)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("got %v, not caused by the context", err)
	}
	_, err = d.WritePDF(context.Background(), &r, 9, o)
	wantStatus(t, err, rmrender.StatusInvalidArgument)
	_, err = d.WritePDF(context.Background(), &r, rmrender.PDFFresh, &rmrender.Options{Fonts: o.Fonts, MaximumPages: -1})
	wantStatus(t, err, rmrender.StatusInvalidArgument)
	_, err = d.WritePDF(context.Background(), nil, rmrender.PDFFresh, o)
	wantStatus(t, err, rmrender.StatusInvalidArgument)
	if err := d.Close(); err != nil {
		t.Fatal(err)
	}
	_, err = d.WritePDF(context.Background(), &r, rmrender.PDFFresh, o)
	wantStatus(t, err, rmrender.StatusInvalidArgument)
	if r.writes != 0 {
		t.Fatalf("%d writes", r.writes)
	}
}

// The limit on PDF bytes bounds the file written: its exact size passes and
// a byte less fails.
func TestWritePDFLimit(t *testing.T) {
	o := withFonts(t, rmrender.Options{})
	d := openDocument(t, notebook(t, "", loadPage(t, strokesPage), loadPage(t, linesPage)))
	whole, _, err := d.PDF(context.Background(), rmrender.PDFFresh, o)
	if err != nil {
		t.Fatal(err)
	}
	limits := rmrender.DefaultLimits()
	limits.MaximumPDFBytes = len(whole)
	o.Limits = &limits
	var r recorder
	if _, err := d.WritePDF(context.Background(), &r, rmrender.PDFFresh, o); err != nil {
		t.Fatal(err)
	}
	limits.MaximumPDFBytes--
	_, err = d.WritePDF(context.Background(), &recorder{}, rmrender.PDFFresh, o)
	wantStatus(t, err, rmrender.StatusLimit)
}

// The pieces an export hands over: at most OutputPiece bytes each, in
// order, copied out of C's memory.
func TestOutputPieces(t *testing.T) {
	data := bytes.Repeat([]byte("0123456789abcdef"), (2*rmrender.OutputPiece+12345)/16)
	var r recorder
	if err := rmrender.WriteOutput(&r, data); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(r.Bytes(), data) || r.writes != 3 || r.largest != rmrender.OutputPiece {
		t.Fatalf("%d writes, the largest %d bytes", r.writes, r.largest)
	}
}
