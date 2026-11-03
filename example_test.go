// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"image/color"
	"image/png"
	"io/fs"
	"log"
	"os"
	"time"

	"github.com/altessa-s/rmrender-go"
	"github.com/altessa-s/rmrender-go/fonts"
)

func ExamplePagePNG() {
	page, err := os.ReadFile("testdata/pages/Normal_AB.rm")
	if err != nil {
		log.Fatal(err)
	}
	f, err := rmrender.NewFonts(fonts.FS)
	if err != nil {
		log.Fatal(err)
	}
	defer f.Close()
	opts := &rmrender.Options{Fonts: f, Template: "P Lines medium", Background: color.White}
	out, err := rmrender.PagePNG(context.Background(), page, 0.5, opts)
	if err != nil {
		fmt.Println(err)
		return
	}
	config, err := png.DecodeConfig(bytes.NewReader(out))
	if err != nil {
		fmt.Println(err)
		return
	}
	fmt.Println(config.Width, config.Height)
	// Output:
	// 702 993
}

func ExamplePagePDF() {
	page, err := os.ReadFile("testdata/pages/Bold_Heading_Bullet_Normal.rm")
	if err != nil {
		log.Fatal(err)
	}
	f, err := rmrender.NewFonts(fonts.FS)
	if err != nil {
		log.Fatal(err)
	}
	defer f.Close()
	pdf, err := rmrender.PagePDF(context.Background(), page, &rmrender.Options{Fonts: f, Paper: rmrender.PaperLetter})
	if err != nil {
		fmt.Println(err)
		return
	}
	fmt.Println(string(pdf[:5]))
	// Output:
	// %PDF-
}

func ExamplePageMarks() {
	page, err := os.ReadFile("testdata/pages/Wikipedia_highlighted_p1.rm")
	if err != nil {
		log.Fatal(err)
	}
	f, err := rmrender.NewFonts(fonts.FS)
	if err != nil {
		log.Fatal(err)
	}
	defer f.Close()
	marks, err := rmrender.PageMarks(page, "p1", &rmrender.Options{Fonts: f})
	if err != nil {
		fmt.Println(err)
		return
	}
	for _, h := range marks.Highlights {
		fmt.Printf("%q in #%06x\n", h.Text, h.Band.Color)
	}
	// Output:
	// "The reMarkable uses electronic paper" in #fffd5a
	// "ReMarkable uses its own operating system, named Codex." in #fffd5a
	// "Codex is based on Linux and optimized for electronic paper" in #fffd5a
	// "display technology.[13]" in #fffd5a
}

func ExampleOpenDocument() {
	data, err := os.ReadFile("testdata/pages/Notes.rmdoc")
	if err != nil {
		log.Fatal(err)
	}
	f, err := rmrender.NewFonts(fonts.FS)
	if err != nil {
		log.Fatal(err)
	}
	defer f.Close()
	doc, err := rmrender.OpenDocument(data, nil)
	if err != nil {
		fmt.Println(err)
		return
	}
	defer doc.Close()
	for i, p := range doc.Pages() {
		fmt.Println(i, p.ID, p.Template)
	}
	opts := &rmrender.Options{Fonts: f, Paper: rmrender.PaperA4}
	pdf, report, err := doc.PDF(context.Background(), rmrender.PDFFresh, opts)
	if err != nil {
		fmt.Println(err)
		return
	}
	fmt.Println(string(pdf[:5]), report.RenderedPages, "pages,", report.SkippedBlocks, "blocks skipped")
	thumbnail, err := doc.PagePNG(context.Background(), 0, 0.1, opts)
	if err != nil {
		fmt.Println(err)
		return
	}
	config, err := png.DecodeConfig(bytes.NewReader(thumbnail))
	if err != nil {
		fmt.Println(err)
		return
	}
	fmt.Println(config.Width, config.Height)
	// Output:
	// 0 p1 P Lines medium
	// 1 p2 P Lines medium
	// 2 p3 P Lines medium
	// %PDF- 3 pages, 0 blocks skipped
	// 140 199
}

func ExampleNewTemplates() {
	names, err := fs.Glob(rmrender.BundledTemplates(), "P Lines *.template")
	if err != nil {
		log.Fatal(err)
	}
	fmt.Println(names)
	// Templates of one's own, found before the bundled ones by the same name.
	mine := rmrender.NewTemplates(os.DirFS("my-templates"))
	_ = &rmrender.Options{Templates: mine, Template: "P Lines small"}
	// Output:
	// [P Lines large.template P Lines medium.template P Lines small.template]
}

func ExampleError() {
	ctx, cancel := context.WithTimeout(context.Background(), time.Millisecond)
	defer cancel()
	<-ctx.Done()
	_, err := rmrender.PagePDF(ctx, []byte("page"), nil)
	fmt.Println(errors.Is(err, rmrender.ErrCancelled), errors.Is(err, context.DeadlineExceeded))
	_, err = rmrender.PagePDF(context.Background(), []byte("not a page"), nil)
	var e *rmrender.Error
	if errors.As(err, &e) {
		fmt.Println(e.Status, "-", e.Message)
	}
	// Output:
	// true true
	// backend failed - page: the stream ended inside a field
}
