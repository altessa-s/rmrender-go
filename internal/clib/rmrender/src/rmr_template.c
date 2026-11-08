/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <float.h>
#include <rmrender/template.h>

#include "rmr_internal.h"

typedef struct {
  rmr_limits limits;
  const rmr_text_provider* provider;
  rmr_point canvas, extent;
  rmr_binding bindings[256];
  size_t binding_count, work, draw_count, element_count, allocated;
  rmr_draw* draws;
  rmr_element* elements;
  rmr_error* error;
} builder;

/* Fails with status s and message, indexed by the builder's work count
 * so far. */
static rmr_status fail(builder* b, rmr_status s, const char* message) {
  return rmr_fail(b->error, s, b->work, message);
}

/* Adds cost to the builder's work counter, failing with LIMIT if that
 * would exceed maximum_elements. */
static rmr_status charge(builder* b, size_t cost) {
  if (cost > b->limits.maximum_elements - b->work)
    return fail(b, RMR_ERR_LIMIT, "template work limit");
  b->work += cost;
  return RMR_OK;
}

/* Resolves a template value to a number, evaluating its expression
 * against the builder's bindings if one is present. */
static rmr_status value(builder* b, rmr_template_value v, double* out) {
  rmr_status s =
      charge(b, v.expression ? rmr_expression_cost(v.expression) : 1);
  if (s != RMR_OK) return s;
  if (!v.expression) {
    *out = v.number;
    return RMR_OK;
  }

  if (rmr_expression_stack_bytes(v.expression) >
      b->limits.maximum_plan_bytes - b->allocated)
    return fail(b, RMR_ERR_LIMIT, "template expression stack limit");
  return rmr_expression_evaluate(v.expression, b->bindings, b->binding_count,
                                 out, b->error);
}

/* True if name is a valid ASCII identifier ([A-Za-z_] then
 * [A-Za-z_0-9]*) of plausible length. */
static bool name_valid(const char* name, size_t length) {
  if (!name || !length || length > 65536) return false;
  for (size_t i = 0; i < length; i++) {
    unsigned char c = (unsigned char)name[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
          (i && c >= '0' && c <= '9')))
      return false;
  }
  return true;
}

/* Binds name to number, overwriting an existing binding of the same name
 * or appending a new one. */
static void bind(builder* b, const char* name, size_t length, double number) {
  for (size_t i = 0; i < b->binding_count; i++) {
    rmr_binding* v = &b->bindings[i];
    if (v->length == length && memcmp(v->name, name, length) == 0) {
      v->value = number;
      return;
    }
  }
  b->bindings[b->binding_count++] = (rmr_binding){name, length, number};
}

/* Resets b's bindings to the built-in canvas constants, then evaluates
 * and binds t's named constants in order. */
static rmr_status environment(builder* b, const rmr_template* t) {
  b->binding_count = 0;
  bind(b, "templateWidth", 13, b->canvas.x);
  bind(b, "templateHeight", 14, b->canvas.y);
  bind(b, "parentWidth", 11, b->canvas.x);
  bind(b, "parentHeight", 12, b->canvas.y);
  bind(b, "paperOriginX", 12, (b->canvas.x - b->canvas.y) / 2);
  bind(b, "paperOriginY", 12, 0);

  for (size_t i = 0; i < t->constant_count; i++) {
    const rmr_template_constant* c = &t->constants[i];
    if (!name_valid(c->name, c->length))
      return fail(b, RMR_ERR_INVALID_ARGUMENT,
                  "invalid template constant name");
    double number;
    rmr_status s = value(b, c->value, &number);
    if (s != RMR_OK) return s;
    bind(b, c->name, c->length, number);
  }

  return RMR_OK;
}

/* Resolves repeat into a repetition count and signed step direction for
 * one axis, clamped to [1, 10000]. */
static rmr_status repetitions(builder* b, rmr_template_repeat repeat,
                              double step, double start, double extent,
                              size_t* count, double* direction) {
  *count = 1;
  *direction = 1;
  if (repeat.kind > RMR_REPEAT_BACKWARD)
    return fail(b, RMR_ERR_INVALID_ARGUMENT, "invalid repetition kind");
  if (repeat.kind == RMR_REPEAT_ONCE || step <= 0) return RMR_OK;

  double n;
  if (repeat.kind == RMR_REPEAT_COUNT) {
    rmr_status s = value(b, repeat.count, &n);
    if (s != RMR_OK) return s;
  } else if (repeat.kind == RMR_REPEAT_FORWARD)
    n = ceil((extent - start) / step);
  else {
    n = ceil(start / step) + 1;
    *direction = -1;
  }

  if (isfinite(n)) *count = (size_t)fmin(fmax(trunc(n), 1), 10000);
  return RMR_OK;
}

/* Resolves x/y to canvas coordinates relative to origin, failing if the
 * result is outside the builder's coordinate limit. */
static rmr_status point(builder* b, rmr_template_value x, rmr_template_value y,
                        rmr_point origin, rmr_point* out) {
  double px, py;
  rmr_status s = value(b, x, &px);
  if (s != RMR_OK) return s;
  s = value(b, y, &py);
  if (s != RMR_OK) return s;

  *out = (rmr_point){origin.x + px - b->canvas.x / 2, origin.y + py};
  return rmr_point_valid(*out, b->limits.maximum_coordinate)
             ? RMR_OK
             : fail(b, RMR_ERR_INVALID_ARGUMENT, "invalid template coordinate");
}

/* Builds one path draw command from p, validating and resolving each
 * element's coordinates relative to origin. */
static rmr_status path(builder* b, const rmr_template_path* p,
                       rmr_point origin) {
  if (p->count && !p->elements)
    return fail(b, RMR_ERR_INVALID_ARGUMENT, "missing template path elements");
  if (p->count > b->limits.maximum_elements - b->element_count)
    return fail(b, RMR_ERR_LIMIT, "template element limit");
  if (!p->count) return RMR_OK;
  if (b->draw_count == b->limits.maximum_draws)
    return fail(b, RMR_ERR_LIMIT, "template draw limit");

  bool stroked = !p->has_fill || p->has_stroke_width || p->has_stroke_color;
  rmr_draw d = {
      .mode = p->has_fill ? (stroked ? RMR_FILL_STROKE : RMR_FILL) : RMR_STROKE,
      .color = p->fill,
      .cap = RMR_CAP_SQUARE,
      .join = RMR_JOIN_MITER,
      .line_width = .5};
  rmr_color stroke = p->has_stroke_color ? p->stroke
                     : p->has_stroke_width
                         ? (rmr_color){0, 0, 0, 1}
                         : (rmr_color){.752941, .752941, .752941, 1};

  if (stroked && p->has_stroke_width) {
    rmr_status s = value(b, p->stroke_width, &d.line_width);
    if (s != RMR_OK) return s;
  }

  if ((p->has_fill && !rmr_color_valid(p->fill)) ||
      (stroked &&
       (!rmr_color_valid(stroke) || !isfinite(d.line_width) ||
        d.line_width <= 0 || d.line_width > b->limits.maximum_width)))
    return fail(b, RMR_ERR_INVALID_ARGUMENT, "invalid template paint");

  if (!p->has_fill) d.color = stroke;
  d.stroke_color = stroke;
  bool open = false;
  size_t start = b->element_count;

  for (size_t i = 0; i < p->count; i++) {
    const rmr_template_element* v = &p->elements[i];
    rmr_status s = charge(b, 1);
    if (s != RMR_OK) return s;
    if (v->kind > RMR_CLOSE || (v->kind != RMR_MOVE && !open))
      return fail(b, RMR_ERR_INVALID_ARGUMENT,
                  "invalid template path structure");

    rmr_element e = {.kind = v->kind};
    if (v->kind == RMR_MOVE) open = true;
    if (v->kind == RMR_CLOSE)
      open = false;
    else {
      s = point(b, v->x, v->y, origin, &e.end);
      if (s != RMR_OK) return s;
    }

    if (v->kind == RMR_CURVE) {
      s = point(b, v->control1_x, v->control1_y, origin, &e.control1);
      if (s != RMR_OK) return s;
      s = point(b, v->control2_x, v->control2_y, origin, &e.control2);
      if (s != RMR_OK) return s;
    }

    if (b->elements) b->elements[b->element_count] = e;
    b->element_count++;
  }

  if (b->draws) {
    d.path = (rmr_path_view){b->elements + start, p->count};
    b->draws[b->draw_count] = d;
  }
  b->draw_count++;
  return RMR_OK;
}

/* Builds one text draw command from t, measuring it through the
 * builder's provider and temporarily binding textWidth for position. */
static rmr_status text_item(builder* b, const rmr_template_text* t,
                            rmr_point origin) {
  if (!b->provider)
    return fail(b, RMR_ERR_UNSUPPORTED, "text metrics provider required");
  if (t->size > 65536) return fail(b, RMR_ERR_LIMIT, "text byte limit");
  if ((!t->utf8 && t->size) || !rmr_utf8_valid(t->utf8, t->size))
    return fail(b, RMR_ERR_INVALID_ARGUMENT, "invalid text UTF-8");

  rmr_status s = charge(b, t->size + 1);
  if (s != RMR_OK) return s;
  if (b->draw_count == b->limits.maximum_draws)
    return fail(b, RMR_ERR_LIMIT, "template draw limit");

  double size = 24;
  if (t->has_font_size) {
    s = value(b, t->font_size, &size);
    if (s != RMR_OK) return s;
  }
  if (!isfinite(size) || size <= 0 || size > b->limits.maximum_width)
    return fail(b, RMR_ERR_INVALID_ARGUMENT, "invalid template font size");

  rmr_text_metrics m = {0};
  s = b->provider->measure(b->provider->context, t->utf8, t->size, size, &m);
  if (s != RMR_OK) return fail(b, s, "text measurement failed");

  if (!isfinite(m.width) || m.width < 0 ||
      m.width > b->limits.maximum_coordinate || !isfinite(m.bounds.width) ||
      !isfinite(m.bounds.height) || m.bounds.width < 0 || m.bounds.height < 0 ||
      !rmr_point_valid((rmr_point){m.bounds.x, m.bounds.y},
                       b->limits.maximum_coordinate) ||
      !rmr_point_valid((rmr_point){m.bounds.x + m.bounds.width,
                                   m.bounds.y + m.bounds.height},
                       b->limits.maximum_coordinate))
    return fail(b, RMR_ERR_BACKEND, "invalid text metrics");

  size_t saved_count = b->binding_count, slot = saved_count;
  for (size_t i = 0; i < saved_count; i++)
    if (b->bindings[i].length == 9 &&
        !memcmp(b->bindings[i].name, "textWidth", 9)) {
      slot = i;
      break;
    }
  if (slot == 256) return fail(b, RMR_ERR_LIMIT, "textWidth binding limit");
  rmr_binding saved = slot < saved_count ? b->bindings[slot] : (rmr_binding){0};
  bind(b, "textWidth", 9, m.width);

  rmr_point position;
  s = point(b, t->position.x, t->position.y, origin, &position);
  /* Resolve the whole position box, even though only origin is used. */
  double ignored;
  if (s == RMR_OK) s = value(b, t->position.width, &ignored);
  if (s == RMR_OK) s = value(b, t->position.height, &ignored);
  b->binding_count = saved_count;
  if (slot < saved_count) b->bindings[slot] = saved;
  if (s != RMR_OK) return s;

  if (b->draws)
    b->draws[b->draw_count] =
        (rmr_draw){.mode = RMR_TEXT,
                   .color = {0, 0, 0, 1},
                   .text = {t->utf8, t->size, size, position, m.bounds}};
  b->draw_count++;
  return RMR_OK;
}

/* Recursively builds draw commands for list, descending into repeated
 * groups up to a nesting depth of 32. */
static rmr_status items(builder* b, const rmr_template_item* list, size_t count,
                        rmr_point origin, unsigned depth) {
  if (count && !list)
    return fail(b, RMR_ERR_INVALID_ARGUMENT, "missing template items");
  if (count > b->limits.maximum_elements - b->work)
    return fail(b, RMR_ERR_LIMIT, "template item limit");

  for (size_t i = 0; i < count; i++) {
    rmr_status s = charge(b, 1);
    if (s != RMR_OK) return s;
    const rmr_template_item* item = &list[i];

    if (item->kind == RMR_TEMPLATE_TEXT) {
      s = text_item(b, &item->text, origin);
      if (s != RMR_OK) return s;
      continue;
    }

    if (item->kind == RMR_TEMPLATE_PATH) {
      s = path(b, &item->path, origin);
      if (s != RMR_OK) return s;
      continue;
    }

    if (item->kind != RMR_TEMPLATE_GROUP)
      return fail(b, RMR_ERR_UNSUPPORTED, "unsupported template item");
    if (depth == 32)
      return fail(b, RMR_ERR_LIMIT, "template group nesting limit");

    const rmr_template_group* g = &item->group;
    double x, y, w, h;
    s = value(b, g->box.x, &x);
    if (s != RMR_OK) return s;
    s = value(b, g->box.y, &y);
    if (s != RMR_OK) return s;
    s = value(b, g->box.width, &w);
    if (s != RMR_OK) return s;
    s = value(b, g->box.height, &h);
    if (s != RMR_OK) return s;

    double parent_w = b->bindings[2].value, parent_h = b->bindings[3].value;
    if (w <= 0) w = parent_w;
    if (h <= 0) h = parent_h;
    rmr_point start = {origin.x + x, origin.y + y};
    if (!isfinite(w) || !isfinite(h) ||
        fabs(w) > b->limits.maximum_coordinate ||
        fabs(h) > b->limits.maximum_coordinate ||
        !rmr_point_valid(start, b->limits.maximum_coordinate))
      return fail(b, RMR_ERR_INVALID_ARGUMENT, "invalid template group box");

    size_t rows, columns;
    double dy, dx;
    s = repetitions(b, g->rows, h, start.y, b->extent.y, &rows, &dy);
    if (s != RMR_OK) return s;
    s = repetitions(b, g->columns, w, start.x, b->extent.x, &columns, &dx);
    if (s != RMR_OK) return s;

    b->bindings[2].value = w;
    b->bindings[3].value = h;

    for (size_t row = 0; row < rows; row++)
      for (size_t column = 0; column < columns; column++) {
        /* Charge even empty repeated groups, so nested empty cells are bounded.
         */
        s = charge(b, 1);
        if (s != RMR_OK) return s;
        rmr_point cell = {start.x + (double)column * w * dx,
                          start.y + (double)row * h * dy};
        s = items(b, g->children, g->count, cell, depth + 1);
        if (s != RMR_OK) return s;
      }

    b->bindings[2].value = parent_w;
    b->bindings[3].value = parent_h;
  }

  return RMR_OK;
}

rmr_status rmr_template_plan_with_text(const rmr_template* t, rmr_point canvas,
                                       rmr_point extent, rmr_rect viewport,
                                       const rmr_text_provider* provider,
                                       const rmr_limits* given, rmr_plan** out,
                                       rmr_error* e) {
  if (provider &&
      (provider->abi_version != RMR_TEXT_PROVIDER_ABI ||
       provider->struct_size < sizeof *provider || !provider->measure))
    return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, 0,
                    "invalid text provider ABI");

  rmr_limits l = given ? *given : rmr_limits_default();
  if (!t || !out || (t->constant_count && !t->constants) ||
      !isfinite(l.maximum_coordinate) || l.maximum_coordinate <= 0 ||
      l.maximum_coordinate > 1e12 || !isfinite(l.maximum_width) ||
      l.maximum_width <= 0 || l.maximum_width > 1e6 ||
      !rmr_point_valid(canvas, l.maximum_coordinate) ||
      !rmr_point_valid(extent, l.maximum_coordinate) || canvas.x <= 0 ||
      canvas.y <= 0 || extent.x <= 0 || extent.y <= 0)
    return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, 0, "invalid template input");
  if (t->constant_count > 250)
    return rmr_fail(e, RMR_ERR_LIMIT, 0, "template constant limit");

  builder b = {.limits = l,
               .provider = provider,
               .canvas = canvas,
               .extent = extent,
               .error = e};
  rmr_status s = environment(&b, t);
  if (s != RMR_OK) return s;
  s = items(&b, t->items, t->count, (rmr_point){0}, 0);
  if (s != RMR_OK) return s;

  if (b.draw_count > l.maximum_plan_bytes / sizeof(rmr_draw))
    return fail(&b, RMR_ERR_LIMIT, "template scratch limit");
  size_t bytes = b.draw_count * sizeof(rmr_draw);
  if (b.element_count > (l.maximum_plan_bytes - bytes) / sizeof(rmr_element))
    return fail(&b, RMR_ERR_LIMIT, "template scratch limit");
  bytes += b.element_count * sizeof(rmr_element);

  b.draws = b.draw_count ? rmr_calloc(b.draw_count, sizeof *b.draws) : nullptr;
  b.elements = b.element_count ? rmr_calloc(b.element_count, sizeof *b.elements)
                               : nullptr;
  if ((b.draw_count && !b.draws) || (b.element_count && !b.elements)) {
    free(b.draws);
    free(b.elements);
    return fail(&b, RMR_ERR_OUT_OF_MEMORY, nullptr);
  }

  b.allocated = bytes;
  b.draw_count = 0;
  b.element_count = 0;
  b.work = 0;

  s = environment(&b, t);
  if (s == RMR_OK) s = items(&b, t->items, t->count, (rmr_point){0}, 0);

  if (s == RMR_OK) {
    l.maximum_plan_bytes -= bytes;
    s = rmr_plan_create(b.draws, b.draw_count, viewport, &l, out, e);
  }

  free(b.draws);
  free(b.elements);
  return s;
}

rmr_status rmr_template_background_plan_with_text(
    const rmr_template* t, rmr_point canvas, rmr_point extent,
    const rmr_plan* foreground, bool visible, const rmr_text_provider* provider,
    const rmr_limits* given, rmr_plan** out, rmr_error* e) {
  if (!foreground || !out)
    return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, 0,
                    "missing foreground or output");

  if (!visible)
    return rmr_plan_create(foreground->draws, foreground->count,
                           foreground->viewport, given, out, e);

  rmr_limits l = given ? *given : rmr_limits_default();
  rmr_plan* background = nullptr;
  rmr_status s = rmr_template_plan_with_text(
      t, canvas, extent, foreground->viewport, provider, &l, &background, e);
  if (s != RMR_OK) return s;

  size_t count = foreground->count, bytes = rmr_plan_bytes(background);
  rmr_draw* draws = nullptr;
  if (count > l.maximum_draws || background->count > l.maximum_draws - count) {
    s = rmr_fail(e, RMR_ERR_LIMIT, 0, "background scene command limit");
    goto done;
  }
  count += background->count;
  if (bytes > l.maximum_plan_bytes ||
      count > (l.maximum_plan_bytes - bytes) / sizeof *draws) {
    s = rmr_fail(e, RMR_ERR_LIMIT, 0, "background scene scratch limit");
    goto done;
  }
  bytes += count * sizeof *draws;

  draws = count ? rmr_calloc(count, sizeof *draws) : nullptr;
  if (count && !draws) {
    s = rmr_fail(e, RMR_ERR_OUT_OF_MEMORY, 0, nullptr);
    goto done;
  }

  if (background->count)
    memcpy(draws, background->draws, background->count * sizeof *draws);
  if (foreground->count)
    memcpy(draws + background->count, foreground->draws,
           foreground->count * sizeof *draws);
  l.maximum_plan_bytes -= bytes;
  s = rmr_plan_create(draws, count, foreground->viewport, &l, out, e);
  if (s == RMR_OK) (*out)->background = background->count;

done:
  free(draws);
  rmr_plan_free(background);
  return s;
}

rmr_status rmr_template_plan(const rmr_template* t, rmr_point canvas,
                             rmr_point extent, rmr_rect viewport,
                             const rmr_limits* limits, rmr_plan** out,
                             rmr_error* error) {
  return rmr_template_plan_with_text(t, canvas, extent, viewport, nullptr,
                                     limits, out, error);
}

rmr_status rmr_template_background_plan(const rmr_template* t, rmr_point canvas,
                                        rmr_point extent,
                                        const rmr_plan* foreground,
                                        bool visible, const rmr_limits* limits,
                                        rmr_plan** out, rmr_error* error) {
  return rmr_template_background_plan_with_text(
      t, canvas, extent, foreground, visible, nullptr, limits, out, error);
}

rmr_status rmr_template_page_extent(rmr_point canvas, rmr_rect viewport,
                                    rmr_point* out, rmr_error* error) {
  double right = viewport.x + viewport.width,
         bottom = viewport.y + viewport.height;
  /* Coordinate conversion can lose a few ulps at an unchanged canvas edge. */
  double ex = 8 * DBL_EPSILON *
              fmax(canvas.x, fmax(fabs(viewport.x), fabs(viewport.width)));
  double ey = 8 * DBL_EPSILON *
              fmax(canvas.y, fmax(fabs(viewport.y), fabs(viewport.height)));

  if (!out || !isfinite(canvas.x) || !isfinite(canvas.y) || canvas.x <= 0 ||
      canvas.y <= 0 || canvas.x > 100000 || canvas.y > 100000 ||
      !isfinite(viewport.x) || !isfinite(viewport.y) ||
      !isfinite(viewport.width) || !isfinite(viewport.height) ||
      viewport.width <= 0 || viewport.height <= 0 || !isfinite(right) ||
      !isfinite(bottom) || viewport.x + canvas.x / 2 > ex || viewport.y > ey ||
      canvas.x / 2 - right > ex || canvas.y - bottom > ey)
    return rmr_fail(error, RMR_ERR_INVALID_ARGUMENT, 0,
                    "viewport must contain a plausible canvas");

  *out = (rmr_point){fmin(fmax(canvas.x, right + canvas.x / 2), 100000),
                     fmin(fmax(canvas.y, bottom), 100000)};
  return RMR_OK;
}
