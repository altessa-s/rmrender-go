/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_APPLE_TYPOGRAPHY_INTERNAL_H
#define RMR_APPLE_TYPOGRAPHY_INTERNAL_H
#include <CoreText/CoreText.h>
#include <math.h>
#include <rmrender/apple_text.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fill `e` with `s` and `message`; shared by the Apple text, document and marks
 * libraries, so exported from the text library; not part of the public API. */
RMR_API rmr_status rmr_apple_text_error(rmr_error* e, rmr_status s,
                                        const char* message);

/* Retained native font for face/bold/italic at size; script selects a Noto
 * fallback family, or -1 for the Latin/serif families. */
CTFontRef rmr_ct_font(const rmr_apple_fonts* fonts, double size,
                      rmr_type_face face, bool bold, bool italic, int script);

/* Per-glyph kerning for c as a fraction of font size, matching calibration. */
double rmr_ct_kern(UniChar c, rmr_type_face face, bool bold, bool italic,
                   int script);

/* rmr_apple_layout_plan's geometry with glyphs as page-space outline fills
 * instead of an embedded text PDF, for SVG output. */
rmr_status rmr_apple_layout_outline_plan(const rmr_apple_layout* layout,
                                         rmr_rect viewport,
                                         const rmr_limits* limits,
                                         rmr_plan** out, rmr_error* error);

/* rmr_apple_page_plan with typed text as outlines; exported for the Apple
 * document library, not part of the public API. */
RMR_API rmr_status rmr_apple_page_outline_plan(
    const rm_page* page, rmr_point canvas, rmr_ink_edge edge, double margin,
    const rmr_page_resources* resources, const rmr_apple_fonts* fonts,
    const rmr_limits* limits, rmr_plan** out, rmr_error* error);

#endif
