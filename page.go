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
	"image/color"
	"unsafe"
)

// parsePage parses the bytes of a .rm page, with o's template named on it.
// The page is the caller's to free.
func parsePage(data []byte, o *Options) (*C.rm_page, error) {
	if len(data) == 0 {
		return nil, errInvalid("empty page")
	}
	var template *C.char
	if o != nil && o.Template != "" {
		t, err := cText(o.Template, "template name")
		if err != nil {
			return nil, err
		}
		defer freeText(t)
		template = t
	}
	var page *C.rm_page
	var cerr C.rmr_error
	status := C.rmrgo_page_parse((*C.uint8_t)(unsafe.Pointer(&data[0])), C.size_t(len(data)), template, &page, &cerr)
	if status != C.RMR_OK {
		return nil, cError(status, &cerr)
	}
	return page, nil
}

// PageImage draws the .rm page data on its paper at scale pixels per .rm
// unit: at 1, an A4 sheet of a tablet page is 1404 pixels wide. The page
// grows with ink past its canvas, as o.Edge says, and the sheet grows with
// it; content is centred across the paper and starts at its top. The image
// is premultiplied RGBA, as an [image.RGBA] always is.
func PageImage(ctx context.Context, data []byte, scale float64, o *Options) (*image.RGBA, error) {
	if err := live(ctx); err != nil {
		return nil, err
	}
	page, err := parsePage(data, o)
	if err != nil {
		return nil, err
	}
	defer C.rm_page_free(page)
	background := cColor(o.background())
	var out C.rmr_bitmap
	err = render(ctx, o, func(r *C.rmrgo_request, cerr *C.rmr_error) C.rmr_status {
		return C.rmrgo_page_bitmap(r, page, C.double(scale), background, &out, cerr)
	})
	if err != nil {
		return nil, err
	}
	return takeBitmap(&out)
}

// PagePNG is [PageImage], encoded as PNG.
func PagePNG(ctx context.Context, data []byte, scale float64, o *Options) ([]byte, error) {
	img, err := PageImage(ctx, data, scale, o)
	if err != nil {
		return nil, err
	}
	return encodePNG(img)
}

// PagePDF draws the .rm page data as a one-page vector PDF on its paper,
// grown as [PageImage] grows it.
func PagePDF(ctx context.Context, data []byte, o *Options) ([]byte, error) {
	if err := live(ctx); err != nil {
		return nil, err
	}
	page, err := parsePage(data, o)
	if err != nil {
		return nil, err
	}
	defer C.rm_page_free(page)
	var out C.rmr_buffer
	err = render(ctx, o, func(r *C.rmrgo_request, cerr *C.rmr_error) C.rmr_status {
		return C.rmrgo_page_pdf(r, page, &out, cerr)
	})
	if err != nil {
		return nil, err
	}
	return takeBuffer(&out)
}

// PageJPEG is [PageImage], flattened onto o.Background, or white without
// one, and encoded as JPEG at quality 1 to 100; 0 means
// [DefaultJPEGQuality].
func PageJPEG(ctx context.Context, data []byte, scale float64, quality int, o *Options) ([]byte, error) {
	if err := checkJPEGQuality(quality); err != nil {
		return nil, err
	}
	img, err := PageImage(ctx, data, scale, o.opaque())
	if err != nil {
		return nil, err
	}
	return encodeJPEG(img, quality)
}

// PageSVG draws the .rm page data as a standalone SVG 1.1 document on the
// sheet [PagePDF] draws it on: width and height in points, and a viewBox
// that spans the sheet. Strokes, highlights and templates stay vectors,
// typed text becomes the outlines of its glyphs, laid out as the PDF lays
// it out, so that the SVG needs no fonts, and images keep their PNG or JPEG
// bytes. o.Background is not drawn: the sheet is transparent around the ink.
// The output is deterministic, and refers to nothing outside itself.
func PageSVG(ctx context.Context, data []byte, o *Options) ([]byte, error) {
	if err := live(ctx); err != nil {
		return nil, err
	}
	page, err := parsePage(data, o)
	if err != nil {
		return nil, err
	}
	defer C.rm_page_free(page)
	var out C.rmr_buffer
	err = render(ctx, o, func(r *C.rmrgo_request, cerr *C.rmr_error) C.rmr_status {
		return C.rmrgo_page_svg(r, page, &out, cerr)
	})
	if err != nil {
		return nil, err
	}
	return takeBuffer(&out)
}

// PageMarks answers the highlights of the .rm page data: the text each one
// covers and the bands it is drawn as, in boxes that are fractions of the
// page [PagePDF] draws with the same options. id names the page in the
// result, and may be empty.
func PageMarks(data []byte, id string, o *Options) (*Marks, error) {
	page, err := parsePage(data, o)
	if err != nil {
		return nil, err
	}
	defer C.rm_page_free(page)
	var out *C.rmr_page_marks
	err = render(context.Background(), o, func(r *C.rmrgo_request, cerr *C.rmr_error) C.rmr_status {
		return C.rmrgo_page_marks(r, page, (*C.char)(unsafe.Pointer(unsafe.StringData(id))), C.size_t(len(id)), &out, cerr)
	})
	if err != nil {
		return nil, err
	}
	defer C.rmr_page_marks_free(out)
	return marksOf(C.rmr_page_marks_view(out))
}

// background answers the fill of a bitmap, nil for none.
func (o *Options) background() color.Color {
	if o == nil {
		return nil
	}
	return o.Background
}

// opaque answers o for a JPEG: a copy with a white background unless it
// names one.
func (o *Options) opaque() *Options {
	var c Options
	if o != nil {
		c = *o
	}
	if c.Background == nil {
		c.Background = color.White
	}
	return &c
}
