/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_LINUX_INTERNAL_H
#define RMR_LINUX_INTERNAL_H
#include <cairo-pdf.h>
#include <cairo.h>
#include <fpdf_edit.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <fpdf_transformpage.h>
#include <fpdfview.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <rmrender/linux.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  rmr_buffer data;
  size_t capacity, maximum;
  rmr_status status;
} rmr_sink;

/* Fills the optional error with s and message and returns s unchanged. */
RMR_API rmr_status rmr_linux_error(rmr_error* e, rmr_status s,
                                   const char* message);

/* Cairo write callback appending data to the sink, growing its buffer
 * geometrically up to the sink's configured maximum. */
RMR_API cairo_status_t rmr_linux_write(void* ctx, const unsigned char* data,
                                       unsigned int size);

/* Composes t with the inverse of page's raw, pre-rotation coordinate
 * mapping. */
rmr_transform rmr_pdfium_raw_transform(FPDF_PAGE page, rmr_transform t);

/* Initializes PDFium on first call, then locks the process-wide recursive
 * mutex guarding it. */
void rmr_pdfium_lock(void);

/* Unlocks the process-wide PDFium mutex. */
void rmr_pdfium_unlock(void);

/* Serializes doc into out via PDFium, enforcing limits->maximum_pdf_bytes;
 * out is unchanged on failure. */
rmr_status rmr_pdfium_save(FPDF_DOCUMENT doc, const rmr_limits* limits,
                           rmr_buffer* out);

/* Copies page index of source into dest as a transformed form XObject
 * placed on page. */
rmr_status rmr_pdfium_form(FPDF_DOCUMENT dest, FPDF_PAGE page,
                           FPDF_DOCUMENT source, size_t index,
                           rmr_transform transform);

/* Renders count plans/sheets into a new multi-page PDF after validating
 * inputs and checking cancel, drawing grained ink as grain says. */
rmr_status rmr_linux_render_pdf(const rmr_plan* const* plans,
                                const rmr_sheet* sheets, size_t count,
                                const rmr_limits* limits, rmr_pdf_grain grain,
                                rmr_cancel_fn cancel, void* context,
                                rmr_buffer* out, rmr_error* e);

/* Builds the single-paragraph Pango layout TEXT commands draw: the fixed
 * sans-serif family at an absolute point size. */
PangoLayout* rmr_linux_label(cairo_t* cr, const char* text, size_t size,
                             double font_size);

/* Appends cr's current path (user space) to path within limits'
 * maximum_elements and clears it. Exported for the Linux text library; not
 * part of the public API. */
RMR_API rmr_status rmr_linux_path_capture(cairo_t* cr, const rmr_limits* limits,
                                          rmr_path* path);

#endif
