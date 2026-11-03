/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <math.h>
#include <rmrender/rmlines.h>
#include <stdio.h>

/* Fill *e (if given) with status s and message, and return s. */
static rmr_status fail(rmr_error* e, rmr_status s, const char* message) {
  if (e) {
    *e = (rmr_error){.code = s};
    snprintf(e->message, sizeof e->message, "%s", message);
  }
  return s;
}

/* True if size is a finite, positive canvas up to 100000 units per side. */
static bool plausible(rmr_point size) {
  return isfinite(size.x) && isfinite(size.y) && size.x > 0 && size.y > 0 &&
         size.x <= 100000 && size.y <= 100000;
}

rmr_status rmr_lines_document_canvas(const rm_page* const* pages, size_t count,
                                     rmr_point content_size, bool landscape,
                                     const rmr_limits* given, rmr_point* out,
                                     rmr_error* error) {
  if (!out || (count && !pages))
    return fail(error, RMR_ERR_INVALID_ARGUMENT, "missing canvas input/output");

  rmr_limits limits = given ? *given : rmr_limits_default();
  if (count > limits.maximum_elements)
    return fail(error, RMR_ERR_LIMIT, "canvas page count limit");

  rmr_point size =
      plausible(content_size) ? content_size : (rmr_point){1404, 1872};
  for (size_t i = 0; i < count; i++) {
    const rm_page* p = pages[i];
    if (!p || !p->has_paper_size) continue;
    rmr_point declared = {p->paper_size.width, p->paper_size.height};
    if (plausible(declared)) {
      size = declared;
      break;
    }
  }

  if (landscape && size.y > size.x) size = (rmr_point){size.y, size.x};
  *out = size;
  return RMR_OK;
}

rmr_status rmr_lines_composed_page_plan(
    const rm_page* page, rmr_point canvas, rmr_ink_edge edge, double margin,
    const rmr_page_resources* given_resources, const rmr_limits* given,
    rmr_plan** out, rmr_error* error) {
  if (!page || !out || !plausible(canvas))
    return fail(error, RMR_ERR_INVALID_ARGUMENT, "invalid composed page input");

  rmr_page_resources resources =
      given_resources ? *given_resources : (rmr_page_resources){0};
  bool visible = page->background_visible && page->has_template_name &&
                 page->template_name.length;
  if (visible && !page->template_name.bytes)
    return fail(error, RMR_ERR_INVALID_ARGUMENT, "missing template name bytes");
  if (visible && !resources.template)
    return fail(error, RMR_ERR_UNSUPPORTED, "template resolver required");

  rmr_limits limits = given ? *given : rmr_limits_default();
  rmr_rect viewport;
  rmr_status status = rmr_lines_content_bounds(
      page, canvas.x, canvas.y, edge, margin, &limits, &viewport, error);
  if (status != RMR_OK) return status;
  rmr_point extent;
  status = rmr_template_page_extent(canvas, viewport, &extent, error);
  if (status != RMR_OK) return status;

  const rmr_template* model = nullptr;
  if (visible) {
    status = resources.template(resources.template_context, page->template_name,
                                &model);
    if (status != RMR_OK)
      return fail(error, status, "template resolution failed");
  }

  rmr_plan* foreground = nullptr;
  status = rmr_lines_scene_plan(page, viewport, resources.image,
                                resources.image_context, resources.raster,
                                &limits, &foreground, error);
  if (status != RMR_OK) return status;

  if (!model) {
    *out = foreground;
    return RMR_OK;
  }

  size_t bytes = rmr_plan_bytes(foreground);
  if (bytes > limits.maximum_plan_bytes) {
    rmr_plan_free(foreground);
    return fail(error, RMR_ERR_LIMIT, "page memory limit");
  }

  limits.maximum_plan_bytes -= bytes;
  rmr_plan* composed = nullptr;
  status = rmr_template_background_plan_with_text(
      model, canvas, extent, foreground, true, resources.text, &limits,
      &composed, error);
  rmr_plan_free(foreground);
  if (status == RMR_OK) *out = composed;
  return status;
}
