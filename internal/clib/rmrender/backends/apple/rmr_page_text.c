/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "rmr_typography_internal.h"

/* Shared implementation behind page_plan, page_ink_plan and page_bounds,
 * building ink/writing above template/image backgrounds, or just measuring
 * their combined bounds when measured is given. */
static rmr_status page_build(const rm_page* page, rmr_point canvas,
                             rmr_ink_edge edge, double margin,
                             const rmr_page_resources* given_resources,
                             const rmr_apple_fonts* fonts,
                             const rmr_limits* given, bool ink_only,
                             rmr_rect* measured, rmr_plan** out, rmr_error* e) {
  if (!page || (!out && !measured) || !isfinite(canvas.x) ||
      !isfinite(canvas.y) || canvas.x <= 0 || canvas.y <= 0 ||
      canvas.x > 100000 || canvas.y > 100000)
    return rmr_apple_text_error(e, RMR_ERR_INVALID_ARGUMENT,
                                "invalid page input");

  rmr_limits limits = given ? *given : rmr_limits_default(), remaining = limits;
  rmr_page_resources resources =
      given_resources ? *given_resources : (rmr_page_resources){0};
  rmr_unit_raster raster = rmr_apple_unit_raster();
  rmr_text_provider template_text = rmr_apple_text_provider();
  if (!resources.raster) resources.raster = &raster;
  if (!resources.text) resources.text = &template_text;

  rmr_apple_layout* layout = nullptr;
  rmr_placed_scene* placed = nullptr;
  rmr_plan *text = nullptr, *highlights = nullptr, *ink = nullptr,
           *foreground = nullptr, *result = nullptr;
  rmr_status s = RMR_OK;
  rmr_text_extent extent = {0};
  bool has_extent = false;
  rmr_anchor_context anchor_context = {0};
  rmr_anchor_provider provider = {0};

  if (page->has_text) {
    if (!fonts)
      return rmr_apple_text_error(e, RMR_ERR_UNSUPPORTED,
                                  "document font provider required");
    s = rmr_apple_layout_create(&page->text, canvas.x, fonts, &remaining,
                                &layout, e);
    if (s != RMR_OK) goto done;
    remaining.maximum_plan_bytes -= rmr_apple_layout_bytes(layout);
    anchor_context =
        (rmr_anchor_context){rmr_apple_layout_anchors(layout), &limits};
    provider = rmr_lines_layout_anchor_provider(&anchor_context);
    has_extent = rmr_apple_layout_bottom(layout, &extent.bottom);
    extent.origin_x = page->text.origin_x;
    extent.origin_y = page->text.origin_y;
    extent.has_column_width = page->text.has_column_width;
    extent.column_width = page->text.column_width;
  }

  s = rmr_lines_place_scene(page->strokes, page->stroke_count, page->images,
                            page->image_count, layout ? &provider : nullptr,
                            &remaining, &placed, e);
  if (s != RMR_OK) goto done;

  remaining.maximum_plan_bytes -= rmr_placed_scene_bytes(placed);
  rm_page resolved = *page;
  resolved.strokes = rmr_placed_scene_strokes(placed);
  resolved.images = rmr_placed_scene_images(placed);
  resolved.has_text = false;
  resolved.highlight_count = 0;

  rmr_rect viewport;
  s = rmr_lines_content_bounds_with_text(&resolved, canvas.x, canvas.y, edge,
                                         margin, has_extent ? &extent : nullptr,
                                         &remaining, &viewport, e);
  if (s != RMR_OK) goto done;

  if (measured) *measured = viewport;
  if (!out) goto done;

  if (ink_only) {
    resolved.image_count = 0;
    s = rmr_lines_scene_plan(&resolved, viewport, resources.image,
                             resources.image_context, resources.raster,
                             &remaining, &result, e);
    goto done;
  }

  if (layout) {
    s = rmr_apple_layout_plan(layout, viewport, &remaining, &text, e);
    if (s != RMR_OK) goto done;
    remaining.maximum_plan_bytes -= rmr_plan_bytes(text);
  }

  s = rmr_lines_highlight_plan(page->highlights, page->highlight_count,
                               viewport, &remaining, &highlights, e);
  if (s != RMR_OK) goto done;
  remaining.maximum_plan_bytes -= rmr_plan_bytes(highlights);

  s = rmr_lines_scene_plan(&resolved, viewport, resources.image,
                           resources.image_context, resources.raster,
                           &remaining, &ink, e);
  if (s != RMR_OK) goto done;
  remaining.maximum_plan_bytes -= rmr_plan_bytes(ink);

  const rmr_plan* parts[] = {text, highlights, ink};
  s = rmr_plan_concat(parts + (text ? 0 : 1), text ? 3 : 2, viewport,
                      &remaining, &foreground, e);
  if (s != RMR_OK) goto done;

  rmr_plan_free(text);
  text = nullptr;
  rmr_plan_free(highlights);
  highlights = nullptr;
  rmr_plan_free(ink);
  ink = nullptr;
  rmr_placed_scene_free(placed);
  placed = nullptr;
  rmr_apple_layout_free(layout);
  layout = nullptr;

  remaining = limits;
  remaining.maximum_plan_bytes -= rmr_plan_bytes(foreground);

  const rmr_template* model = nullptr;
  if (page->background_visible && page->has_template_name &&
      page->template_name.length) {
    if (!page->template_name.bytes) {
      s = rmr_apple_text_error(e, RMR_ERR_INVALID_ARGUMENT,
                               "missing template name");
      goto done;
    }
    if (!resources.template) {
      s = rmr_apple_text_error(e, RMR_ERR_UNSUPPORTED,
                               "template resolver required");
      goto done;
    }
    s = resources.template(resources.template_context, page->template_name,
                           &model);
    if (s != RMR_OK) {
      rmr_apple_text_error(e, s, "template resolver failed");
      goto done;
    }
  }

  if (model) {
    rmr_point coverage;
    s = rmr_template_page_extent(canvas, viewport, &coverage, e);
    if (s != RMR_OK) goto done;
    s = rmr_template_background_plan_with_text(model, canvas, coverage,
                                               foreground, true, resources.text,
                                               &remaining, &result, e);
  } else {
    result = foreground;
    foreground = nullptr;
  }

done:
  rmr_apple_layout_free(layout);
  rmr_placed_scene_free(placed);
  rmr_plan_free(text);
  rmr_plan_free(highlights);
  rmr_plan_free(ink);
  rmr_plan_free(foreground);
  if (s == RMR_OK && out)
    *out = result;
  else
    rmr_plan_free(result);
  return s;
}

rmr_status rmr_apple_page_plan(const rm_page* page, rmr_point canvas,
                               rmr_ink_edge edge, double margin,
                               const rmr_page_resources* resources,
                               const rmr_apple_fonts* fonts,
                               const rmr_limits* limits, rmr_plan** out,
                               rmr_error* error) {
  return page_build(page, canvas, edge, margin, resources, fonts, limits, false,
                    nullptr, out, error);
}

rmr_status rmr_apple_page_ink_plan(const rm_page* page, rmr_point canvas,
                                   rmr_ink_edge edge, double margin,
                                   const rmr_apple_fonts* fonts,
                                   const rmr_limits* limits, rmr_plan** out,
                                   rmr_error* error) {
  return page_build(page, canvas, edge, margin, nullptr, fonts, limits, true,
                    nullptr, out, error);
}

rmr_status rmr_apple_page_bounds(const rm_page* page, rmr_point canvas,
                                 rmr_ink_edge edge, double margin,
                                 const rmr_apple_fonts* fonts,
                                 const rmr_limits* limits, rmr_rect* out,
                                 rmr_error* error) {
  if (!out)
    return rmr_apple_text_error(error, RMR_ERR_INVALID_ARGUMENT,
                                "missing bounds output");
  rmr_rect result;
  rmr_status status = page_build(page, canvas, edge, margin, nullptr, fonts,
                                 limits, false, &result, nullptr, error);
  if (status == RMR_OK) *out = result;
  return status;
}
