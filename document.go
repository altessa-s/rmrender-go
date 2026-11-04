// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender

/*
#include "bridge.h"
*/
import "C"

import (
	"context"
	"image"
	"sync"
	"unsafe"
)

// DocumentOptions says how a .rmdoc archive is opened.
type DocumentOptions struct {
	// ID names the document to open, for an archive that holds more than
	// one. Empty means the first document with a .content file, or else the
	// first document at all.
	ID string
	// IncludeUnlisted recovers the .rm files of a document whose .content
	// lists no pages, in name order. Deleted pages are never recovered.
	IncludeUnlisted bool
	// Limits bound opening; nil means [DefaultLimits].
	Limits *Limits
}

// PageInfo describes one listed page of a document.
type PageInfo struct {
	// ID is the page's identifier, the name of its .rm file.
	ID string
	// Template names the template the page is drawn on, empty for none.
	Template string
	// SourcePage is the page of the imported PDF drawn beneath, -1 for none
	// or for one the PDF does not have.
	SourcePage int
}

// Report counts what a document export did, and what it could not keep.
type Report struct {
	// DocumentPages is how many pages the document has, and RenderedPages
	// how many the PDF holds.
	DocumentPages, RenderedPages int
	// SkippedBlocks is how many blocks of the pages did not parse and were
	// left out.
	SkippedBlocks int
	// RefusedSources is how many pages name an imported PDF page the PDF
	// does not have.
	RefusedSources int
	// LinksPreserved and LinksDropped count the links of an imported PDF
	// a fresh export carried over and could not. The other modes keep the
	// original annotations whole and count none.
	LinksPreserved, LinksDropped int
}

// Document is a .rmdoc archive opened for drawing: its pages in order, their
// templates, attachments and imported PDF.
//
// A Document holds its archive in C memory until it is closed; it has no
// finalizer. Its calls take turns.
type Document struct {
	mu    sync.Mutex
	c     *C.rmrgo_document
	id    string
	pages []PageInfo
	count int
}

// OpenDocument unpacks the .rmdoc archive data and opens the document in it.
// data is read during the call only. Each page is parsed when it is drawn.
func OpenDocument(data []byte, o *DocumentOptions) (*Document, error) {
	if o == nil {
		o = &DocumentOptions{}
	}
	if len(data) == 0 {
		return nil, errInvalid("empty archive")
	}
	limits, err := cLimits(o.Limits)
	if err != nil {
		return nil, err
	}
	var id *C.char
	if o.ID != "" {
		id, err = cText(o.ID, "document ID")
		if err != nil {
			return nil, err
		}
		defer freeText(id)
	}
	var out *C.rmrgo_document
	var cerr C.rmr_error
	status := C.rmrgo_document_open((*C.uint8_t)(unsafe.Pointer(&data[0])), C.size_t(len(data)),
		id, C.size_t(len(o.ID)), C.bool(o.IncludeUnlisted), &limits, &out, &cerr)
	if status != C.RMR_OK {
		return nil, cError(status, &cerr)
	}
	d := &Document{c: out, id: C.GoString(C.rmrgo_document_id(out))}
	if d.count, err = goLength(C.rmrgo_document_count(out), "page count"); err != nil {
		C.rmrgo_document_free(out)
		return nil, err
	}
	listed, err := goLength(C.rmrgo_document_listed(out), "page count")
	if err != nil {
		C.rmrgo_document_free(out)
		return nil, err
	}
	sources := uint64(C.rmrgo_document_source_pages(out))
	d.pages = make([]PageInfo, listed)
	for i := range d.pages {
		p := C.rmrgo_document_page(out, C.size_t(i))
		info := PageInfo{SourcePage: -1}
		if p.id != nil {
			info.ID = C.GoString(p.id)
		}
		if p.template_name != nil {
			info.Template = C.GoString(p.template_name)
		}
		if p.has_source && uint64(p.source_page) < sources && uint64(p.source_page) <= maxInt {
			info.SourcePage = int(p.source_page)
		}
		d.pages[i] = info
	}
	return d, nil
}

// ID answers the identifier of the open document.
func (d *Document) ID() string { return d.id }

// Len answers how many pages the document draws as: its listed pages, or
// the pages of its imported PDF when it lists none, or one blank sheet.
func (d *Document) Len() int { return d.count }

// Pages answers the listed pages, in order; none for a document that lists
// none.
func (d *Document) Pages() []PageInfo {
	return append([]PageInfo(nil), d.pages...)
}

// Close releases the document's C memory, after any call in flight. A closed
// Document makes a call fail with StatusInvalidArgument; closing it again
// does nothing.
func (d *Document) Close() error {
	d.mu.Lock()
	defer d.mu.Unlock()
	C.rmrgo_document_free(d.c)
	d.c = nil
	return nil
}

// use runs f with the document locked and open.
func (d *Document) use(f func(c *C.rmrgo_document) error) error {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.c == nil {
		return errInvalid("document is closed")
	}
	return f(d.c)
}

// index answers page i for C, refusing one out of range.
func (d *Document) index(i int) (C.size_t, error) {
	if i < 0 || i >= d.count {
		return 0, errInvalid("page index out of range")
	}
	return C.size_t(i), nil
}

// PageImage draws page i as the document's PDF lays it out, at scale pixels
// per .rm unit, with its template, attachments and, beneath, its page of the
// imported PDF.
func (d *Document) PageImage(ctx context.Context, i int, scale float64, o *Options) (*image.RGBA, error) {
	index, err := d.index(i)
	if err != nil {
		return nil, err
	}
	background := cColor(o.background())
	var out C.rmr_bitmap
	err = d.use(func(c *C.rmrgo_document) error {
		return render(ctx, o, func(r *C.rmrgo_request, cerr *C.rmr_error) C.rmr_status {
			return C.rmrgo_document_bitmap(r, c, index, C.double(scale), background, &out, cerr)
		})
	})
	if err != nil {
		return nil, err
	}
	return takeBitmap(&out)
}

// PagePNG is [Document.PageImage], encoded as PNG.
func (d *Document) PagePNG(ctx context.Context, i int, scale float64, o *Options) ([]byte, error) {
	img, err := d.PageImage(ctx, i, scale, o)
	if err != nil {
		return nil, err
	}
	return encodePNG(img)
}

// PageJPEG is [Document.PageImage], flattened onto o.Background, or white
// without one, and encoded as JPEG at quality 1 to 100; 0 means
// [DefaultJPEGQuality].
func (d *Document) PageJPEG(ctx context.Context, i int, scale float64, quality int, o *Options) ([]byte, error) {
	if err := checkJPEGQuality(quality); err != nil {
		return nil, err
	}
	img, err := d.PageImage(ctx, i, scale, o.opaque())
	if err != nil {
		return nil, err
	}
	return encodeJPEG(img, quality)
}

// PageSVG draws page i as a standalone SVG document, the page [Document.SVG]
// holds at i: on the sheet of the document's PDF, with typed text as glyph
// outlines and, beneath the marks, its page of the imported PDF rasterized
// at o.PDFRasterDPI and embedded as a PNG.
func (d *Document) PageSVG(ctx context.Context, i int, o *Options) ([]byte, error) {
	index, err := d.index(i)
	if err != nil {
		return nil, err
	}
	var out C.rmr_buffer
	err = d.use(func(c *C.rmrgo_document) error {
		return render(ctx, o, func(r *C.rmrgo_request, cerr *C.rmr_error) C.rmr_status {
			return C.rmrgo_document_page_svg(r, c, index, &out, cerr)
		})
	})
	if err != nil {
		return nil, err
	}
	return takeBuffer(&out)
}

// SVG draws the whole document as SVG, one standalone document per page,
// in order, as [Document.PageSVG] draws each: notebook pages on o.Paper,
// imported pages at their own size with the PDF page rasterized beneath the
// marks. Crop boxes, links and the outline of an imported PDF are not kept.
// o.MaximumPages caps the pages, and o.Limits.MaximumPDFBytes bounds the
// bytes of all of them together. What was recovered from is counted in the
// report, as for [Document.PDF].
func (d *Document) SVG(ctx context.Context, o *Options) ([][]byte, Report, error) {
	maximum, err := o.maximumPages()
	if err != nil {
		return nil, Report{}, err
	}
	var out C.rmr_svg_pages
	var cr C.rmr_document_report
	err = d.use(func(c *C.rmrgo_document) error {
		return render(ctx, o, func(r *C.rmrgo_request, cerr *C.rmr_error) C.rmr_status {
			return C.rmrgo_document_svg(r, c, C.size_t(maximum), &out, &cr, cerr)
		})
	})
	if err != nil {
		return nil, Report{}, err
	}
	pages, err := takeSVGPages(&out)
	if err != nil {
		return nil, Report{}, err
	}
	report, err := reportOf(&cr)
	if err != nil {
		return nil, Report{}, err
	}
	return pages, report, nil
}

// PageMarks answers the highlights of page i, in boxes that are fractions of
// the page as the document's PDF lays it out. The page's ID names it.
func (d *Document) PageMarks(i int, o *Options) (*Marks, error) {
	index, err := d.index(i)
	if err != nil {
		return nil, err
	}
	var out *C.rmr_page_marks
	err = d.use(func(c *C.rmrgo_document) error {
		return render(context.Background(), o, func(r *C.rmrgo_request, cerr *C.rmr_error) C.rmr_status {
			return C.rmrgo_document_marks(r, c, index, &out, cerr)
		})
	})
	if err != nil {
		return nil, err
	}
	defer C.rmr_page_marks_free(out)
	return marksOf(C.rmr_page_marks_view(out))
}

// PDF draws the whole document as one PDF, in the given mode: notebook pages
// on o.Paper, imported pages at their own size. Pages that fail to parse
// fail the call; what was recovered from is counted in the report.
func (d *Document) PDF(ctx context.Context, mode PDFMode, o *Options) ([]byte, Report, error) {
	var report Report
	var cmode C.uint32_t
	switch mode {
	case PDFFresh:
		cmode = C.RMRGO_PDF_FRESH
	case PDFPreserving:
		cmode = C.RMRGO_PDF_PRESERVING
	case PDFRecomposed:
		cmode = C.RMRGO_PDF_RECOMPOSED
	default:
		return nil, report, errInvalid("unknown PDF mode")
	}
	maximum, err := o.maximumPages()
	if err != nil {
		return nil, report, err
	}
	var out C.rmr_buffer
	var cr C.rmr_document_report
	err = d.use(func(c *C.rmrgo_document) error {
		return render(ctx, o, func(r *C.rmrgo_request, cerr *C.rmr_error) C.rmr_status {
			return C.rmrgo_document_pdf(r, c, cmode, C.size_t(maximum), &out, &cr, cerr)
		})
	})
	if err != nil {
		return nil, report, err
	}
	pdf, err := takeBuffer(&out)
	if err != nil {
		return nil, report, err
	}
	if report, err = reportOf(&cr); err != nil {
		return nil, Report{}, err
	}
	return pdf, report, nil
}

// maximumPages answers the page cap of a document export, zero for all.
func (o *Options) maximumPages() (int, error) {
	if o == nil {
		return 0, nil
	}
	if o.MaximumPages < 0 {
		return 0, errInvalid("negative page maximum")
	}
	return o.MaximumPages, nil
}

// reportOf answers the C library's report of a document export.
func reportOf(cr *C.rmr_document_report) (Report, error) {
	var report Report
	for _, f := range []struct {
		to   *int
		from C.size_t
	}{
		{&report.DocumentPages, cr.document_pages},
		{&report.RenderedPages, cr.rendered_pages},
		{&report.SkippedBlocks, cr.skipped_blocks},
		{&report.RefusedSources, cr.refused_sources},
		{&report.LinksPreserved, cr.pdf.links_preserved},
		{&report.LinksDropped, cr.pdf.links_dropped},
	} {
		var err error
		if *f.to, err = goLength(f.from, "report count"); err != nil {
			return Report{}, err
		}
	}
	return report, nil
}
