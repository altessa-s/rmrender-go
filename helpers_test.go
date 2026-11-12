// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"archive/zip"
	"bytes"
	"errors"
	"fmt"
	"hash/crc32"
	"image"
	"image/png"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"github.com/altessa-s/rmrender-go"
	"github.com/altessa-s/rmrender-go/fonts"
)

// The fixture pages, from testdata/pages.
const (
	// Two handwritten letters.
	strokesPage = "Normal_AB.rm"
	// Highlighted text of an imported PDF.
	highlightedPage = "Wikipedia_highlighted_p1.rm"
	// Typed text in four paragraph styles.
	typedPage = "Bold_Heading_Bullet_Normal.rm"
	// Ruled-line strokes.
	linesPage = "Lines_v2.rm"
	// Highlighter and shader strokes in many colours.
	colorPage = "More_color_highlight_shader_v3.15.4.2.rm"
)

// The canvas of a tablet page, in .rm units.
const canvasWidth, canvasHeight = 1404, 1872

// loadPage answers a fixture page.
func loadPage(t testing.TB, name string) []byte {
	t.Helper()
	data, err := os.ReadFile(filepath.Join("testdata", "pages", name))
	if err != nil {
		t.Fatal(err)
	}
	return data
}

var shared struct {
	once  sync.Once
	fonts *rmrender.Fonts
	err   error
}

// bundledFonts answers the bundled fonts, loaded once for the whole test
// binary and never closed.
func bundledFonts(t testing.TB) *rmrender.Fonts {
	t.Helper()
	shared.once.Do(func() {
		shared.fonts, shared.err = rmrender.NewFonts(fonts.FS)
	})
	if shared.err != nil {
		t.Fatal(shared.err)
	}
	return shared.fonts
}

// withFonts answers o with the bundled fonts, which every page that holds a
// text block needs.
func withFonts(t testing.TB, o rmrender.Options) *rmrender.Options {
	t.Helper()
	o.Fonts = bundledFonts(t)
	return &o
}

// decodePNG decodes data, failing the test unless it is a PNG.
func decodePNG(t testing.TB, data []byte) image.Image {
	t.Helper()
	img, err := png.Decode(bytes.NewReader(data))
	if err != nil {
		t.Fatalf("not a PNG: %v", err)
	}
	return img
}

// wantSize fails the test unless img is width by height pixels.
func wantSize(t testing.TB, img image.Image, width, height int) {
	t.Helper()
	if got := img.Bounds().Size(); got.X != width || got.Y != height {
		t.Fatalf("image is %dx%d, want %dx%d", got.X, got.Y, width, height)
	}
}

// wantPDF fails the test unless data is a PDF of pages pages.
func wantPDF(t testing.TB, data []byte, pages int) {
	t.Helper()
	if !bytes.HasPrefix(data, []byte("%PDF-")) {
		t.Fatalf("not a PDF: %q", data[:min(len(data), 16)])
	}
	n, err := rmrender.PDFPageCount(data)
	if err != nil {
		t.Fatal(err)
	}
	if n != pages {
		t.Fatalf("PDF has %d pages, want %d", n, pages)
	}
}

// wantStatus fails the test unless err is an *Error of the given status.
func wantStatus(t testing.TB, err error, want rmrender.Status) {
	t.Helper()
	var e *rmrender.Error
	if !errors.As(err, &e) {
		t.Fatalf("got %v, want an *Error of %v", err, want)
	}
	if e.Status != want {
		t.Fatalf("got %v, want %v", err, want)
	}
}

// inked counts the pixels of img that are not transparent.
func inked(img image.Image) int {
	n := 0
	b := img.Bounds()
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			if _, _, _, a := img.At(x, y).RGBA(); a != 0 {
				n++
			}
		}
	}
	return n
}

// file is one entry of a test archive.
type file struct {
	name string
	data []byte
}

// archive packs files as a stored .rmdoc archive, in order.
func archive(t testing.TB, files ...file) []byte {
	t.Helper()
	var b bytes.Buffer
	w := zip.NewWriter(&b)
	for _, f := range files {
		h := &zip.FileHeader{
			Name:               f.name,
			Method:             zip.Store,
			CRC32:              crc32.ChecksumIEEE(f.data),
			CompressedSize64:   uint64(len(f.data)),
			UncompressedSize64: uint64(len(f.data)),
		}
		out, err := w.CreateRaw(h)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := out.Write(f.data); err != nil {
			t.Fatal(err)
		}
	}
	if err := w.Close(); err != nil {
		t.Fatal(err)
	}
	return b.Bytes()
}

// docID is the document of the test archives.
const docID = "a1b2"

// notebook packs a notebook of pages, p1, p2 and on, each on template; a nil
// page is listed without a .rm file, as the tablet leaves a page nobody wrote
// on.
func notebook(t testing.TB, template string, pages ...[]byte) []byte {
	t.Helper()
	var entries []string
	files := []file{{docID + ".metadata", []byte(`{"visibleName":"Notes","type":"DocumentType"}`)}}
	for i, p := range pages {
		id := fmt.Sprintf("p%d", i+1)
		entries = append(entries, fmt.Sprintf(`{"id":%q,"idx":{"value":"b%c"},"template":{"value":%q}}`, id, 'a'+i, template))
		if p != nil {
			files = append(files, file{docID + "/" + id + ".rm", p})
		}
	}
	content := `{"fileType":"notebook","cPages":{"pages":[` + strings.Join(entries, ",") + `]}}`
	files = append(files, file{docID + ".content", []byte(content)})
	return archive(t, files...)
}

// imported packs a document of an imported PDF, with page marks drawn on its
// first page.
func imported(t testing.TB, pdf, marks []byte) []byte {
	t.Helper()
	pages, err := rmrender.PDFPageCount(pdf)
	if err != nil {
		t.Fatal(err)
	}
	var entries []string
	for i := range pages {
		entries = append(entries, fmt.Sprintf(`{"id":"s%d","idx":{"value":"b%c"},"redir":{"value":%d}}`, i+1, 'a'+i, i))
	}
	content := `{"fileType":"pdf","cPages":{"pages":[` + strings.Join(entries, ",") + `]}}`
	return archive(t,
		file{docID + ".content", []byte(content)},
		file{docID + ".pdf", pdf},
		file{docID + "/s1.rm", marks},
	)
}

// openDocument opens data, closed when the test ends.
func openDocument(t testing.TB, data []byte) *rmrender.Document {
	t.Helper()
	d, err := rmrender.OpenDocument(data, nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = d.Close() })
	return d
}
