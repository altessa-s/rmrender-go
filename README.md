# rmrender-go

reMarkable pages and documents, drawn from Go: a `.rm` page to a PNG or a one-page PDF, a `.rmdoc` document page by page or whole to a PDF,
with handwriting and its pen textures, typed text, highlights, images and page templates; and the highlights of a page, read as Go values.

Idiomatic cgo bindings over **[rmrender-lib](https://github.com/altessa-s/rmrender-lib)**, the portable C23 renderer, whose sources are
vendored into this module together with the libraries it reads its input with: **[rmlines-lib](https://github.com/altessa-s/rmlines-lib)**
for `.rm` pages, **[rmdoc-lib](https://github.com/altessa-s/rmdoc-lib)** for `.rmdoc` archives and
**[rmmarks-lib](https://github.com/altessa-s/rmmarks-lib)** for highlights. Drawing is native: CoreGraphics and CoreText on macOS, Cairo, Pango
and PDFium on Linux. The same renderer is available for Swift as **[rmrender-swift](https://github.com/altessa-s/rmrender-swift)**.

Part of **[Slate Drive](https://slatedrive.app)** ([repository](https://github.com/altessa-s/slatedrive)), the macOS app that shows a reMarkable
library in Finder.

## Requirements

- Go 1.24 or newer, with cgo enabled (`CGO_ENABLED=1`, the default wherever a C compiler is found).
- A **C23 compiler**: Apple's clang in a current Xcode, clang 19 or newer, or GCC 15 or newer, as rmrender-lib asks. The libraries are written
  with `nullptr`, typed enums and `constexpr`, which older compilers do not fully read. Point `CC` at the compiler when the default one is too
  old.
- **macOS**: nothing else. CoreGraphics, CoreText and zlib come with the SDK.
- **Linux**: the Cairo stack and PDFium. Cairo 1.16 or newer, Pango 1.50 or newer, Fontconfig, HarfBuzz 3.3 or newer, GdkPixbuf and zlib are
  found with pkg-config; on Debian 13 or Ubuntu 24.04:

  ```sh
  apt-get install clang pkg-config python3 libcairo2-dev libpango1.0-dev libharfbuzz-dev \
      libgdk-pixbuf-2.0-dev libfontconfig1-dev zlib1g-dev
  ```

  PDFium is not packaged by distributions. rmrender-lib's `fetch-pdfium.py`, vendored in this module, fetches a pinned, checksum-verified
  build (`linux-x64`, `linux-arm64`, `mac-arm64`, `mac-x64`); its headers and library reach cgo through `CGO_CFLAGS` and `CGO_LDFLAGS`, with
  an rpath so that the program finds `libpdfium.so` when it runs:

  ```sh
  module=$(go list -m -f '{{.Dir}}' github.com/altessa-s/rmrender-go)
  python3 "$module/internal/clib/rmrender/scripts/fetch-pdfium.py" --platform linux-x64 --output "$HOME/pdfium"
  export CGO_CFLAGS="-I$HOME/pdfium/include"
  export CGO_LDFLAGS="-L$HOME/pdfium/lib -Wl,-rpath,$HOME/pdfium/lib"
  go build ./...
  ```

  Ship `libpdfium.so` and the graphics libraries, with their licences, beside a program that uses the Cairo backend.

The backend follows `GOOS`: CoreGraphics on `darwin`, Cairo on `linux`. On macOS the `rmrender_cairo` build tag selects the Cairo backend
instead, with the same Homebrew packages (`brew install cairo pango fontconfig harfbuzz gdk-pixbuf`) and the `mac-arm64` or `mac-x64` PDFium.
Anywhere else, or without cgo, the package does not build, and says so.

## Installing

```sh
go get github.com/altessa-s/rmrender-go
```

```go
import (
	"github.com/altessa-s/rmrender-go"       // package rmrender
	"github.com/altessa-s/rmrender-go/fonts" // the bundled fonts, 27 MB
)
```

## Drawing a page

```go
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
```

`PageImage` answers an `*image.RGBA` and `PagePNG` the same encoded; the scale is pixels per `.rm` unit, so at 1 a tablet page is 1404 pixels
wide on every paper. The sheet is the `Paper` at the canvas's width: `PaperA4` (the zero value), `PaperLetter` or `PaperTablet`, turned for a
landscape canvas. It grows with ink past the canvas, as `Edge` says: `EdgeRibbon` follows the tablet's notebook export, `EdgeUncut` keeps the
overhanging width of strokes, and `EdgeCentreline` follows centrelines, adding `Margin`. Content is centred across the sheet and starts at its
top. `Background` fills the sheet first; without it the image is transparent around the ink.

A `.rm` file does not name its template: `Template` does, from the bundled ones or your own. Images the page places come from `Images`, an
`fs.FS` they are looked up in by file name; without it they keep their place in the drawing order and paint nothing.

Pages with a text block need `Fonts`, and the tablet writes one on most pages, empty or not: without them such a page fails with
`StatusUnsupported`. `NewFonts` reads the fonts once from an `fs.FS`, here package `fonts`, which embeds them; keep one `Fonts` for the life of
the program and `Close` it at the end.

```go
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
```

`PagePDF` draws the same page as one vector PDF page on the paper, in PDF points.

## Highlights

```go
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
```

`PageMarks` answers what a page has highlighted, without drawing it: the highlighter strokes as `Bands`, each with its colour and the identifier
of its stroke, and the highlighted text of an imported PDF as `Highlights`, each with its words and its band. Boxes are fractions of the page as
`PagePDF` lays it out with the same options, from its top left corner, so they map onto the drawn page at any scale; they are ready for
[rmmarks-go](https://github.com/altessa-s/rmmarks-go), which reads which words a band covers.

## Documents

```go
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
```

`OpenDocument` unpacks a `.rmdoc` archive into memory the `Document` owns, so the bytes may be reused at once, and resolves its page order,
templates, attachments and imported PDF. `Len` is how many pages it draws as: its listed pages, or, for a PDF that lists none, the PDF's pages,
or one blank sheet. Each page is parsed when it is drawn.

`PageImage`, `PagePNG` and `PageMarks` take one page, laid out as the document's PDF lays it out: notebook pages on the paper with their
templates, pages of an imported PDF at the PDF page's size, with that page drawn beneath the marks. `PDF` draws the whole document in one of
three modes, which differ only for an imported PDF:

- `PDFFresh` draws a new PDF. Imported pages come in as vectors and may be reordered or hidden; links and the outline are kept, forms,
  signatures and other annotations are not.
- `PDFPreserving` appends the marks to the imported PDF as an incremental update: its bytes stay as they were, with annotations, forms,
  attachments and signed revisions. A changed page order, count or visibility, encryption and other structures the update cannot keep are
  refused, never redrawn. The appended revision is not signed.
- `PDFRecomposed` writes a new single-revision PDF from the imported one's objects, allowing pages to be reordered, removed or repeated and
  keeping forms and annotations. Existing signatures stop verifying.

A page that does not parse fails the call. What could be recovered from is counted in the `Report`: blocks of a page left out, pages that name
an imported page the PDF does not have, and the links a fresh export could not carry over. `MaximumPages` caps the pages drawn.

## Templates

```go
names, err := fs.Glob(rmrender.BundledTemplates(), "P Lines *.template")
if err != nil {
	log.Fatal(err)
}
fmt.Println(names)
// Templates of one's own, found before the bundled ones by the same name.
mine := rmrender.NewTemplates(os.DirFS("my-templates"))
_ = &rmrender.Options{Templates: mine, Template: "P Lines small"}
```

`BundledTemplates` holds rmrender-lib's page templates, the tablet's templates redrawn as vector models, as `<name>.template` files. A page's
template is looked up by name in the layers given to `NewTemplates`, in order, and then in the bundled ones; a name none of them holds leaves
the page blank behind its ink, and a malformed file fails the call. Nothing is read from disk unless a layer reads it.

## Cancellation and errors

```go
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
```

Every call that draws takes a `context.Context`. A context that has ended stops the call before it begins, and one that ends during it stops the
drawing between draw operations; the call then fails with `StatusCancelled`, unwrapping to the context's cause. A native call already running,
such as decoding one image, is not interrupted.

Every failure is an `*Error` whose `Status` mirrors the C library's, matched with `errors.Is` against the sentinels: `ErrInvalidArgument`,
`ErrOutOfMemory`, `ErrLimit`, `ErrUnsupported`, `ErrBackend` and `ErrCancelled`. Beside the status, it carries the C library's message, the
`Index` of the input it concerns where there is one, and in `Err` what reading a font, template or image failed with. A page that does not
parse is `StatusBackend`, or `StatusUnsupported` for a format generation the parser does not read, as in the document export; an archive that
is not one is `StatusInvalidArgument`. `Limits` bound points, path elements, draw commands, plan, bitmap and PDF bytes, bitmap sides,
coordinates and widths; `DefaultLimits` answers the C library's defaults, and a call over one fails with `StatusLimit`.

## Design

**Values out, handles only where C keeps state.** Images, PDFs, marks and reports are Go values, copied out of C memory and released before
the call returns. `Fonts` and `Document` are handles because the C library keeps the loaded fonts and the unpacked archive between calls; both
must be closed, and neither has a finalizer.

**One call at a time per font set.** rmrender-lib's fonts and the layouts made with them serve one thread at a time, so a `Fonts` holds a lock
for the length of every call that uses it, and `Close` waits for those in flight. Calls with different `Fonts` run in parallel. The calls of one
`Document` take turns the same way. `Templates` are read only and may be shared. PDFium is process-wide; the C library serializes its use.

**Resources through `fs.FS`, never through paths.** Fonts, templates and images reach C through callbacks: a `runtime/cgo.Handle` carried as an
integer, and exported Go functions that read the file and lend its bytes to C, pinned with `runtime.Pinner` until the call returns. A read
failure becomes the error's cause; a panic is recovered before it can unwind through C and raised again once C has returned. The fonts are
loaded from memory, so no temporary files are written, except by the Cairo backend on macOS, where Fontconfig needs files.

**Cancellation without callbacks per draw.** A context is watched with `context.AfterFunc`, which raises an atomic flag in C memory that the
library polls between draw operations.

**One program, four libraries twice.** rmlines-go, rmdoc-go and rmmarks-go compile the same C libraries under their own names, and a program
may import them beside this package. Every external symbol of the vendored C is renamed to `rmrgo_<name>` by `clib_symbols.h`, which
`clib_prelude.h` includes and cgo force-includes into every C file of the package, so the sources stay byte for byte as vendored. A test reads
the test binary's symbol table and fails on any name left unprefixed; `internal/coexist`, a module of its own, links all four packages into one
test binary.

**The library's own pipeline.** A page of a document is laid out as rmrender-lib's document export lays it out, page by page, so the PNG of a
page is the page of the PDF. A lone `.rm` page is laid out the same way, with the edge mode and template it is given.

## Layout

| Path | What is in it |
|---|---|
| `*.go` | The API: `page.go`, `document.go`, `marks.go`, `resources.go` (fonts and templates), `options.go`, `errors.go`, `render.go` (locking and cancellation), `callbacks.go` (the exported callbacks), `memory.go`, `cgo.go` |
| `bridge.c`, `bridge.h` | The C side: the backend chosen at build time behind one set of names, the callbacks into Go, and the page and document pipelines |
| `clib_*.c` | One include each, compiling a vendored C source into the package with its backend's build constraint: cgo builds only the C files of the package directory |
| `clib_prelude.h`, `clib_symbols.h`, `clib_digest.h` | The force-included prelude; generated, the symbol renames and a digest of `internal/clib`, so that a change there rebuilds the package |
| `fonts/` | Package `fonts`: the bundled Noto and EB Garamond fonts with their OFL texts, embedded |
| `internal/clib/` | The vendored libraries, `rmrender/`, `rmlines/`, `rmdoc/` and `rmmarks/`, each with its sources, headers, licence and notices, and the commit of each in `VERSION`; rmrender-lib's templates and `fetch-pdfium.py` |
| `internal/coexist/` | A module of its own that links this package with rmlines-go, rmdoc-go and rmmarks-go |
| `scripts/vendor.py` | Re-vendors the four libraries from their checkouts and regenerates the wrappers and headers |
| `testdata/pages/` | Sample `.rm` pages and a `.rmdoc` built from them, with their licence |

miniz, the deflate codec rmdoc-lib compiles in, is vendored under `internal/clib/rmdoc/miniz/` rather than a `vendor/` directory, because the Go
module zip leaves out files below any directory named `vendor`; the vendoring script points the library's two codec includes there.

```sh
python3 scripts/vendor.py --rmrender ../rmrender-lib --rmlines ../rmlines-lib --rmdoc ../rmdoc-lib \
    --rmmarks ../rmmarks-lib --pdfium /path/to/pdfium
go test -race ./...
GOEXPERIMENT=cgocheck2 go test ./...
go test -run='^$' -fuzz=FuzzPage -fuzztime=30s .
go -C internal/coexist test ./...
```

The script reads the symbols to rename from objects of both backends, so it runs on macOS with the Homebrew Cairo stack and a PDFium SDK.

## See also

- **[rmrender-lib](https://github.com/altessa-s/rmrender-lib)** — the C23 renderer these bindings compile.
- **[rmrender-swift](https://github.com/altessa-s/rmrender-swift)** — the same renderer as a Swift package.
- **[rmlines-go](https://github.com/altessa-s/rmlines-go)** — the `.rm` v6 page format for Go: pages, strokes, typed text and highlights, read and
  written.
- **[rmdoc-go](https://github.com/altessa-s/rmdoc-go)** — the `.rmdoc` container for Go: the archive a document and its pages travel in.
- **[rmmarks-go](https://github.com/altessa-s/rmmarks-go)** — marked text for Go: which words a highlight covers, and how OCR from cropped strips
  returns to the page.

## Acknowledgements

reMarkable does not publish its file formats. This package is built on openly available information and on the research of third-party
developers who documented them.

## License

MIT — see [LICENSE](LICENSE). The vendored C under `internal/clib/` is rmrender-lib, rmlines-lib, rmdoc-lib and rmmarks-lib, each under its own
MIT licence beside it; the miniz codec keeps its [MIT licence](internal/clib/rmdoc/miniz/LICENSE.txt), and generated Unicode tables carry the
Unicode data licence (`internal/clib/*/UNICODE-LICENSE.txt`). The bundled Noto and EB Garamond fonts carry the SIL Open Font License
(`fonts/*-OFL.txt`). The sample pages in `testdata/pages/` are under [their own MIT licence](testdata/pages/LICENSE).

reMarkable is a trademark of reMarkable AS. This project is not affiliated with, endorsed by or sponsored by reMarkable AS.
