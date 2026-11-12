/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_LINUX_H
#define RMR_LINUX_H
#include "grain.h"
#include "output.h"
#include "svg.h"
#include "template.h"

/* Cairo/PDFium bitmap rendering. PDFium calls are serialized process-wide; see
 * README. */
[[nodiscard]] RMR_API rmr_status
rmr_linux_bitmap(const rmr_plan* plan, double scale, rmr_color background,
                 const rmr_limits* limits, rmr_cancel_fn cancel,
                 void* cancel_context, rmr_bitmap* out, rmr_error* error);

/* Multi-page vector PDF. One viewport per plan; scale is points per .rm unit.
 * Rectangular PDF command clips retain vectors; other PDF clips return
 * UNSUPPORTED. Bounded memory output; no file is created, overwritten or
 * removed. */
[[nodiscard]] RMR_API rmr_status
rmr_linux_pdf(const rmr_plan* const* plans, size_t count, double scale,
              const rmr_limits* limits, rmr_cancel_fn cancel,
              void* cancel_context, rmr_buffer* out, rmr_error* error);

/* Explicit sheet output: size/transform from rmr_sheet_create or paper_sheet.
 * The physical sheet clips output; plan.viewport is the measurement extent,
 * not an extra clip. rmr_linux_bitmap and rmr_linux_pdf clip to the viewport.
 * Bitmap requires whole pixel dimensions and never rounds a second time.
 * Sheet transforms must have uniform positive scale, b=c=0, d=-a.
 * All sheets are validated before PDF output begins. */
[[nodiscard]] RMR_API rmr_status rmr_linux_bitmap_on_sheet(
    const rmr_plan* plan, const rmr_sheet* sheet, rmr_color background,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* cancel_context,
    rmr_bitmap* out, rmr_error* error);

/* Like rmr_linux_pdf, but each page uses its own explicit sheet (size and
 * transform) instead of a single shared scale. */
[[nodiscard]] RMR_API rmr_status rmr_linux_pdf_sheets(
    const rmr_plan* const* plans, const rmr_sheet* sheets, size_t count,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* cancel_context,
    rmr_buffer* out, rmr_error* error);

/* rmr_linux_pdf and rmr_linux_pdf_sheets drawing as options say (see
 * rmr_pdf_options in output.h; null selects the defaults, which the functions
 * without options use). */
[[nodiscard]] RMR_API rmr_status rmr_linux_pdf_with_options(
    const rmr_plan* const* plans, size_t count, double scale,
    const rmr_limits* limits, const rmr_pdf_options* options,
    rmr_cancel_fn cancel, void* cancel_context, rmr_buffer* out,
    rmr_error* error);
[[nodiscard]] RMR_API rmr_status rmr_linux_pdf_sheets_with_options(
    const rmr_plan* const* plans, const rmr_sheet* sheets, size_t count,
    const rmr_limits* limits, const rmr_pdf_options* options,
    rmr_cancel_fn cancel, void* cancel_context, rmr_buffer* out,
    rmr_error* error);

/* Stateless Pango sans-serif metrics, matching Linux TEXT drawing. */
RMR_API rmr_text_provider rmr_linux_text_provider(void);

/* rmr_svg_sheets with Cairo/Pango services: TEXT commands become the unhinted
 * outlines of the Pango line Linux TEXT drawing shapes; PDF commands are
 * rasterized by PDFium through rmr_linux_bitmap_on_sheet at raster_dpi (zero
 * selects 150) and embedded as PNG. One owned SVG document per plan/sheet
 * pair. */
[[nodiscard]] RMR_API rmr_status rmr_linux_svg(
    const rmr_plan* const* plans, const rmr_sheet* sheets, size_t count,
    double raster_dpi, const rmr_limits* limits, rmr_cancel_fn cancel,
    void* cancel_context, rmr_svg_pages* out, rmr_error* error);

#endif
