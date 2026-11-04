/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_SVG_H
#define RMR_SVG_H
#include "output.h"

/* Platform services for the commands a portable writer cannot express alone.
 * text returns the filled (nonzero) glyph outlines of one TEXT command in page
 * coordinates, y down, shaped exactly as the platform's TEXT drawing; out is
 * caller-owned (rmr_path_free). raster draws plan onto sheet over a
 * transparent background as premultiplied RGBA8, first row at top, like the
 * platform bitmap entry points; it is used for PDF commands. A null callback
 * removes the matching capability, so such plans return UNSUPPORTED. */
#define RMR_SVG_PROVIDER_ABI 1u
typedef struct {
  uint32_t abi_version;
  size_t struct_size;
  void* context;
  rmr_status (*text)(void* context, const rmr_text_view* text,
                     const rmr_limits* limits, rmr_path* out);
  rmr_status (*raster)(void* context, const rmr_plan* plan,
                       const rmr_sheet* sheet, const rmr_limits* limits,
                       rmr_bitmap* out);
} rmr_svg_provider;

/* One standalone UTF-8 SVG 1.1 document per page, in input order. */
typedef struct {
  rmr_buffer* pages;
  size_t count;
} rmr_svg_pages;

/* Releases every page and the array, then zeroes pages; null is a no-op. */
RMR_API void rmr_svg_pages_free(rmr_svg_pages* pages);

/* Portable SVG output, one document per plan on its sheet. Sheet units are
 * PDF points: width/height are written in pt and the viewBox spans the sheet,
 * which clips output. Sheet transforms must have uniform positive scale,
 * b=c=0, d=-a; all sheets are validated before output begins. Paths, strokes,
 * groups, blend modes, erasers and grain masks stay vector (masks embed their
 * one-bit raster as PNG); images embed their PNG/JPEG bytes unchanged and are
 * not decoded. TEXT becomes outline paths through provider->text. PDF
 * commands are rasterized through provider->raster at raster_dpi pixels per
 * inch of sheet (zero selects 150) and embedded as PNG. Output is
 * deterministic, self-contained and transactional; maximum_pdf_bytes bounds
 * the total of all pages and maximum_draws the page count. Provider may be
 * null when plans contain no TEXT or PDF commands. */
[[nodiscard]] RMR_API rmr_status rmr_svg_sheets(
    const rmr_plan* const* plans, const rmr_sheet* sheets, size_t count,
    const rmr_svg_provider* provider, double raster_dpi,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* cancel_context,
    rmr_svg_pages* out, rmr_error* error);

#endif
