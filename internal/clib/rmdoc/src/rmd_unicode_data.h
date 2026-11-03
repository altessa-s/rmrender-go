/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Unicode 16.0.0 tables generated into rmd_unicode_data.c by
 * scripts/generate-unicode.py. */

#ifndef RMD_UNICODE_DATA_H
#define RMD_UNICODE_DATA_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t first, last;
  uint8_t value;
} rmd_unicode_range;

typedef struct {
  uint32_t scalar;
  uint32_t parts[4];
  uint8_t count;
} rmd_decomposition;

/* Canonical combining classes and the Cf (format) category, as sorted ranges.
 */
extern const rmd_unicode_range rmd_combining[];
extern const size_t rmd_combining_count;
extern const rmd_unicode_range rmd_format[];
extern const size_t rmd_format_count;

/* Canonical decompositions, sorted by scalar; Hangul syllables are computed. */
extern const rmd_decomposition rmd_decompositions[];
extern const size_t rmd_decompositions_count;

#endif
