/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Unicode 16.0.0 tables generated into rmm_unicode_data.c by
 * tools/generate_unicode.py. */

#ifndef RMM_UNICODE_DATA_H
#define RMM_UNICODE_DATA_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t scalar;
  uint32_t parts[4];
  uint8_t count;
} rmm_decomposition;

typedef struct {
  uint32_t first, last;
  uint8_t value;
} rmm_unicode_range;

/* Canonical decompositions, sorted by scalar; Hangul syllables are computed. */
extern const rmm_decomposition rmm_decompositions[];
extern const size_t rmm_decompositions_count;

/* Canonical combining classes, as sorted ranges. */
extern const rmm_unicode_range rmm_combining[];
extern const size_t rmm_combining_count;

/* Grapheme cluster break classes of assigned code points, as sorted ranges. */
extern const rmm_unicode_range rmm_grapheme[];
extern const size_t rmm_grapheme_count;

#endif
