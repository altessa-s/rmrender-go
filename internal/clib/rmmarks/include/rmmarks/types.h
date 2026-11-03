/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMMARKS_TYPES_H
#define RMMARKS_TYPES_H

#include <stddef.h>
#include <stdint.h>

#include "rmmarks/export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Separate from RMLines' rm_* namespace: both headers may be included and
 * both libraries linked without a dependency between them. */
typedef struct rmm_str {
  const char* bytes;
  size_t length;
} rmm_str;
/* Literal only. Inputs are counted UTF-8 and need not be NUL terminated.
 * An empty {nullptr, 0} is accepted. Outputs are also NUL terminated and
 * may contain embedded NULs: length, not strlen, is authoritative. */
#define RMM_STR(literal) ((rmm_str){(literal), sizeof(literal) - 1u})

typedef struct rmm_page_box {
  double x0, y0, x1, y1;
} rmm_page_box;
typedef struct rmm_placed_text {
  rmm_str text;
  rmm_page_box box;
} rmm_placed_text;
typedef struct rmm_recognized_line {
  rmm_str text;
  rmm_page_box box;
} rmm_recognized_line;
typedef struct rmm_line {
  const rmm_placed_text* pieces;
  size_t count;
} rmm_line;
typedef struct rmm_mark_band {
  rmm_page_box box;
  uint32_t color; /* Resolved RGB, not a palette index. */
  rmm_str item;   /* bytes == nullptr means absent. */
} rmm_mark_band;
typedef struct rmm_mark_strip {
  rmm_page_box on_page, in_sheet;
} rmm_mark_strip;
typedef struct rmm_marked_highlight {
  rmm_str text;
  rmm_mark_band band;
} rmm_marked_highlight;
typedef struct rmm_marked_page {
  const rmm_mark_band* bands;
  size_t band_count;
  const rmm_marked_highlight* highlights;
  size_t highlight_count;
  rmm_str page_id; /* bytes == nullptr means absent. */
} rmm_marked_page;
typedef struct rmm_marked_passage {
  rmm_str text;
  size_t band;
  rmm_page_box place;
  rmm_str second; /* Absent if readings agree or only one found words. */
  const size_t* again;
  size_t again_count;
} rmm_marked_passage;

/* One reading per band; duplicate band keys are invalid. Input arrays are
 * borrowed until the operation returns. A brought result can be passed
 * directly as the alone readings of rmm_text_passages. */
typedef struct rmm_band_reading {
  size_t band;
  const rmm_line* lines;
  size_t line_count;
} rmm_band_reading;

struct rmm_arena;
typedef struct rmm_passages {
  const rmm_marked_passage* items;
  size_t count;
  struct rmm_arena* arena; /* Private ownership. Do not modify. */
} rmm_passages;
typedef struct rmm_blocks {
  const rmm_page_box* items;
  size_t count;
  struct rmm_arena* arena;
} rmm_blocks;
typedef struct rmm_readings {
  const rmm_band_reading* items;
  size_t count;
  struct rmm_arena* arena;
} rmm_readings;

typedef enum rmm_status : int {
  RMM_OK = 0,
  RMM_ERR_INVALID_ARGUMENT,
  RMM_ERR_OUT_OF_MEMORY,
  RMM_ERR_INVALID_UTF8,
} rmm_status;
typedef struct rmm_error {
  rmm_status status;
  char message[160];
} rmm_error;

/* A fixed, human-readable string for status; never null. */
RMM_API const char* rmm_status_string(rmm_status status);
/* Fractional coordinates, origin top-left; never clamped to [0, 1]. */
RMM_API rmm_page_box rmm_box_inside(rmm_page_box box, rmm_page_box frame);
/* Inverse of rmm_box_inside: expands fractional coordinates back into
 * frame's own coordinate space. */
RMM_API rmm_page_box rmm_box_within(rmm_page_box box, rmm_page_box frame);
/* True if page is null or has neither mark bands nor highlights. */
RMM_API bool rmm_page_is_empty(const rmm_marked_page* page);
/* Wraps a recognized line as placed text, keeping its text and box. */
RMM_API rmm_placed_text rmm_place_line(rmm_recognized_line line);

#ifdef __cplusplus
}
#endif
#endif
