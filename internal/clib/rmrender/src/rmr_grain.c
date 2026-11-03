/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Grain input validation, mask bounds and mask storage. */
#include <rmrender/grain.h>

#include "rmr_internal.h"

rmr_status rmr_grain_validate(const rmr_grain_point* p, size_t n, rmr_limits l,
                              rmr_error* e) {
  if ((!p && n) || !isfinite(l.maximum_coordinate) ||
      l.maximum_coordinate <= 0 || l.maximum_coordinate > 1e12 ||
      !isfinite(l.maximum_width) || l.maximum_width <= 0 ||
      l.maximum_width > 1e6)
    return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, 0,
                    "invalid grain input or limits");
  if (n > l.maximum_points || n > INT32_MAX)
    return rmr_fail(e, RMR_ERR_LIMIT, 0, "grain point limit");

  for (size_t i = 0; i < n; i++)
    if (!rmr_point_valid((rmr_point){p[i].x, p[i].y}, l.maximum_coordinate) ||
        !isfinite(p[i].width) || p[i].width < 0 || p[i].width > l.maximum_width)
      return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, i, "invalid grain point");

  return RMR_OK;
}

rmr_status rmr_grain_box(const rmr_grain_point* p, size_t n,
                         const rmr_limits* given, rmr_rect* out, rmr_error* e) {
  rmr_limits l = given ? *given : rmr_limits_default();
  if (!out || !n)
    return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, 0, "empty grain bounds");
  rmr_status s = rmr_grain_validate(p, n, l, e);
  if (s != RMR_OK) return s;

  double x0 = p[0].x, y0 = p[0].y, x1 = x0, y1 = y0;
  for (size_t i = 0; i < n; i++) {
    double h = (double)p[i].width / 2 + 1;
    x0 = fmin(x0, p[i].x - h);
    x1 = fmax(x1, p[i].x + h);
    y0 = fmin(y0, p[i].y - h);
    y1 = fmax(y1, p[i].y + h);
  }

  *out = (rmr_rect){floor(x0), floor(y0), ceil(x1) - floor(x0),
                    ceil(y1) - floor(y0)};
  return RMR_OK;
}

void rmr_mask_free(rmr_mask* m) {
  if (m) {
    free(m->bytes);
    *m = (rmr_mask){0};
  }
}

rmr_status rmr_mask_layout(rmr_rect box, rmr_limits l, rmr_mask* out,
                           rmr_error* e) {
  if (!out ||
      !rmr_point_valid((rmr_point){box.x, box.y}, l.maximum_coordinate) ||
      floor(box.x) != box.x || floor(box.y) != box.y || !isfinite(box.width) ||
      !isfinite(box.height) || box.width < 1 || box.height < 1 ||
      floor(box.width) != box.width || floor(box.height) != box.height)
    return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, 0, "invalid grain box");
  if (box.width > 16384 || box.height > 16384 ||
      box.width * box.height > 8388608 ||
      box.width * 2 > l.maximum_bitmap_side ||
      box.height * 2 > l.maximum_bitmap_side)
    return rmr_fail(e, RMR_ERR_LIMIT, 0, "grain box limit");

  size_t w = (size_t)box.width, h = (size_t)box.height,
         stride = (w * 2 + 7) / 8;
  if (stride * h * 2 > l.maximum_bitmap_bytes ||
      stride * h * 2 > l.maximum_plan_bytes)
    return rmr_fail(e, RMR_ERR_LIMIT, 0, "mask byte limit");

  *out = (rmr_mask){nullptr, w * 2, h * 2, stride, box};
  return RMR_OK;
}
