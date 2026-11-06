/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_APPLE_PDF_H
#define RMR_APPLE_PDF_H
#include "apple.h"
#include "pdf.h"

typedef struct rmr_apple_pdf_source rmr_apple_pdf_source;

/* Opens bytes with CoreGraphics; bytes are copied. Encrypted/locked documents
 * and a non-unit page UserUnit return UNSUPPORTED; too many pages returns
 * LIMIT. */
[[nodiscard]] RMR_API rmr_status rmr_apple_pdf_source_create(
    const uint8_t* bytes, size_t size, const rmr_limits* limits,
    rmr_apple_pdf_source** out, rmr_error* error);

/* Releases source and its CoreGraphics objects; a null pointer is a no-op. */
RMR_API void rmr_apple_pdf_source_free(rmr_apple_pdf_source* source);

/* Page count; null source returns 0. */
RMR_API size_t rmr_apple_pdf_source_count(const rmr_apple_pdf_source* source);

/* Displayed media-box size in PDF points, after source rotation. */
[[nodiscard]] RMR_API rmr_status
rmr_apple_pdf_source_size(const rmr_apple_pdf_source* source, size_t page,
                          rmr_point* out, rmr_error* error);

/* Rebuild an ordered PDF with vector source pages and mark plans. Retains URI
 * and supported local links plus remapped outlines. This is a fresh PDF, not
 * incremental editing: forms, signatures and arbitrary annotations are not
 * retained. Link refusals are counted. No file I/O; transactional bounded
 * output. Annotations and intentional source crops follow source transforms,
 * including rotation and reordered pages. Hidden source pages do not impose
 * their crop. */
[[nodiscard]] RMR_API rmr_status rmr_apple_pdf_overlay(
    const rmr_apple_pdf_source* source, const rmr_pdf_page* pages, size_t count,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* context,
    rmr_buffer* out, rmr_pdf_report* report, rmr_error* error);

/* Strict append-only plan overlay. Exactly one visible ordered source page per
 * input page; insertion, removal and reorder return UNSUPPORTED. Rotation is
 * handled without moving original objects. Boxes expand only for overflowing
 * marks. An intentional source CropBox remains even when MediaBox expands. See
 * pdf.h for supported structures and signature semantics. No silent redraw
 * fallback. */
[[nodiscard]] RMR_API rmr_status rmr_apple_pdf_incremental(
    const rmr_apple_pdf_source* source, const rmr_pdf_page* pages, size_t count,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* context,
    rmr_buffer* out, rmr_pdf_incremental_report* report, rmr_error* error);

/* Fresh export preserving interactive objects with reordered, removed or
 * duplicated source pages. Keeps each source page's native coordinates and
 * rotation, applying marks through the same mapping as incremental export.
 * Copies have separate widgets sharing the original field value. All pages
 * must have a visible source; notebooks without a source render normally.
 * Insertion/hidden-source pages and XFA return UNSUPPORTED. Signature bytes
 * survive but cryptographic validity does not. See rmr_pdf_select_pages. */
[[nodiscard]] RMR_API rmr_status rmr_apple_pdf_recompose(
    const rmr_apple_pdf_source* source, const rmr_pdf_page* pages, size_t count,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* context,
    rmr_buffer* out, rmr_error* error);

/* rmr_apple_pdf_overlay, _incremental and _recompose drawing the marks as
 * options say (see rmr_pdf_options in output.h; null selects the defaults,
 * which the functions without options use). */
[[nodiscard]] RMR_API rmr_status rmr_apple_pdf_overlay_with_options(
    const rmr_apple_pdf_source* source, const rmr_pdf_page* pages, size_t count,
    const rmr_limits* limits, const rmr_pdf_options* options,
    rmr_cancel_fn cancel, void* context, rmr_buffer* out,
    rmr_pdf_report* report, rmr_error* error);
[[nodiscard]] RMR_API rmr_status rmr_apple_pdf_incremental_with_options(
    const rmr_apple_pdf_source* source, const rmr_pdf_page* pages, size_t count,
    const rmr_limits* limits, const rmr_pdf_options* options,
    rmr_cancel_fn cancel, void* context, rmr_buffer* out,
    rmr_pdf_incremental_report* report, rmr_error* error);
[[nodiscard]] RMR_API rmr_status rmr_apple_pdf_recompose_with_options(
    const rmr_apple_pdf_source* source, const rmr_pdf_page* pages, size_t count,
    const rmr_limits* limits, const rmr_pdf_options* options,
    rmr_cancel_fn cancel, void* context, rmr_buffer* out, rmr_error* error);

#endif
