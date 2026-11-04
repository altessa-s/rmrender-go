/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "rmr_typography_internal.h"

/* Builds a page's render plan, or, when out is null, only measures its
 * content bounds; shared by the entry points below. outlines draws typed
 * text as glyph outline fills instead of an embedded text PDF. */
static rmr_status page_build(const rm_page* page, rmr_point canvas,
                             rmr_ink_edge edge, double margin,
                             const rmr_page_resources* given_resources,
                             const rmr_linux_fonts* fonts,
                             const rmr_limits* given, bool ink_only,
                             bool outlines, rmr_rect* measured, rmr_plan** out,
                             rmr_error* e) {
  if (!page || (!out && !measured) || !isfinite(canvas.x) ||
      !isfinite(canvas.y) || canvas.x <= 0 || canvas.y <= 0 ||
      canvas.x > 100000 || canvas.y > 100000)
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT, "invalid page input");

  rmr_limits limits = given ? *given : rmr_limits_default(), remaining = limits;
  rmr_page_resources resources =
      given_resources ? *given_resources : (rmr_page_resources){0};
  rmr_text_provider template_text = rmr_linux_text_provider();
  if (!resources.text) resources.text = &template_text;
  rmr_linux_layout* layout = nullptr;
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
      return rmr_linux_error(e, RMR_ERR_UNSUPPORTED,
                             "document font provider required");
    s = rmr_linux_layout_create(&page->text, canvas.x, fonts, &remaining,
                                &layout, e);
    if (s != RMR_OK) goto done;
    remaining.maximum_plan_bytes -= rmr_linux_layout_bytes(layout);

    anchor_context =
        (rmr_anchor_context){rmr_linux_layout_anchors(layout), &limits};
    provider = rmr_lines_layout_anchor_provider(&anchor_context);

    has_extent = rmr_linux_layout_bottom(layout, &extent.bottom);
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
    s = outlines
            ? rmr_linux_layout_outline_plan(layout, viewport, &remaining, &text,
                                            e)
            : rmr_linux_layout_plan(layout, viewport, &remaining, &text, e);
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
  rmr_linux_layout_free(layout);
  layout = nullptr;

  remaining = limits;
  remaining.maximum_plan_bytes -= rmr_plan_bytes(foreground);
  const rmr_template* model = nullptr;
  if (page->background_visible && page->has_template_name &&
      page->template_name.length) {
    if (!page->template_name.bytes) {
      s = rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT, "missing template name");
      goto done;
    }
    if (!resources.template) {
      s = rmr_linux_error(e, RMR_ERR_UNSUPPORTED, "template resolver required");
      goto done;
    }
    s = resources.template(resources.template_context, page->template_name,
                           &model);
    if (s != RMR_OK) {
      rmr_linux_error(e, s, "template resolver failed");
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
  rmr_linux_layout_free(layout);
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

rmr_status rmr_linux_page_plan(const rm_page* page, rmr_point canvas,
                               rmr_ink_edge edge, double margin,
                               const rmr_page_resources* resources,
                               const rmr_linux_fonts* fonts,
                               const rmr_limits* limits, rmr_plan** out,
                               rmr_error* error) {
  return page_build(page, canvas, edge, margin, resources, fonts, limits, false,
                    false, nullptr, out, error);
}

rmr_status rmr_linux_page_ink_plan(const rm_page* page, rmr_point canvas,
                                   rmr_ink_edge edge, double margin,
                                   const rmr_linux_fonts* fonts,
                                   const rmr_limits* limits, rmr_plan** out,
                                   rmr_error* error) {
  return page_build(page, canvas, edge, margin, nullptr, fonts, limits, true,
                    false, nullptr, out, error);
}

rmr_status rmr_linux_page_bounds(const rm_page* page, rmr_point canvas,
                                 rmr_ink_edge edge, double margin,
                                 const rmr_linux_fonts* fonts,
                                 const rmr_limits* limits, rmr_rect* out,
                                 rmr_error* error) {
  if (!out)
    return rmr_linux_error(error, RMR_ERR_INVALID_ARGUMENT,
                           "missing bounds output");

  rmr_rect result;
  rmr_status status = page_build(page, canvas, edge, margin, nullptr, fonts,
                                 limits, false, false, &result, nullptr, error);
  if (status == RMR_OK) *out = result;
  return status;
}

rmr_status rmr_linux_page_outline_plan(const rm_page* page, rmr_point canvas,
                                       rmr_ink_edge edge, double margin,
                                       const rmr_page_resources* resources,
                                       const rmr_linux_fonts* fonts,
                                       const rmr_limits* limits, rmr_plan** out,
                                       rmr_error* error) {
  return page_build(page, canvas, edge, margin, resources, fonts, limits, false,
                    true, nullptr, out, error);
}

rmr_status rmr_linux_page_svg(const rm_page* page, rmr_point canvas,
                              rmr_ink_edge edge, double margin,
                              const rmr_page_resources* resources,
                              const rmr_linux_fonts* fonts, rmr_paper paper,
                              double raster_dpi, const rmr_limits* limits,
                              rmr_cancel_fn cancel, void* context,
                              rmr_buffer* out, rmr_error* error) {
  if (!out)
    return rmr_linux_error(error, RMR_ERR_INVALID_ARGUMENT,
                           "missing SVG output");

  rmr_plan* plan = nullptr;
  rmr_status s = page_build(page, canvas, edge, margin, resources, fonts,
                            limits, false, true, nullptr, &plan, error);
  if (s != RMR_OK) return s;

  rmr_sheet sheet;
  s = rmr_paper_sheet(paper, canvas.x, canvas.y, rmr_plan_viewport(plan),
                      &sheet, error);

  rmr_svg_pages pages = {0};
  const rmr_plan* plans[] = {plan};
  if (s == RMR_OK)
    s = rmr_linux_svg(plans, &sheet, 1, raster_dpi, limits, cancel, context,
                      &pages, error);
  rmr_plan_free(plan);
  if (s != RMR_OK) return s;

  *out = pages.pages[0];
  pages.pages[0] = (rmr_buffer){0};
  rmr_svg_pages_free(&pages);
  return RMR_OK;
}
