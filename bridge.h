// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

// The C side of the bindings, declared for the Go files and defined in
// bridge.c: the backend chosen at build time behind one set of names, the
// callbacks that reach Go through a runtime/cgo.Handle carried as an integer,
// and the page and document pipelines composed from the library's calls.
// Declarations only, because Go files that export to C include it.

#ifndef RMRENDER_GO_BRIDGE_H
#define RMRENDER_GO_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include <rmdoc/rmdoc.h>
#include <rmlines/parser.h>
#include <rmrender/archive.h>
#include <rmrender/marks.h>
#include <rmrender/pdf.h>

#if defined(RMRGO_APPLE)
#include <rmrender/apple_document.h>
#include <rmrender/apple_marks.h>
typedef rmr_apple_fonts rmrgo_fonts;
#elif defined(RMRGO_CAIRO)
#include <rmrender/linux_document.h>
#include <rmrender/linux_marks.h>
typedef rmr_linux_fonts rmrgo_fonts;
#else
#error "rmrender-go builds on darwin (CoreGraphics or Cairo) and linux (Cairo)"
#endif

// A cancellation flag in C memory, raised from Go when a context ends and
// polled by the library between draw operations.
void *rmrgo_flag_new(void);
void rmrgo_flag_raise(void *flag);
void rmrgo_flag_free(void *flag);

// Fonts resolved by name through the Go callback of handle, during this
// call only.
rmr_status rmrgo_fonts_create(uintptr_t handle, const rmr_limits *limits, rmrgo_fonts **out,
                              rmr_error *error);
void rmrgo_fonts_free(rmrgo_fonts *fonts);

// What one render call is given. handle reaches the Go callbacks for
// templates and, for a lone page, images; cancel is a flag or null.
typedef struct {
    const rmrgo_fonts *fonts;
    uintptr_t handle;
    rmr_paper paper;
    rmr_ink_edge edge;
    double margin;
    rmr_limits limits;
    void *cancel;
} rmrgo_request;

// A parsed .rm page, with the template name set when one is given.
rmr_status rmrgo_page_parse(const uint8_t *bytes, size_t size, const char *template_name,
                            rm_page **out, rmr_error *error);

// A lone page drawn to a bitmap at scale pixels per .rm unit, to a one-page
// PDF, or read for its highlights.
rmr_status rmrgo_page_bitmap(const rmrgo_request *request, const rm_page *page, double scale,
                             rmr_color background, rmr_bitmap *out, rmr_error *error);
rmr_status rmrgo_page_pdf(const rmrgo_request *request, const rm_page *page, rmr_buffer *out,
                          rmr_error *error);
rmr_status rmrgo_page_marks(const rmrgo_request *request, const rm_page *page, const char *id,
                            size_t id_length, rmr_page_marks **out, rmr_error *error);

// An unpacked .rmdoc archive and the document chosen in it.
typedef struct rmrgo_document rmrgo_document;

rmr_status rmrgo_document_open(const uint8_t *bytes, size_t size, const char *id, size_t id_length,
                               bool include_unlisted, const rmr_limits *limits,
                               rmrgo_document **out, rmr_error *error);
void rmrgo_document_free(rmrgo_document *document);
const char *rmrgo_document_id(const rmrgo_document *document);
size_t rmrgo_document_count(const rmrgo_document *document);
size_t rmrgo_document_listed(const rmrgo_document *document);
const rmr_document_page *rmrgo_document_page(const rmrgo_document *document, size_t index);
size_t rmrgo_document_source_pages(const rmrgo_document *document);

rmr_status rmrgo_document_bitmap(const rmrgo_request *request, rmrgo_document *document,
                                 size_t index, double scale, rmr_color background,
                                 rmr_bitmap *out, rmr_error *error);
rmr_status rmrgo_document_marks(const rmrgo_request *request, rmrgo_document *document,
                                size_t index, rmr_page_marks **out, rmr_error *error);

// The three document exports, as the library names them: fresh, preserving
// (an incremental update) and recomposed.
#define RMRGO_PDF_FRESH 0
#define RMRGO_PDF_PRESERVING 1
#define RMRGO_PDF_RECOMPOSED 2

rmr_status rmrgo_document_pdf(const rmrgo_request *request, rmrgo_document *document,
                              uint32_t mode, size_t maximum_pages, rmr_buffer *out,
                              rmr_document_report *report, rmr_error *error);

#endif
