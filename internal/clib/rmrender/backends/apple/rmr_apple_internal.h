/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_APPLE_INTERNAL_H
#define RMR_APPLE_INTERNAL_H
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <rmrender/apple.h>

/* Declared publicly in apple_context.h; forward-declared here for backends/
 * apple/rmr_pdf.c, which draws a plan into a page it is already composing. */
rmr_status rmr_apple_draw_context(CGContextRef cg, const rmr_plan* plan,
                                  const rmr_sheet* sheet,
                                  const rmr_limits* limits,
                                  rmr_cancel_fn cancel, void* context,
                                  rmr_error* error);

/* Consulted before each grain mask is drawn: true with *out filled to draw
 * the mask with other dimensions over the same bytes. */
typedef bool (*rmr_apple_mask_fn)(void* context, const rmr_mask_view* mask,
                                  rmr_mask_view* out);

/* rmr_apple_draw_context for draws first to end - 1 of plan only, with an
 * optional mask hook. */
rmr_status rmr_apple_draw_context_range(
    CGContextRef cg, const rmr_plan* plan, size_t first, size_t end,
    const rmr_sheet* sheet, const rmr_limits* limits, rmr_apple_mask_fn masks,
    void* masks_context, rmr_cancel_fn cancel, void* context, rmr_error* error);

/* Builds the Helvetica CoreText line TEXT commands draw, for utf8/size at
 * font_size in color. */
rmr_status rmr_apple_text_line(const char* utf8, size_t size, double font_size,
                               rmr_color color, CTLineRef* out);

/* Appends the glyph outlines of line to path, mapping line space (baseline
 * origin, y up) through map; glyphs without outlines are skipped. Exported
 * for the Apple text library; not part of the public API. */
RMR_API rmr_status rmr_apple_line_outline(CTLineRef line, CGAffineTransform map,
                                          const rmr_limits* limits,
                                          rmr_path* path);

#endif
