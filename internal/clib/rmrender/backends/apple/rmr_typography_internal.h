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

#endif
