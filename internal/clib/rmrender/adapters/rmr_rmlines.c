/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <math.h>
#include <rmrender/rmlines.h>
#include <stdio.h>
#include <stdlib.h>

#include "rmr_width.h"

/* Fill *e (if given) with status s, item index and message, and return s. */
static rmr_status error(rmr_error* e, rmr_status s, size_t index,
                        const char* message) {
  if (e) {
    *e = (rmr_error){.code = s, .index = index};
    snprintf(e->message, sizeof e->message, "%s", message);
  }
  return s;
}

/* True if t is one of the highlighter tool variants. */
static bool highlighter(rm_pen_tool t) {
  return t == RM_TOOL_HIGHLIGHTER_1 || t == RM_TOOL_HIGHLIGHTER_2;
}

/* Line cap for s: round for non-highlighters, butt for a highlighter stroke
 * that moves, square for one collapsed to a single point. */
static rmr_cap ending(const rm_stroke* s) {
  if (!highlighter(s->tool)) return RMR_CAP_ROUND;
  for (size_t i = 1; i < s->point_count; i++)
    if (s->points[i].x != s->points[0].x || s->points[i].y != s->points[0].y)
      return RMR_CAP_BUTT;
  return RMR_CAP_SQUARE;
}

/* The grain family a tool is rendered with. */
static rmr_grain grain(rm_pen_tool t) {
  switch (t) {
    case RM_TOOL_PENCIL_1:
      return RMR_GRAIN_STAMP;
    case RM_TOOL_PENCIL_2:
    case RM_TOOL_MECHANICAL_PENCIL_2:
      return RMR_GRAIN_NOISE;
    case RM_TOOL_PAINTBRUSH_2:
      return RMR_GRAIN_BRISTLE;
    case RM_TOOL_BALLPOINT_1:
      return RMR_GRAIN_SCRATCH;
    case RM_TOOL_BALLPOINT_2:
      return RMR_GRAIN_STREAK;
    default:
      return RMR_GRAIN_SOLID;
  }
}

/* The RGBA color for pen color c, substituting a lighter shade for a few
 * colors when the stroke darkens (highlighter/shader) rather than paints. */
static rmr_color palette(rm_pen_color c, bool darkens) {
  static const rmr_color colors[] = {{0, 0, 0, 1},
                                     {144.0 / 255, 144.0 / 255, 144.0 / 255, 1},
                                     {1, 1, 1, 1},
                                     {0.98, 0.87, 0.16, 1},
                                     {0.16, 0.72, 0.35, 1},
                                     {0.95, 0.44, 0.7, 1},
                                     {48.0 / 255, 74.0 / 255, 224.0 / 255, 1},
                                     {194.0 / 255, 49.0 / 255, 50.0 / 255, 1},
                                     {0.5, 0.5, 0.5, 1},
                                     {0.98, 0.87, 0.16, 1},
                                     {145.0 / 255, 218.0 / 255, 113.0 / 255, 1},
                                     {116.0 / 255, 210.0 / 255, 232.0 / 255, 1},
                                     {192.0 / 255, 127.0 / 255, 210.0 / 255, 1},
                                     {250.0 / 255, 231.0 / 255, 25.0 / 255, 1},
                                     {190.0 / 255, 234.0 / 255, 254.0 / 255, 1},
                                     {1, 195.0 / 255, 140.0 / 255, 1}};

  if (darkens) {
    if (c == RM_COLOR_YELLOW || c == RM_COLOR_YELLOW_2 ||
        c == RM_COLOR_HIGHLIGHT)
      return (rmr_color){254.0 / 255, 253.0 / 255, 96.0 / 255, 1};
    if (c == RM_COLOR_GREEN || c == RM_COLOR_GREEN_2)
      return (rmr_color){169.0 / 255, 250.0 / 255, 92.0 / 255, 1};
    if (c == RM_COLOR_PINK) return (rmr_color){1, 85.0 / 255, 207.0 / 255, 1};
    if (c == RM_COLOR_GRAY_OVERLAP) return (rmr_color){0, 0, 0, 1};
  }

  return colors[c];
}

rmr_status rmr_lines_style(const rm_stroke* s, rmr_stroke_style* out,
                           rmr_error* e) {
  if (!s || !out || (!s->points && s->point_count) ||
      s->color < RM_COLOR_BLACK || s->color > RM_COLOR_HIGHLIGHTER_ORANGE)
    return error(e, RMR_ERR_INVALID_ARGUMENT, 0, "invalid stroke");

  bool darkens = highlighter(s->tool) || s->tool == RM_TOOL_SHADER;
  rmr_stroke_style result = {
      .color = palette(s->color, darkens),
      .blend = darkens ? RMR_BLEND_DARKEN : RMR_BLEND_NORMAL,
      .cap = ending(s),
      .join = highlighter(s->tool) ? RMR_JOIN_MITER : RMR_JOIN_ROUND,
      .grain = grain(s->tool),
      .stroked = highlighter(s->tool) || s->tool == RM_TOOL_FINELINER_1 ||
                 s->tool == RM_TOOL_FINELINER_2,
      .eraser = s->tool == RM_TOOL_ERASER || s->tool == RM_TOOL_ERASER_AREA};

  float sum = 0;
  bool varying = false;
  for (size_t i = 0; i < s->point_count; i++) {
    sum += rmr_drawn_width(s->tool, s->points[i]);
    if (s->points[i].width != s->points[0].width) varying = true;
  }

  if (s->tool == RM_TOOL_UNKNOWN) result.stroked = !varying;
  result.line_width = s->point_count
                          ? rmr_line_width((double)sum / (double)s->point_count)
                          : 0.7;
  result.color.alpha = s->tool == RM_TOOL_SHADER ||
                               (darkens && s->color == RM_COLOR_GRAY_OVERLAP)
                           ? 0.25098
                           : 1;

  if (s->has_argb) {
    result.color.red = (double)((s->argb >> 16) & 255) / 255;
    result.color.green = (double)((s->argb >> 8) & 255) / 255;
    result.color.blue = (double)(s->argb & 255) / 255;
    unsigned int alpha = s->argb >> 24;
    if (alpha) result.color.alpha = (double)alpha / 255;
  }

  *out = result;
  return RMR_OK;
}

rmr_status rmr_lines_outline(const rm_stroke* s, const rmr_limits* given,
                             rmr_path* out, rmr_error* e) {
  rmr_limits l = given ? *given : rmr_limits_default();
  if (!s || !out || (!s->points && s->point_count))
    return error(e, RMR_ERR_INVALID_ARGUMENT, 0, "invalid stroke");

  if (s->point_count > l.maximum_points ||
      s->point_count > l.maximum_plan_bytes / sizeof(rmr_sample))
    return error(e, RMR_ERR_LIMIT, 0, "stroke points limit");

  rmr_sample* points =
      calloc(s->point_count ? s->point_count : 1, sizeof *points);
  if (!points)
    return error(e, RMR_ERR_OUT_OF_MEMORY, 0, "stroke allocation failed");

  for (size_t i = 0; i < s->point_count; i++)
    points[i] =
        (rmr_sample){{s->points[i].x, s->points[i].y},
                     rmr_line_width(rmr_drawn_width(s->tool, s->points[i]))};

  l.maximum_plan_bytes -= s->point_count * sizeof *points;
  rmr_status status = rmr_outline(
      points, s->point_count, ending(s),
      highlighter(s->tool) ? RMR_JOIN_MITER : RMR_JOIN_ROUND, &l, out, e);
  free(points);
  return status;
}

rmr_status rmr_lines_mask_with_raster(const rm_stroke* s,
                                      const rmr_unit_raster* raster,
                                      const rmr_limits* given, rmr_mask* out,
                                      rmr_error* e) {
  rmr_limits l = given ? *given : rmr_limits_default();
  if (!s || !out || (!s->points && s->point_count))
    return error(e, RMR_ERR_INVALID_ARGUMENT, 0, "invalid stroke");

  if (grain(s->tool) == RMR_GRAIN_SOLID)
    return error(e, RMR_ERR_UNSUPPORTED, 0, "solid ink has no mask");

  if (raster && (raster->abi_version != RMR_UNIT_RASTER_ABI ||
                 raster->struct_size < sizeof *raster || !raster->render))
    return error(e, RMR_ERR_INVALID_ARGUMENT, 0, "invalid unit rasterizer ABI");

  if (s->point_count > l.maximum_points ||
      s->point_count > l.maximum_plan_bytes / sizeof(rmr_grain_point))
    return error(e, RMR_ERR_LIMIT, 0, "grain point limit");

  if (s->point_count < 2) {
    *out = (rmr_mask){0};
    return RMR_OK;
  }

  rmr_grain_point* points = calloc(s->point_count, sizeof *points);
  if (!points)
    return error(e, RMR_ERR_OUT_OF_MEMORY, 0, "grain points allocation failed");

  for (size_t i = 0; i < s->point_count; i++) {
    rm_stroke_point p = s->points[i];
    points[i] =
        (rmr_grain_point){p.x, p.y, rmr_line_width(rmr_drawn_width(s->tool, p)),
                          p.pressure, p.speed};
  }

  l.maximum_plan_bytes -= s->point_count * sizeof *points;
  rmr_rect box;
  rmr_status status = rmr_grain_box(points, s->point_count, &l, &box, e);
  if (status == RMR_OK)
    status = rmr_grain_mask(points, s->point_count, grain(s->tool), box, raster,
                            &l, out, e);
  free(points);
  return status;
}

typedef struct {
  size_t position;
  rmr_rect bounds;
} erase_record;

/* Bounding box of path's control/end points, or {0} if it is empty. */
static rmr_rect path_box(rmr_path path) {
  double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
  for (size_t i = 0; i < path.count; i++) {
    rmr_element v = path.elements[i];
    rmr_point p[] = {v.end, v.control1, v.control2};
    size_t n = v.kind == RMR_CLOSE ? 0 : v.kind == RMR_CURVE ? 3 : 1;
    for (size_t k = 0; k < n; k++) {
      x0 = fmin(x0, p[k].x);
      y0 = fmin(y0, p[k].y);
      x1 = fmax(x1, p[k].x);
      y1 = fmax(y1, p[k].y);
    }
  }

  return isfinite(x0) ? (rmr_rect){x0, y0, x1 - x0, y1 - y0} : (rmr_rect){0};
}

/* s's point bounds, padded by half its widest drawn width (more for
 * highlighters/square caps), for eraser-intersection testing. */
static rmr_rect stroke_reach(const rm_stroke* s, rmr_cap cap) {
  double widest = 0, x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY,
         y1 = -INFINITY;
  for (size_t i = 0; i < s->point_count; i++) {
    rm_stroke_point p = s->points[i];
    widest = fmax(widest, rmr_drawn_width(s->tool, p));
    x0 = fmin(x0, p.x);
    x1 = fmax(x1, p.x);
    y0 = fmin(y0, p.y);
    y1 = fmax(y1, p.y);
  }

  double pad = widest / 4 / 2 *
                   (highlighter(s->tool)    ? 2
                    : cap == RMR_CAP_SQUARE ? sqrt(2)
                                            : 1) +
               1;

  return isfinite(x0) ? (rmr_rect){x0 - pad, y0 - pad, x1 - x0 + 2 * pad,
                                   y1 - y0 + 2 * pad}
                      : (rmr_rect){0};
}

/* True if nonempty rects a and b overlap. */
static bool intersects(rmr_rect a, rmr_rect b) {
  return a.width > 0 && a.height > 0 && b.width > 0 && b.height > 0 &&
         a.x <= b.x + b.width && b.x <= a.x + a.width &&
         a.y <= b.y + b.height && b.y <= a.y + a.height;
}

/* True if a and b are both multi-point shader strokes from the same source
 * (author/counter), so they belong in the same compositing group. */
static bool same_source(const rm_stroke* a, const rm_stroke* b) {
  return a->tool == RM_TOOL_SHADER && a->has_source_id && a->point_count > 1 &&
         b->tool == RM_TOOL_SHADER && b->has_source_id && b->point_count > 1 &&
         a->source_id.author == b->source_id.author &&
         a->source_id.counter == b->source_id.counter;
}

/* Like rmr_lines_plan, but accepts a unit raster for grain families that
 * need one (validated, not invoked); pass nullptr for all-grain support
 * without a raster. */
rmr_status rmr_lines_plan_with_raster(const rm_stroke* strokes, size_t count,
                                      rmr_rect viewport,
                                      const rmr_unit_raster* raster,
                                      const rmr_limits* given, rmr_plan** out,
                                      rmr_error* e) {
  rmr_limits l = given ? *given : rmr_limits_default();
  if (!out || (!strokes && count))
    return error(e, RMR_ERR_INVALID_ARGUMENT, 0, "invalid strokes");

  if (count > l.maximum_draws ||
      count > l.maximum_plan_bytes /
                  (sizeof(rmr_draw) + sizeof(rmr_path) + sizeof(rmr_mask) +
                   sizeof(erase_record) + sizeof(rmr_rect)))
    return error(e, RMR_ERR_LIMIT, 0, "draw count limit");

  /* Preflight every unsupported feature before allocating or producing a plan.
   */
  size_t total_points = 0;
  for (size_t i = 0; i < count; i++) {
    const rm_stroke* s = &strokes[i];
    if (s->point_count > l.maximum_points - total_points)
      return error(e, RMR_ERR_LIMIT, i, "point count limit");
    total_points += s->point_count;
    if (raster && grain(s->tool) != RMR_GRAIN_SOLID) {
      if (raster->abi_version != RMR_UNIT_RASTER_ABI ||
          raster->struct_size < sizeof *raster || !raster->render)
        return error(e, RMR_ERR_INVALID_ARGUMENT, i,
                     "invalid unit rasterizer ABI");
    }
    if (s->has_anchor)
      return error(e, RMR_ERR_UNSUPPORTED, i, "text anchors await placement");
  }

  if (!count) return rmr_plan_create(nullptr, 0, viewport, &l, out, e);

  rmr_draw* draws = calloc(count ? count : 1, sizeof *draws);
  rmr_path* paths = calloc(count ? count : 1, sizeof *paths);
  rmr_mask* masks = calloc(count, sizeof *masks);
  erase_record* erasers = calloc(count, sizeof *erasers);
  rmr_rect* reaches = calloc(count, sizeof *reaches);
  rmr_path_view* cuts = nullptr;
  if (!draws || !paths || !masks || !erasers || !reaches) {
    free(erasers);
    free(reaches);
    free(masks);
    free(draws);
    free(paths);
    return error(e, RMR_ERR_OUT_OF_MEMORY, 0, "draw allocation failed");
  }

  rmr_status status = RMR_OK;
  size_t used_bytes = count * (sizeof *draws + sizeof *paths + sizeof *masks +
                               sizeof *erasers + sizeof *reaches),
         used_elements = 0;
  for (size_t i = 0; i < count; i++) {
    const rm_stroke* s = &strokes[i];
    rmr_stroke_style style;
    status = rmr_lines_style(s, &style, e);
    if (status != RMR_OK) break;
    if (style.eraser && (s->tool == RM_TOOL_ERASER_AREA || s->point_count < 2))
      continue;

    if (style.stroked && s->point_count > 1) {
      if (s->point_count > l.maximum_elements - used_elements ||
          s->point_count >
              (l.maximum_plan_bytes - used_bytes) / sizeof(rmr_element)) {
        status = error(e, RMR_ERR_LIMIT, i, "path memory limit");
        break;
      }
      paths[i].elements = calloc(s->point_count, sizeof(rmr_element));
      if (!paths[i].elements) {
        status = error(e, RMR_ERR_OUT_OF_MEMORY, i, "path allocation failed");
        break;
      }
      paths[i].count = s->point_count;
      for (size_t j = 0; j < s->point_count; j++)
        paths[i].elements[j] =
            (rmr_element){.kind = j ? RMR_LINE : RMR_MOVE,
                          .end = {s->points[j].x, s->points[j].y}};
    } else {
      rmr_limits remaining = l;
      remaining.maximum_plan_bytes -= used_bytes;
      remaining.maximum_elements -= used_elements;
      status = rmr_lines_outline(s, &remaining, &paths[i], e);
      if (status != RMR_OK) break;
    }
    used_bytes += paths[i].count * sizeof(rmr_element);
    used_elements += paths[i].count;

    if (style.grain != RMR_GRAIN_SOLID && s->point_count > 1) {
      rmr_limits remaining = l;
      remaining.maximum_plan_bytes -= used_bytes;
      status = rmr_lines_mask_with_raster(s, raster, &remaining, &masks[i], e);
      if (status != RMR_OK) break;
      used_bytes += masks[i].stride * masks[i].height;
    }

    draws[i] =
        (rmr_draw){.path = {paths[i].elements, paths[i].count},
                   .mode = style.stroked ? RMR_STROKE : RMR_FILL,
                   .color = style.color,
                   .blend = style.blend,
                   .line_width = style.line_width,
                   .cap = style.cap,
                   .join = style.join,
                   .miter_limit = 2,
                   .mask = {masks[i].bytes, masks[i].width, masks[i].height,
                            masks[i].stride, masks[i].box}};
    reaches[i] = stroke_reach(s, style.cap);

    if (s->tool == RM_TOOL_SHADER && s->has_source_id && s->point_count > 1) {
      bool continues = i && same_source(&strokes[i - 1], s);
      draws[i].group = continues ? draws[i - 1].group : (uint64_t)i + 1;
      draws[i].group_alpha =
          continues ? draws[i - 1].group_alpha : style.color.alpha;
      draws[i].group_blend = RMR_BLEND_DARKEN;
      draws[i].color.alpha = 1;
      draws[i].blend = RMR_BLEND_NORMAL;
    }
  }

  if (status == RMR_OK) {
    size_t erase_count = 0, reference_count = 0, work = 0;
    for (size_t i = 0; i < count; i++)
      if (strokes[i].tool == RM_TOOL_ERASER) {
        if (paths[i].count)
          erasers[erase_count++] = (erase_record){i, path_box(paths[i])};
        draws[i].path = (rmr_path_view){0};
      }

    for (size_t i = 0; i < count && status == RMR_OK; i++) {
      if (!draws[i].path.count) continue;
      for (size_t k = 0; k < erase_count; k++) {
        if (erasers[k].position <= i) continue;
        if (work++ == l.maximum_elements) {
          status = error(e, RMR_ERR_LIMIT, i, "eraser intersection work limit");
          break;
        }
        if (intersects(reaches[i], erasers[k].bounds)) {
          if (reference_count == l.maximum_elements) {
            status = error(e, RMR_ERR_LIMIT, i, "eraser reference limit");
            break;
          }
          draws[i].eraser_count++;
          reference_count++;
        }
      }
    }

    if (status == RMR_OK && reference_count) {
      if (reference_count > (l.maximum_plan_bytes - used_bytes) / sizeof *cuts)
        status = error(e, RMR_ERR_LIMIT, 0, "eraser reference memory limit");
      else {
        cuts = calloc(reference_count, sizeof *cuts);
        if (!cuts)
          status = error(e, RMR_ERR_OUT_OF_MEMORY, 0,
                         "eraser references allocation failed");
        else {
          used_bytes += reference_count * sizeof *cuts;
          size_t at = 0;
          for (size_t i = 0; i < count; i++)
            if (draws[i].eraser_count) {
              draws[i].erasers = cuts + at;
              for (size_t k = 0; k < erase_count; k++)
                if (erasers[k].position > i &&
                    intersects(reaches[i], erasers[k].bounds)) {
                  rmr_path p = paths[erasers[k].position];
                  cuts[at++] = (rmr_path_view){p.elements, p.count};
                }
            }
        }
      }
    }
  }

  if (status == RMR_OK) {
    rmr_limits remaining = l;
    remaining.maximum_plan_bytes -= used_bytes;
    status = rmr_plan_create(draws, count, viewport, &remaining, out, e);
  }

  for (size_t i = 0; i < count; i++) {
    rmr_path_free(&paths[i]);
    rmr_mask_free(&masks[i]);
  }

  free(cuts);
  free(erasers);
  free(reaches);
  free(masks);
  free(paths);
  free(draws);
  return status;
}

rmr_status rmr_lines_mask(const rm_stroke* stroke, const rmr_limits* limits,
                          rmr_mask* out, rmr_error* error) {
  return rmr_lines_mask_with_raster(stroke, nullptr, limits, out, error);
}

rmr_status rmr_lines_plan(const rm_stroke* strokes, size_t count,
                          rmr_rect viewport, const rmr_limits* limits,
                          rmr_plan** out, rmr_error* error) {
  return rmr_lines_plan_with_raster(strokes, count, viewport, nullptr, limits,
                                    out, error);
}
