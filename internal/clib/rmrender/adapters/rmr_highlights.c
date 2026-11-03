/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <math.h>
#include <rmrender/rmlines.h>
#include <stdio.h>
#include <stdlib.h>

/* Fill *e (if given) with status s and a generic message, and return s. */
static rmr_status fail(rmr_error* e, rmr_status s) {
  if (e) {
    *e = (rmr_error){.code = s};
    snprintf(e->message, sizeof e->message,
             "invalid highlight geometry or resource limit");
  }
  return s;
}

/* Opaque fill color for a highlight, derived from its stroke color/ARGB,
 * with the legacy yellow family remapped to a fixed highlight yellow. */
rmr_status rmr_lines_highlight_color(const rm_highlight* h, rmr_color* out,
                                     rmr_error* e) {
  if (!h || !out) return fail(e, RMR_ERR_INVALID_ARGUMENT);

  rm_stroke palette = {.tool = RM_TOOL_FINELINER_2,
                       .color = h->color,
                       .has_argb = h->has_argb,
                       .argb = h->argb};
  rmr_stroke_style style;
  rmr_status s = rmr_lines_style(&palette, &style, e);
  if (s != RMR_OK) return s;

  rmr_color color = style.color;
  color.alpha = 1;
  if (!h->has_argb &&
      (h->color == RM_COLOR_YELLOW || h->color == RM_COLOR_YELLOW_2 ||
       h->color == RM_COLOR_HIGHLIGHT))
    color = (rmr_color){1, .996, .356, 1};

  *out = color;
  return RMR_OK;
}

rmr_status rmr_lines_highlight_plan(const rm_highlight* highlights,
                                    size_t count, rmr_rect viewport,
                                    const rmr_limits* given, rmr_plan** out,
                                    rmr_error* e) {
  if (!out || (!highlights && count)) return fail(e, RMR_ERR_INVALID_ARGUMENT);

  rmr_limits l = given ? *given : rmr_limits_default();
  if (count > l.maximum_elements) return fail(e, RMR_ERR_LIMIT);

  size_t rectangles = 0;
  for (size_t i = 0; i < count; i++) {
    if (!highlights[i].rectangles && highlights[i].rectangle_count)
      return fail(e, RMR_ERR_INVALID_ARGUMENT);
    if (highlights[i].rectangle_count > l.maximum_draws - rectangles)
      return fail(e, RMR_ERR_LIMIT);
    rectangles += highlights[i].rectangle_count;
  }

  if (rectangles > l.maximum_elements / 5 ||
      rectangles >
          l.maximum_plan_bytes / (sizeof(rmr_draw) + 5 * sizeof(rmr_element)))
    return fail(e, RMR_ERR_LIMIT);

  size_t scratch = rectangles * (sizeof(rmr_draw) + 5 * sizeof(rmr_element));
  rmr_draw* draws = rectangles ? calloc(rectangles, sizeof *draws) : nullptr;
  rmr_element* paths =
      rectangles ? calloc(rectangles * 5, sizeof *paths) : nullptr;
  if (rectangles && (!draws || !paths)) {
    free(draws);
    free(paths);
    return fail(e, RMR_ERR_OUT_OF_MEMORY);
  }

  size_t at = 0;
  rmr_status s = RMR_OK;
  for (size_t i = 0; i < count && s == RMR_OK; i++) {
    const rm_highlight* h = &highlights[i];
    rmr_color color;
    s = rmr_lines_highlight_color(h, &color, e);
    if (s != RMR_OK) break;
    for (size_t j = 0; j < h->rectangle_count; j++) {
      rm_rect r = h->rectangles[j];
      if (!isfinite(r.width) || !isfinite(r.height) || r.width < 0 ||
          r.height < 0) {
        s = RMR_ERR_INVALID_ARGUMENT;
        break;
      }
      rmr_element* p = paths + at * 5;
      p[0] = (rmr_element){.kind = RMR_MOVE, .end = {r.x, r.y}};
      p[1] = (rmr_element){.kind = RMR_LINE, .end = {r.x + r.width, r.y}};
      p[2] = (rmr_element){.kind = RMR_LINE,
                           .end = {r.x + r.width, r.y + r.height}};
      p[3] = (rmr_element){.kind = RMR_LINE, .end = {r.x, r.y + r.height}};
      p[4] = (rmr_element){.kind = RMR_CLOSE};
      draws[at++] = (rmr_draw){.mode = RMR_FILL,
                               .path = {p, 5},
                               .color = color,
                               .blend = RMR_BLEND_MULTIPLY};
    }
  }

  if (s == RMR_OK) {
    l.maximum_plan_bytes -= scratch;
    s = rmr_plan_create(draws, rectangles, viewport, &l, out, e);
  } else
    fail(e, s);
  free(draws);
  free(paths);
  return s;
}
