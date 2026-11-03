/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <math.h>
#include <rmrender/rmlines.h>
#include <stdio.h>

#include "rmr_width.h"

/* Fill *e (if given) with status s, item index i and message, and return s. */
static rmr_status fail(rmr_error* e, rmr_status s, size_t i,
                       const char* message) {
  if (e) {
    *e = (rmr_error){.code = s, .index = i};
    snprintf(e->message, sizeof e->message, "%s", message);
  }
  return s;
}

/* Preserve CGRect's origin/size union arithmetic, including rounding at the
 * sheet edges; storing only min/max changes centreline margin decisions. */
static void include(rmr_rect* b, double x, double y, double half) {
  double left = fmin(b->x, x - half), bottom = fmin(b->y, y - half);
  double right = fmax(b->x + b->width, (x - half) + half * 2);
  double top = fmax(b->y + b->height, (y - half) + half * 2);
  *b = (rmr_rect){left, bottom, right - left, top - bottom};
}

/* Like rmr_lines_content_bounds, but also grows the viewport to cover a
 * laid-out text extent (and its ink-edge-specific text foot); callers with
 * text/highlights on the page must go through this entry point. */
rmr_status rmr_lines_content_bounds_with_text(const rm_page* page, double w,
                                              double h, rmr_ink_edge edge,
                                              double margin,
                                              const rmr_text_extent* text,
                                              const rmr_limits* given,
                                              rmr_rect* out, rmr_error* e) {
  rmr_limits l = given ? *given : rmr_limits_default();
  if (!page || !out || !isfinite(l.maximum_coordinate) ||
      l.maximum_coordinate <= 0 || l.maximum_coordinate > 1e12 ||
      !isfinite(l.maximum_width) || l.maximum_width <= 0 ||
      l.maximum_width > 1e6 || !isfinite(w) || !isfinite(h) || w <= 0 ||
      h <= 0 || w / 2 > l.maximum_coordinate || h > l.maximum_coordinate ||
      edge > RMR_EDGE_CENTRELINE || !isfinite(margin) || margin < 0 ||
      margin > l.maximum_coordinate ||
      (edge != RMR_EDGE_CENTRELINE && margin != 0) ||
      (page->stroke_count && !page->strokes) ||
      (page->image_count && !page->images))
    return fail(e, RMR_ERR_INVALID_ARGUMENT, 0, "invalid page bounds input");
  if (page->has_text && !text)
    return fail(e, RMR_ERR_UNSUPPORTED, 0,
                "text/highlights require bounds providers");
  if (page->stroke_count > l.maximum_draws ||
      page->image_count > l.maximum_draws - page->stroke_count)
    return fail(e, RMR_ERR_LIMIT, 0, "page bounds item limit");

  rmr_rect bounds = {0, 0, w, h};
  double leftmost = INFINITY, rightmost = -INFINITY, deepest = INFINITY,
         highest = -INFINITY;
  size_t points = 0;
  for (size_t i = 0; i < page->stroke_count; i++) {
    const rm_stroke* s = &page->strokes[i];
    if (s->has_anchor)
      return fail(e, RMR_ERR_UNSUPPORTED, i,
                  "stroke anchor requires placement");
    if (s->point_count && !s->points)
      return fail(e, RMR_ERR_INVALID_ARGUMENT, i, "missing points");
    if (s->point_count > l.maximum_points - points)
      return fail(e, RMR_ERR_LIMIT, i, "bounds point limit");
    points += s->point_count;
    /* Erasers, including their coordinates, never participate in bounds. */
    if (s->tool == RM_TOOL_ERASER || s->tool == RM_TOOL_ERASER_AREA) continue;
    for (size_t k = 0; k < s->point_count; k++) {
      rm_stroke_point p = s->points[k];
      if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.width) ||
          p.width < 0 || fabs(p.x) > l.maximum_coordinate ||
          fabs(p.y) > l.maximum_coordinate)
        return fail(e, RMR_ERR_INVALID_ARGUMENT, i, "invalid bounds point");
      double width = rmr_line_width(rmr_drawn_width(s->tool, p));
      if (width > l.maximum_width)
        return fail(e, RMR_ERR_LIMIT, i, "bounds width limit");
      double x = (double)p.x + w / 2, y = h - (double)p.y;
      leftmost = fmin(leftmost, x);
      rightmost = fmax(rightmost, x);
      deepest = fmin(deepest, y);
      highest = fmax(highest, y);
      include(&bounds, x, y, edge == RMR_EDGE_CENTRELINE ? 0 : width / 2);
    }
  }

  if (page->image_count > (l.maximum_points - points) / 4)
    return fail(e, RMR_ERR_LIMIT, 0, "bounds image vertex limit");
  for (size_t i = 0; i < page->image_count; i++) {
    const rm_page_image* im = &page->images[i];
    if (im->has_anchor)
      return fail(e, RMR_ERR_UNSUPPORTED, i, "image anchor requires placement");
    for (size_t k = 0; k < 4; k++) {
      double x = im->vertices[k].x, y = im->vertices[k].y;
      if (!isfinite(x) || !isfinite(y) || fabs(x) > l.maximum_coordinate ||
          fabs(y) > l.maximum_coordinate)
        return fail(e, RMR_ERR_INVALID_ARGUMENT, i,
                    "invalid bounds image vertex");
      x += w / 2;
      y = h - y;
      leftmost = fmin(leftmost, x);
      rightmost = fmax(rightmost, x);
      deepest = fmin(deepest, y);
      highest = fmax(highest, y);
      include(&bounds, x, y, 0);
    }
  }

  if (text) {
    if (!isfinite(text->origin_x) || !isfinite(text->origin_y) ||
        !isfinite(text->bottom) ||
        fabs(text->origin_x) > l.maximum_coordinate ||
        fabs(text->origin_y) > l.maximum_coordinate ||
        fabs(text->bottom) > l.maximum_coordinate ||
        (text->has_column_width &&
         (!isfinite(text->column_width) || text->column_width <= 0 ||
          fabs(text->origin_x + text->column_width) > l.maximum_coordinate)))
      return fail(e, RMR_ERR_INVALID_ARGUMENT, 0, "invalid text extent");
    include(&bounds, text->origin_x + w / 2, h - text->origin_y, 0);
    if (text->has_column_width) {
      leftmost = fmin(leftmost, text->origin_x + w / 2);
      rightmost = fmax(rightmost, text->origin_x + text->column_width + w / 2);
    }
    double bottom = h - text->bottom;
    if (edge == RMR_EDGE_RIBBON)
      include(&bounds, bounds.x, bottom < -25 ? bottom - 25 : fmax(bottom, 0),
              0);
    else if (edge == RMR_EDGE_UNCUT) {
      if (bottom < 0) include(&bounds, bounds.x, bottom - 25, 0);
    } else
      include(&bounds, bounds.x, bottom, 0);
  }

  bool squared = edge != RMR_EDGE_CENTRELINE && (leftmost < 0 || rightmost > w);
  if (squared) {
    double side = fmax(w, h);
    bounds.x = (w - side) / 2;
    bounds.width = side;
  }

  if (edge == RMR_EDGE_RIBBON && isfinite(deepest)) {
    if (deepest < -47) {
      bounds.y -= 47;
      bounds.height += 47;
    } else if (bounds.y < 0) {
      bounds.height -= -bounds.y;
      bounds.y = 0;
    }
  }

  if (edge == RMR_EDGE_UNCUT && deepest < 0) {
    bounds.y -= 47;
    bounds.height += 47;
  }

  if (edge == RMR_EDGE_RIBBON) {
    if (!squared && isfinite(leftmost) && leftmost >= 0 && bounds.x < 0) {
      bounds.width -= -bounds.x;
      bounds.x = 0;
    }
    if (!squared && isfinite(rightmost) && rightmost <= w &&
        bounds.x + bounds.width > w)
      bounds.width -= (bounds.x + bounds.width) - w;
    if (isfinite(highest) && highest <= h && bounds.y + bounds.height > h)
      bounds.height -= (bounds.y + bounds.height) - h;
  }

  if (edge == RMR_EDGE_CENTRELINE) {
    if (bounds.x < 0) {
      bounds.x -= margin;
      bounds.width += margin;
    }
    if (bounds.x + bounds.width > w) bounds.width += margin;
    if (bounds.y < 0) {
      bounds.y -= margin;
      bounds.height += margin;
    }
    if (bounds.y + bounds.height > h) bounds.height += margin;
  }

  rmr_rect result = {bounds.x - w / 2, h - (bounds.y + bounds.height),
                     bounds.width, bounds.height};
  if (fabs(result.x) > l.maximum_coordinate ||
      fabs(result.x + result.width) > l.maximum_coordinate ||
      fabs(result.y) > l.maximum_coordinate ||
      fabs(result.y + result.height) > l.maximum_coordinate)
    return fail(e, RMR_ERR_LIMIT, 0, "grown page coordinate limit");
  *out = result;
  return RMR_OK;
}

rmr_status rmr_lines_content_bounds(const rm_page* page, double w, double h,
                                    rmr_ink_edge edge, double margin,
                                    const rmr_limits* limits, rmr_rect* out,
                                    rmr_error* e) {
  if (page && (page->has_text || page->highlight_count))
    return fail(e, RMR_ERR_UNSUPPORTED, 0,
                "text/highlights require bounds providers");
  return rmr_lines_content_bounds_with_text(page, w, h, edge, margin, nullptr,
                                            limits, out, e);
}

rmr_status rmr_lines_page_plan(const rm_page* page, double w, double h,
                               rmr_ink_edge edge, double margin,
                               rmr_asset_resolve_fn resolve, void* context,
                               const rmr_unit_raster* raster,
                               const rmr_limits* limits, rmr_plan** out,
                               rmr_error* e) {
  if (!out) return fail(e, RMR_ERR_INVALID_ARGUMENT, 0, "missing plan output");

  rmr_rect viewport;
  rmr_status s =
      rmr_lines_content_bounds(page, w, h, edge, margin, limits, &viewport, e);
  if (s != RMR_OK) return s;
  return rmr_lines_scene_plan(page, viewport, resolve, context, raster, limits,
                              out, e);
}
