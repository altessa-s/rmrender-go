/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "rmr_pdf_writer.h"

#include <CoreGraphics/CoreGraphics.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/rmr_pdf_edit.h"
#include "../common/rmr_pdf_stream.h"

/* What CoreGraphics' backend in rmr_apple.c does, operator for operator:
 *
 * - The context transform is the sheet's (rmr_apple_draw_context concatenates
 *   it onto the page's identity matrix, which copies it exactly).
 * - Paths are added in user space and kept in device space: each point is
 *   fma(a, x, c * y) + tx, fma(b, x, d * y) + ty. Fills and clips print those
 *   device coordinates.
 * - Strokes print "q <transform> cm", then each device point mapped back
 *   through CGAffineTransformInvert's inverse, so a user coordinate can come
 *   back one rounding away from the plan's.
 * - A grain mask is drawn under its path clip after TranslateCTM (fused:
 *   fma(a, x, tx)), ScaleCTM(1, -1) and DrawImage into (0, 0, w, h): the
 *   image matrix is [a*w 0 0 -d*h tx' ty'], at Perceptual intent.
 * - Colors are in sRGB through an ICCBased space; the context alpha goes to
 *   /ca for fills and /CA for strokes, the blend mode to /BM.
 * - Every number goes through rmr_pdf_number, CoreGraphics' own formatting.
 *
 * Graphics state is emitted where it changes, as CoreGraphics does; only its
 * order and the q/Q nesting differ, which do not change what is painted. */

enum { BLOCK = 64 };

/* A grain mask's bytes and the dimensions first seen with them: CoreGraphics
 * keys images on their bytes alone, so a later mask with the same bytes is
 * drawn with the first one's width and height. number is the writer's image
 * object, 0 until written. */
typedef struct {
  rmr_pdf_digest digest;
  uint32_t width, height, stride, number;
} image_entry;

typedef struct {
  char key[48];
  uint32_t number;
} state_entry;

/* Shared objects already written and their numbers: one set per merged
 * document, or one per overlay PDF. */
typedef struct {
  image_entry* images; /* open addressing, capacity a power of two */
  size_t image_count, image_capacity;
  state_entry* states;
  size_t state_count;
  uint32_t profile, space;
} catalog;

static void catalog_free(catalog* c) {
  free(c->images);
  free(c->states);
  *c = (catalog){0};
}

struct rmr_apple_writer {
  rmr_limits limits;
  unsigned decimals;
  bool merged;
  rmr_pdf_pipeline* pipeline;
  size_t window;
  CFDataRef profile;
  /* Merged documents. */
  rmr_pdf_builder* builder;
  catalog shared;
  rmr_pdf_page_objects* pages;
  size_t page_count, page_capacity;
};

/* Per-page serialization state; the rmr_backend context. */
typedef struct {
  rmr_apple_writer* writer;
  catalog* resources;
  catalog local;
  rmr_pdf_batch* batch;
  uint32_t next; /* overlay numbering; 0 numbers from the builder */
  rmr_pdf_text content;
  uint32_t *xobjects, *states;
  size_t xobject_count, xobject_capacity, state_count, state_capacity;
  bool space;
  CGAffineTransform ctm, inverse;
  bool clip;
  double width, height;
  unsigned decimals;
  /* Graphics state in effect outside q/Q. */
  bool has_fill, has_stroke;
  double fill[3], stroke[3], ca, stroke_alpha, line_width, miter;
  rmr_blend blend;
  int cap, join;
} page;

static uint32_t number(page* p) {
  return p->next ? p->next++ : rmr_pdf_builder_reserve(p->writer->builder);
}

/* Appends n to a growable list unless it is already the list's member. */
static bool list_add(uint32_t** list, size_t* count, size_t* capacity,
                     uint32_t n) {
  for (size_t i = *count; i-- > 0;)
    if ((*list)[i] == n) return true;
  if (*count == *capacity) {
    size_t grown = *capacity ? *capacity * 2 : 16;
    void* p = realloc(*list, grown * sizeof **list);
    if (!p) return false;
    *list = p;
    *capacity = grown;
  }
  (*list)[(*count)++] = n;
  return true;
}

static void put(page* p, const char* s) { rmr_pdf_text_string(&p->content, s); }

/* A number exactly as CoreGraphics prints it. */
static void exact(page* p, double v) {
  rmr_pdf_text_number(&p->content, v, 0);
  rmr_pdf_text_append(&p->content, " ", 1);
}

/* A path coordinate: as CoreGraphics prints it, or rounded when asked. */
static void coordinate(page* p, double v) {
  rmr_pdf_text_number(&p->content, v, p->decimals);
  rmr_pdf_text_append(&p->content, " ", 1);
}

/* The sRGB ICCBased color space, written once per catalog. */
static uint32_t color_space(page* p) {
  catalog* c = p->resources;
  if (!c->space) {
    CFDataRef icc = p->writer->profile;
    size_t size = (size_t)CFDataGetLength(icc);
    uint32_t profile = number(p), space = number(p);
    rmr_pdf_object* stream = rmr_pdf_batch_add(p->batch, profile, true);
    rmr_pdf_object* array = rmr_pdf_batch_add(p->batch, space, false);
    uint8_t* data = stream ? malloc(size) : nullptr;
    if (!profile || !space || !array || !data) {
      free(data);
      return 0;
    }
    memcpy(data, CFDataGetBytePtr(icc), size);
    stream->data = data;
    stream->size = size;
    rmr_pdf_text_string(&stream->dict, "/N 3 /Alternate /DeviceRGB");
    rmr_pdf_text_format(&array->dict, "[ /ICCBased %u 0 R ]", profile);
    c->profile = profile;
    c->space = space;
  }
  p->space = true;
  return c->space;
}

/* A graphics state dictionary holding one entry (key and value text),
 * written once per catalog; its number, or 0 on failure. */
static uint32_t graphics_state(page* p, const char* key, double alpha,
                               const char* name) {
  char text[48];
  char value[RMR_PDF_NUMBER_SIZE];
  if (name)
    snprintf(text, sizeof text, "/%s /%s", key, name);
  else {
    size_t length = rmr_pdf_number(value, alpha);
    snprintf(text, sizeof text, "/%s %.*s", key, (int)length, value);
  }

  catalog* c = p->resources;
  for (size_t i = 0; i < c->state_count; i++)
    if (!strcmp(c->states[i].key, text)) {
      return list_add(&p->states, &p->state_count, &p->state_capacity,
                      c->states[i].number)
                 ? c->states[i].number
                 : 0;
    }

  if (c->state_count % BLOCK == 0) {
    void* grown =
        realloc(c->states, (c->state_count + BLOCK) * sizeof *c->states);
    if (!grown) return 0;
    c->states = grown;
  }
  uint32_t n = number(p);
  rmr_pdf_object* o = n ? rmr_pdf_batch_add(p->batch, n, false) : nullptr;
  if (!o) return 0;
  rmr_pdf_text_format(&o->dict, "<< /Type /ExtGState %s >>", text);
  state_entry* e = &c->states[c->state_count++];
  snprintf(e->key, sizeof e->key, "%s", text);
  e->number = n;
  return list_add(&p->states, &p->state_count, &p->state_capacity, n) ? n : 0;
}

static bool use_state(page* p, const char* key, double alpha,
                      const char* name) {
  uint32_t n = graphics_state(p, key, alpha, name);
  if (!n) return false;
  rmr_pdf_text_format(&p->content, "/RMRg%u gs\n", n);
  return true;
}

static const char* blend_name(rmr_blend b) {
  return b == RMR_BLEND_DARKEN     ? "Darken"
         : b == RMR_BLEND_MULTIPLY ? "Multiply"
                                   : "Normal";
}

/* Moves the outer graphics state's fill alpha, stroke alpha and blend mode
 * to the given values. */
static bool outer_state(page* p, double ca, double stroke_alpha,
                        rmr_blend blend) {
  if (ca != p->ca) {
    if (!use_state(p, "ca", ca, nullptr)) return false;
    p->ca = ca;
  }
  if (stroke_alpha != p->stroke_alpha) {
    if (!use_state(p, "CA", stroke_alpha, nullptr)) return false;
    p->stroke_alpha = stroke_alpha;
  }
  if (blend != p->blend) {
    if (!use_state(p, "BM", 0, blend_name(blend))) return false;
    p->blend = blend;
  }
  return true;
}

static bool set_color(page* p, rmr_color c, bool stroke) {
  uint32_t space = color_space(p);
  if (!space) return false;
  rmr_pdf_text_format(&p->content, "/RMRc%u %s ", space, stroke ? "CS" : "cs");
  exact(p, c.red);
  exact(p, c.green);
  exact(p, c.blue);
  put(p, stroke ? "SC\n" : "sc\n");
  return true;
}

static bool outer_color(page* p, rmr_color c, bool stroke) {
  bool* has = stroke ? &p->has_stroke : &p->has_fill;
  double* now = stroke ? p->stroke : p->fill;
  if (*has && now[0] == c.red && now[1] == c.green && now[2] == c.blue)
    return true;
  if (!set_color(p, c, stroke)) return false;
  *has = true;
  now[0] = c.red;
  now[1] = c.green;
  now[2] = c.blue;
  return true;
}

/* Line width, cap, join and miter limit as CGContext sets them. */
static void outer_line(page* p, const rmr_draw* d) {
  int cap = d->cap == RMR_CAP_ROUND ? 1 : d->cap == RMR_CAP_SQUARE ? 2 : 0;
  int join = d->join == RMR_JOIN_ROUND ? 1 : d->join == RMR_JOIN_MITER ? 0 : 2;
  double miter = d->miter_limit != 0 ? d->miter_limit : 10;
  if (d->line_width != p->line_width) {
    exact(p, d->line_width);
    put(p, "w\n");
    p->line_width = d->line_width;
  }
  if (cap != p->cap) {
    rmr_pdf_text_format(&p->content, "%d J\n", cap);
    p->cap = cap;
  }
  if (join != p->join) {
    rmr_pdf_text_format(&p->content, "%d j\n", join);
    p->join = join;
  }
  if (miter != p->miter) {
    exact(p, miter);
    put(p, "M\n");
    p->miter = miter;
  }
}

/* A user-space point in device space, as CG::Path::apply_transform maps it. */
static CGPoint device(CGAffineTransform t, double x, double y) {
  double cy = t.c * y, dy = t.d * y;
  return CGPointMake(fma(t.a, x, cy) + t.tx, fma(t.b, x, dy) + t.ty);
}

typedef struct {
  page* p;
  bool user;          /* print stroke coordinates, mapped back */
  CGPoint start, now; /* device space */
} path_writer;

static void point(path_writer* w, CGPoint d) {
  if (w->user) d = device(w->p->inverse, d.x, d.y);
  coordinate(w->p, d.x);
  coordinate(w->p, d.y);
}

static void move(path_writer* w, CGPoint d) {
  point(w, d);
  put(w->p, "m\n");
  w->start = w->now = d;
}

static void line(path_writer* w, CGPoint d) {
  point(w, d);
  put(w->p, "l\n");
  w->now = d;
}

static void curve(path_writer* w, CGPoint c1, CGPoint c2, CGPoint end) {
  point(w, c1);
  point(w, c2);
  point(w, end);
  put(w->p, "c\n");
  w->now = end;
}

static void close_path(path_writer* w) {
  put(w->p, "h\n");
  w->now = w->start;
}

/* A plan path in device space, or mapped back for a stroke. */
static void plan_path(page* p, rmr_path_view v, bool user) {
  path_writer w = {.p = p, .user = user};
  CGAffineTransform t = p->ctm;
  for (size_t i = 0; i < v.count; i++) {
    const rmr_element* e = &v.elements[i];
    switch (e->kind) {
      case RMR_MOVE:
        move(&w, device(t, e->end.x, e->end.y));
        break;
      case RMR_LINE:
        line(&w, device(t, e->end.x, e->end.y));
        break;
      case RMR_CURVE:
        curve(&w, device(t, e->control1.x, e->control1.y),
              device(t, e->control2.x, e->control2.y),
              device(t, e->end.x, e->end.y));
        break;
      case RMR_CLOSE:
        close_path(&w);
        break;
    }
  }
}

/* CGPathApply callback writing a CoreGraphics path in device space; a
 * quadratic segment becomes the cubic CoreGraphics' writer derives from the
 * device points. */
static void native_element(void* info, const CGPathElement* e) {
  path_writer* w = info;
  CGAffineTransform t = w->p->ctm;
  const CGPoint* q = e->points;
  switch (e->type) {
    case kCGPathElementMoveToPoint:
      move(w, device(t, q[0].x, q[0].y));
      break;
    case kCGPathElementAddLineToPoint:
      line(w, device(t, q[0].x, q[0].y));
      break;
    case kCGPathElementAddQuadCurveToPoint: {
      CGPoint c = device(t, q[0].x, q[0].y), end = device(t, q[1].x, q[1].y);
      curve(w,
            CGPointMake(fma(c.x, 2, w->now.x) / 3, fma(c.y, 2, w->now.y) / 3),
            CGPointMake(fma(c.x, 2, end.x) / 3, fma(c.y, 2, end.y) / 3), end);
      break;
    }
    case kCGPathElementAddCurveToPoint:
      curve(w, device(t, q[0].x, q[0].y), device(t, q[1].x, q[1].y),
            device(t, q[2].x, q[2].y));
      break;
    case kCGPathElementCloseSubpath:
      close_path(w);
      break;
  }
}

/* The same CGPath rmr_apple.c builds from a plan path. */
static CGPathRef native_path(rmr_path_view v) {
  CGMutablePathRef path = CGPathCreateMutable();
  if (!path) return nullptr;
  for (size_t i = 0; i < v.count; i++) {
    const rmr_element* e = &v.elements[i];
    switch (e->kind) {
      case RMR_MOVE:
        CGPathMoveToPoint(path, nullptr, e->end.x, e->end.y);
        break;
      case RMR_LINE:
        CGPathAddLineToPoint(path, nullptr, e->end.x, e->end.y);
        break;
      case RMR_CURVE:
        CGPathAddCurveToPoint(path, nullptr, e->control1.x, e->control1.y,
                              e->control2.x, e->control2.y, e->end.x, e->end.y);
        break;
      case RMR_CLOSE:
        CGPathCloseSubpath(path);
        break;
    }
  }
  return path;
}

/* The draw's outline after its erasers, as rmr_apple.c computes it with
 * CoreGraphics' path operations. */
static rmr_status erased_path(const rmr_draw* d, CGPathRef* out) {
  if (__builtin_available(macOS 13.0, iOS 16.0, *)) {
    CGPathRef path = native_path(d->path);
    if (!path) return RMR_ERR_OUT_OF_MEMORY;
    if (d->mode == RMR_STROKE) {
      CGLineCap cap = d->cap == RMR_CAP_ROUND    ? kCGLineCapRound
                      : d->cap == RMR_CAP_SQUARE ? kCGLineCapSquare
                                                 : kCGLineCapButt;
      CGLineJoin join = d->join == RMR_JOIN_ROUND   ? kCGLineJoinRound
                        : d->join == RMR_JOIN_MITER ? kCGLineJoinMiter
                                                    : kCGLineJoinBevel;
      CGPathRef outline = CGPathCreateCopyByStrokingPath(
          path, nullptr, d->line_width, cap, join,
          d->miter_limit != 0 ? d->miter_limit : 10);
      CGPathRelease(path);
      path = outline;
      if (!path) return RMR_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < d->eraser_count; i++) {
      CGPathRef mask = native_path(d->erasers[i]);
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
    *out = path;
    return RMR_OK;
  }
  return RMR_ERR_UNSUPPORTED;
}

/* The entry for the mask's bytes, added with its dimensions when they were
 * not seen before (*found false); null on allocation failure. */
static image_entry* image_lookup(catalog* c, const rmr_mask_view* m,
                                 bool* found) {
  rmr_pdf_digest digest =
      rmr_pdf_hash(m->bytes, m->stride * m->height, 0x5245414c4d41534bu);
  if (c->image_count + 1 > c->image_capacity / 4 * 3) {
    size_t capacity = c->image_capacity ? c->image_capacity * 2 : 256;
    image_entry* grown = calloc(capacity, sizeof *grown);
    if (!grown) return nullptr;
    for (size_t i = 0; i < c->image_capacity; i++)
      if (c->images[i].width) {
        size_t at = c->images[i].digest.a & (capacity - 1);
        while (grown[at].width) at = (at + 1) & (capacity - 1);
        grown[at] = c->images[i];
      }
    free(c->images);
    c->images = grown;
    c->image_capacity = capacity;
  }

  size_t at = digest.a & (c->image_capacity - 1);
  for (; c->images[at].width; at = (at + 1) & (c->image_capacity - 1))
    if (c->images[at].digest.a == digest.a &&
        c->images[at].digest.b == digest.b) {
      *found = true;
      return &c->images[at];
    }
  *found = false;
  c->images[at] = (image_entry){digest, (uint32_t)m->width, (uint32_t)m->height,
                                (uint32_t)m->stride, 0};
  c->image_count++;
  return &c->images[at];
}

/* The grain mask as an image XObject, its rows packed as PDF expects, with
 * the dimensions first seen with its bytes. Its number, or 0 on failure. */
static uint32_t mask_image(page* p, const rmr_mask_view* m) {
  bool found;
  image_entry* e = image_lookup(p->resources, m, &found);
  if (!e) return 0;
  if (!e->number) {
    size_t width = e->width, height = e->height, stride = e->stride,
           row = (width + 7) / 8, size = row * height;
    uint8_t* data = malloc(size ? size : 1);
    if (!data) return 0;
    for (size_t y = 0; y < height; y++)
      memcpy(data + y * row, m->bytes + y * stride, row);

    uint32_t n = number(p);
    rmr_pdf_object* o = n ? rmr_pdf_batch_add(p->batch, n, true) : nullptr;
    if (!o) {
      free(data);
      return 0;
    }
    o->data = data;
    o->size = size;
    rmr_pdf_text_format(&o->dict,
                        "/Type /XObject /Subtype /Image /Width %zu /Height %zu "
                        "/ImageMask true /BitsPerComponent 1",
                        width, height);
    e->number = n;
  }
  return list_add(&p->xobjects, &p->xobject_count, &p->xobject_capacity,
                  e->number)
             ? e->number
             : 0;
}

bool rmr_apple_writer_mask(void* context, const rmr_mask_view* m,
                           rmr_mask_view* out) {
  rmr_apple_writer* w = context;
  bool found;
  image_entry* e = image_lookup(&w->shared, m, &found);
  if (!e || !found ||
      (e->width == m->width && e->height == m->height &&
       e->stride == m->stride))
    return false;
  *out = *m;
  out->width = e->width;
  out->height = e->height;
  out->stride = e->stride;
  return true;
}

/* "q <context transform> cm", as CoreGraphics opens a stroke. */
static void transform(page* p) {
  put(p, "q ");
  exact(p, p->ctm.a);
  exact(p, p->ctm.b);
  exact(p, p->ctm.c);
  exact(p, p->ctm.d);
  exact(p, p->ctm.tx);
  exact(p, p->ctm.ty);
  put(p, "cm\n");
}

/* rmr_backend begin: the sheet's transform; an overlay is clipped to its
 * sheet. */
static rmr_status begin(void* context, rmr_rect viewport) {
  (void)viewport;
  page* p = context;
  if (p->clip) {
    put(p, "q\n0 0 ");
    exact(p, p->width);
    exact(p, p->height);
    put(p, "re\nW n\n");
  }
  return RMR_OK;
}

/* rmr_backend draw: one fill, stroke, combined fill and stroke or grain mask,
 * after its erasers. */
static rmr_status draw(void* context, const rmr_draw* d) {
  page* p = context;
  CGPathRef erased = nullptr;
  if (d->eraser_count) {
    rmr_status s = erased_path(d, &erased);
    if (s != RMR_OK) return s;
    if (CGPathIsEmpty(erased)) {
      CGPathRelease(erased);
      return RMR_OK; /* Erased away: nothing to paint. */
    }
  } else if (!d->path.count)
    return RMR_OK; /* An empty path paints nothing. */

  bool ok = true;
  path_writer w = {.p = p};
  if (d->mode == RMR_FILL_STROKE) {
    outer_line(p, d);
    ok = outer_color(p, d->color, false) &&
         outer_color(p, d->stroke_color, true) &&
         outer_state(p, d->color.alpha, d->stroke_color.alpha, d->blend);
    transform(p);
    plan_path(p, d->path, true);
    put(p, "B\nQ\n");
  } else if (d->mode == RMR_FILL || erased) {
    rmr_color opaque = {d->color.red, d->color.green, d->color.blue, 1};
    if (d->mask.bytes) {
      uint32_t image = mask_image(p, &d->mask);
      if (!image) ok = false;
      put(p, "q\n");
      if (erased)
        CGPathApply(erased, &w, native_element);
      else
        plan_path(p, d->path, false);
      put(p, "W n\n/Perceptual ri\n");
      /* Inside q: color and state changes end at Q. */
      double ca = p->ca;
      rmr_blend blend = p->blend;
      ok = ok && set_color(p, opaque, false) &&
           outer_state(p, d->color.alpha, p->stroke_alpha, d->blend);
      p->ca = ca;
      p->blend = blend;

      CGAffineTransform t = p->ctm;
      const rmr_mask_view* m = &d->mask;
      double top = m->box.y + m->box.height;
      double x = fma(t.c, top, fma(t.a, m->box.x, t.tx));
      double y = fma(t.d, top, fma(t.b, m->box.x, t.ty));
      double a = t.a * 1, b = t.b * 1, c = t.c * -1, e = t.d * -1;
      double zero = 0;
      put(p, "q ");
      exact(p, fma(a, m->box.width, c * zero));
      exact(p, fma(b, m->box.width, e * zero));
      exact(p, fma(a, zero, c * m->box.height));
      exact(p, fma(b, zero, e * m->box.height));
      exact(p, x + fma(a, zero, c * zero));
      exact(p, y + fma(b, zero, e * zero));
      rmr_pdf_text_format(&p->content, "cm\n/RMRx%u Do\nQ\nQ\n", image);
    } else {
      ok = outer_color(p, opaque, false) &&
           outer_state(p, d->color.alpha, p->stroke_alpha, d->blend);
      if (erased)
        CGPathApply(erased, &w, native_element);
      else
        plan_path(p, d->path, false);
      put(p, "f\n");
    }
  } else {
    outer_line(p, d);
    rmr_color opaque = {d->color.red, d->color.green, d->color.blue, 1};
    ok = outer_color(p, opaque, true) &&
         outer_state(p, p->ca, d->color.alpha, d->blend);
    transform(p);
    plan_path(p, d->path, true);
    put(p, "S\nQ\n");
  }

  if (erased) CGPathRelease(erased);
  /* The output limit is the document's, checked as pages are appended. */
  return ok && !p->content.failed ? RMR_OK : RMR_ERR_OUT_OF_MEMORY;
}

/* rmr_backend end: closes an overlay's clip. */
static rmr_status end(void* context) {
  page* p = context;
  if (p->clip) put(p, "Q\n");
  return p->content.failed ? RMR_ERR_OUT_OF_MEMORY : RMR_OK;
}

static void abort_page(void* context) { (void)context; }

/* The capabilities rmr_apple.c's backend advertises. */
static rmr_backend backend(page* p) {
  uint64_t capabilities = RMR_CAP_FILL | RMR_CAP_STROKE | RMR_CAP_DARKEN |
                          RMR_CAP_MULTIPLY | RMR_CAP_MASK | RMR_CAP_GROUP |
                          RMR_CAP_IMAGE | RMR_CAP_FILL_STROKE | RMR_CAP_MITER |
                          RMR_CAP_TEXT | RMR_CAP_PDF;
  if (__builtin_available(macOS 13.0, iOS 16.0, *))
    capabilities |= RMR_CAP_ERASE;
  return (rmr_backend){
      RMR_BACKEND_ABI, sizeof(rmr_backend), capabilities, p, begin, draw, end,
      abort_page};
}

/* Draws the writer leaves to CoreGraphics. */
static bool native_only(const rmr_draw* d) {
  return d->group || d->mode == RMR_TEXT || d->mode == RMR_IMAGE ||
         d->mode == RMR_PDF;
}

size_t rmr_apple_writer_split(const rmr_plan* plan) {
  for (size_t i = rmr_plan_count(plan); i-- > 0;)
    if (native_only(rmr_plan_draw(plan, i))) return i + 1;
  return 0;
}

rmr_status rmr_apple_writer_create(const rmr_limits* limits, unsigned decimals,
                                   bool merged, rmr_apple_writer** out) {
  rmr_apple_writer* w = calloc(1, sizeof *w);
  if (!w) return RMR_ERR_OUT_OF_MEMORY;
  w->limits = limits ? *limits : rmr_limits_default();
  w->decimals = decimals;
  w->merged = merged;

  size_t workers = rmr_pdf_pipeline_workers();
  w->window = 2 * workers + 2;
  w->pipeline = rmr_pdf_pipeline_create(workers);
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  w->profile = srgb ? CGColorSpaceCopyICCData(srgb) : nullptr;
  if (srgb) CGColorSpaceRelease(srgb);
  rmr_status s = !w->pipeline || !w->profile ? RMR_ERR_OUT_OF_MEMORY : RMR_OK;
  /* CoreGraphics checks nothing here; the builder only accounts output. */
  if (s == RMR_OK && merged)
    s = rmr_pdf_builder_create(&w->limits, nullptr, nullptr, &w->builder);
  if (s != RMR_OK) {
    rmr_apple_writer_free(w);
    return s;
  }
  *out = w;
  return RMR_OK;
}

void rmr_apple_writer_free(rmr_apple_writer* w) {
  if (!w) return;
  rmr_pdf_pipeline_free(w->pipeline);
  if (w->profile) CFRelease(w->profile);
  rmr_pdf_builder_free(w->builder);
  catalog_free(&w->shared);
  for (size_t i = 0; i < w->page_count; i++) {
    free((void*)(uintptr_t)w->pages[i].xobjects);
    free((void*)(uintptr_t)w->pages[i].states);
    free((void*)(uintptr_t)w->pages[i].spaces);
  }
  free(w->pages);
  free(w);
}

void rmr_apple_writer_output(rmr_apple_writer* w, const rmr_output* output,
                             const uint8_t* base, size_t size) {
  rmr_pdf_builder_version(w->builder, base, size);
  rmr_pdf_builder_output(w->builder, output);
}

size_t rmr_apple_writer_pending(const rmr_apple_writer* w) {
  return rmr_pdf_pipeline_pending(w->pipeline);
}

/* Moves serialized batches into the builder, oldest first: every pending one
 * when all is set, otherwise those ready, waiting only while more than the
 * window are queued. */
rmr_status rmr_apple_writer_flush(rmr_apple_writer* w, bool all) {
  for (;;) {
    size_t pending = rmr_pdf_pipeline_pending(w->pipeline);
    if (!pending) return RMR_OK;
    rmr_pdf_batch* b =
        rmr_pdf_pipeline_next(w->pipeline, all || pending > w->window);
    if (!b) return RMR_OK;
    rmr_status s = b->status;
    if (s == RMR_OK) {
      uint32_t* numbers = calloc(b->count ? b->count : 1, sizeof *numbers);
      if (!numbers)
        s = RMR_ERR_OUT_OF_MEMORY;
      else {
        for (size_t i = 0; i < b->count; i++) numbers[i] = b->objects[i].number;
        s = rmr_pdf_builder_append(w->builder, b->chunk.bytes, b->chunk.size,
                                   numbers, b->offsets, b->count, nullptr);
        free(numbers);
      }
    }
    rmr_pdf_batch_free(b);
    if (s != RMR_OK) return s;
  }
}

/* Serializes draws first onwards of plan as page state p, already set up,
 * into p->batch, starting from PDF's initial graphics state. */
static rmr_status serialize(page* p, const rmr_plan* plan, size_t first,
                            rmr_cancel_fn cancel, void* context, rmr_error* e) {
  p->ca = p->stroke_alpha = 1;
  p->line_width = 1;
  p->miter = 10;
  rmr_backend b = backend(p);
  return rmr_plan_execute_range(plan, first, rmr_plan_count(plan), &b, cancel,
                                context, e);
}

static void page_free(page* p) {
  rmr_pdf_text_free(&p->content);
  free(p->xobjects);
  free(p->states);
  catalog_free(&p->local);
  rmr_pdf_batch_free(p->batch);
}

/* Fill `e` with `code` and `message`, if non-null; always returns `code`. */
static rmr_status fail(rmr_error* e, rmr_status code, const char* message) {
  if (e) {
    *e = (rmr_error){.code = code};
    snprintf(e->message, sizeof e->message, "%s", message);
  }
  return code;
}

/* rmr_apple.c's valid_sheet for PDF output. */
static bool valid_sheet(const rmr_sheet* s) {
  if (!s || !isfinite(s->width) || !isfinite(s->height) || s->width <= 0 ||
      s->height <= 0)
    return false;
  rmr_transform t = s->transform;
  return isfinite(t.a) && t.a > 0 && t.b == 0 && t.c == 0 && t.d == -t.a &&
         isfinite(t.tx) && isfinite(t.ty);
}

rmr_status rmr_apple_writer_page(rmr_apple_writer* w, size_t index,
                                 const rmr_plan* plan, size_t first,
                                 const rmr_sheet* sheet, rmr_cancel_fn cancel,
                                 void* context, rmr_error* e) {
  if (!w->merged) return fail(e, RMR_ERR_INVALID_ARGUMENT, "not a document");
  /* As rmr_apple_draw_context checks. */
  if (!valid_sheet(sheet))
    return fail(e, RMR_ERR_INVALID_ARGUMENT, "invalid native output sheet");
  rmr_transform t = sheet->transform;
  page p = {.writer = w,
            .resources = &w->shared,
            .batch = calloc(1, sizeof(rmr_pdf_batch)),
            .ctm = CGAffineTransformMake(t.a, t.b, t.c, t.d, t.tx, t.ty),
            .decimals = w->decimals};
  p.inverse = CGAffineTransformInvert(p.ctm);
  if (w->page_count == w->page_capacity) {
    size_t capacity = w->page_capacity ? w->page_capacity * 2 : 64;
    void* grown = realloc(w->pages, capacity * sizeof *w->pages);
    if (!grown) {
      page_free(&p);
      return fail(e, RMR_ERR_OUT_OF_MEMORY, "PDF page allocation failed");
    }
    w->pages = grown;
    w->page_capacity = capacity;
  }
  if (!p.batch)
    return fail(e, RMR_ERR_OUT_OF_MEMORY, "PDF page allocation failed");

  rmr_status s = serialize(&p, plan, first, cancel, context, e);
  if (s != RMR_OK) {
    page_free(&p);
    return s;
  }

  uint32_t contents = rmr_pdf_builder_reserve(w->builder);
  rmr_pdf_object* o =
      contents ? rmr_pdf_batch_add(p.batch, contents, true) : nullptr;
  uint32_t* spaces = p.space ? malloc(sizeof *spaces) : nullptr;
  if (!o || (p.space && !spaces) || p.content.failed) {
    free(spaces);
    page_free(&p);
    return fail(e, RMR_ERR_OUT_OF_MEMORY, "PDF page allocation failed");
  }
  o->data = p.content.bytes;
  o->size = p.content.size;
  p.content = (rmr_pdf_text){0};
  if (spaces) spaces[0] = w->shared.space;

  w->pages[w->page_count++] =
      (rmr_pdf_page_objects){.page = index,
                             .contents = contents,
                             .xobjects = p.xobjects,
                             .states = p.states,
                             .spaces = spaces,
                             .xobject_count = p.xobject_count,
                             .state_count = p.state_count,
                             .space_count = spaces ? 1 : 0};
  p.xobjects = p.states = nullptr;
  rmr_pdf_pipeline_submit(w->pipeline, p.batch);
  p.batch = nullptr;
  page_free(&p);
  return RMR_OK;
}

size_t rmr_apple_writer_pages(const rmr_apple_writer* w) {
  return w->page_count;
}

rmr_status rmr_apple_writer_merge(rmr_apple_writer* w, const uint8_t* base,
                                  size_t size, rmr_buffer* out) {
  rmr_status s = rmr_apple_writer_flush(w, true);
  if (s != RMR_OK) return s;
  return rmr_pdf_builder_merge(w->builder, base, size, w->pages, w->page_count,
                               out, nullptr);
}

rmr_status rmr_apple_writer_overlay(rmr_apple_writer* w, size_t tag,
                                    const rmr_plan* plan,
                                    const rmr_sheet* sheet,
                                    rmr_cancel_fn cancel, void* context,
                                    rmr_error* e) {
  if (w->merged) return fail(e, RMR_ERR_INVALID_ARGUMENT, "not an overlay");
  /* As rmr_apple_pdf_sheets checks one page. */
  if (!plan) return fail(e, RMR_ERR_INVALID_ARGUMENT, "null page plan");
  if (!valid_sheet(sheet))
    return fail(e, RMR_ERR_INVALID_ARGUMENT, "invalid PDF sheet");
  rmr_transform t;
  rmr_status s = rmr_page_to_pdf(rmr_plan_viewport(plan), 1, &t, e);
  if (s != RMR_OK) return s;
  if (sheet->width > 10000000 || sheet->height > 10000000)
    return fail(e, RMR_ERR_LIMIT, "PDF dimensions too large");
  if (cancel && cancel(context)) return fail(e, RMR_ERR_CANCELLED, "cancelled");

  t = sheet->transform;
  /* 1 catalog, 2 page tree, 3 page, 4 content; resources from 5. */
  page p = {.writer = w,
            .batch = calloc(1, sizeof(rmr_pdf_batch)),
            .next = 5,
            .ctm = CGAffineTransformMake(t.a, t.b, t.c, t.d, t.tx, t.ty),
            .clip = true,
            .width = sheet->width,
            .height = sheet->height,
            .decimals = w->decimals};
  p.resources = &p.local;
  p.inverse = CGAffineTransformInvert(p.ctm);
  if (!p.batch)
    return fail(e, RMR_ERR_OUT_OF_MEMORY,
                "PDF output memory limit or allocation failure");

  s = serialize(&p, plan, 0, cancel, context, e);
  if (s != RMR_OK) {
    page_free(&p);
    return s;
  }

  rmr_pdf_object* catalog = rmr_pdf_batch_add(p.batch, 1, false);
  rmr_pdf_object* tree = rmr_pdf_batch_add(p.batch, 2, false);
  rmr_pdf_object* leaf = rmr_pdf_batch_add(p.batch, 3, false);
  rmr_pdf_object* content = rmr_pdf_batch_add(p.batch, 4, true);
  if (!catalog || !tree || !leaf || !content || p.content.failed) {
    page_free(&p);
    return fail(e, RMR_ERR_OUT_OF_MEMORY,
                "PDF output memory limit or allocation failure");
  }
  rmr_pdf_text_string(&catalog->dict, "<< /Type /Catalog /Pages 2 0 R >>");
  rmr_pdf_text_string(&tree->dict,
                      "<< /Type /Pages /Kids [ 3 0 R ] /Count 1 >>");
  rmr_pdf_text* d = &leaf->dict;
  rmr_pdf_text_string(d, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ");
  rmr_pdf_text_number(d, sheet->width, 0);
  rmr_pdf_text_string(d, " ");
  rmr_pdf_text_number(d, sheet->height, 0);
  rmr_pdf_text_string(d, "] /Contents 4 0 R /Resources <<");
  if (p.space)
    rmr_pdf_text_format(d, " /ColorSpace << /RMRc%u %u 0 R >>", p.local.space,
                        p.local.space);
  if (p.state_count) {
    rmr_pdf_text_string(d, " /ExtGState <<");
    for (size_t i = 0; i < p.state_count; i++)
      rmr_pdf_text_format(d, " /RMRg%u %u 0 R", p.states[i], p.states[i]);
    rmr_pdf_text_string(d, " >>");
  }
  if (p.xobject_count) {
    rmr_pdf_text_string(d, " /XObject <<");
    for (size_t i = 0; i < p.xobject_count; i++)
      rmr_pdf_text_format(d, " /RMRx%u %u 0 R", p.xobjects[i], p.xobjects[i]);
    rmr_pdf_text_string(d, " >>");
  }
  rmr_pdf_text_string(d, " >> >>");
  content->data = p.content.bytes;
  content->size = p.content.size;
  p.content = (rmr_pdf_text){0};

  p.batch->root = 1;
  p.batch->tag = tag;
  rmr_pdf_pipeline_submit(w->pipeline, p.batch);
  p.batch = nullptr;
  page_free(&p);
  return RMR_OK;
}

bool rmr_apple_writer_next_overlay(rmr_apple_writer* w, bool wait,
                                   rmr_buffer* out, size_t* tag,
                                   rmr_status* status) {
  *status = RMR_OK;
  rmr_pdf_batch* b = rmr_pdf_pipeline_next(w->pipeline, wait);
  if (!b) return false;
  *status = b->status;
  if (b->status == RMR_OK) {
    *out = (rmr_buffer){b->chunk.bytes, b->chunk.size};
    *tag = b->tag;
    b->chunk = (rmr_pdf_text){0};
  }
  rmr_pdf_batch_free(b);
  return *status == RMR_OK;
}
