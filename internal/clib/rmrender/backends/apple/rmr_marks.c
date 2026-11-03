/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <math.h>

#include "../../adapters/rmr_width.h"
#include "rmr_marks_platform.h"

rmr_status mark_stroke_bounds(const rm_stroke* stroke,
                              const rmr_stroke_style* style, rmr_rect* out) {
  rmr_status status = RMR_OK;
  CGRect box = CGRectNull;
  CGMutablePathRef path = CGPathCreateMutable();
  if (!path) {
    status = RMR_ERR_OUT_OF_MEMORY;
    return status;
  }

  double widest = 0;
  for (size_t j = 0; j < stroke->point_count; j++) {
    rm_stroke_point p = stroke->points[j];
    double half = rmr_line_width(rmr_drawn_width(stroke->tool, p)) / 2;
    if (!isfinite(p.width) || p.width < 0) {
      status = RMR_ERR_INVALID_ARGUMENT;
      break;
    }
    widest = fmax(widest, half * 2);
    if (j)
      CGPathAddLineToPoint(path, nullptr, p.x, p.y);
    else
      CGPathMoveToPoint(path, nullptr, p.x, p.y);
    box = CGRectUnion(box,
                      CGRectMake(p.x - half, p.y - half, half * 2, half * 2));
  }

  if (status == RMR_OK && stroke->point_count > 1) {
    CGPathRef outline = CGPathCreateCopyByStrokingPath(
        path, nullptr, widest,
        style->cap == RMR_CAP_SQUARE ? kCGLineCapSquare : kCGLineCapButt,
        kCGLineJoinRound, 10);
    if (!outline)
      status = RMR_ERR_OUT_OF_MEMORY;
    else {
      CGRect b = CGPathGetPathBoundingBox(outline);
      if (!CGRectIsNull(b) && isfinite(b.size.width) && isfinite(b.size.height))
        box = b;
      CGPathRelease(outline);
    }
  }

  CGPathRelease(path);
  if (status != RMR_OK) return status;
  *out =
      (rmr_rect){box.origin.x, box.origin.y, box.size.width, box.size.height};
  return status;
}
