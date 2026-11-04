/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Portable SVG writer executing plans through the backend ABI. */
#include <rmrender/svg.h>
#include <zlib.h>

#include "rmr_internal.h"

typedef struct {
  rmr_buffer data;
  size_t capacity, maximum;
  rmr_status status;
} sink;

typedef struct {
  sink* out;
  const rmr_svg_provider* provider;
  rmr_rect viewport;
  rmr_sheet sheet;
  rmr_limits limits;
  double pixels_per_unit;
  uint64_t group;
  size_t ids;
} writer;

/* Appends size bytes, growing geometrically up to the sink maximum. A failed
 * sink ignores further output and keeps its first error. */
static void put(sink* s, const void* bytes, size_t size) {
  if (s->status != RMR_OK) return;

  if (size > s->maximum - s->data.size) {
    s->status = RMR_ERR_LIMIT;
    return;
  }

  size_t need = s->data.size + size;
  if (need > s->capacity) {
    size_t capacity = s->capacity ? s->capacity : 4096;
    while (capacity < need)
      capacity = capacity > SIZE_MAX / 2 ? need : capacity * 2;
    if (capacity > s->maximum) capacity = s->maximum;

    uint8_t* grown = rmr_realloc(s->data.bytes, capacity);
    if (!grown) {
      s->status = RMR_ERR_OUT_OF_MEMORY;
      return;
    }
    s->data.bytes = grown;
    s->capacity = capacity;
  }

  memcpy(s->data.bytes + s->data.size, bytes, size);
  s->data.size = need;
}

/* Appends a null-terminated string. */
static void text(sink* s, const char* string) {
  put(s, string, strlen(string));
}

/* Appends value rounded to places decimals without exponent, trailing zeros
 * or locale dependence; magnitudes beyond exact integer range fail LIMIT. */
static void number(sink* s, double value, int places) {
  static const double scales[] = {1, 10, 100, 1000, 1e4, 1e5, 1e6};
  double scaled = round(value * scales[places]);
  if (!isfinite(scaled) || fabs(scaled) > 9e15) {
    if (s->status == RMR_OK) s->status = RMR_ERR_LIMIT;
    return;
  }

  char digits[32];
  size_t at = sizeof digits;
  uint64_t magnitude = (uint64_t)fabs(scaled);
  int written = 0;
  bool fraction = false;
  for (; written < places; written++, magnitude /= 10) {
    unsigned digit = (unsigned)(magnitude % 10);
    if (!fraction && !digit) continue;
    fraction = true;
    digits[--at] = (char)('0' + digit);
  }
  if (fraction) digits[--at] = '.';

  do {
    digits[--at] = (char)('0' + magnitude % 10);
    magnitude /= 10;
  } while (magnitude);

  if (scaled < 0) digits[--at] = '-';
  put(s, digits + at, sizeof digits - at);
}

/* Appends a coordinate with page-unit precision. */
static void coordinate(sink* s, double value) { number(s, value, 3); }

/* Appends ` name="value"` with value as a coordinate. */
static void attribute(sink* s, const char* name, double value) {
  text(s, " ");
  text(s, name);
  text(s, "=\"");
  coordinate(s, value);
  text(s, "\"");
}

/* Appends a #rrggbb color with 8-bit channels. */
static void hex(sink* s, rmr_color c) {
  static const char digits[] = "0123456789abcdef";
  double channels[] = {c.red, c.green, c.blue};
  char out[7] = {'#'};
  for (size_t i = 0; i < 3; i++) {
    unsigned v = (unsigned)lrint(rmr_clamp(channels[i], 0, 1) * 255);
    out[1 + 2 * i] = digits[v >> 4];
    out[2 + 2 * i] = digits[v & 15];
  }
  put(s, out, sizeof out);
}

/* Appends an affine matrix() transform attribute. */
static void matrix(sink* s, rmr_transform t) {
  double values[] = {t.a, t.b, t.c, t.d, t.tx, t.ty};
  text(s, " transform=\"matrix(");
  for (size_t i = 0; i < 6; i++) {
    if (i) text(s, " ");
    number(s, values[i], 6);
  }
  text(s, ")\"");
}

/* Appends `d` path data for view; nonzero filling is the SVG default. */
static void path_data(sink* s, rmr_path_view view) {
  text(s, " d=\"");
  for (size_t i = 0; i < view.count; i++) {
    const rmr_element* v = &view.elements[i];
    if (i) text(s, " ");
    switch (v->kind) {
      case RMR_MOVE:
      case RMR_LINE:
        text(s, v->kind == RMR_MOVE ? "M" : "L");
        coordinate(s, v->end.x);
        text(s, " ");
        coordinate(s, v->end.y);
        break;
      case RMR_CURVE: {
        rmr_point points[] = {v->control1, v->control2, v->end};
        text(s, "C");
        for (size_t k = 0; k < 3; k++) {
          if (k) text(s, " ");
          coordinate(s, points[k].x);
          text(s, " ");
          coordinate(s, points[k].y);
        }
        break;
      }
      case RMR_CLOSE:
        text(s, "Z");
        break;
    }
  }
  text(s, "\"");
}

/* Appends size bytes as standard padded base64. */
static void base64(sink* s, const uint8_t* bytes, size_t size) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char block[256];
  size_t used = 0;

  for (size_t i = 0; i < size; i += 3) {
    uint32_t v = (uint32_t)bytes[i] << 16;
    if (i + 1 < size) v |= (uint32_t)bytes[i + 1] << 8;
    if (i + 2 < size) v |= bytes[i + 2];

    block[used++] = alphabet[v >> 18];
    block[used++] = alphabet[(v >> 12) & 63];
    block[used++] = i + 1 < size ? alphabet[(v >> 6) & 63] : '=';
    block[used++] = i + 2 < size ? alphabet[v & 63] : '=';
    if (used == sizeof block) {
      put(s, block, used);
      used = 0;
    }
  }

  put(s, block, used);
}

/* Appends UTF-8 text escaped for an attribute value. Characters XML forbids
 * (C0 controls, U+FFFE, U+FFFF) become spaces or U+FFFD. */
static void escaped(sink* s, const char* utf8, size_t size) {
  for (size_t i = 0; i < size; i++) {
    unsigned char c = (unsigned char)utf8[i];
    if (c == '&')
      text(s, "&amp;");
    else if (c == '<')
      text(s, "&lt;");
    else if (c == '>')
      text(s, "&gt;");
    else if (c == '"')
      text(s, "&quot;");
    else if (c < 0x20)
      text(s, " ");
    else if (c == 0xef && i + 2 < size && (unsigned char)utf8[i + 1] == 0xbf &&
             ((unsigned char)utf8[i + 2] & 0xfe) == 0xbe) {
      text(s, "\xef\xbf\xbd");
      i += 2;
    } else
      put(s, &c, 1);
  }
}

/* Appends one big-endian 32-bit value to a byte array. */
static uint8_t* big_endian(uint8_t* at, uint32_t v) {
  at[0] = (uint8_t)(v >> 24);
  at[1] = (uint8_t)(v >> 16);
  at[2] = (uint8_t)(v >> 8);
  at[3] = (uint8_t)v;
  return at + 4;
}

/* Writes a PNG chunk (length, type, data, CRC) at at, returning its end. */
static uint8_t* chunk(uint8_t* at, const char type[4], const uint8_t* data,
                      size_t size) {
  at = big_endian(at, (uint32_t)size);
  memcpy(at, type, 4);
  if (size) memcpy(at + 4, data, size);
  uLong crc = crc32(0, at, (uInt)(size + 4));
  return big_endian(at + 4 + size, (uint32_t)crc);
}

/* Encodes filtered scanlines (each prefixed with its filter byte) as a
 * deterministic PNG of the given bit depth and color type. */
static rmr_status png(const uint8_t* scanlines, size_t size, size_t width,
                      size_t height, uint8_t depth, uint8_t type,
                      rmr_buffer* out) {
  if (width > UINT32_MAX || height > UINT32_MAX || size > UINT32_MAX / 2)
    return RMR_ERR_LIMIT;

  uLong bound = compressBound((uLong)size);
  uint8_t* packed = rmr_alloc(bound);
  if (!packed) return RMR_ERR_OUT_OF_MEMORY;

  uLongf packed_size = bound;
  if (compress2(packed, &packed_size, scanlines, (uLong)size, 6) != Z_OK) {
    free(packed);
    return RMR_ERR_OUT_OF_MEMORY;
  }

  size_t total = 8 + 25 + 12 + packed_size + 12;
  uint8_t* bytes = rmr_alloc(total);
  if (!bytes) {
    free(packed);
    return RMR_ERR_OUT_OF_MEMORY;
  }

  uint8_t header[13];
  big_endian(header, (uint32_t)width);
  big_endian(header + 4, (uint32_t)height);
  header[8] = depth;
  header[9] = type;
  header[10] = header[11] = header[12] = 0;

  memcpy(bytes, "\x89PNG\r\n\x1a\n", 8);
  uint8_t* at = chunk(bytes + 8, "IHDR", header, sizeof header);
  at = chunk(at, "IDAT", packed, packed_size);
  chunk(at, "IEND", nullptr, 0);
  free(packed);

  *out = (rmr_buffer){bytes, total};
  return RMR_OK;
}

/* Appends `data:` URI of a PNG or JPEG; other bytes fail BACKEND. */
static rmr_status data_uri(sink* s, const uint8_t* bytes, size_t size) {
  const char* type = size >= 8 && !memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) ? "png"
                     : size >= 2 && bytes[0] == 0xff && bytes[1] == 0xd8
                         ? "jpeg"
                         : nullptr;
  if (!type) return RMR_ERR_BACKEND;

  text(s, " xlink:href=\"data:image/");
  text(s, type);
  text(s, ";base64,");
  base64(s, bytes, size);
  text(s, "\"");
  return RMR_OK;
}

/* Appends the document-unique definition id for n. */
static void identifier(sink* s, size_t id) {
  text(s, "r");
  number(s, (double)id, 0);
}

/* Appends ` style` for a blend mode and optional isolation; nothing for a
 * plain normal element. */
static void blend(sink* s, rmr_blend mode, bool isolate) {
  if (mode == RMR_BLEND_NORMAL && !isolate) return;

  text(s, " style=\"");
  if (isolate) text(s, "isolation:isolate");
  if (isolate && mode != RMR_BLEND_NORMAL) text(s, ";");
  if (mode == RMR_BLEND_DARKEN) text(s, "mix-blend-mode:darken");
  if (mode == RMR_BLEND_MULTIPLY) text(s, "mix-blend-mode:multiply");
  text(s, "\"");
}

/* Appends ` name="alpha"` unless alpha is opaque. */
static void opacity(sink* s, const char* name, double alpha) {
  if (alpha >= 1) return;
  text(s, " ");
  text(s, name);
  text(s, "=\"");
  number(s, alpha, 4);
  text(s, "\"");
}

/* Appends fill paint attributes for color. */
static void fill(sink* s, rmr_color color) {
  text(s, " fill=\"");
  hex(s, color);
  text(s, "\"");
  opacity(s, "fill-opacity", color.alpha);
}

/* Appends stroke paint and geometry attributes; miter limit defaults to 10
 * like the native backends. */
static void stroke(writer* w, const rmr_draw* d, rmr_color color) {
  static const char* const caps[] = {"round", "square", "butt"};
  static const char* const joins[] = {"round", "bevel", "miter"};
  sink* s = w->out;

  text(s, " stroke=\"");
  hex(s, color);
  text(s, "\"");
  opacity(s, "stroke-opacity", color.alpha);
  attribute(s, "stroke-width", d->line_width);
  text(s, " stroke-linecap=\"");
  text(s, caps[d->cap]);
  text(s, "\" stroke-linejoin=\"");
  text(s, joins[d->join]);
  text(s, "\"");
  if (d->join == RMR_JOIN_MITER) {
    text(s, " stroke-miterlimit=\"");
    number(s, d->miter_limit != 0 ? d->miter_limit : 10, 4);
    text(s, "\"");
  }
}

/* Page-space rectangle covered by the sheet, used as the region of masks
 * that must not crop anything visible. */
static rmr_rect visible(const writer* w) {
  rmr_transform t = w->sheet.transform;
  return (rmr_rect){-t.tx / t.a, (t.ty - w->sheet.height) / t.a,
                    w->sheet.width / t.a, w->sheet.height / t.a};
}

/* Appends `<mask>` region attributes in user space for r. */
static void region(sink* s, rmr_rect r) {
  text(s, " maskUnits=\"userSpaceOnUse\"");
  attribute(s, "x", r.x);
  attribute(s, "y", r.y);
  attribute(s, "width", r.width);
  attribute(s, "height", r.height);
}

/* Writes an eraser luminance mask removing every eraser contour from the
 * visible sheet, then opens a group using it and carrying the draw's blend
 * mode, so the subtraction applies before compositing. */
static void erasers_open(writer* w, const rmr_draw* d) {
  sink* s = w->out;
  size_t id = ++w->ids;
  rmr_rect r = visible(w);

  text(s, "<mask id=\"");
  identifier(s, id);
  text(s, "\"");
  region(s, r);
  text(s, "><rect");
  attribute(s, "x", r.x);
  attribute(s, "y", r.y);
  attribute(s, "width", r.width);
  attribute(s, "height", r.height);
  text(s, " fill=\"#fff\"/>");
  for (size_t i = 0; i < d->eraser_count; i++) {
    text(s, "<path");
    path_data(s, d->erasers[i]);
    text(s, " fill=\"#000\"/>");
  }
  text(s, "</mask>\n<g mask=\"url(#");
  identifier(s, id);
  text(s, ")\"");
  blend(s, d->blend, false);
  text(s, ">\n");
}

/* Writes a one-bit grain mask as a luminance mask over its box; painting
 * pixels (zero bits) become white. Returns its id through id. */
static rmr_status grain(writer* w, const rmr_mask_view* m, size_t* id) {
  size_t row = (m->width + 7) / 8;
  if (row + 1 > SIZE_MAX / m->height) return RMR_ERR_LIMIT;

  size_t size = (row + 1) * m->height;
  uint8_t* lines = rmr_alloc(size);
  if (!lines) return RMR_ERR_OUT_OF_MEMORY;

  for (size_t y = 0; y < m->height; y++) {
    uint8_t* line = lines + y * (row + 1);
    line[0] = 0;
    for (size_t x = 0; x < row; x++)
      line[1 + x] = (uint8_t)~m->bytes[y * m->stride + x];
  }

  rmr_buffer encoded = {0};
  rmr_status s = png(lines, size, m->width, m->height, 1, 0, &encoded);
  free(lines);
  if (s != RMR_OK) return s;

  sink* o = w->out;
  *id = ++w->ids;
  text(o, "<mask id=\"");
  identifier(o, *id);
  text(o, "\"");
  region(o, m->box);
  text(o, "><image");
  attribute(o, "x", m->box.x);
  attribute(o, "y", m->box.y);
  attribute(o, "width", m->box.width);
  attribute(o, "height", m->box.height);
  text(o, " preserveAspectRatio=\"none\" image-rendering=\"optimizeSpeed\"");
  s = data_uri(o, encoded.bytes, encoded.size);
  text(o, "/></mask>\n");
  rmr_buffer_free(&encoded);
  return s;
}

/* Writes a clipPath for path and returns its id. */
static size_t clip(writer* w, rmr_path_view path) {
  sink* s = w->out;
  size_t id = ++w->ids;
  text(s, "<clipPath id=\"");
  identifier(s, id);
  text(s, "\"><path");
  path_data(s, path);
  text(s, "/></clipPath>\n");
  return id;
}

/* Writes an encoded image clipped to its nonzero path, mapped from the unit
 * square through its transform, with alpha and blend applied. */
static rmr_status image(writer* w, const rmr_draw* d) {
  sink* s = w->out;
  size_t id = clip(w, d->path);
  text(s, "<g clip-path=\"url(#");
  identifier(s, id);
  text(s, ")\"");
  blend(s, d->blend, false);
  text(s, "><image width=\"1\" height=\"1\" preserveAspectRatio=\"none\"");
  opacity(s, "opacity", d->color.alpha);
  matrix(s, d->image.transform);
  rmr_status status = data_uri(s, d->image.bytes, d->image.size);
  text(s, "/></g>\n");
  return status;
}

/* Converts the provider's premultiplied raster to straight-alpha RGBA
 * scanlines and encodes it as PNG. */
static rmr_status raster_png(const rmr_bitmap* b, rmr_buffer* out) {
  if (b->width > (SIZE_MAX - 1) / 4 || b->width * 4 + 1 > SIZE_MAX / b->height)
    return RMR_ERR_LIMIT;

  size_t row = b->width * 4 + 1, size = row * b->height;
  uint8_t* lines = rmr_alloc(size);
  if (!lines) return RMR_ERR_OUT_OF_MEMORY;

  for (size_t y = 0; y < b->height; y++) {
    uint8_t* line = lines + y * row;
    const uint8_t* source = b->pixels + y * b->stride;
    line[0] = 0;
    for (size_t x = 0; x < b->width * 4; x += 4) {
      unsigned alpha = source[x + 3];
      for (size_t c = 0; c < 3; c++)
        line[1 + x + c] =
            alpha ? (uint8_t)((source[x + c] * 255u + alpha / 2) / alpha) : 0;
      line[1 + x + 3] = (uint8_t)alpha;
    }
  }

  rmr_status s = png(lines, size, b->width, b->height, 8, 6, out);
  free(lines);
  return s;
}

/* Rasterizes a PDF command through the provider over the part of the sheet
 * its clip can reach, then places the PNG back in page space with the
 * command's alpha and blend. */
static rmr_status pdf(writer* w, const rmr_draw* d) {
  rmr_transform t = w->sheet.transform;
  double k = w->pixels_per_unit, top = w->sheet.height - t.ty;
  rmr_rect area = visible(w);
  double left = area.x + area.width, right = area.x,
         upper = area.y + area.height, lower = area.y;

  for (size_t i = 0; i < d->path.count; i++) {
    const rmr_element* v = &d->path.elements[i];
    if (v->kind == RMR_CLOSE) continue;
    rmr_point points[] = {v->end, v->control1, v->control2};
    for (size_t p = 0; p < (v->kind == RMR_CURVE ? 3u : 1u); p++) {
      left = fmin(left, points[p].x);
      right = fmax(right, points[p].x);
      upper = fmin(upper, points[p].y);
      lower = fmax(lower, points[p].y);
    }
  }

  left = fmax(left, area.x);
  right = fmin(right, area.x + area.width);
  upper = fmax(upper, area.y);
  lower = fmin(lower, area.y + area.height);
  if (!(left < right && upper < lower)) return RMR_OK;

  /* Snap outward to whole pixels, ignoring rounding noise at the edges. */
  double x0 = floor(k * (t.a * left + t.tx) + 1e-6),
         x1 = ceil(k * (t.a * right + t.tx) - 1e-6);
  double y0 = floor(k * (t.a * upper + top) + 1e-6),
         y1 = ceil(k * (t.a * lower + top) - 1e-6);
  double width = x1 - x0, height = y1 - y0;
  if (width < 1 || height < 1) return RMR_OK;
  if (width > w->limits.maximum_bitmap_side ||
      height > w->limits.maximum_bitmap_side)
    return RMR_ERR_LIMIT;

  rmr_sheet raster = {
      width,
      height,
      {k * t.a, 0, 0, -k * t.a, k * t.tx - x0, height - k * top + y0}};
  rmr_draw single = *d;
  single.color = (rmr_color){0, 0, 0, 1};
  single.blend = RMR_BLEND_NORMAL;

  rmr_plan* plan = nullptr;
  rmr_status s =
      rmr_plan_create(&single, 1, w->viewport, &w->limits, &plan, nullptr);
  rmr_bitmap bitmap = {0};
  if (s == RMR_OK)
    s = w->provider->raster(w->provider->context, plan, &raster, &w->limits,
                            &bitmap);
  rmr_plan_free(plan);
  if (s == RMR_OK &&
      (!bitmap.pixels || bitmap.width != (size_t)width ||
       bitmap.height != (size_t)height || bitmap.stride < bitmap.width * 4))
    s = RMR_ERR_BACKEND;

  rmr_buffer encoded = {0};
  if (s == RMR_OK) s = raster_png(&bitmap, &encoded);
  free(bitmap.pixels);
  if (s != RMR_OK) return s;

  sink* o = w->out;
  double scale = k * t.a;
  text(o, "<image x=\"");
  number(o, (x0 / k - t.tx) / t.a, 6);
  text(o, "\" y=\"");
  number(o, (y0 / k - top) / t.a, 6);
  text(o, "\" width=\"");
  number(o, width / scale, 6);
  text(o, "\" height=\"");
  number(o, height / scale, 6);
  text(o, "\" preserveAspectRatio=\"none\"");
  opacity(o, "opacity", d->color.alpha);
  blend(o, d->blend, false);
  s = data_uri(o, encoded.bytes, encoded.size);
  text(o, "/>\n");
  rmr_buffer_free(&encoded);
  return s;
}

/* Checks provider glyph outlines against the plan limits. */
static bool outline_valid(rmr_path_view p, const rmr_limits* l) {
  if ((!p.elements && p.count) || p.count > l->maximum_elements) return false;
  for (size_t i = 0; i < p.count; i++) {
    const rmr_element* v = &p.elements[i];
    if (v->kind > RMR_CLOSE ||
        (v->kind != RMR_CLOSE &&
         !rmr_point_valid(v->end, l->maximum_coordinate)) ||
        (v->kind == RMR_CURVE &&
         (!rmr_point_valid(v->control1, l->maximum_coordinate) ||
          !rmr_point_valid(v->control2, l->maximum_coordinate))))
      return false;
  }
  return true;
}

/* Writes a TEXT command as its provider glyph outlines, labelled with the
 * original string for assistive technology. */
static rmr_status glyphs(writer* w, const rmr_draw* d) {
  rmr_path outline = {0};
  rmr_status s =
      w->provider->text(w->provider->context, &d->text, &w->limits, &outline);
  if (s != RMR_OK) return s;

  rmr_path_view view = {outline.elements, outline.count};
  if (!outline_valid(view, &w->limits)) {
    rmr_path_free(&outline);
    return RMR_ERR_BACKEND;
  }

  if (outline.count) {
    sink* o = w->out;
    text(o, "<path");
    path_data(o, view);
    fill(o, d->color);
    blend(o, d->blend, false);
    text(o, " aria-label=\"");
    escaped(o, d->text.utf8, d->text.size);
    text(o, "\"/>\n");
  }

  rmr_path_free(&outline);
  return RMR_OK;
}

/* Closes the open transparency group, if any. */
static void group_end(writer* w) {
  if (!w->group) return;
  text(w->out, "</g>\n");
  w->group = 0;
}

/* rmr_backend begin: the document header is written before execution. */
static rmr_status begin(void* context, rmr_rect viewport) {
  (void)context;
  (void)viewport;
  return RMR_OK;
}

/* rmr_backend draw: opens/closes isolated groups as tokens change, then
 * writes the command as vector paths, masks, images or rasterized PDF. */
static rmr_status draw(void* context, const rmr_draw* d) {
  writer* w = context;
  sink* s = w->out;

  if (w->group != d->group) {
    group_end(w);
    if (d->group) {
      text(s, "<g");
      opacity(s, "opacity", d->group_alpha);
      blend(s, d->group_blend, true);
      text(s, ">\n");
      w->group = d->group;
    }
  }

  rmr_status status = RMR_OK;
  if (d->mode == RMR_IMAGE)
    status = image(w, d);
  else if (d->mode == RMR_PDF)
    status = pdf(w, d);
  else if (d->mode == RMR_TEXT)
    status = glyphs(w, d);
  else {
    size_t mask = 0;
    if (d->mask.bytes) {
      status = grain(w, &d->mask, &mask);
      if (status != RMR_OK) return status;
    }

    if (d->eraser_count) erasers_open(w, d);

    text(s, "<path");
    path_data(s, d->path);
    if (d->mode == RMR_STROKE) {
      text(s, " fill=\"none\"");
      stroke(w, d, d->color);
    } else {
      fill(s, d->color);
      if (d->mode == RMR_FILL_STROKE) stroke(w, d, d->stroke_color);
    }
    if (mask) {
      text(s, " mask=\"url(#");
      identifier(s, mask);
      text(s, ")\"");
    }
    if (!d->eraser_count) blend(s, d->blend, false);
    text(s, "/>\n");

    if (d->eraser_count) text(s, "</g>\n");
  }

  return status != RMR_OK ? status : s->status;
}

/* rmr_backend end: closes any group and reports sink failures. */
static rmr_status end(void* context) {
  writer* w = context;
  group_end(w);
  return w->out->status;
}

/* rmr_backend abort: output is discarded by the caller. */
static void abort_draw(void* context) { ((writer*)context)->group = 0; }

/* Checks a sheet's size and its uniform, axis-aligned y-flipping
 * transform. */
static bool sheet_valid(const rmr_sheet* s) {
  if (!s || !isfinite(s->width) || !isfinite(s->height) || s->width <= 0 ||
      s->height <= 0 || s->width > 10000000 || s->height > 10000000)
    return false;
  rmr_transform t = s->transform;
  return isfinite(t.a) && t.a > 0 && t.a <= 1e6 && t.b == 0 && t.c == 0 &&
         t.d == -t.a && isfinite(t.tx) && isfinite(t.ty) &&
         fabs(t.tx) <= 1e12 && fabs(t.ty) <= 1e12;
}

/* Writes one SVG document for plan on sheet into s. */
static rmr_status page(const rmr_plan* plan, const rmr_sheet* sheet,
                       const rmr_svg_provider* provider, double dpi,
                       const rmr_limits* limits, rmr_cancel_fn cancel,
                       void* context, sink* s, rmr_error* e) {
  rmr_transform t = sheet->transform;
  text(s,
       "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
       "<svg xmlns=\"http://www.w3.org/2000/svg\" "
       "xmlns:xlink=\"http://www.w3.org/1999/xlink\" version=\"1.1\" "
       "width=\"");
  coordinate(s, sheet->width);
  text(s, "pt\" height=\"");
  coordinate(s, sheet->height);
  text(s, "pt\" viewBox=\"0 0 ");
  coordinate(s, sheet->width);
  text(s, " ");
  coordinate(s, sheet->height);
  text(s, "\">\n<g");
  matrix(s, (rmr_transform){t.a, 0, 0, t.a, t.tx, sheet->height - t.ty});
  text(s, ">\n");
  if (s->status != RMR_OK)
    return rmr_fail(e, s->status, 0, "SVG output limit or allocation failure");

  writer w = {.out = s,
              .provider = provider,
              .viewport = rmr_plan_viewport(plan),
              .sheet = *sheet,
              .limits = *limits,
              .pixels_per_unit = dpi / 72};
  uint64_t capabilities = (UINT64_C(1) << 12) - 1;
  if (!provider || !provider->text) capabilities &= ~RMR_CAP_TEXT;
  if (!provider || !provider->raster) capabilities &= ~RMR_CAP_PDF;
  rmr_backend b = {RMR_BACKEND_ABI, sizeof b, capabilities, &w,
                   begin,           draw,     end,          abort_draw};
  rmr_status status = rmr_plan_execute(plan, &b, cancel, context, e);
  if (status != RMR_OK) return status;

  text(s, "</g>\n</svg>\n");
  return s->status == RMR_OK
             ? RMR_OK
             : rmr_fail(e, s->status, 0,
                        "SVG output limit or allocation failure");
}

void rmr_svg_pages_free(rmr_svg_pages* pages) {
  if (!pages) return;
  for (size_t i = 0; i < pages->count; i++) rmr_buffer_free(&pages->pages[i]);
  free(pages->pages);
  *pages = (rmr_svg_pages){0};
}

rmr_status rmr_svg_sheets(const rmr_plan* const* plans, const rmr_sheet* sheets,
                          size_t count, const rmr_svg_provider* provider,
                          double raster_dpi, const rmr_limits* given,
                          rmr_cancel_fn cancel, void* context,
                          rmr_svg_pages* out, rmr_error* e) {
  rmr_limits l = given ? *given : rmr_limits_default();
  if (!plans || !sheets || !count || !out || !isfinite(raster_dpi) ||
      raster_dpi < 0 || raster_dpi > 10000 ||
      (provider && (provider->abi_version != RMR_SVG_PROVIDER_ABI ||
                    provider->struct_size < sizeof *provider)))
    return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, 0, "invalid SVG arguments");
  if (count > l.maximum_draws ||
      count > l.maximum_plan_bytes / sizeof(rmr_buffer))
    return rmr_fail(e, RMR_ERR_LIMIT, 0, "SVG page count limit");

  for (size_t i = 0; i < count; i++)
    if (!plans[i] || !sheet_valid(&sheets[i]))
      return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, i, "invalid SVG page");

  if (cancel && cancel(context))
    return rmr_fail(e, RMR_ERR_CANCELLED, 0, nullptr);

  rmr_buffer* pages = rmr_calloc(count, sizeof *pages);
  if (!pages) return rmr_fail(e, RMR_ERR_OUT_OF_MEMORY, 0, nullptr);

  double dpi = raster_dpi > 0 ? raster_dpi : 150;
  size_t used = 0;
  rmr_status status = RMR_OK;
  for (size_t i = 0; i < count && status == RMR_OK; i++) {
    sink s = {.maximum = l.maximum_pdf_bytes - used};
    status =
        page(plans[i], &sheets[i], provider, dpi, &l, cancel, context, &s, e);
    if (status == RMR_OK) {
      pages[i] = s.data;
      used += s.data.size;
    } else
      free(s.data.bytes);
  }

  rmr_svg_pages result = {pages, count};
  if (status != RMR_OK) {
    rmr_svg_pages_free(&result);
    return status;
  }

  *out = result;
  return RMR_OK;
}
