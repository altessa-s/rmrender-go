/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <math.h>
#include <rmrender/apple.h>
#include <stdlib.h>
#include <string.h>

#include "rmr_apple_internal.h"

typedef struct {
  rmr_element* elements;
  size_t count, capacity, maximum;
  CGAffineTransform map;
  CGPoint current, start;
  rmr_status status;
} builder;

/* Appends one element, growing the builder within its element limit. */
static void append(builder* b, rmr_element element) {
  if (b->status != RMR_OK) return;

  if (b->count == b->maximum) {
    b->status = RMR_ERR_LIMIT;
    return;
  }

  if (b->count == b->capacity) {
    size_t capacity = b->capacity ? b->capacity * 2 : 64;
    if (capacity > b->maximum) capacity = b->maximum;
    rmr_element* grown = realloc(b->elements, capacity * sizeof *grown);
    if (!grown) {
      b->status = RMR_ERR_OUT_OF_MEMORY;
      return;
    }
    b->elements = grown;
    b->capacity = capacity;
  }

  b->elements[b->count++] = element;
}

/* Maps a glyph-path point to page coordinates. */
static rmr_point mapped(const builder* b, CGPoint p) {
  CGPoint q = CGPointApplyAffineTransform(p, b->map);
  return (rmr_point){q.x, q.y};
}

/* CGPathApply callback: converts one element, raising quadratic segments to
 * the equivalent cubic. */
static void element(void* info, const CGPathElement* e) {
  builder* b = info;
  const CGPoint* p = e->points;

  switch (e->type) {
    case kCGPathElementMoveToPoint:
      append(b, (rmr_element){.kind = RMR_MOVE, .end = mapped(b, p[0])});
      b->current = b->start = p[0];
      break;
    case kCGPathElementAddLineToPoint:
      append(b, (rmr_element){.kind = RMR_LINE, .end = mapped(b, p[0])});
      b->current = p[0];
      break;
    case kCGPathElementAddQuadCurveToPoint: {
      CGPoint c1 = {b->current.x + 2 * (p[0].x - b->current.x) / 3,
                    b->current.y + 2 * (p[0].y - b->current.y) / 3};
      CGPoint c2 = {p[1].x + 2 * (p[0].x - p[1].x) / 3,
                    p[1].y + 2 * (p[0].y - p[1].y) / 3};
      append(b, (rmr_element){.kind = RMR_CURVE,
                              .control1 = mapped(b, c1),
                              .control2 = mapped(b, c2),
                              .end = mapped(b, p[1])});
      b->current = p[1];
      break;
    }
    case kCGPathElementAddCurveToPoint:
      append(b, (rmr_element){.kind = RMR_CURVE,
                              .control1 = mapped(b, p[0]),
                              .control2 = mapped(b, p[1]),
                              .end = mapped(b, p[2])});
      b->current = p[2];
      break;
    case kCGPathElementCloseSubpath:
      append(b, (rmr_element){.kind = RMR_CLOSE});
      b->current = b->start;
      break;
  }
}

/* Appends the outlines of one glyph run, placed at its glyph positions. */
static void run_outline(builder* b, CTRunRef run, CGAffineTransform map) {
  CFIndex count = CTRunGetGlyphCount(run);
  CTFontRef font =
      CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName);
  if (count <= 0 || !font) return;

  CGGlyph* glyphs = malloc((size_t)count * sizeof *glyphs);
  CGPoint* positions = malloc((size_t)count * sizeof *positions);
  if (!glyphs || !positions) {
    b->status = RMR_ERR_OUT_OF_MEMORY;
    free(glyphs);
    free(positions);
    return;
  }

  CTRunGetGlyphs(run, CFRangeMake(0, 0), glyphs);
  CTRunGetPositions(run, CFRangeMake(0, 0), positions);
  /* Glyphs are shown at their positions in text space: the run's text
   * matrix, which carries the font's own width and slant, scales positions
   * from the line origin and outlines alike, as in the PDF output. Glyph
   * paths are plain em space. */
  CGAffineTransform text = CTRunGetTextMatrix(run);
  text.tx = text.ty = 0;
  CGAffineTransform space = CGAffineTransformConcat(text, map);

  for (CFIndex i = 0; i < count && b->status == RMR_OK; i++) {
    CGPathRef glyph = CTFontCreatePathForGlyph(font, glyphs[i], nullptr);
    if (!glyph) continue;
    b->map = CGAffineTransformConcat(
        CGAffineTransformMakeTranslation(positions[i].x, positions[i].y),
        space);
    CGPathApply(glyph, b, element);
    CGPathRelease(glyph);
  }

  free(glyphs);
  free(positions);
}

rmr_status rmr_apple_line_outline(CTLineRef line, CGAffineTransform map,
                                  const rmr_limits* limits, rmr_path* path) {
  if (!line || !limits || !path || path->count > limits->maximum_elements)
    return RMR_ERR_INVALID_ARGUMENT;

  builder b = {.maximum = limits->maximum_elements - path->count};
  CFArrayRef runs = CTLineGetGlyphRuns(line);
  for (CFIndex i = 0; i < CFArrayGetCount(runs) && b.status == RMR_OK; i++)
    run_outline(&b, CFArrayGetValueAtIndex(runs, i), map);

  if (b.status == RMR_OK && b.count) {
    rmr_element* joined =
        realloc(path->elements, (path->count + b.count) * sizeof *joined);
    if (!joined)
      b.status = RMR_ERR_OUT_OF_MEMORY;
    else {
      memcpy(joined + path->count, b.elements, b.count * sizeof *joined);
      path->elements = joined;
      path->count += b.count;
    }
  }

  free(b.elements);
  return b.status;
}

/* rmr_svg_provider text callback: TEXT glyph outlines from the same CoreText
 * Helvetica line the backend draws, flipped onto the y-down baseline. */
static rmr_status text_outline(void* context, const rmr_text_view* text,
                               const rmr_limits* limits, rmr_path* out) {
  (void)context;
  if (!text || !limits || !out) return RMR_ERR_INVALID_ARGUMENT;
  if (!text->size) return RMR_OK;

  CTLineRef line = nullptr;
  rmr_status s = rmr_apple_text_line(text->utf8, text->size, text->font_size,
                                     (rmr_color){0, 0, 0, 1}, &line);
  if (s != RMR_OK) return s;

  rmr_path path = {0};
  s = rmr_apple_line_outline(
      line, CGAffineTransformMake(1, 0, 0, -1, text->origin.x, text->origin.y),
      limits, &path);
  CFRelease(line);

  if (s == RMR_OK)
    *out = path;
  else
    rmr_path_free(&path);
  return s;
}

/* rmr_svg_provider raster callback: CoreGraphics bitmap over transparency. */
static rmr_status raster(void* context, const rmr_plan* plan,
                         const rmr_sheet* sheet, const rmr_limits* limits,
                         rmr_bitmap* out) {
  (void)context;
  return rmr_apple_bitmap_on_sheet(plan, sheet, (rmr_color){0}, limits, nullptr,
                                   nullptr, out, nullptr);
}

rmr_status rmr_apple_svg(const rmr_plan* const* plans, const rmr_sheet* sheets,
                         size_t count, double raster_dpi,
                         const rmr_limits* limits, rmr_cancel_fn cancel,
                         void* context, rmr_svg_pages* out, rmr_error* e) {
  rmr_svg_provider provider = {RMR_SVG_PROVIDER_ABI, sizeof provider, nullptr,
                               text_outline, raster};
  return rmr_svg_sheets(plans, sheets, count, &provider, raster_dpi, limits,
                        cancel, context, out, e);
}
