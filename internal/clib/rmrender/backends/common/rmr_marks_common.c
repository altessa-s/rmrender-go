/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Highlight extraction and mark sheets, shared by the Apple and Cairo marks
 * libraries. Each one compiles this file with its own rmr_marks_platform.h on
 * the include path. */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rmr_marks_platform.h"

struct rmr_page_marks {
  rmm_marked_page page;
  rmm_mark_band* bands;
  rmm_marked_highlight* highlights;
  char* strings;
};

const rmm_marked_page* rmr_page_marks_view(const rmr_page_marks* m) {
  return m ? &m->page : nullptr;
}

/* Frees m's bands, highlights, string storage and m itself; a null
 * pointer is a no-op. */
void rmr_page_marks_free(rmr_page_marks* m) {
  if (m) {
    free(m->bands);
    free(m->highlights);
    free(m->strings);
    free(m);
  }
}

/* Clamps x to the unit interval. */
static double held(double x) { return fmin(fmax(x, 0), 1); }

/* Rectangle r as a page-relative box within frame, each edge held to the unit
 * interval. */
static rmm_page_box normalized(rmr_rect r, rmr_rect frame) {
  return (rmm_page_box){held((r.x - frame.x) / frame.width),
                        held((r.y - frame.y) / frame.height),
                        held(((r.x + r.width) - frame.x) / frame.width),
                        held(((r.y + r.height) - frame.y) / frame.height)};
}

/* Packs c's red, green and blue channels into a 0xRRGGBB value. */
static uint32_t rgb(rmr_color c) {
  return (uint32_t)(c.red * 255) << 16 | (uint32_t)(c.green * 255) << 8 |
         (uint32_t)(c.blue * 255);
}

rmr_status mark_page_marks(const rm_page* page, rmr_point canvas,
                           rmr_rect frame, rm_str id, const mark_fonts* fonts,
                           const rmr_limits* given, rmr_page_marks** out,
                           rmr_error* e) {
  if (!page || !out || (!id.bytes && id.length) ||
      (!page->strokes && page->stroke_count) ||
      (!page->highlights && page->highlight_count) || !isfinite(frame.x) ||
      !isfinite(frame.y) || !isfinite(frame.width) || !isfinite(frame.height) ||
      frame.width <= 0 || frame.height <= 0)
    return mark_error(e, RMR_ERR_INVALID_ARGUMENT, "invalid marks input");

  rmr_limits l = given ? *given : rmr_limits_default();
  if (page->stroke_count > l.maximum_draws ||
      page->highlight_count > l.maximum_draws - page->stroke_count ||
      id.length >= l.maximum_plan_bytes)
    return mark_error(e, RMR_ERR_LIMIT, "marks count limit");
  size_t bytes = sizeof(rmr_page_marks), text_bytes = id.length + 1;
  if (bytes > l.maximum_plan_bytes ||
      page->stroke_count >
          (l.maximum_plan_bytes - bytes) / sizeof(rmm_mark_band))
    return mark_error(e, RMR_ERR_LIMIT, "marks storage limit");
  bytes += page->stroke_count * sizeof(rmm_mark_band);
  if (page->highlight_count >
      (l.maximum_plan_bytes - bytes) / sizeof(rmm_marked_highlight))
    return mark_error(e, RMR_ERR_LIMIT, "marks storage limit");
  bytes += page->highlight_count * sizeof(rmm_marked_highlight);
  size_t items = page->stroke_count + page->highlight_count;
  if (items > (l.maximum_plan_bytes - bytes) / 25)
    return mark_error(e, RMR_ERR_LIMIT, "marks ID limit");
  bytes += items * 25;
  if (text_bytes > l.maximum_plan_bytes - bytes)
    return mark_error(e, RMR_ERR_LIMIT, "marks strings limit");

  size_t rectangles = 0;
  for (size_t i = 0; i < page->highlight_count; i++) {
    const rm_highlight* h = &page->highlights[i];
    if ((!h->rectangles && h->rectangle_count) ||
        (!h->text.bytes && h->has_text && h->text.length))
      return mark_error(e, RMR_ERR_INVALID_ARGUMENT, "invalid highlight data");
    if (h->rectangle_count > l.maximum_elements - rectangles)
      return mark_error(e, RMR_ERR_LIMIT, "highlight rectangle limit");
    rectangles += h->rectangle_count;
    if (h->has_text) {
      if (h->text.length >= l.maximum_plan_bytes - bytes - text_bytes)
        return mark_error(e, RMR_ERR_LIMIT, "highlight text limit");
      text_bytes += h->text.length + 1;
    }
  }
  bytes += text_bytes;

  rmr_limits remaining = l;
  remaining.maximum_plan_bytes -= bytes;
  mark_layout* layout = nullptr;
  rmr_placed_scene* placed = nullptr;
  rmr_anchor_context ac = {0};
  rmr_anchor_provider provider = {0};
  rmr_status status = RMR_OK;
  if (page->has_text) {
    status = mark_layout_create(&page->text, canvas.x, fonts, &remaining,
                                &layout, e);
    if (status != RMR_OK) return status;
    remaining.maximum_plan_bytes -= mark_layout_bytes(layout);
    ac = (rmr_anchor_context){mark_layout_anchors(layout), &l};
    provider = rmr_lines_layout_anchor_provider(&ac);
  }

  status = rmr_lines_place_scene(page->strokes, page->stroke_count, nullptr, 0,
                                 layout ? &provider : nullptr, &remaining,
                                 &placed, e);
  if (status != RMR_OK) {
    mark_layout_free(layout);
    return status;
  }

  rmr_page_marks* m = calloc(1, sizeof *m);
  if (!m) {
    status = RMR_ERR_OUT_OF_MEMORY;
    goto done;
  }
  m->bands = page->stroke_count ? calloc(page->stroke_count, sizeof *m->bands)
                                : nullptr;
  m->highlights = page->highlight_count
                      ? calloc(page->highlight_count, sizeof *m->highlights)
                      : nullptr;
  m->strings = malloc(text_bytes + items * 25);
  if ((page->stroke_count && !m->bands) ||
      (page->highlight_count && !m->highlights) || !m->strings) {
    status = RMR_ERR_OUT_OF_MEMORY;
    goto done;
  }
  m->page.bands = m->bands;
  m->page.highlights = m->highlights;
  size_t at = 0;
  if (id.bytes) {
    memcpy(m->strings, id.bytes, id.length);
    m->strings[id.length] = 0;
    m->page.page_id = (rmm_str){m->strings, id.length};
    at = id.length + 1;
  }

  const rm_stroke* strokes = rmr_placed_scene_strokes(placed);
  for (size_t i = 0; i < page->stroke_count; i++) {
    const rm_stroke* stroke = &strokes[i];
    if (!rm_pen_tool_is_highlighter(stroke->tool) || !stroke->point_count)
      continue;
    rmr_stroke_style style;
    status = rmr_lines_style(stroke, &style, e);
    if (status != RMR_OK) goto done;
    rmr_rect box;
    status = mark_stroke_bounds(stroke, &style, &box);
    if (status != RMR_OK) goto done;
    int n = rm_item_id_format(stroke->id, m->strings + at, 25);
    m->bands[m->page.band_count++] = (rmm_mark_band){
        normalized(box, frame), rgb(style.color), {m->strings + at, (size_t)n}};
    at += 25;
  }

  for (size_t i = 0; i < page->highlight_count; i++) {
    const rm_highlight* h = &page->highlights[i];
    if (!h->has_text || !h->text.length || !h->rectangle_count) continue;
    rmr_rect box = {0};
    for (size_t j = 0; j < h->rectangle_count; j++) {
      rm_rect r = h->rectangles[j];
      if (!isfinite(r.x) || !isfinite(r.y) || !isfinite(r.width) ||
          !isfinite(r.height) || r.width < 0 || r.height < 0) {
        status = RMR_ERR_INVALID_ARGUMENT;
        goto done;
      }
      if (!j)
        box = (rmr_rect){r.x, r.y, r.width, r.height};
      else {
        double x = fmin(box.x, r.x), y = fmin(box.y, r.y);
        box = (rmr_rect){x, y, fmax(box.x + box.width, r.x + r.width) - x,
                         fmax(box.y + box.height, r.y + r.height) - y};
      }
    }

    rmr_color color;
    status = rmr_lines_highlight_color(h, &color, e);
    if (status != RMR_OK) goto done;
    int n = rm_item_id_format(h->id, m->strings + at, 25);
    rmm_mark_band band = {
        normalized(box, frame), rgb(color), {m->strings + at, (size_t)n}};
    at += 25;
    memcpy(m->strings + at, h->text.bytes, h->text.length);
    m->strings[at + h->text.length] = 0;
    m->highlights[m->page.highlight_count++] =
        (rmm_marked_highlight){{m->strings + at, h->text.length}, band};
    at += h->text.length + 1;
  }

done:
  rmr_placed_scene_free(placed);
  mark_layout_free(layout);
  if (status != RMR_OK) {
    rmr_page_marks_free(m);
    return mark_error(e, status, "mark extraction failed");
  }
  *out = m;
  return RMR_OK;
}

rmm_page_box rmr_mark_around(rmm_page_box b, double margin) {
  double air = (b.y1 - b.y0) * margin;
  return (rmm_page_box){fmax(0, b.x0 - air * 2), fmax(0, b.y0 - air),
                        fmin(1, b.x1 + air * 2), fmin(1, b.y1 + air)};
}

void rmr_mark_sheet_free(rmr_mark_sheet* sheet) {
  if (sheet) {
    rmr_bitmap_free(&sheet->bitmap);
    free((void*)(uintptr_t)sheet->strips);
    *sheet = (rmr_mark_sheet){0};
  }
}

rmr_status mark_mark_sheet(const rmr_bitmap* page, const rmm_page_box* blocks,
                           size_t count, size_t margin, size_t rule,
                           const rmr_limits* given, rmr_mark_sheet* out,
                           rmr_error* e) {
  if (!page || !out || (!blocks && count) || !page->pixels || !page->width ||
      !page->height || page->width > SIZE_MAX / 4 ||
      page->stride < page->width * 4)
    return mark_error(e, RMR_ERR_INVALID_ARGUMENT, "invalid mark sheet input");

  rmr_limits l = given ? *given : rmr_limits_default();
  if (page->width > l.maximum_bitmap_side ||
      page->height > l.maximum_bitmap_side ||
      page->stride > l.maximum_bitmap_bytes / page->height ||
      count > l.maximum_elements || margin > l.maximum_bitmap_side ||
      rule > l.maximum_bitmap_side)
    return mark_error(e, RMR_ERR_LIMIT, "mark sheet input limit");

  typedef struct {
    size_t x, y, w, h, index;
  } cut;
  if (count > l.maximum_plan_bytes / (sizeof(cut) + sizeof(rmm_mark_strip)))
    return mark_error(e, RMR_ERR_LIMIT, "mark sheet storage limit");

  cut* cuts = count ? calloc(count, sizeof *cuts) : nullptr;
  if (count && !cuts)
    return mark_error(e, RMR_ERR_OUT_OF_MEMORY, "mark sheet allocation failed");

  size_t used = 0, width = 0, height = rule;
  rmr_status status = RMR_OK;
  for (size_t i = 0; i < count; i++) {
    rmm_page_box b = blocks[i];
    if (!isfinite(b.x0) || !isfinite(b.y0) || !isfinite(b.x1) ||
        !isfinite(b.y1) || b.x1 < b.x0 || b.y1 < b.y0) {
      status = RMR_ERR_INVALID_ARGUMENT;
      goto done;
    }

    double x0 = fmax(0, b.x0 * (double)page->width - (double)margin),
           y0 = fmax(0, b.y0 * (double)page->height - (double)margin);
    double x1 = fmin((double)page->width,
                     b.x1 * (double)page->width + (double)margin),
           y1 = fmin((double)page->height,
                     b.y1 * (double)page->height + (double)margin);
    if (x1 <= x0 || y1 <= y0) continue;
    size_t x = (size_t)floor(x0), y = (size_t)floor(y0),
           w = (size_t)ceil(x1) - x, h = (size_t)ceil(y1) - y;
    if (w < 2 || h < 2) continue;
    if (h > l.maximum_bitmap_side - height ||
        rule > l.maximum_bitmap_side - height - h) {
      status = RMR_ERR_LIMIT;
      goto done;
    }
    height += h + rule;
    if (w > width) width = w;
    cuts[used++] = (cut){x, y, w, h, i};
  }

  if (!used) {
    *out = (rmr_mark_sheet){0};
    goto done;
  }

  size_t scratch = count * sizeof(cut),
         strip_bytes = used * sizeof(rmm_mark_strip);
  if (width > SIZE_MAX / 4 || width * 4 > l.maximum_bitmap_bytes / height ||
      strip_bytes > l.maximum_plan_bytes - scratch ||
      width * 4 > (l.maximum_plan_bytes - scratch - strip_bytes) / height) {
    status = RMR_ERR_LIMIT;
    goto done;
  }

  uint8_t* pixels = malloc(width * 4 * height);
  rmm_mark_strip* strips = calloc(used, sizeof *strips);
  if (!pixels || !strips) {
    free(pixels);
    free(strips);
    status = RMR_ERR_OUT_OF_MEMORY;
    goto done;
  }

  memset(pixels, 255, width * 4 * height);
  size_t top = rule;
  for (size_t i = 0; i < used; i++) {
    cut c = cuts[i];
    for (size_t y = 0; y < c.h; y++) {
      const uint8_t* source = page->pixels + (c.y + y) * page->stride + c.x * 4;
      uint8_t* target = pixels + (top + y) * width * 4;
      for (size_t x = 0; x < c.w; x++) {
        unsigned white = 255u - source[x * 4 + 3];
        for (size_t channel = 0; channel < 3; channel++) {
          unsigned value = source[x * 4 + channel] + white;
          target[x * 4 + channel] = (uint8_t)(value > 255 ? 255 : value);
        }
      }
    }

    strips[i] = (rmm_mark_strip){
        blocks[c.index],
        {0, (double)top / (double)height, (double)c.w / (double)width,
         (double)(top + c.h) / (double)height}};
    top += c.h;

    for (size_t y = 0; y < rule; y++)
      for (size_t x = 0; x < width; x++) {
        uint8_t* pixel = pixels + ((top + y) * width + x) * 4;
        pixel[0] = pixel[1] = pixel[2] = 0;
      }
    top += rule;
  }

  *out = (rmr_mark_sheet){{pixels, width, height, width * 4}, strips, used};

done:
  free(cuts);
  return status == RMR_OK
             ? RMR_OK
             : mark_error(e, status, "mark sheet construction failed");
}
