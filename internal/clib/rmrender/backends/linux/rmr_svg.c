/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <rmrender/linux.h>

#include "rmr_linux_internal.h"

rmr_status rmr_linux_path_capture(cairo_t* cr, const rmr_limits* limits,
                                  rmr_path* path) {
  if (!cr || !limits || !path || path->count > limits->maximum_elements)
    return RMR_ERR_INVALID_ARGUMENT;

  cairo_path_t* native = cairo_copy_path(cr);
  cairo_new_path(cr);
  if (native->status != CAIRO_STATUS_SUCCESS) {
    rmr_status s = native->status == CAIRO_STATUS_NO_MEMORY
                       ? RMR_ERR_OUT_OF_MEMORY
                       : RMR_ERR_BACKEND;
    cairo_path_destroy(native);
    return s;
  }

  size_t count = 0;
  for (int i = 0; i < native->num_data; i += native->data[i].header.length)
    count++;

  rmr_status s = RMR_OK;
  if (count > limits->maximum_elements - path->count)
    s = RMR_ERR_LIMIT;
  else if (count) {
    rmr_element* joined =
        realloc(path->elements, (path->count + count) * sizeof *joined);
    if (!joined)
      s = RMR_ERR_OUT_OF_MEMORY;
    else {
      path->elements = joined;
      for (int i = 0; i < native->num_data;
           i += native->data[i].header.length) {
        const cairo_path_data_t* d = &native->data[i];
        rmr_element* v = &path->elements[path->count++];
        switch (d->header.type) {
          case CAIRO_PATH_MOVE_TO:
          case CAIRO_PATH_LINE_TO:
            *v = (rmr_element){.kind = d->header.type == CAIRO_PATH_MOVE_TO
                                           ? RMR_MOVE
                                           : RMR_LINE,
                               .end = {d[1].point.x, d[1].point.y}};
            break;
          case CAIRO_PATH_CURVE_TO:
            *v = (rmr_element){.kind = RMR_CURVE,
                               .control1 = {d[1].point.x, d[1].point.y},
                               .control2 = {d[2].point.x, d[2].point.y},
                               .end = {d[3].point.x, d[3].point.y}};
            break;
          case CAIRO_PATH_CLOSE_PATH:
            *v = (rmr_element){.kind = RMR_CLOSE};
            break;
        }
      }
    }
  }

  cairo_path_destroy(native);
  return s;
}

/* rmr_svg_provider text callback: outlines of the same Pango line Linux TEXT
 * drawing shows, without hinting, as a vector PDF surface would embed it. */
static rmr_status text_outline(void* context, const rmr_text_view* text,
                               const rmr_limits* limits, rmr_path* out) {
  (void)context;
  if (!text || !limits || !out || (!text->utf8 && text->size) ||
      text->size > 65536 || !isfinite(text->font_size) ||
      text->font_size <= 0 || text->font_size > 100000 ||
      !g_utf8_validate(text->utf8 ? text->utf8 : "", (gssize)text->size,
                       nullptr))
    return RMR_ERR_INVALID_ARGUMENT;
  if (!text->size) return RMR_OK;

  cairo_surface_t* surface =
      cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, nullptr);
  cairo_t* cr = cairo_create(surface);
  cairo_font_options_t* options = cairo_font_options_create();
  cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
  cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_NONE);
  cairo_set_font_options(cr, options);
  cairo_font_options_destroy(options);

  PangoLayout* p = rmr_linux_label(cr, text->utf8, text->size, text->font_size);
  cairo_move_to(
      cr, text->origin.x,
      text->origin.y - (double)pango_layout_get_baseline(p) / PANGO_SCALE);
  pango_cairo_layout_path(cr, p);
  g_object_unref(p);

  rmr_path path = {0};
  rmr_status s = cairo_status(cr) == CAIRO_STATUS_SUCCESS
                     ? rmr_linux_path_capture(cr, limits, &path)
                     : RMR_ERR_BACKEND;
  cairo_destroy(cr);
  cairo_surface_destroy(surface);

  if (s == RMR_OK)
    *out = path;
  else
    rmr_path_free(&path);
  return s;
}

/* rmr_svg_provider raster callback: Cairo/PDFium bitmap over transparency. */
static rmr_status raster(void* context, const rmr_plan* plan,
                         const rmr_sheet* sheet, const rmr_limits* limits,
                         rmr_bitmap* out) {
  (void)context;
  return rmr_linux_bitmap_on_sheet(plan, sheet, (rmr_color){0}, limits, nullptr,
                                   nullptr, out, nullptr);
}

rmr_status rmr_linux_svg(const rmr_plan* const* plans, const rmr_sheet* sheets,
                         size_t count, double raster_dpi,
                         const rmr_limits* limits, rmr_cancel_fn cancel,
                         void* context, rmr_svg_pages* out, rmr_error* e) {
  rmr_svg_provider provider = {RMR_SVG_PROVIDER_ABI, sizeof provider, nullptr,
                               text_outline, raster};
  return rmr_svg_sheets(plans, sheets, count, &provider, raster_dpi, limits,
                        cancel, context, out, e);
}
