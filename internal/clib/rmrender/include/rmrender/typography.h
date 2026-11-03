/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_TYPOGRAPHY_H
#define RMR_TYPOGRAPHY_H
#include "rmlines.h"

typedef enum rmr_type_face : uint32_t {
  RMR_FACE_SANS,
  RMR_FACE_MEDIUM,
  RMR_FACE_SERIF
} rmr_type_face;

typedef struct {
  double size, height, ascent, descent, before, after, first_baseline_drop;
  rmr_type_face face;
} rmr_type_metrics;

/* Calibrated paragraph metrics, independent of font implementation. UTF-8 is
 * strict, including embedded NUL. Counted bytes are bounded by
 * maximum_elements. Missing format means body. Output/error are unchanged on
 * success/failure respectively, following the core convention. */
[[nodiscard]] RMR_API rmr_status rmr_lines_type_metrics(
    const rm_paragraph_format* format, rm_str text, const rmr_limits* limits,
    rmr_type_metrics* out, rmr_error* error);

typedef rmr_status (*rmr_font_resolve_fn)(void* context, const char* resource,
                                          rmr_asset_view* out);

typedef struct {
  size_t paragraph, from, to; /* UTF-16 code-unit range */
  double baseline, start, end;
} rmr_text_line;

#endif
