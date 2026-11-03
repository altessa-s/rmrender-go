/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Per-face, per-size width calibration and per-character spacing corrections,
 * the same tables as rmrender-swift's Calibration.swift; defined in
 * rmr_font_calibration.c. */

#ifndef RMR_FONT_CALIBRATION_H
#define RMR_FONT_CALIBRATION_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char* key;
  double size, width;
} rmr_stretch_entry;

/* Width factors by face key and point size. */
extern const rmr_stretch_entry rmr_stretches[];
extern const size_t rmr_stretch_count;

/* Spacing corrections by UTF-16 code unit, as a share of the size. */
double rmr_noto_glyph_share(uint16_t c);
double rmr_sans_glyph_share(uint16_t c);
double rmr_serif_glyph_share(uint16_t c);
double rmr_sans_bold_glyph_share(uint16_t c);
double rmr_sans_medium_glyph_share(uint16_t c);
double rmr_sans_italic_glyph_share(uint16_t c);
double rmr_serif_bold_glyph_share(uint16_t c);
double rmr_serif_italic_glyph_share(uint16_t c);

#endif
