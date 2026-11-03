/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <stdio.h>

#include "rmm_internal.h"

const char* rmm_status_string(rmm_status status) {
  switch (status) {
    case RMM_OK:
      return "success";
    case RMM_ERR_INVALID_ARGUMENT:
      return "invalid argument";
    case RMM_ERR_OUT_OF_MEMORY:
      return "out of memory";
    case RMM_ERR_INVALID_UTF8:
      return "invalid UTF-8";
    default:
      return "unknown status";
  }
}

rmm_status rmm_finish(rmm_error* error, rmm_status status) {
  if (error != nullptr) {
    *error = (rmm_error){.status = status};
    snprintf(error->message, sizeof error->message, "%s",
             rmm_status_string(status));
  }
  return status;
}

bool rmm_array_valid(const void* data, size_t count, size_t element) {
  return (count == 0 || data != nullptr) && count <= SIZE_MAX / element;
}

bool rmm_box_valid(rmm_page_box b) {
  return isfinite(b.x0) && isfinite(b.y0) && isfinite(b.x1) && isfinite(b.y1) &&
         isfinite(b.x1 - b.x0) && isfinite(b.y1 - b.y0) &&
         isfinite(b.y0 + b.y1);
}

rmm_page_box rmm_box_inside(rmm_page_box b, rmm_page_box f) {
  double w = f.x1 - f.x0, h = f.y1 - f.y0;
  if (!(w > 0 && h > 0)) return b;
  return (rmm_page_box){(b.x0 - f.x0) / w, (b.y0 - f.y0) / h, (b.x1 - f.x0) / w,
                        (b.y1 - f.y0) / h};
}

rmm_page_box rmm_box_within(rmm_page_box b, rmm_page_box f) {
  double w = f.x1 - f.x0, h = f.y1 - f.y0;
  return (rmm_page_box){f.x0 + b.x0 * w, f.y0 + b.y0 * h, f.x0 + b.x1 * w,
                        f.y0 + b.y1 * h};
}

rmm_page_box rmm_box_union(rmm_page_box a, rmm_page_box b) {
  return (rmm_page_box){fmin(a.x0, b.x0), fmin(a.y0, b.y0), fmax(a.x1, b.x1),
                        fmax(a.y1, b.y1)};
}

bool rmm_page_is_empty(const rmm_marked_page* page) {
  return page == nullptr ||
         (page->band_count == 0 && page->highlight_count == 0);
}

rmm_placed_text rmm_place_line(rmm_recognized_line line) {
  return (rmm_placed_text){line.text, line.box};
}

rmm_status rmm_validate_lines(const rmm_line* lines, size_t count,
                              size_t* pieces) {
  if (!rmm_array_valid(lines, count, sizeof *lines))
    return RMM_ERR_INVALID_ARGUMENT;
  size_t total = 0;
  for (size_t i = 0; i < count; i += 1) {
    if (!rmm_array_valid(lines[i].pieces, lines[i].count,
                         sizeof *lines[i].pieces) ||
        lines[i].count > SIZE_MAX - total)
      return RMM_ERR_INVALID_ARGUMENT;
    total += lines[i].count;
    for (size_t j = 0; j < lines[i].count; j += 1) {
      rmm_placed_text p = lines[i].pieces[j];
      if (!rmm_box_valid(p.box) ||
          !rmm_array_valid(p.text.bytes, p.text.length, 1))
        return RMM_ERR_INVALID_ARGUMENT;
      if (!rmm_utf8_valid(p.text)) return RMM_ERR_INVALID_UTF8;
    }
  }
  if (pieces != nullptr) *pieces = total;
  return RMM_OK;
}

void rmm_passages_free(rmm_passages* r) {
  if (r != nullptr) rmm_arena_destroy(r->arena);
}

void rmm_blocks_free(rmm_blocks* r) {
  if (r != nullptr) rmm_arena_destroy(r->arena);
}

void rmm_readings_free(rmm_readings* r) {
  if (r != nullptr) rmm_arena_destroy(r->arena);
}
