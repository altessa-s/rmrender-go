/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <float.h>
#include <math.h>
#include <rmrender/rmlines.h>
#include <stdio.h>
#include <stdlib.h>

/* Fill *e (if given) with status s, item index and message, and return s. */
static rmr_status fail(rmr_error* e, rmr_status s, size_t i,
                       const char* message) {
  if (e) {
    *e = (rmr_error){.code = s, .index = i};
    snprintf(e->message, sizeof e->message, "%s", message);
  }
  return s;
}

/* The same three-corner affine solve as PageRenderer. Corner 2 contributes
 * only to clipping; no projective warp is inferred for non-parallelograms. */
static bool transform(const rm_image_vertex* v, rmr_transform* t) {
  double du1 = v[1].u - v[0].u, dv1 = v[1].v - v[0].v;
  double du3 = v[3].u - v[0].u, dv3 = v[3].v - v[0].v;
  double det = du1 * dv3 - du3 * dv1;
  if (fabs(det) <= DBL_EPSILON) return false;

  t->a = ((v[1].x - v[0].x) * dv3 - (v[3].x - v[0].x) * dv1) / det;
  t->b = ((v[1].y - v[0].y) * dv3 - (v[3].y - v[0].y) * dv1) / det;
  t->c = ((v[3].x - v[0].x) * du1 - (v[1].x - v[0].x) * du3) / det;
  t->d = ((v[3].y - v[0].y) * du1 - (v[1].y - v[0].y) * du3) / det;
  t->tx = v[0].x - v[0].u * t->a - v[0].v * t->c;
  t->ty = v[0].y - v[0].u * t->b - v[0].v * t->d;

  return true;
}

rmr_status rmr_lines_scene_plan(const rm_page* page, rmr_rect viewport,
                                rmr_asset_resolve_fn resolve, void* context,
                                const rmr_unit_raster* raster,
                                const rmr_limits* given, rmr_plan** out,
                                rmr_error* e) {
  rmr_limits limits = given ? *given : rmr_limits_default();
  if (!page || !out || (page->stroke_count && !page->strokes) ||
      (page->image_count && (!page->images || !resolve)))
    return fail(e, RMR_ERR_INVALID_ARGUMENT, 0, "invalid scene input");

  if (page->has_text || page->highlight_count)
    return fail(e, RMR_ERR_UNSUPPORTED, 0,
                "text and highlights require page providers");

  if (page->stroke_count > limits.maximum_draws ||
      page->image_count > limits.maximum_draws - page->stroke_count)
    return fail(e, RMR_ERR_LIMIT, 0, "scene command limit");

  for (size_t i = 0; i < page->image_count; i++) {
    const rm_page_image* im = &page->images[i];
    if (im->file.length && !im->file.bytes)
      return fail(e, RMR_ERR_INVALID_ARGUMENT, i, "missing image filename");
    if (im->has_anchor)
      return fail(e, RMR_ERR_UNSUPPORTED, i,
                  "image text anchor requires placement");
    if (i && im->strokes_before < page->images[i - 1].strokes_before)
      return fail(e, RMR_ERR_INVALID_ARGUMENT, i,
                  "images are not in scene order");
    for (size_t k = 0; k < 4; k++) {
      rm_image_vertex v = im->vertices[k];
      if (!isfinite(v.x) || !isfinite(v.y) || !isfinite(v.u) ||
          !isfinite(v.v) || fabs(v.x) > limits.maximum_coordinate ||
          fabs(v.y) > limits.maximum_coordinate)
        return fail(e, RMR_ERR_INVALID_ARGUMENT, i, "invalid image vertex");
    }
  }

  rmr_plan* ink = nullptr;
  rmr_status status = rmr_lines_plan_with_raster(
      page->strokes, page->stroke_count, viewport, raster, &limits, &ink, e);
  if (status != RMR_OK) return status;

  if (!page->image_count) {
    *out = ink;
    return RMR_OK;
  }

  size_t count = page->stroke_count + page->image_count;
  size_t bytes = rmr_plan_bytes(ink);
  rmr_draw* draws = nullptr;
  rmr_element* clips = nullptr;
  if (bytes > limits.maximum_plan_bytes ||
      count > (limits.maximum_plan_bytes - bytes) / sizeof *draws) {
    status = fail(e, RMR_ERR_LIMIT, 0, "scene scratch memory limit");
    goto done;
  }

  bytes += count * sizeof *draws;
  if (page->image_count >
      (limits.maximum_plan_bytes - bytes) / (5 * sizeof *clips)) {
    status = fail(e, RMR_ERR_LIMIT, 0, "image clip memory limit");
    goto done;
  }

  bytes += page->image_count * 5 * sizeof *clips;
  draws = calloc(count, sizeof *draws);
  clips = calloc(page->image_count * 5, sizeof *clips);
  if (!draws || !clips) {
    status = fail(e, RMR_ERR_OUT_OF_MEMORY, 0, "scene allocation failed");
    goto done;
  }

  size_t stroke = 0, image = 0, at = 0;
  while (at < count) {
    if (image < page->image_count &&
        (stroke == page->stroke_count ||
         page->images[image].strokes_before <= stroke)) {
      const rm_page_image* im = &page->images[image];
      rmr_draw* d = &draws[at++];
      rmr_asset_view asset = {0};
      status = resolve(context, im->file, &asset);
      if (status != RMR_OK) {
        fail(e, status, image, "image resolver failed");
        goto done;
      }
      if ((!asset.bytes) != (!asset.size)) {
        status =
            fail(e, RMR_ERR_INVALID_ARGUMENT, image, "invalid resolved asset");
        goto done;
      }
      if (asset.size && transform(im->vertices, &d->image.transform)) {
        d->mode = RMR_IMAGE;
        d->color.alpha = 1;
        d->image.bytes = asset.bytes;
        d->image.size = asset.size;
        rmr_element* path = clips + image * 5;
        for (size_t k = 0; k < 4; k++)
          path[k] =
              (rmr_element){.kind = k ? RMR_LINE : RMR_MOVE,
                            .end = {im->vertices[k].x, im->vertices[k].y}};
        path[4].kind = RMR_CLOSE;
        d->path = (rmr_path_view){path, 5};
      }
      image++;
    } else {
      rmr_draw d = *rmr_plan_draw(ink, stroke);
      if (d.group) {
        if (at && draws[at - 1].group == d.group)
          d.group_alpha = draws[at - 1].group_alpha;
        else {
          rmr_stroke_style style;
          status = rmr_lines_style(&page->strokes[stroke], &style, e);
          if (status != RMR_OK) goto done;
          d.group_alpha = style.color.alpha;
        }
      }
      draws[at++] = d;
      stroke++;
    }
  }

  limits.maximum_plan_bytes -= bytes;
  status = rmr_plan_create(draws, count, viewport, &limits, out, e);

done:
  free(clips);
  free(draws);
  rmr_plan_free(ink);
  return status;
}
