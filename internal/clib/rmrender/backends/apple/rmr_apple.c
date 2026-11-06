/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <limits.h>
#include <math.h>
#include <rmrender/apple.h>
#include <rmrender/apple_context.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/rmr_pdf_options.h"
#include "rmr_apple_internal.h"

/* Fill `e` with `code` and `message`, if non-null; always returns `code`. */
static rmr_status error(rmr_error* e, rmr_status code, const char* message) {
  if (e) {
    *e = (rmr_error){.code = code};
    snprintf(e->message, sizeof e->message, "%s", message);
  }
  return code;
}

typedef struct {
  CGContextRef cg;
  double scale;
  bool saved;
  uint64_t group;
  rmr_limits limits;
  const rmr_sheet* sheet;
  bool unclipped;
} apple_context;

/* rmr_backend begin: save graphics state, derive and apply the page-to-
 * context transform, and clip to the sheet or viewport. */
static rmr_status begin(void* context, rmr_rect viewport) {
  apple_context* a = context;
  rmr_transform t;
  rmr_status s = RMR_OK;

  if (a->sheet)
    t = a->sheet->transform;
  else
    s = rmr_page_to_pdf(viewport, a->scale, &t, nullptr);

  if (s != RMR_OK) return s;

  CGContextSaveGState(a->cg);
  a->saved = true;
  if (a->sheet && !a->unclipped)
    CGContextClipToRect(a->cg,
                        CGRectMake(0, 0, a->sheet->width, a->sheet->height));
  CGContextConcatCTM(a->cg,
                     CGAffineTransformMake(t.a, t.b, t.c, t.d, t.tx, t.ty));
  if (!a->sheet)
    CGContextClipToRect(a->cg, CGRectMake(viewport.x, viewport.y,
                                          viewport.width, viewport.height));

  return RMR_OK;
}

/* End an open transparency group and restore the state saved to open it. */
static void close_group(apple_context* a) {
  if (a->group) {
    CGContextEndTransparencyLayer(a->cg);
    CGContextRestoreGState(a->cg);
    a->group = 0;
  }
}

/* Map an rmr_blend to the matching CGBlendMode. */
static CGBlendMode blend(rmr_blend b) {
  return b == RMR_BLEND_DARKEN     ? kCGBlendModeDarken
         : b == RMR_BLEND_MULTIPLY ? kCGBlendModeMultiply
                                   : kCGBlendModeNormal;
}

/* Map an rmr_cap to the matching CGLineCap. */
static CGLineCap line_cap(rmr_cap c) {
  return c == RMR_CAP_ROUND    ? kCGLineCapRound
         : c == RMR_CAP_SQUARE ? kCGLineCapSquare
                               : kCGLineCapButt;
}

/* Map an rmr_join to the matching CGLineJoin. */
static CGLineJoin line_join(rmr_join j) {
  return j == RMR_JOIN_ROUND   ? kCGLineJoinRound
         : j == RMR_JOIN_MITER ? kCGLineJoinMiter
                               : kCGLineJoinBevel;
}

/* rmr_backend abort: close any open group and restore the saved state. */
static void restore(void* context) {
  apple_context* a = context;
  close_group(a);
  if (a->saved) {
    CGContextRestoreGState(a->cg);
    a->saved = false;
  }
}

/* rmr_backend end: same cleanup as abort, always reporting success. */
static rmr_status end(void* context) {
  restore(context);
  return RMR_OK;
}

/* Build a CGPath from a plan path view; nullptr on allocation failure. */
static CGPathRef make_path(rmr_path_view view) {
  CGMutablePathRef path = CGPathCreateMutable();
  if (!path) return nullptr;

  for (size_t i = 0; i < view.count; i++) {
    const rmr_element* v = &view.elements[i];
    switch (v->kind) {
      case RMR_MOVE:
        CGPathMoveToPoint(path, nullptr, v->end.x, v->end.y);
        break;
      case RMR_LINE:
        CGPathAddLineToPoint(path, nullptr, v->end.x, v->end.y);
        break;
      case RMR_CURVE:
        CGPathAddCurveToPoint(path, nullptr, v->control1.x, v->control1.y,
                              v->control2.x, v->control2.y, v->end.x, v->end.y);
        break;
      case RMR_CLOSE:
        CGPathCloseSubpath(path);
        break;
    }
  }

  return path;
}

/* PDF contexts may retain images/pages until close, beyond plan execution.
 * Give the native provider its own storage rather than a borrowed plan buffer.
 */
static CGDataProviderRef owned_provider(const uint8_t* bytes, size_t size) {
  if (size > LONG_MAX) return nullptr;
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, bytes, (CFIndex)size);
  if (!data) return nullptr;
  CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
  CFRelease(data);
  return provider;
}

/* Decode and draw a PNG/JPEG image command, clipped to its path. */
static rmr_status draw_image(apple_context* a, const rmr_draw* d) {
  const rmr_image_view* im = &d->image;
  CGDataProviderRef provider = owned_provider(im->bytes, im->size);
  if (!provider) return RMR_ERR_OUT_OF_MEMORY;

  CGImageRef image = nullptr;
  if (im->size >= 4 && memcmp(im->bytes, "\x89PNG", 4) == 0)
    image = CGImageCreateWithPNGDataProvider(provider, nullptr, true,
                                             kCGRenderingIntentDefault);
  else if (im->size >= 2 && im->bytes[0] == 0xff && im->bytes[1] == 0xd8)
    image = CGImageCreateWithJPEGDataProvider(provider, nullptr, true,
                                              kCGRenderingIntentDefault);
  CGDataProviderRelease(provider);

  if (!image) return RMR_ERR_BACKEND;

  size_t width = CGImageGetWidth(image), height = CGImageGetHeight(image);
  if (!width || !height || width > a->limits.maximum_bitmap_side ||
      height > a->limits.maximum_bitmap_side || width > 64000000 / height ||
      width > a->limits.maximum_bitmap_bytes / 4 / height) {
    CGImageRelease(image);
    return RMR_ERR_LIMIT;
  }

  CGPathRef path = make_path(d->path);
  if (!path) {
    CGImageRelease(image);
    return RMR_ERR_OUT_OF_MEMORY;
  }

  CGContextSaveGState(a->cg);
  CGContextSetBlendMode(a->cg, blend(d->blend));
  CGContextSetAlpha(a->cg, d->color.alpha);
  CGContextAddPath(a->cg, path);
  CGContextClip(a->cg);
  CGPathRelease(path);
  rmr_transform t = im->transform;
  CGContextConcatCTM(a->cg,
                     CGAffineTransformMake(t.a, t.b, t.c, t.d, t.tx, t.ty));
  CGContextTranslateCTM(a->cg, 0, 1);
  CGContextScaleCTM(a->cg, 1, -1);

  CGContextDrawImage(a->cg, CGRectMake(0, 0, 1, 1), image);
  CGContextRestoreGState(a->cg);
  CGImageRelease(image);
  return RMR_OK;
}

/* Draw one page of an embedded PDF image command, clipped to its path. */
static rmr_status draw_pdf(apple_context* a, const rmr_draw* d) {
  if (d->image.size > a->limits.maximum_pdf_bytes || d->pdf_page == SIZE_MAX)
    return RMR_ERR_LIMIT;

  CGDataProviderRef provider = owned_provider(d->image.bytes, d->image.size);
  if (!provider) return RMR_ERR_OUT_OF_MEMORY;
  CGPDFDocumentRef document = CGPDFDocumentCreateWithProvider(provider);
  CGDataProviderRelease(provider);
  if (!document) return RMR_ERR_BACKEND;

  if (CGPDFDocumentIsEncrypted(document) &&
      !CGPDFDocumentIsUnlocked(document)) {
    CGPDFDocumentRelease(document);
    return RMR_ERR_UNSUPPORTED;
  }

  CGPDFPageRef page = CGPDFDocumentGetPage(document, d->pdf_page + 1);
  if (!page) {
    CGPDFDocumentRelease(document);
    return RMR_ERR_INVALID_ARGUMENT;
  }

  CGRect box = CGPDFPageGetBoxRect(page, kCGPDFMediaBox);
  if (!isfinite(box.origin.x) || !isfinite(box.origin.y) ||
      !isfinite(box.size.width) || !isfinite(box.size.height) ||
      box.size.width <= 0 || box.size.height <= 0) {
    CGPDFDocumentRelease(document);
    return RMR_ERR_BACKEND;
  }

  CGPathRef path = make_path(d->path);
  if (!path) {
    CGPDFDocumentRelease(document);
    return RMR_ERR_OUT_OF_MEMORY;
  }

  CGContextSaveGState(a->cg);
  CGContextSetBlendMode(a->cg, blend(d->blend));
  CGContextSetAlpha(a->cg, d->color.alpha);
  CGContextAddPath(a->cg, path);
  CGContextClip(a->cg);
  CGPathRelease(path);

  rmr_transform t = d->image.transform;
  CGContextConcatCTM(a->cg,
                     CGAffineTransformMake(t.a, t.b, t.c, t.d, t.tx, t.ty));
  CGContextTranslateCTM(a->cg, 0, 1);
  CGContextScaleCTM(a->cg, 1, -1);
  CGContextConcatCTM(
      a->cg, CGPDFPageGetDrawingTransform(page, kCGPDFMediaBox,
                                          CGRectMake(0, 0, 1, 1), 0, false));
  CGContextDrawPDFPage(a->cg, page);

  CGContextRestoreGState(a->cg);
  CGPDFDocumentRelease(document);
  return RMR_OK;
}

rmr_status rmr_apple_text_line(const char* utf8, size_t size, double font_size,
                               rmr_color color, CTLineRef* out) {
  if ((!utf8 && size) || !isfinite(font_size) || font_size <= 0 ||
      font_size > 1e6)
    return RMR_ERR_INVALID_ARGUMENT;
  if (size > 65536) return RMR_ERR_LIMIT;

  CFStringRef string = CFStringCreateWithBytes(
      kCFAllocatorDefault, (const UInt8*)(utf8 ? utf8 : ""), (CFIndex)size,
      kCFStringEncodingUTF8, false);
  if (!string) return RMR_ERR_INVALID_ARGUMENT;

  CTFontRef font = CTFontCreateWithName(CFSTR("Helvetica"), font_size, nullptr);
  CGColorRef paint =
      CGColorCreateSRGB(color.red, color.green, color.blue, color.alpha);
  const void* keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
  const void* values[] = {font, paint};
  CFDictionaryRef attributes =
      font && paint ? CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
                                         &kCFTypeDictionaryKeyCallBacks,
                                         &kCFTypeDictionaryValueCallBacks)
                    : nullptr;
  CFAttributedStringRef attributed =
      attributes
          ? CFAttributedStringCreate(kCFAllocatorDefault, string, attributes)
          : nullptr;
  CTLineRef line =
      attributed ? CTLineCreateWithAttributedString(attributed) : nullptr;

  if (attributed) CFRelease(attributed);
  if (attributes) CFRelease(attributes);
  if (paint) CGColorRelease(paint);
  if (font) CFRelease(font);
  CFRelease(string);

  if (!line) return RMR_ERR_OUT_OF_MEMORY;
  *out = line;
  return RMR_OK;
}

/* rmr_text_provider measure callback: typographic width and ink bounds. */
static rmr_status measure_text(void* context, const char* utf8, size_t size,
                               double font_size, rmr_text_metrics* out) {
  (void)context;
  if (!out) return RMR_ERR_INVALID_ARGUMENT;

  CTLineRef line = nullptr;
  rmr_status s = rmr_apple_text_line(utf8, size, font_size,
                                     (rmr_color){0, 0, 0, 1}, &line);
  if (s != RMR_OK) return s;

  CGFloat ascent = 0, descent = 0, leading = 0;
  double width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
  CGRect ink =
      CTLineGetBoundsWithOptions(line, kCTLineBoundsUseGlyphPathBounds);
  CGRect box =
      CGRectUnion(CGRectMake(0, -descent, width, ascent + descent), ink);
  rmr_text_metrics metrics = {
      width,
      {box.origin.x, -CGRectGetMaxY(box), box.size.width, box.size.height}};

  CFRelease(line);
  if (!size) metrics = (rmr_text_metrics){0};

  *out = metrics;
  return RMR_OK;
}

rmr_text_provider rmr_apple_text_provider(void) {
  return (rmr_text_provider){RMR_TEXT_PROVIDER_ABI, sizeof(rmr_text_provider),
                             nullptr, measure_text};
}

/* Draw a single text run at its baseline origin. */
static rmr_status draw_text(apple_context* a, const rmr_draw* d) {
  CTLineRef line = nullptr;
  rmr_status s = rmr_apple_text_line(d->text.utf8, d->text.size,
                                     d->text.font_size, d->color, &line);
  if (s != RMR_OK) return s;

  CGContextSaveGState(a->cg);
  CGContextSetBlendMode(a->cg, blend(d->blend));
  CGContextSetAlpha(a->cg, 1);
  CGContextTranslateCTM(a->cg, d->text.origin.x, d->text.origin.y);
  CGContextScaleCTM(a->cg, 1, -1);
  CGContextSetTextMatrix(a->cg, CGAffineTransformIdentity);
  CGContextSetTextPosition(a->cg, 0, 0);
  CTLineDraw(line, a->cg);

  CGContextRestoreGState(a->cg);
  CFRelease(line);
  return RMR_OK;
}

/* rmr_backend draw: dispatch to the PDF/image/text/path drawers, opening
 * and closing transparency groups as the command stream's group changes. */
static rmr_status draw(void* context, const rmr_draw* d) {
  apple_context* a = context;
  if (a->group != d->group) {
    close_group(a);
    if (d->group) {
      CGContextSaveGState(a->cg);
      CGContextSetBlendMode(a->cg, blend(d->group_blend));
      CGContextSetAlpha(a->cg, d->group_alpha);
      CGContextBeginTransparencyLayer(a->cg, nullptr);
      a->group = d->group;
    }
  }

  if (d->mode == RMR_PDF) return draw_pdf(a, d);
  if (d->mode == RMR_IMAGE) return draw_image(a, d);
  if (d->mode == RMR_TEXT) return draw_text(a, d);

  CGPathRef path = make_path(d->path);
  if (!path) return RMR_ERR_OUT_OF_MEMORY;

  if (d->eraser_count) {
    if (__builtin_available(macOS 13.0, iOS 16.0, *)) {
      if (d->mode == RMR_STROKE) {
        CGPathRef outline = CGPathCreateCopyByStrokingPath(
            path, nullptr, d->line_width, line_cap(d->cap), line_join(d->join),
            d->miter_limit != 0 ? d->miter_limit : 10);
        CGPathRelease(path);
        path = outline;
        if (!path) return RMR_ERR_OUT_OF_MEMORY;
      }

      for (size_t i = 0; i < d->eraser_count; i++) {
        CGPathRef mask = make_path(d->erasers[i]);
        if (!mask) {
          CGPathRelease(path);
          return RMR_ERR_OUT_OF_MEMORY;
        }
        CGPathRef cut = CGPathCreateCopyBySubtractingPath(path, mask, false);
        CGPathRelease(mask);
        CGPathRelease(path);
        path = cut;
        if (!path) return RMR_ERR_BACKEND;
      }
    } else {
      CGPathRelease(path);
      return RMR_ERR_UNSUPPORTED;
    }
  }

  CGContextSaveGState(a->cg);
  CGContextSetBlendMode(a->cg, d->blend == RMR_BLEND_DARKEN ? kCGBlendModeDarken
                               : d->blend == RMR_BLEND_MULTIPLY
                                   ? kCGBlendModeMultiply
                                   : kCGBlendModeNormal);
  CGContextSetAlpha(a->cg, d->color.alpha);
  CGContextAddPath(a->cg, path);
  CGPathRelease(path);

  if (d->mode == RMR_FILL_STROKE) {
    CGContextSetAlpha(a->cg, 1);
    CGContextSetRGBFillColor(a->cg, d->color.red, d->color.green, d->color.blue,
                             d->color.alpha);
    CGContextSetRGBStrokeColor(a->cg, d->stroke_color.red,
                               d->stroke_color.green, d->stroke_color.blue,
                               d->stroke_color.alpha);
    CGContextSetLineWidth(a->cg, d->line_width);
    CGContextSetLineCap(a->cg, line_cap(d->cap));
    CGContextSetLineJoin(a->cg, line_join(d->join));
    CGContextSetMiterLimit(a->cg, d->miter_limit != 0 ? d->miter_limit : 10);
    CGContextDrawPath(a->cg, kCGPathFillStroke);
  } else if (d->mode == RMR_FILL || d->eraser_count) {
    CGContextSetRGBFillColor(a->cg, d->color.red, d->color.green, d->color.blue,
                             1);
    if (d->mask.bytes) {
      const rmr_mask_view* m = &d->mask;
      CGDataProviderRef provider =
          owned_provider(m->bytes, m->stride * m->height);
      CGImageRef image =
          provider ? CGImageMaskCreate(m->width, m->height, 1, 1, m->stride,
                                       provider, nullptr, false)
                   : nullptr;
      if (provider) CGDataProviderRelease(provider);

      if (!image) {
        CGContextRestoreGState(a->cg);
        return RMR_ERR_OUT_OF_MEMORY;
      }

      CGContextClip(a->cg);
      CGContextSetInterpolationQuality(a->cg, kCGInterpolationNone);
      /* Images have their own bottom-up placement; page coordinates are
       * top-down. Flip just the image so packed row zero stays at box.y. */
      CGContextTranslateCTM(a->cg, m->box.x, m->box.y + m->box.height);
      CGContextScaleCTM(a->cg, 1, -1);
      CGContextDrawImage(a->cg, CGRectMake(0, 0, m->box.width, m->box.height),
                         image);
      CGImageRelease(image);
    } else
      CGContextFillPath(a->cg);
  } else {
    CGContextSetRGBStrokeColor(a->cg, d->color.red, d->color.green,
                               d->color.blue, 1);
    CGContextSetLineWidth(a->cg, d->line_width);
    CGContextSetLineCap(a->cg, d->cap == RMR_CAP_ROUND    ? kCGLineCapRound
                               : d->cap == RMR_CAP_SQUARE ? kCGLineCapSquare
                                                          : kCGLineCapButt);
    CGContextSetLineJoin(a->cg, line_join(d->join));
    CGContextSetMiterLimit(a->cg, d->miter_limit != 0 ? d->miter_limit : 10);
    CGContextStrokePath(a->cg);
  }

  CGContextRestoreGState(a->cg);
  return RMR_OK;
}

/* Build the rmr_backend vtable and runtime-available capability set. */
static rmr_backend backend(apple_context* context) {
  uint64_t capabilities = RMR_CAP_FILL | RMR_CAP_STROKE | RMR_CAP_DARKEN |
                          RMR_CAP_MULTIPLY | RMR_CAP_MASK | RMR_CAP_GROUP |
                          RMR_CAP_IMAGE | RMR_CAP_FILL_STROKE | RMR_CAP_MITER |
                          RMR_CAP_TEXT | RMR_CAP_PDF;
  if (__builtin_available(macOS 13.0, iOS 16.0, *))
    capabilities |= RMR_CAP_ERASE;
  return (rmr_backend){RMR_BACKEND_ABI,
                       sizeof(rmr_backend),
                       capabilities,
                       context,
                       begin,
                       draw,
                       end,
                       restore};
}

void rmr_bitmap_free(rmr_bitmap* b) {
  if (b) {
    free(b->pixels);
    *b = (rmr_bitmap){0};
  }
}

/* Whether sheet has finite positive dimensions and an axis-aligned,
 * uniformly-scaled transform; pixels additionally requires integral width
 * and height. */
static bool valid_sheet(const rmr_sheet* s, bool pixels) {
  if (!s || !isfinite(s->width) || !isfinite(s->height) || s->width <= 0 ||
      s->height <= 0 ||
      (pixels &&
       (floor(s->width) != s->width || floor(s->height) != s->height)))
    return false;
  rmr_transform t = s->transform;
  return isfinite(t.a) && t.a > 0 && t.b == 0 && t.c == 0 && t.d == -t.a &&
         isfinite(t.tx) && isfinite(t.ty);
}

/* Shared implementation behind rmr_apple_bitmap and rmr_apple_bitmap_on_sheet.
 */
static rmr_status bitmap_impl(const rmr_plan* plan, double scale,
                              const rmr_sheet* sheet, rmr_color background,
                              const rmr_limits* given, rmr_cancel_fn cancel,
                              void* context, rmr_bitmap* out, rmr_error* e) {
  rmr_limits l = given ? *given : rmr_limits_default();
  if (!plan || !out || (sheet && !valid_sheet(sheet, true)) ||
      !isfinite(scale) || scale <= 0 || !isfinite(background.red) ||
      !isfinite(background.green) || !isfinite(background.blue) ||
      !isfinite(background.alpha) || background.red < 0 || background.red > 1 ||
      background.green < 0 || background.green > 1 || background.blue < 0 ||
      background.blue > 1 || background.alpha < 0 || background.alpha > 1)
    return error(e, RMR_ERR_INVALID_ARGUMENT, "invalid bitmap arguments");
  if (cancel && cancel(context))
    return error(e, RMR_ERR_CANCELLED, "cancelled");

  rmr_rect v = rmr_plan_viewport(plan);
  double width = sheet ? sheet->width : ceil(v.width * scale);
  double height = sheet ? sheet->height : ceil(v.height * scale);
  if (!isfinite(width) || !isfinite(height) || width < 1 || height < 1 ||
      width > l.maximum_bitmap_side || height > l.maximum_bitmap_side ||
      width > (double)(SIZE_MAX / 4))
    return error(e, RMR_ERR_LIMIT, "bitmap side limit");
  size_t w = (size_t)width, h = (size_t)height, stride = w * 4;
  if (h > l.maximum_bitmap_bytes / stride)
    return error(e, RMR_ERR_LIMIT, "bitmap byte limit");

  uint8_t* pixels = calloc(h, stride);
  if (!pixels)
    return error(e, RMR_ERR_OUT_OF_MEMORY, "bitmap allocation failed");

  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef cg = space
                        ? CGBitmapContextCreate(pixels, w, h, 8, stride, space,
                                                kCGImageAlphaPremultipliedLast |
                                                    kCGBitmapByteOrder32Big)
                        : nullptr;
  if (space) CGColorSpaceRelease(space);
  if (!cg) {
    free(pixels);
    return error(e, RMR_ERR_BACKEND, "bitmap context unavailable");
  }

  CGContextSetRGBFillColor(cg, background.red, background.green,
                           background.blue, background.alpha);
  CGContextFillRect(cg, CGRectMake(0, 0, width, height));
  /* Round-up adds spare pixels at bottom/right, not a shift in page
   * coordinates. */
  if (!sheet) CGContextTranslateCTM(cg, 0, height - v.height * scale);

  apple_context state = {.cg = cg, .scale = scale, .limits = l, .sheet = sheet};
  rmr_backend b = backend(&state);
  rmr_status status = rmr_plan_execute(plan, &b, cancel, context, e);
  CGContextRelease(cg);
  if (status != RMR_OK) {
    free(pixels);
    return status;
  }

  *out = (rmr_bitmap){pixels, w, h, stride};
  return RMR_OK;
}

typedef struct {
  rmr_buffer buffer;
  size_t capacity, maximum;
  rmr_status status;
} pdf_sink;

/* CGDataConsumer callback: append bytes to the sink buffer, growing it and
 * enforcing the sink's maximum output size. */
static size_t write_pdf(void* info, const void* bytes, size_t n) {
  pdf_sink* s = info;
  if (s->status != RMR_OK) return 0;

  if (n > s->maximum - s->buffer.size) {
    s->status = RMR_ERR_LIMIT;
    return 0;
  }

  size_t need = s->buffer.size + n;
  if (need > s->capacity) {
    size_t cap = s->capacity ? s->capacity : 4096;
    while (cap < need) {
      if (cap > s->maximum / 2) {
        cap = need;
        break;
      }
      cap *= 2;
    }
    if (cap > s->maximum) cap = s->maximum;

    void* p = realloc(s->buffer.bytes, cap);
    if (!p) {
      s->status = RMR_ERR_OUT_OF_MEMORY;
      return 0;
    }
    s->buffer.bytes = p;
    s->capacity = cap;
  }

  if (n) memcpy(s->buffer.bytes + s->buffer.size, bytes, n);
  s->buffer.size += n;
  return n;
}

/* Shared implementation behind rmr_apple_pdf and rmr_apple_pdf_sheets. */
static rmr_status pdf_impl(const rmr_plan* const* plans,
                           const rmr_sheet* sheets, size_t count, double scale,
                           const rmr_limits* given, rmr_cancel_fn cancel,
                           void* context, rmr_buffer* out, rmr_error* e) {
  rmr_limits l = given ? *given : rmr_limits_default();
  if (!plans || !count || !out || !isfinite(scale) || scale <= 0)
    return error(e, RMR_ERR_INVALID_ARGUMENT, "invalid PDF arguments");
  if (count > l.maximum_draws)
    return error(e, RMR_ERR_LIMIT, "PDF page count limit");

  for (size_t i = 0; i < count; i++) {
    if (!plans[i]) return error(e, RMR_ERR_INVALID_ARGUMENT, "null page plan");
    if (sheets && !valid_sheet(&sheets[i], false))
      return error(e, RMR_ERR_INVALID_ARGUMENT, "invalid PDF sheet");
    rmr_transform t;
    rmr_status s =
        rmr_page_to_pdf(rmr_plan_viewport(plans[i]), sheets ? 1 : scale, &t, e);
    if (s != RMR_OK) return s;
    rmr_rect v = rmr_plan_viewport(plans[i]);
    double width = sheets ? sheets[i].width : v.width * scale;
    double height = sheets ? sheets[i].height : v.height * scale;
    if (width > 10000000 || height > 10000000)
      return error(e, RMR_ERR_LIMIT, "PDF dimensions too large");
  }

  if (cancel && cancel(context))
    return error(e, RMR_ERR_CANCELLED, "cancelled");

  pdf_sink sink = {.maximum = l.maximum_pdf_bytes};
  CGDataConsumerCallbacks callbacks = {write_pdf, nullptr};
  CGDataConsumerRef consumer = CGDataConsumerCreate(&sink, &callbacks);
  if (!consumer)
    return error(e, RMR_ERR_OUT_OF_MEMORY, "PDF consumer unavailable");

  CGContextRef cg = CGPDFContextCreate(consumer, nullptr, nullptr);
  CGDataConsumerRelease(consumer);
  if (!cg) {
    rmr_buffer_free(&sink.buffer);
    return error(e, RMR_ERR_BACKEND, "PDF context unavailable");
  }

  rmr_status status = RMR_OK;
  for (size_t i = 0; i < count; i++) {
    rmr_rect v = rmr_plan_viewport(plans[i]);
    CGRect box = CGRectMake(0, 0, sheets ? sheets[i].width : v.width * scale,
                            sheets ? sheets[i].height : v.height * scale);
    CFDataRef data =
        CFDataCreate(kCFAllocatorDefault, (const UInt8*)&box, sizeof box);
    if (!data) {
      status = error(e, RMR_ERR_OUT_OF_MEMORY, "page box allocation failed");
      break;
    }

    const void *keys[] = {kCGPDFContextMediaBox}, *values[] = {data};
    CFDictionaryRef info = CFDictionaryCreate(kCFAllocatorDefault, keys, values,
                                              1, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    CFRelease(data);
    if (!info) {
      status =
          error(e, RMR_ERR_OUT_OF_MEMORY, "page dictionary allocation failed");
      break;
    }

    CGPDFContextBeginPage(cg, info);
    CFRelease(info);
    apple_context state = {.cg = cg,
                           .scale = scale,
                           .limits = l,
                           .sheet = sheets ? &sheets[i] : nullptr};
    rmr_backend b = backend(&state);
    status = rmr_plan_execute(plans[i], &b, cancel, context, e);
    CGPDFContextEndPage(cg);

    if (status != RMR_OK || sink.status != RMR_OK) break;
  }

  CGPDFContextClose(cg);
  CGContextRelease(cg);

  if (status == RMR_OK && sink.status != RMR_OK)
    status =
        error(e, sink.status, "PDF output memory limit or allocation failure");
  if (status != RMR_OK) {
    rmr_buffer_free(&sink.buffer);
    return status;
  }

  *out = sink.buffer;
  return RMR_OK;
}

rmr_status rmr_apple_bitmap(const rmr_plan* plan, double scale,
                            rmr_color background, const rmr_limits* limits,
                            rmr_cancel_fn cancel, void* context,
                            rmr_bitmap* out, rmr_error* e) {
  return bitmap_impl(plan, scale, nullptr, background, limits, cancel, context,
                     out, e);
}

rmr_status rmr_apple_bitmap_on_sheet(const rmr_plan* plan,
                                     const rmr_sheet* sheet,
                                     rmr_color background,
                                     const rmr_limits* limits,
                                     rmr_cancel_fn cancel, void* context,
                                     rmr_bitmap* out, rmr_error* e) {
  if (!sheet) return error(e, RMR_ERR_INVALID_ARGUMENT, "missing bitmap sheet");
  return bitmap_impl(plan, 1, sheet, background, limits, cancel, context, out,
                     e);
}

rmr_status rmr_apple_pdf(const rmr_plan* const* plans, size_t count,
                         double scale, const rmr_limits* limits,
                         rmr_cancel_fn cancel, void* context, rmr_buffer* out,
                         rmr_error* e) {
  return pdf_impl(plans, nullptr, count, scale, limits, cancel, context, out,
                  e);
}

rmr_status rmr_apple_pdf_sheets(const rmr_plan* const* plans,
                                const rmr_sheet* sheets, size_t count,
                                const rmr_limits* limits, rmr_cancel_fn cancel,
                                void* context, rmr_buffer* out, rmr_error* e) {
  if (!sheets) return error(e, RMR_ERR_INVALID_ARGUMENT, "missing PDF sheets");
  return pdf_impl(plans, sheets, count, 1, limits, cancel, context, out, e);
}

/* CoreGraphics draws grain masks straight onto the page whatever options say;
 * they are read only to refuse invalid ones as the Cairo backend does. */
rmr_status rmr_apple_pdf_with_options(const rmr_plan* const* plans,
                                      size_t count, double scale,
                                      const rmr_limits* limits,
                                      const rmr_pdf_options* options,
                                      rmr_cancel_fn cancel, void* context,
                                      rmr_buffer* out, rmr_error* e) {
  rmr_pdf_options checked;
  if (!rmr_pdf_options_read(options, &checked))
    return error(e, RMR_ERR_INVALID_ARGUMENT, "invalid PDF options");
  return rmr_apple_pdf(plans, count, scale, limits, cancel, context, out, e);
}

rmr_status rmr_apple_pdf_sheets_with_options(
    const rmr_plan* const* plans, const rmr_sheet* sheets, size_t count,
    const rmr_limits* limits, const rmr_pdf_options* options,
    rmr_cancel_fn cancel, void* context, rmr_buffer* out, rmr_error* e) {
  rmr_pdf_options checked;
  if (!rmr_pdf_options_read(options, &checked))
    return error(e, RMR_ERR_INVALID_ARGUMENT, "invalid PDF options");
  return rmr_apple_pdf_sheets(plans, sheets, count, limits, cancel, context,
                              out, e);
}

rmr_status rmr_apple_draw_context(CGContextRef cg, const rmr_plan* plan,
                                  const rmr_sheet* sheet,
                                  const rmr_limits* limits,
                                  rmr_cancel_fn cancel, void* context,
                                  rmr_error* e) {
  if (!cg || !sheet || !valid_sheet(sheet, false))
    return error(e, RMR_ERR_INVALID_ARGUMENT, "invalid native output sheet");
  apple_context state = {.cg = cg,
                         .scale = 1,
                         .limits = limits ? *limits : rmr_limits_default(),
                         .sheet = sheet,
                         .unclipped = true};
  rmr_backend b = backend(&state);
  return rmr_plan_execute(plan, &b, cancel, context, e);
}
