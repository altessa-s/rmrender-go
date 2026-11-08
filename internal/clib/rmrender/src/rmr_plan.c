/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "rmr_internal.h"
#include "rmr_pdf_edit.h"

/* Grows the bounding box [*x0,*y0]-[*x1,*y1] to also cover point p. */
static void include(rmr_point p, double* x0, double* y0, double* x1,
                    double* y1) {
  *x0 = fmin(*x0, p.x);
  *y0 = fmin(*y0, p.y);
  *x1 = fmax(*x1, p.x);
  *y1 = fmax(*y1, p.y);
}

/* Validates path's elements and coordinates and checks it is a
 * well-formed sequence of moves/segments/closes. */
static rmr_status validate_path(rmr_path_view path, rmr_limits l, size_t index,
                                rmr_error* e) {
  if (!path.elements && path.count)
    return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, index,
                    "missing path elements");

  bool open = false;
  for (size_t j = 0; j < path.count; j++) {
    const rmr_element* item = &path.elements[j];
    if (item->kind > RMR_CLOSE ||
        (item->kind != RMR_CLOSE &&
         !rmr_point_valid(item->end, l.maximum_coordinate)) ||
        (item->kind == RMR_CURVE &&
         (!rmr_point_valid(item->control1, l.maximum_coordinate) ||
          !rmr_point_valid(item->control2, l.maximum_coordinate))))
      return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, index,
                      "invalid path coordinate or element");
    if (item->kind == RMR_MOVE)
      open = true;
    else if (!open)
      return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, index,
                      "path segment without move");
    if (item->kind == RMR_CLOSE) open = false;
  }

  return RMR_OK;
}

rmr_status rmr_plan_create(const rmr_draw* draws, size_t count,
                           rmr_rect viewport, const rmr_limits* given,
                           rmr_plan** out, rmr_error* e) {
  rmr_limits l = given ? *given : rmr_limits_default();
  if (!out || (!draws && count) || !isfinite(l.maximum_coordinate) ||
      l.maximum_coordinate <= 0 || l.maximum_coordinate > 1e12 ||
      !isfinite(l.maximum_width) || l.maximum_width <= 0 ||
      l.maximum_width > 1e6 ||
      !rmr_point_valid((rmr_point){viewport.x, viewport.y},
                       l.maximum_coordinate) ||
      !isfinite(viewport.width) || !isfinite(viewport.height) ||
      viewport.width <= 0 || viewport.height <= 0 ||
      !rmr_point_valid((rmr_point){viewport.x + viewport.width,
                                   viewport.y + viewport.height},
                       l.maximum_coordinate))
    return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, 0,
                    "invalid viewport or input");
  if (count > l.maximum_draws || count > SIZE_MAX / sizeof(rmr_draw))
    return rmr_fail(e, RMR_ERR_LIMIT, 0, "draw count limit");

  size_t elements = 0, mask_bytes = 0, eraser_count = 0;
  uint64_t caps = 0;

  for (size_t i = 0; i < count; i++) {
    const rmr_draw* d = &draws[i];
    bool stroked = d->mode == RMR_STROKE || d->mode == RMR_FILL_STROKE;
    if ((!d->path.elements && d->path.count) || d->mode > RMR_PDF ||
        d->cap > RMR_CAP_BUTT || d->join > RMR_JOIN_MITER ||
        d->blend > RMR_BLEND_MULTIPLY || !rmr_color_valid(d->color) ||
        (stroked && (!isfinite(d->miter_limit) || d->miter_limit < 0 ||
                     (d->miter_limit > 0 && d->miter_limit < 1) ||
                     d->miter_limit > 1000 || !isfinite(d->line_width) ||
                     d->line_width <= 0 || d->line_width > l.maximum_width)))
      return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, i, "invalid paint or path");
    if (d->path.count > l.maximum_elements - elements)
      return rmr_fail(e, RMR_ERR_LIMIT, i, "path element limit");
    elements += d->path.count;

    caps |= d->mode == RMR_PDF     ? RMR_CAP_PDF
            : d->mode == RMR_TEXT  ? RMR_CAP_TEXT
            : d->mode == RMR_IMAGE ? RMR_CAP_IMAGE
            : d->mode == RMR_FILL  ? RMR_CAP_FILL
                                   : RMR_CAP_STROKE;

    if (d->mode == RMR_FILL_STROKE) {
      if (!rmr_color_valid(d->stroke_color) || d->mask.bytes ||
          d->eraser_count || d->group)
        return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, i,
                        "invalid combined fill/stroke");
      caps |= RMR_CAP_FILL | RMR_CAP_STROKE | RMR_CAP_FILL_STROKE;
    }

    if (stroked && d->join == RMR_JOIN_MITER) caps |= RMR_CAP_MITER;

    if (d->mode == RMR_TEXT) {
      const rmr_text_view* t = &d->text;
      if ((!t->utf8 && t->size) || d->path.count || d->mask.bytes ||
          d->eraser_count || d->group || !isfinite(t->font_size) ||
          t->font_size <= 0 || t->font_size > l.maximum_width ||
          !rmr_point_valid(t->origin, l.maximum_coordinate) ||
          !isfinite(t->bounds.width) || !isfinite(t->bounds.height) ||
          t->bounds.width < 0 || t->bounds.height < 0 ||
          !rmr_point_valid(
              (rmr_point){t->origin.x + t->bounds.x, t->origin.y + t->bounds.y},
              l.maximum_coordinate) ||
          !rmr_point_valid(
              (rmr_point){t->origin.x + t->bounds.x + t->bounds.width,
                          t->origin.y + t->bounds.y + t->bounds.height},
              l.maximum_coordinate))
        return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, i, "invalid text command");
      if (t->size > 65536 || t->size > l.maximum_plan_bytes - mask_bytes)
        return rmr_fail(e, RMR_ERR_LIMIT, i, "text byte limit");
      if (!rmr_utf8_valid(t->utf8, t->size))
        return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, i, "invalid text UTF-8");
      mask_bytes += t->size;
    } else if (d->text.utf8 || d->text.size) {
      return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, i,
                      "text bytes on non-text command");
    }

    if (d->mode == RMR_IMAGE || d->mode == RMR_PDF) {
      const rmr_image_view* im = &d->image;
      rmr_transform t = im->transform;
      if (!im->bytes || !im->size || d->mask.bytes || d->eraser_count ||
          d->group || !isfinite(t.a) || !isfinite(t.b) || !isfinite(t.c) ||
          !isfinite(t.d) || !isfinite(t.tx) || !isfinite(t.ty))
        return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, i,
                        "invalid image command");
      for (unsigned corner = 0; corner < 4; corner++) {
        rmr_point p =
            rmr_transform_point(t, (rmr_point){corner & 1, corner >> 1});
        if (!rmr_point_valid(p, l.maximum_coordinate))
          return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, i,
                          "image transform out of range");
      }
      if (im->size > l.maximum_plan_bytes - mask_bytes)
        return rmr_fail(e, RMR_ERR_LIMIT, i, "encoded image memory limit");
      mask_bytes += im->size;
    } else if (d->image.bytes || d->image.size) {
      return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, i,
                      "image bytes on geometry command");
    }

    if (d->blend == RMR_BLEND_DARKEN) caps |= RMR_CAP_DARKEN;
    if (d->blend == RMR_BLEND_MULTIPLY) caps |= RMR_CAP_MULTIPLY;

    if (d->mask.bytes) {
      const rmr_mask_view* m = &d->mask;
      if (d->mode != RMR_FILL || !m->width || !m->height ||
          m->width > SIZE_MAX - 7 || m->stride < (m->width + 7) / 8 ||
          !rmr_point_valid((rmr_point){m->box.x, m->box.y},
                           l.maximum_coordinate) ||
          !isfinite(m->box.width) || !isfinite(m->box.height) ||
          m->box.width <= 0 || m->box.height <= 0 ||
          !rmr_point_valid(
              (rmr_point){m->box.x + m->box.width, m->box.y + m->box.height},
              l.maximum_coordinate))
        return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, i, "invalid mask");
      if (m->width > l.maximum_bitmap_side ||
          m->height > l.maximum_bitmap_side ||
          m->stride > l.maximum_bitmap_bytes / m->height ||
          m->stride > (l.maximum_plan_bytes - mask_bytes) / m->height)
        return rmr_fail(e, RMR_ERR_LIMIT, i, "mask memory limit");
      mask_bytes += m->stride * m->height;
      caps |= RMR_CAP_MASK;
    } else if (d->mask.width || d->mask.height || d->mask.stride) {
      return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, i, "missing mask bytes");
    }

    rmr_status status = validate_path(d->path, l, i, e);
    if (status != RMR_OK) return status;

    if (d->eraser_count && !d->erasers)
      return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, i, "missing eraser paths");
    if (d->eraser_count > l.maximum_elements - eraser_count)
      return rmr_fail(e, RMR_ERR_LIMIT, i, "eraser reference limit");
    eraser_count += d->eraser_count;
    if (d->eraser_count) caps |= RMR_CAP_ERASE;

    for (size_t k = 0; k < d->eraser_count; k++) {
      rmr_path_view path = d->erasers[k];
      if (path.count > l.maximum_elements - elements)
        return rmr_fail(e, RMR_ERR_LIMIT, i, "eraser geometry limit");
      elements += path.count;
      status = validate_path(path, l, i, e);
      if (status != RMR_OK) return status;
    }

    if (d->group) {
      if (!isfinite(d->group_alpha) || d->group_alpha < 0 ||
          d->group_alpha > 1 || d->group_blend > RMR_BLEND_MULTIPLY ||
          d->color.alpha != 1 || d->blend != RMR_BLEND_NORMAL)
        return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, i,
                        "invalid transparency group");
      if (i && draws[i - 1].group == d->group &&
          (draws[i - 1].group_alpha != d->group_alpha ||
           draws[i - 1].group_blend != d->group_blend))
        return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, i,
                        "inconsistent group style");
      caps |= RMR_CAP_GROUP;
      if (d->group_blend == RMR_BLEND_DARKEN) caps |= RMR_CAP_DARKEN;
      if (d->group_blend == RMR_BLEND_MULTIPLY) caps |= RMR_CAP_MULTIPLY;
    }
  }

  size_t bytes = sizeof(rmr_plan);
  if (bytes > l.maximum_plan_bytes ||
      count > (l.maximum_plan_bytes - bytes) / sizeof(rmr_draw))
    return rmr_fail(e, RMR_ERR_LIMIT, 0, "plan memory limit");
  bytes += count * sizeof(rmr_draw);
  if (eraser_count > (l.maximum_plan_bytes - bytes) / sizeof(rmr_path_view))
    return rmr_fail(e, RMR_ERR_LIMIT, 0, "eraser reference memory limit");
  bytes += eraser_count * sizeof(rmr_path_view);
  if (elements > (l.maximum_plan_bytes - bytes) / sizeof(rmr_element))
    return rmr_fail(e, RMR_ERR_LIMIT, 0, "plan memory limit");
  bytes += elements * sizeof(rmr_element);
  if (mask_bytes > l.maximum_plan_bytes - bytes)
    return rmr_fail(e, RMR_ERR_LIMIT, 0, "plan payload memory limit");

  rmr_plan* p = rmr_calloc(1, sizeof *p);
  if (!p) return rmr_fail(e, RMR_ERR_OUT_OF_MEMORY, 0, nullptr);

  p->draws = count ? rmr_calloc(count, sizeof *p->draws) : nullptr;
  p->elements = elements ? rmr_calloc(elements, sizeof *p->elements) : nullptr;
  p->masks = mask_bytes ? rmr_alloc(mask_bytes) : nullptr;
  p->erasers =
      eraser_count ? rmr_calloc(eraser_count, sizeof *p->erasers) : nullptr;
  if ((count && !p->draws) || (elements && !p->elements) ||
      (mask_bytes && !p->masks) || (eraser_count && !p->erasers)) {
    rmr_plan_free(p);
    return rmr_fail(e, RMR_ERR_OUT_OF_MEMORY, 0, nullptr);
  }

  p->bytes = bytes + mask_bytes;
  p->viewport = viewport;
  p->count = count;
  p->caps = caps;

  size_t at = 0, mask_at = 0, eraser_at = 0;
  double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
  for (size_t i = 0; i < count; i++) {
    const rmr_draw* d = &draws[i];
    p->draws[i] = *d;

    if (d->mode == RMR_IMAGE || d->mode == RMR_PDF) {
      memcpy(p->masks + mask_at, d->image.bytes, d->image.size);
      p->draws[i].image.bytes = p->masks + mask_at;
      mask_at += d->image.size;
    }

    if (d->mode == RMR_TEXT) {
      p->draws[i].text.utf8 =
          d->text.size ? (const char*)(p->masks + mask_at) : nullptr;
      if (d->text.size) memcpy(p->masks + mask_at, d->text.utf8, d->text.size);
      mask_at += d->text.size;
      if (d->text.bounds.width > 0 && d->text.bounds.height > 0) {
        include((rmr_point){d->text.origin.x + d->text.bounds.x,
                            d->text.origin.y + d->text.bounds.y},
                &x0, &y0, &x1, &y1);
        include(
            (rmr_point){
                d->text.origin.x + d->text.bounds.x + d->text.bounds.width,
                d->text.origin.y + d->text.bounds.y + d->text.bounds.height},
            &x0, &y0, &x1, &y1);
      }
    }

    if (d->mask.bytes) {
      size_t size = d->mask.stride * d->mask.height;
      memcpy(p->masks + mask_at, d->mask.bytes, size);
      p->draws[i].mask.bytes = p->masks + mask_at;
      mask_at += size;
    }

    p->draws[i].path.elements = d->path.count ? p->elements + at : nullptr;
    if (d->path.count)
      memcpy(p->elements + at, d->path.elements,
             d->path.count * sizeof *p->elements);
    at += d->path.count;

    p->draws[i].erasers = d->eraser_count ? p->erasers + eraser_at : nullptr;
    for (size_t k = 0; k < d->eraser_count; k++) {
      rmr_path_view path = d->erasers[k];
      p->erasers[eraser_at++] =
          (rmr_path_view){path.count ? p->elements + at : nullptr, path.count};
      if (path.count)
        memcpy(p->elements + at, path.elements,
               path.count * sizeof *p->elements);
      at += path.count;
    }

    double lx0 = INFINITY, ly0 = INFINITY, lx1 = -INFINITY, ly1 = -INFINITY;
    for (size_t j = 0; j < d->path.count; j++) {
      const rmr_element* item = &d->path.elements[j];
      if (item->kind != RMR_CLOSE) include(item->end, &lx0, &ly0, &lx1, &ly1);
      if (item->kind == RMR_CURVE) {
        include(item->control1, &lx0, &ly0, &lx1, &ly1);
        include(item->control2, &lx0, &ly0, &lx1, &ly1);
      }
    }

    if (isfinite(lx0)) {
      /* Square caps can project sqrt(2)*half-width on a coordinate axis. */
      double pad =
          (d->mode == RMR_STROKE || d->mode == RMR_FILL_STROKE)
              ? d->line_width / 2 *
                    (d->join == RMR_JOIN_MITER
                         ? 2 * (d->miter_limit != 0 ? d->miter_limit : 10)
                     : d->cap == RMR_CAP_SQUARE ? sqrt(2)
                                                : 1)
              : 0;
      include((rmr_point){lx0 - pad, ly0 - pad}, &x0, &y0, &x1, &y1);
      include((rmr_point){lx1 + pad, ly1 + pad}, &x0, &y0, &x1, &y1);
    }
  }
  if (isfinite(x0)) p->bounds = (rmr_rect){x0, y0, x1 - x0, y1 - y0};

  *out = p;
  return RMR_OK;
}

void rmr_plan_free(rmr_plan* p) {
  if (p) {
    free(p->draws);
    free(p->elements);
    free(p->masks);
    free(p->erasers);
    free(p);
  }
}

size_t rmr_plan_bytes(const rmr_plan* p) { return p ? p->bytes : 0; }

size_t rmr_plan_count(const rmr_plan* p) { return p ? p->count : 0; }

rmr_rect rmr_plan_viewport(const rmr_plan* p) {
  return p ? p->viewport : (rmr_rect){0};
}

rmr_rect rmr_plan_bounds(const rmr_plan* p) {
  return p ? p->bounds : (rmr_rect){0};
}

const rmr_draw* rmr_plan_draw(const rmr_plan* p, size_t i) {
  return p && i < p->count ? &p->draws[i] : nullptr;
}

size_t rmr_plan_background(const rmr_plan* p) { return p ? p->background : 0; }

rmr_status rmr_plan_execute(const rmr_plan* p, const rmr_backend* b,
                            rmr_cancel_fn cancel, void* context, rmr_error* e) {
  return rmr_plan_execute_range(p, 0, p ? p->count : 0, b, cancel, context, e);
}

rmr_status rmr_plan_execute_range(const rmr_plan* p, size_t first, size_t end,
                                  const rmr_backend* b, rmr_cancel_fn cancel,
                                  void* context, rmr_error* e) {
  if (!p || !b || first > end || end > p->count ||
      b->abi_version != RMR_BACKEND_ABI || b->struct_size < sizeof *b ||
      !b->begin || !b->draw || !b->end || !b->abort)
    return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, 0,
                    "invalid backend ABI or callbacks");
  if ((p->caps & b->capabilities) != p->caps)
    return rmr_fail(e, RMR_ERR_UNSUPPORTED, 0,
                    "backend lacks required capabilities");
  if (cancel && cancel(context))
    return rmr_fail(e, RMR_ERR_CANCELLED, 0, nullptr);

  rmr_status s = b->begin(b->context, p->viewport);
  size_t index = first;
  if (s == RMR_OK)
    for (; index < end; index++) {
      if (cancel && cancel(context)) {
        s = RMR_ERR_CANCELLED;
        break;
      }
      s = b->draw(b->context, &p->draws[index]);
      if (s != RMR_OK) break;
    }

  if (s == RMR_OK) {
    if (cancel && cancel(context))
      s = RMR_ERR_CANCELLED;
    else
      s = b->end(b->context);
  }

  if (s != RMR_OK) {
    b->abort(b->context);
    return rmr_fail(e, s, index, "backend execution failed or cancelled");
  }

  return RMR_OK;
}

rmr_status rmr_plan_concat(const rmr_plan* const* plans, size_t count,
                           rmr_rect viewport, const rmr_limits* given,
                           rmr_plan** out, rmr_error* e) {
  if (!out || (!plans && count))
    return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, 0, "missing plans");

  rmr_limits l = given ? *given : rmr_limits_default();
  if (count > l.maximum_elements)
    return rmr_fail(e, RMR_ERR_LIMIT, 0, "plan count limit");

  size_t draws = 0;
  for (size_t i = 0; i < count; i++) {
    if (!plans[i]) return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, i, "null plan");
    if (plans[i]->count > l.maximum_draws - draws)
      return rmr_fail(e, RMR_ERR_LIMIT, i, "combined draw limit");
    draws += plans[i]->count;
  }

  if (draws > l.maximum_plan_bytes / sizeof(rmr_draw))
    return rmr_fail(e, RMR_ERR_LIMIT, 0, "composition scratch limit");

  rmr_draw* commands = draws ? rmr_alloc(draws * sizeof *commands) : nullptr;
  if (draws && !commands) return rmr_fail(e, RMR_ERR_OUT_OF_MEMORY, 0, nullptr);

  size_t at = 0;
  uint64_t token = 0;
  for (size_t i = 0; i < count; i++) {
    uint64_t previous = 0;
    for (size_t j = 0; j < plans[i]->count; j++) {
      rmr_draw d = plans[i]->draws[j];
      uint64_t original = d.group;
      if (original && original != previous) token++;
      if (original) d.group = token;
      previous = original;
      commands[at++] = d;
    }
  }

  l.maximum_plan_bytes -= draws * sizeof *commands;
  rmr_status status = rmr_plan_create(commands, draws, viewport, &l, out, e);
  free(commands);
  return status;
}
