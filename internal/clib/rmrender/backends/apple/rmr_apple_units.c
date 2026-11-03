/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <CoreGraphics/CoreGraphics.h>
#include <math.h>
#include <rmrender/apple.h>

/* rmr_unit_raster callback: rasterize unit spans as antialiasing-free
 * grayscale strokes into bytes. */
static rmr_status units(void* context, const rmr_unit_span* spans, size_t count,
                        rmr_rect box, size_t width, size_t height,
                        uint8_t* bytes, size_t stride) {
  (void)context;
  if (!bytes || (!spans && count) || !width || !height || width > 16384 ||
      height > 16384 || width * height > 8388608 || stride < width ||
      stride > SIZE_MAX / height || box.width != (double)width ||
      box.height != (double)height || !isfinite(box.x) || !isfinite(box.y) ||
      fabs(box.x) > 1e12 || fabs(box.y) > 1e12)
    return RMR_ERR_INVALID_ARGUMENT;
  for (size_t i = 0; i < count; i++)
    if (!isfinite(spans[i].from.x) || !isfinite(spans[i].from.y) ||
        !isfinite(spans[i].to.x) || !isfinite(spans[i].to.y) ||
        !isfinite(spans[i].width) || spans[i].width <= 0 ||
        spans[i].width > 1000002 || fabs(spans[i].from.x) > 1e12 ||
        fabs(spans[i].from.y) > 1e12 || fabs(spans[i].to.x) > 1e12 ||
        fabs(spans[i].to.y) > 1e12 || spans[i].cap > RMR_CAP_BUTT)
      return RMR_ERR_INVALID_ARGUMENT;

  CGColorSpaceRef space = CGColorSpaceCreateDeviceGray();
  CGContextRef cg = space
                        ? CGBitmapContextCreate(bytes, width, height, 8, stride,
                                                space, kCGImageAlphaNone)
                        : nullptr;
  if (space) CGColorSpaceRelease(space);
  if (!cg) return RMR_ERR_BACKEND;

  CGContextTranslateCTM(cg, 0, (double)height);
  CGContextScaleCTM(cg, 1, -1);
  CGContextTranslateCTM(cg, -box.x, -box.y);
  CGContextSetShouldAntialias(cg, false);

  for (size_t i = 0; i < count; i++) {
    const rmr_unit_span* s = &spans[i];
    CGContextSetLineCap(cg, s->cap == RMR_CAP_ROUND    ? kCGLineCapRound
                            : s->cap == RMR_CAP_SQUARE ? kCGLineCapSquare
                                                       : kCGLineCapButt);
    CGContextSetGrayStrokeColor(cg, (double)s->value / 255, 1);
    CGContextSetLineWidth(cg, s->width);
    CGContextMoveToPoint(cg, s->from.x, s->from.y);
    CGContextAddLineToPoint(cg, s->to.x, s->to.y);
    CGContextStrokePath(cg);
  }

  CGContextRelease(cg);
  return RMR_OK;
}

rmr_unit_raster rmr_apple_unit_raster(void) {
  return (rmr_unit_raster){RMR_UNIT_RASTER_ABI, sizeof(rmr_unit_raster),
                           nullptr, units};
}
