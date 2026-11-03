/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Internal backend helpers; not part of the public SDK. */
#ifndef RMR_PDF_EDIT_H
#define RMR_PDF_EDIT_H
#include <rmrender/pdf.h>

/* Appends an incremental update clipping a one-page PDF's content to path,
 * mapped through transform. */
RMR_API rmr_status rmr_pdf_clip(const uint8_t* pdf, size_t size,
                                rmr_path_view path, rmr_transform transform,
                                const rmr_limits* limits, rmr_buffer* out);

typedef struct {
  size_t page;
  rmr_point point;
} rmr_pdf_destination;

typedef struct {
  size_t page;
  rmr_rect rect;
  rmr_pdf_destination destination;
} rmr_pdf_local_link;

typedef struct {
  size_t parent; /* SIZE_MAX for top level; otherwise an earlier entry. */
  const uint8_t* title; /* UTF-16LE, without trailing NUL. */
  size_t title_size;
  rmr_pdf_destination destination; /* SIZE_MAX page for a grouping node. */
  bool open;
} rmr_pdf_outline;

/* Appends an incremental update adding local link annotations and,
 * optionally, a new document outline. */
RMR_API rmr_status rmr_pdf_navigation(
    const uint8_t* pdf, size_t size, const rmr_pdf_local_link* links,
    size_t link_count, const rmr_pdf_outline* outline, size_t outline_count,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* context,
    rmr_buffer* out);

#endif
