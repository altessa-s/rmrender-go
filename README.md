# rmrender-go

reMarkable pages and documents, drawn from Go: a `.rm` page to a PNG, a JPEG, a one-page PDF or an SVG, a `.rmdoc` document page by page or
whole to a PDF or to SVG, with handwriting and its pen textures, typed text, highlights, images and page templates; and the highlights of a page,
read as Go values.

Idiomatic cgo bindings over **[rmrender-lib](https://github.com/altessa-s/rmrender-lib)**, the portable C23 renderer, whose sources are
vendored into this module together with the libraries it reads its input with: **[rmlines-lib](https://github.com/altessa-s/rmlines-lib)**
for `.rm` pages, **[rmdoc-lib](https://github.com/altessa-s/rmdoc-lib)** for `.rmdoc` archives and
**[rmmarks-lib](https://github.com/altessa-s/rmmarks-lib)** for highlights. Drawing is native: CoreGraphics and CoreText on macOS, Cairo, Pango
and PDFium on Linux and Windows. The same renderer is available for Swift as **[rmrender-swift](https://github.com/altessa-s/rmrender-swift)**.

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
- **Windows** (x64 and ARM64): the Cairo backend, built as rmrender-lib builds it on Windows, with the [MSYS2](https://www.msys2.org)
  **CLANG64** toolchain on x64 or **CLANGARM64** on ARM64: its clang is the C23 compiler, and its pkg-config finds the Cairo stack. On ARM64
  every `mingw-w64-clang-x86_64-` package below is `mingw-w64-clang-aarch64-`, PDFium is the `win-arm64` SDK, and Go's race detector is not
  available. In a CLANG64 shell:

  ```sh
  pacman -S mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-pkgconf mingw-w64-clang-x86_64-python \
      mingw-w64-clang-x86_64-cairo mingw-w64-clang-x86_64-pango mingw-w64-clang-x86_64-fontconfig \
      mingw-w64-clang-x86_64-harfbuzz mingw-w64-clang-x86_64-gdk-pixbuf2 mingw-w64-clang-x86_64-zlib
  ```

  The official Go for Windows works with it (or MSYS2's `mingw-w64-clang-x86_64-go`), with `CC=clang` and CLANG64's `bin` ahead on `PATH`.
  PDFium comes from the same script, as the `win-x64` SDK: `include/`, the import library `lib/pdfium.dll.lib` (the package links it with
  `-lpdfium.dll`) and `bin/pdfium.dll`. Give cgo the paths in Windows form, as `cygpath -m` writes them:

  ```sh
  module=$(go list -m -f '{{.Dir}}' github.com/altessa-s/rmrender-go)
  pdfium="$(cygpath -m "$HOME")/pdfium"
  python "$module/internal/clib/rmrender/scripts/fetch-pdfium.py" --platform win-x64 --output "$pdfium"
  export CC=clang CGO_ENABLED=1
  export CGO_CFLAGS="-I$pdfium/include"
  export CGO_LDFLAGS="-L$pdfium/lib"
  export PATH="$(cygpath -u "$pdfium")/bin:$PATH"   # pdfium.dll, for go test and go run
  go build ./...
  ```

  The vendored C is compiled in statically, as rmrender-lib's static build is: none of the libraries' DLL macros (`RMRENDER_SHARED`,
  `RMRENDER_BUILDING` and the like) is defined, and the package refuses them. A program built this way loads DLLs when it runs: it imports
  `pdfium.dll` and CLANG64's `libcairo-2.dll`, `libpango-1.0-0.dll`, `libpangocairo-1.0-0.dll`, `libpangoft2-1.0-0.dll`,
  `libfontconfig-1.dll`, `libharfbuzz-0.dll`, `libglib-2.0-0.dll`, `libgobject-2.0-0.dll`, `libgdk_pixbuf-2.0-0.dll` and `zlib1.dll`, and
  those load more in turn (FreeType, libpng, pixman, FriBidi, the C++ and unwind runtimes and others). Ship them all beside the `.exe`, with
  their licences: `ntldd -R program.exe` (`pacman -S mingw-w64-clang-x86_64-ntldd`) lists the full set, and every DLL it resolves under
  CLANG64's `bin` or the PDFium SDK is one to ship.
  Fonts reach Fontconfig as files: the first use writes each bundled font once to `%LOCALAPPDATA%\rmrender\fonts` (under `XDG_DATA_HOME`
  when that is set), named by its digest, and later font sets reuse the files. Paths are UTF-8; a cache directory whose path is not ASCII is
  reached through its 8.3 alias, so on a volume without 8.3 names it needs an ASCII path.

The backend follows `GOOS`: CoreGraphics on `darwin`, Cairo on `linux` and `windows`. On macOS the `rmrender_cairo` build tag selects the Cairo
backend instead, with the same Homebrew packages (`brew install cairo pango fontconfig harfbuzz gdk-pixbuf`) and the `mac-arm64` or `mac-x64`
PDFium. Anywhere else, or without cgo, the package does not build, and says so.

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
out, err := rmrender.PageJPEG(context.Background(), page, 0.5, 85, &rmrender.Options{Fonts: f, Template: "P Dots S"})
if err != nil {
	fmt.Println(err)
	return
}
config, err := jpeg.DecodeConfig(bytes.NewReader(out))
if err != nil {
	fmt.Println(err)
	return
}
fmt.Println(config.Width, config.Height)
```

`PageJPEG` is `PageImage` encoded as JPEG, at a quality from 1 to 100, or 0 for `DefaultJPEGQuality`, 75. JPEG has no transparency: the page is
drawn on `Background`, white without one, and what a translucent background leaves is flattened onto white. `Document.PageJPEG` does the same for
a page of a document.

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

What could be recovered from is counted in the `Report`: blocks of a page left out, pages that name an imported page the PDF does not have,
the links a fresh export could not carry over, and unreadable pages. `MaximumPages` caps the pages drawn.

An **unreadable page** is one whose `.rm` file is in the archive but does not parse: truncated, malformed, or of a version the parser does not
read. (A listed page without a `.rm` file is not one: the tablet writes none for a page nobody wrote on, and it is drawn as an empty page on its
template.) `Options.Unreadable` says what every call of a `Document` does with it:

| `Unreadable` | Notebook page | Page over an imported PDF | `Report.UnreadablePages` |
|---|---|---|---|
| `UnreadableStrict` (zero value) | the call fails with an `*Error` naming the page, its cause an `*UnreadablePageError` | the same | — |
| `UnreadableBlank` | an empty page on its template, as a page without a `.rm` file | the PDF page without marks | counted |
| `UnreadablePlaceholder` | a plain sheet of the page's size, no template, "Rendering error" in the middle | the PDF page with the label over it | counted |

The label is dark grey on a light rounded box, its type a twentieth of the page's width, set in Noto Sans, the face typed text is set in, so a
PDF embeds it as it embeds typed text; it needs `Fonts`. Bitmaps and SVG carry it too, which a text recogniser reads. `CheckPage` answers
whether one page parses, with the error a strict drawing of it fails with. A page that cannot be parsed for want of memory or past a `Limits`
bound fails the call whatever the option says.

`WritePDF` writes the same file to an `io.Writer` as it is produced instead of answering it whole, so a long document's PDF need not fit in
memory twice, or at all: the bytes are those of `PDF`, handed to the writer in order in pieces of at most 4 MiB, on a separate goroutine synchronously with the export. A
fresh export writes each page as it is finished; a preserving one writes the imported PDF and then the update, and a recomposed one its
final rewrite, as they go. `Limits.MaximumPDFBytes` still bounds the file, 256 MiB by default: raise it for larger ones. The writer's
failure stops the export with `StatusBackend` and the writer's error as its cause; on any failure the writer has received part of a file,
so write to a temporary file and rename it once `WritePDF` succeeds. A panic or `runtime.Goexit` in the writer stops C and is raised again
on the calling goroutine after cleanup, running its deferred functions. The worker does not inherit goroutine-bound state such as
`runtime.LockOSThread`.

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
dir, err := os.MkdirTemp("", "notes")
if err != nil {
	fmt.Println(err)
	return
}
defer os.RemoveAll(dir)
// The file is written as it is produced, into a temporary file renamed
// once it is whole.
out, err := os.CreateTemp(dir, "notes-*.pdf")
if err != nil {
	fmt.Println(err)
	return
}
opts := &rmrender.Options{Fonts: f}
report, err := doc.WritePDF(context.Background(), out, rmrender.PDFFresh, opts)
if closeErr := out.Close(); err == nil {
	err = closeErr
}
pdf := filepath.Join(dir, "notes.pdf")
if err == nil {
	err = os.Rename(out.Name(), pdf)
}
if err != nil {
	fmt.Println(err)
	return
}
head := make([]byte, 5)
if file, err := os.Open(pdf); err == nil {
	_, _ = io.ReadFull(file, head)
	file.Close()
}
fmt.Println(string(head), report.RenderedPages, "pages")
```

`PDFGrain` says how `PDF` and `PagePDF` draw grained ink, pencils, brushes and ballpoints, whose grain is a stencil clipped to the stroke.
`PDFGrainMask`, the zero value, draws opaque ink's stencil straight onto the page; `PDFGrainGroup` wraps each grained stroke in a transparency
group of its own, as the Cairo backend did up to 1.4.1, which doubles the PDF's size and draws the edges lighter in most viewers. Translucent
ink is grouped either way. Only the Cairo backend tells them apart: CoreGraphics always draws the mask way, and its output is the same for both.

## SVG

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
svg, err := rmrender.PageSVG(context.Background(), page, &rmrender.Options{Fonts: f, Paper: rmrender.PaperLetter})
if err != nil {
	fmt.Println(err)
	return
}
var root struct {
	Width   string `xml:"width,attr"`
	Height  string `xml:"height,attr"`
	ViewBox string `xml:"viewBox,attr"`
}
if err := xml.Unmarshal(svg, &root); err != nil {
	fmt.Println(err)
	return
}
fmt.Println(root.Width, root.Height, root.ViewBox)
```

`PageSVG` draws a page as a standalone SVG 1.1 document on the sheet `PagePDF` draws it on: `width` and `height` in points and a `viewBox`
that spans the sheet, which clips it. Strokes, highlights with their blend modes, erasers and templates stay vectors, pencil and brush grain
is a mask holding its one-bit raster as an embedded PNG, and images keep their PNG or JPEG bytes as `data:` URIs. Typed text becomes the filled
outlines of its glyphs, laid out with the backend's fonts as the PDF lays it out, so the SVG needs no fonts; it is not selectable. `Background`
is not drawn. The output is deterministic and refers to nothing outside itself.

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
opts := &rmrender.Options{Fonts: f, PDFRasterDPI: 200}
pages, report, err := doc.SVG(context.Background(), opts)
if err != nil {
	fmt.Println(err)
	return
}
for i, svg := range pages {
	fmt.Println(i, string(svg[:38]))
}
fmt.Println(report.RenderedPages, "of", report.DocumentPages, "pages")
```

`Document.SVG` draws a whole document, one SVG document per page in order, on the sheets of its PDF, with a `Report` as `PDF` gives one, and
`Document.PageSVG` one of those pages alone. An imported PDF page cannot stay vector in SVG: it is rasterized beneath the marks at
`PDFRasterDPI` pixels per inch of the sheet, 150 when it is zero, and embedded as a PNG; its crop box, links and outline are not kept.
`MaximumPages` caps the pages, and `Limits.MaximumPDFBytes` bounds the bytes of all of them together.

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
the page blank behind its ink, and a malformed file fails the call. Nothing is read from disk unless a layer reads it. `NoTemplates` holds no
name at all: every page, of a lone `.rm` or of a document, is drawn with its ink and imported PDF pages only.

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
parse is `StatusBackend`, or `StatusUnsupported` for a format generation the parser does not read, as in the document export, and a page of a
`Document` that does not parse has an `*UnreadablePageError` for its cause and its place for `Index`; an archive that
is not one is `StatusInvalidArgument`. `Limits` bound points, path elements, draw commands, plan, bitmap and PDF bytes, bitmap sides,
coordinates and widths, and the PDF byte limit bounds SVG output too; `DefaultLimits` answers the C library's defaults, and a call over one
fails with `StatusLimit`.

## Design

**Values out, handles only where C keeps state.** Images, PDFs, SVG documents, marks and reports are Go values, copied out of C memory and
released before the call returns. `Fonts` and `Document` are handles because the C library keeps the loaded fonts and the unpacked archive
between calls; both must be closed, and neither has a finalizer.

**One call at a time per font set.** rmrender-lib's fonts and the layouts made with them serve one thread at a time, so a `Fonts` holds a lock
for the length of every call that uses it, and `Close` waits for those in flight. Calls with different `Fonts` run in parallel. The calls of one
`Document` take turns the same way. `Templates` are read only and may be shared. PDFium is process-wide; the C library serializes its use.

**Resources through `fs.FS`, never through paths.** Fonts, templates and images reach C through callbacks: a `runtime/cgo.Handle` carried as an
integer, and exported Go functions that read the file and lend its bytes to C, pinned with `runtime.Pinner` until the call returns. A read
failure becomes the error's cause. Reads run on a separate goroutine, synchronously with the call. A panic or `runtime.Goexit` stops C;
after C returns, the handle is deleted, lent bytes dropped and pins released, then the calling goroutine panics or exits, running its
deferred functions. One worker is reused for the reads and writer pieces of each call. The fonts are
loaded from memory, so no temporary files are written, except by the Cairo backend on macOS, where Fontconfig needs files, and on Windows,
where it keeps them in the font cache the requirements describe.

**Cancellation without callbacks per draw.** A context is watched with `context.AfterFunc`, which raises an atomic flag in C memory that the
library polls between draw operations.

**One program, four libraries twice.** rmlines-go, rmdoc-go and rmmarks-go compile the same C libraries under their own names, and a program
may import them beside this package. Every external symbol of the vendored C is renamed to `rmrgo_<name>` by `clib_symbols.h`, which
`clib_prelude.h` includes and cgo force-includes into every C file of the package, so the sources stay byte for byte as vendored. A test reads
the test binary's symbol table and fails on any name left unprefixed; `internal/coexist`, a module of its own, links all four packages into one
test binary.

**The library's own pipeline.** A page of a document is laid out as rmrender-lib's document export lays it out, page by page, so the PNG of a
page is the page of the PDF. A lone `.rm` page is laid out the same way, with the edge mode and template it is given. SVG output takes the
library's own page and document SVG pipelines, so a page of `Document.SVG`, the same `Document.PageSVG` and the lone page's `PageSVG` are the
same bytes.

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

The script reads the PDFium headers of the SDK rmrender-lib pins: it fetches that SDK once, checksum-verified, into the user's cache
directory, and `--pdfium` names one already on disk.

miniz, the deflate codec rmdoc-lib compiles in, is vendored under `internal/clib/rmdoc/miniz/` rather than a `vendor/` directory, because the Go
module zip leaves out files below any directory named `vendor`; the vendoring script points the library's two codec includes there.

```sh
python3 scripts/vendor.py --rmrender ../rmrender-lib --rmlines ../rmlines-lib --rmdoc ../rmdoc-lib \
    --rmmarks ../rmmarks-lib
go test -race ./...
GOEXPERIMENT=cgocheck2 go test ./...
go test -run='^$' -fuzz=FuzzPage -fuzztime=30s .
go -C internal/coexist test ./...
```

The script reads the symbols to rename from objects of both backends, so it runs on macOS with the Homebrew Cairo stack and a PDFium SDK.
Windows compiles the Cairo backend's sources, whose `_WIN32` branches define no other external symbol; the symbol test reads the Windows test
binary's COFF table as well.

## Bindings

- **[rmrender-lib](https://github.com/altessa-s/rmrender-lib)** — the C23 renderer these bindings compile.
- **[rmrender-swift](https://github.com/altessa-s/rmrender-swift)** — the same renderer as a Swift package.

## See also

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
