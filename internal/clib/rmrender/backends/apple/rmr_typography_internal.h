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

/* The label RMR_UNREADABLE_PLACEHOLDER draws, centred on page (.rm units, y
 * down) and set in the face typed text is set in at 5% of its width, as a
 * plan over viewport: its text as an embedded text PDF, as typed text is
 * drawn, or as glyph outline fills when outlines is set, as SVG output draws
 * typed text. An empty plan for a page with no area. Exported for the Apple
 * document library and rmrender-go; not part of the public API. */
RMR_API rmr_status rmr_apple_unreadable_plan(rmr_rect page, rmr_rect viewport,
                                             const rmr_apple_fonts* fonts,
                                             bool outlines,
                                             const rmr_limits* limits,
                                             rmr_plan** out, rmr_error* error);

#endif
