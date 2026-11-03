/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <math.h>

#include "../../adapters/rmr_width.h"
#include "rmr_marks_platform.h"

rmr_status mark_stroke_bounds(const rm_stroke* stroke,
                              const rmr_stroke_style* style, rmr_rect* out) {
  double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY,
         widest = 0;
  cairo_surface_t* surface =
      cairo_recording_surface_create(CAIRO_CONTENT_ALPHA, nullptr);
  cairo_t* cr = cairo_create(surface);
  rmr_status status = RMR_OK;

  for (size_t i = 0; i < stroke->point_count; i++) {
    rm_stroke_point p = stroke->points[i];
    if (!isfinite(p.width) || p.width < 0) {
      status = RMR_ERR_INVALID_ARGUMENT;
      break;
    }
    double half = rmr_line_width(rmr_drawn_width(stroke->tool, p)) / 2;
    widest = fmax(widest, half * 2);
    x0 = fmin(x0, p.x - half);
    y0 = fmin(y0, p.y - half);
    x1 = fmax(x1, p.x + half);
    y1 = fmax(y1, p.y + half);
    if (i)
      cairo_line_to(cr, p.x, p.y);
    else
      cairo_move_to(cr, p.x, p.y);
  }

  if (status == RMR_OK && stroke->point_count > 1) {
    cairo_set_line_width(cr, widest);
    cairo_set_line_cap(cr, style->cap == RMR_CAP_SQUARE ? CAIRO_LINE_CAP_SQUARE
                                                        : CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    double left, top, right, bottom;
    cairo_stroke_extents(cr, &left, &top, &right, &bottom);
    if (right > left || bottom > top) {
      x0 = left;
      y0 = top;
      x1 = right;
      y1 = bottom;
    }
  }

  cairo_status_t cs = cairo_status(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  if (cs != CAIRO_STATUS_SUCCESS)
    status =
        cs == CAIRO_STATUS_NO_MEMORY ? RMR_ERR_OUT_OF_MEMORY : RMR_ERR_BACKEND;

  if (status == RMR_OK) *out = (rmr_rect){x0, y0, x1 - x0, y1 - y0};
  return status;
}
