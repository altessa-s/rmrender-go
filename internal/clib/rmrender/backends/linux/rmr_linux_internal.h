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

#include "../../src/rmr_pdf_edit.h"
#include "../common/rmr_pdf_stream.h"

/* Serializes font creation, shaping, drawing and destruction. Cairo shares
 * a FreeType library across font maps, even when their faces differ. The
 * recursive lock also covers layout cleanup called from a failed operation. */
RMR_API void rmr_linux_font_lock(void);
RMR_API void rmr_linux_font_unlock(void);

/* Fetches a native bookmark within the core reader's accepted entry count.
 * A further entry ends collection and sets overflow so its caller can drop
 * the partial outline, even if the native reader follows a cycle. */
RMR_API FPDF_BOOKMARK rmr_linux_bookmark_next(FPDF_DOCUMENT document,
                                              FPDF_BOOKMARK parent,
                                              FPDF_BOOKMARK previous,
                                              size_t* remaining,
                                              bool* overflow);

typedef struct {
  rmr_buffer data;
  size_t capacity, maximum;
  rmr_status status;
} rmr_sink;

/* Fills the optional error with s and message and returns s unchanged. */
RMR_API rmr_status rmr_linux_error(rmr_error* e, rmr_status s,
                                   const char* message);

/* The causes of a failed PDF export that the Cairo backend tells apart,
 * worded as the Apple backend words the same causes. */
#define RMR_LINUX_BYTE_LIMIT "PDF output byte limit"
#define RMR_LINUX_ALLOCATION "PDF sink allocation failed"

/* Fills the optional error after a PDF builder call failed with s, from the
 * builder's own report of it. Returns s. */
rmr_status rmr_linux_builder_error(rmr_error* e, rmr_status s,
                                   const rmr_error* builder);

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

/* Called for each page of a parts writer once its marks are imported: forms
 * are the page's Cairo segments and clipped embedded PDFs, in drawing order,
 * as Form XObjects; extra is what rmr_linux_parts_close was given. A
 * failure is described in error. */
typedef rmr_status (*rmr_linux_page_fn)(void* context, size_t page,
                                        const uint32_t* forms, size_t count,
                                        rmr_buffer extra, rmr_error* error);

/* Pages' marks drawn by Cairo, each page into one PDF per segment (a segment
 * ends where an embedded PDF is drawn, which becomes a part of its own);
 * the segments are finished, Cairo's own serialization, on worker threads,
 * and every part is imported into builder in drawing order. */
typedef struct rmr_linux_parts rmr_linux_parts;

rmr_status rmr_linux_parts_create(rmr_pdf_builder* builder,
                                  rmr_linux_page_fn done, void* context,
                                  rmr_progress_fn progress,
                                  void* progress_context, size_t total,
                                  rmr_linux_parts** out);
/* Failure cleanup may settle earlier pages, but must not report them. */
void rmr_linux_parts_stop_progress(rmr_linux_parts* parts);
void rmr_linux_parts_free(rmr_linux_parts* parts);

/* Draws plan on sheet as page `page`'s parts and queues their finishing; the
 * PDFium lock must be held. Fails as rmr_plan_execute does, except that a
 * part passing maximum_pdf_bytes is reported as RMR_LINUX_BYTE_LIMIT. */
rmr_status rmr_linux_parts_draw(rmr_linux_parts* parts, size_t page,
                                const rmr_plan* plan, const rmr_sheet* sheet,
                                const rmr_limits* limits, rmr_pdf_grain grain,
                                rmr_cancel_fn cancel, void* context,
                                rmr_error* error);

/* Ends page `page`, handing extra (owned) to the page callback. */
rmr_status rmr_linux_parts_close(rmr_linux_parts* parts, size_t page,
                                 rmr_buffer extra);

/* Imports finished parts and completes pages, oldest first: all of them
 * when all is set, otherwise waiting only while too many are queued. The
 * first failure in that order is returned and described in error. */
rmr_status rmr_linux_parts_collect(rmr_linux_parts* parts, bool all,
                                   rmr_error* error);

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
