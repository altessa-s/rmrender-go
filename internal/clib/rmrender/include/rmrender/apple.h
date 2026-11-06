/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_APPLE_H
#define RMR_APPLE_H
#include "grain.h"
#include "output.h"
#include "svg.h"
#include "template.h"

/* Eraser path subtraction requires macOS 13+/iOS 16+ and an SDK exposing it.
 * Older runtimes omit RMR_CAP_ERASE and reject affected plans before drawing.
 * RGBA8, premultiplied alpha, sRGB, first row at top, tightly packed.
 * Transparent background is {0,0,0,0}. Scale is pixels per .rm unit.
 * PNG/JPEG image commands decode through CoreGraphics. Each decoded image is
 * bounded by maximum_bitmap_side, maximum_bitmap_bytes (4 bytes per pixel)
 * and a fixed 64-million-pixel ceiling, checked before drawing. Corrupt data
 * returns BACKEND; oversize returns LIMIT. Output remains unchanged on failure.
 * Callers encode the returned bitmap pixels themselves. */
[[nodiscard]] RMR_API rmr_status
rmr_apple_bitmap(const rmr_plan* plan, double scale, rmr_color background,
                 const rmr_limits* limits, rmr_cancel_fn cancel,
                 void* cancel_context, rmr_bitmap* out, rmr_error* error);

/* Multi-page vector PDF. One viewport per plan; scale is points per .rm unit.
 * Bounded memory output; no file is created, overwritten or removed. */
[[nodiscard]] RMR_API rmr_status
rmr_apple_pdf(const rmr_plan* const* plans, size_t count, double scale,
              const rmr_limits* limits, rmr_cancel_fn cancel,
              void* cancel_context, rmr_buffer* out, rmr_error* error);

/* Stateless CoreGraphics unit rasterizer. A fresh context is used for each map.
 */
RMR_API rmr_unit_raster rmr_apple_unit_raster(void);

/* Explicit sheet output: size/transform from rmr_sheet_create or paper_sheet.
 * The physical sheet clips output; plan.viewport is the measurement extent,
 * not an extra clip. rmr_apple_bitmap and rmr_apple_pdf clip to the viewport.
 * Bitmap requires whole pixel dimensions and never rounds a second time.
 * Sheet transforms must have uniform positive scale, b=c=0, d=-a.
 * All sheets are validated before PDF output begins. */
[[nodiscard]] RMR_API rmr_status rmr_apple_bitmap_on_sheet(
    const rmr_plan* plan, const rmr_sheet* sheet, rmr_color background,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* cancel_context,
    rmr_bitmap* out, rmr_error* error);

/* Like rmr_apple_pdf, but each page uses its own explicit sheet (size and
 * transform) instead of a single shared scale. */
[[nodiscard]] RMR_API rmr_status rmr_apple_pdf_sheets(
    const rmr_plan* const* plans, const rmr_sheet* sheets, size_t count,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* cancel_context,
    rmr_buffer* out, rmr_error* error);

/* rmr_apple_pdf and rmr_apple_pdf_sheets drawing as options say (see
 * rmr_pdf_options in output.h; null selects the defaults, which the functions
 * without options use). */
[[nodiscard]] RMR_API rmr_status rmr_apple_pdf_with_options(
    const rmr_plan* const* plans, size_t count, double scale,
    const rmr_limits* limits, const rmr_pdf_options* options,
    rmr_cancel_fn cancel, void* cancel_context, rmr_buffer* out,
    rmr_error* error);
[[nodiscard]] RMR_API rmr_status rmr_apple_pdf_sheets_with_options(
    const rmr_plan* const* plans, const rmr_sheet* sheets, size_t count,
    const rmr_limits* limits, const rmr_pdf_options* options,
    rmr_cancel_fn cancel, void* cancel_context, rmr_buffer* out,
    rmr_error* error);

/* Stateless CoreText Helvetica metrics, matching Apple TEXT drawing. */
RMR_API rmr_text_provider rmr_apple_text_provider(void);

/* rmr_svg_sheets with CoreText services: TEXT commands become the outlines of
 * the Helvetica line Apple TEXT drawing shapes (glyphs without outlines, such
 * as bitmap emoji, are omitted); PDF commands are rasterized by
 * rmr_apple_bitmap_on_sheet at raster_dpi (zero selects 150) and embedded as
 * PNG. One owned SVG document per plan/sheet pair. */
[[nodiscard]] RMR_API rmr_status rmr_apple_svg(
    const rmr_plan* const* plans, const rmr_sheet* sheets, size_t count,
    double raster_dpi, const rmr_limits* limits, rmr_cancel_fn cancel,
    void* cancel_context, rmr_svg_pages* out, rmr_error* error);

#endif
