// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

// Command authored writes the pages of testdata/authored with rmlines-go's
// writer. It lives in the coexist module because that module, not the main
// one, depends on rmlines-go. From the repository root:
//
//	go -C internal/coexist run ./authored ../../testdata/authored
//
// The writer answers the same bytes for the same draft every time, so a run
// reproduces the committed files; the test beside this file checks it.
package main

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/altessa-s/rmlines-go"
)

// pages are the files written, by name.
var pages = map[string]func() ([]byte, error){
	"Image_placed.rm": imagePlaced,
}

// imagePlaced is a tablet page with one black fineliner stroke and, over it,
// the image picture.png placed at (300, 600), 400 units wide and 300 high.
func imagePlaced() ([]byte, error) {
	stroke := []rmlines.DraftPoint{
		rmlines.DefaultPoint(200, 200),
		rmlines.DefaultPoint(600, 400),
		rmlines.DefaultPoint(900, 300),
	}
	return rmlines.Write(&rmlines.Draft{
		Author:  [16]byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
		Sheet:   rmlines.Sheet{ScreenWidth: 1404, ScreenHeight: 1872, CanvasWidth: 1404, CanvasHeight: 1872},
		Column:  rmlines.ColumnOf(rmlines.ColumnMedium),
		Strokes: []rmlines.DraftStroke{rmlines.DefaultStroke(rmlines.ToolFineliner2, rmlines.ColorBlack, stroke)},
		Images: []rmlines.DraftImage{{
			File: "picture.png",
			Key:  [16]byte{0xa0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
			X:    300, Y: 600, Width: 400, Height: 300,
		}},
	})
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: authored <directory>")
		os.Exit(2)
	}
	for name, page := range pages {
		data, err := page()
		if err == nil {
			err = os.WriteFile(filepath.Join(os.Args[1], name), data, 0o644)
		}
		if err != nil {
			fmt.Fprintf(os.Stderr, "%s: %v\n", name, err)
			os.Exit(1)
		}
	}
}
