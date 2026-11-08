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
 * - A transparency group (BeginTransparencyLayer) is a Form XObject: an
 *   isolated, non-knockout group blending in Generic RGB, clipped inside to
 *   the page's media box, its members drawn as on the page, painted inside
 *   q/Q under the group's alpha (/ca) and blend mode (/BM). Its BBox is the
 *   union of what CoreGraphics records for each member (see member_box); a
 *   group whose BBox misses the media box is not written at all.
 * - Every number goes through rmr_pdf_number, CoreGraphics' own formatting.
 *
 * Graphics state is emitted where it changes, as CoreGraphics does; only its
 * order and the q/Q nesting differ, which do not change what is painted.
 *
 * A page's template, its plan's leading background draws, is written as a
 * Form XObject of its own, once per document for every page whose template
 * draws the same: CoreGraphics would draw it on the page itself, and the
 * form paints what those operators paint. */

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

/* A template's Form XObject, by the digest of its dictionary and content. */
typedef struct {
  rmr_pdf_digest digest;
  uint32_t number;
} form_entry;

/* Shared objects already written and their numbers: one set per merged
 * document, or one per overlay PDF. */
typedef struct {
  image_entry* images; /* open addressing, capacity a power of two */
  size_t image_count, image_capacity;
  state_entry* states;
  size_t state_count;
  form_entry* forms; /* open addressing, capacity a power of two */
  size_t form_count, form_capacity;
  uint32_t profile, space, group_profile, group_space;
} catalog;

static void catalog_free(catalog* c) {
  free(c->images);
  free(c->states);
  free(c->forms);
  *c = (catalog){0};
}

struct rmr_apple_writer {
  rmr_limits limits;
  unsigned decimals;
  bool merged;
  rmr_pdf_pipeline* pipeline;
  size_t window;
  /* sRGB, which colors are in, and Generic RGB, which CoreGraphics'
   * transparency layers blend in. */
  CFDataRef profile, group_profile;
  /* Merged documents. */
  rmr_pdf_builder* builder;
  catalog shared;
  rmr_pdf_page_objects* pages;
  size_t page_count, page_capacity;
};

/* A content stream being written, the resources it names and the graphics
 * state in effect outside q/Q: a page's, or a Form XObject's. */
typedef struct {
  rmr_pdf_text content;
  uint32_t *xobjects, *states;
  size_t xobject_count, xobject_capacity, state_count, state_capacity;
  bool space;
  bool has_fill, has_stroke;
  double fill[3], stroke[3], ca, stroke_alpha, line_width, miter;
  rmr_blend blend;
  int cap, join;
} stream;

/* PDF's initial graphics state. */
static void initial(stream* s) {
  s->has_fill = s->has_stroke = false;
  s->ca = s->stroke_alpha = 1;
  s->line_width = 1;
  s->miter = 10;
  s->blend = RMR_BLEND_NORMAL;
  s->cap = s->join = 0;
}

static void stream_free(stream* s) {
  rmr_pdf_text_free(&s->content);
  free(s->xobjects);
  free(s->states);
  *s = (stream){0};
}

/* Per-page serialization state; the rmr_backend context. */
typedef struct {
  rmr_apple_writer* writer;
  catalog* resources;
  catalog local;
  rmr_pdf_batch* batch;
  uint32_t next; /* overlay numbering; 0 numbers from the builder */
  stream s;      /* being written */
  stream outer;  /* the page's, while s is a form's */
  /* The open transparency group, 0 for none, and its members' bounds. */
  uint64_t group;
  double group_alpha;
  rmr_blend group_blend;
  CGRect box;
  CGAffineTransform ctm, inverse;
  bool clip;
  double width, height;
  unsigned decimals;
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

static void put(page* p, const char* s) {
  rmr_pdf_text_string(&p->s.content, s);
}

/* A number exactly as CoreGraphics prints it. */
static void exact(page* p, double v) {
  rmr_pdf_text_number(&p->s.content, v, 0);
  rmr_pdf_text_append(&p->s.content, " ", 1);
}

/* A path coordinate: as CoreGraphics prints it, or rounded when asked. */
static void coordinate(page* p, double v) {
  rmr_pdf_text_number(&p->s.content, v, p->decimals);
  rmr_pdf_text_append(&p->s.content, " ", 1);
}

/* An ICCBased color space of profile icc, its stream and array objects
 * written once per catalog into *profile and *space; *space, or 0 on
 * failure. */
static uint32_t icc_space(page* p, CFDataRef icc, uint32_t* profile,
                          uint32_t* space) {
  if (!*space) {
    size_t size = (size_t)CFDataGetLength(icc);
    uint32_t data_number = number(p), array = number(p);
    /* Adding an object may move those added before it. */
    size_t at = p->batch->count;
    bool added = data_number && array &&
                 rmr_pdf_batch_add(p->batch, data_number, true) &&
                 rmr_pdf_batch_add(p->batch, array, false);
    rmr_pdf_object* data = added ? &p->batch->objects[at] : nullptr;
    rmr_pdf_object* list = added ? &p->batch->objects[at + 1] : nullptr;
    uint8_t* bytes = added ? malloc(size) : nullptr;
    if (!bytes) return 0;
    memcpy(bytes, CFDataGetBytePtr(icc), size);
    data->data = bytes;
    data->size = size;
    rmr_pdf_text_string(&data->dict, "/N 3 /Alternate /DeviceRGB");
    rmr_pdf_text_format(&list->dict, "[ /ICCBased %u 0 R ]", data_number);
    *profile = data_number;
    *space = array;
  }
  return *space;
}

/* The sRGB ICCBased color space, written once per catalog. */
static uint32_t color_space(page* p) {
  catalog* c = p->resources;
  uint32_t space = icc_space(p, p->writer->profile, &c->profile, &c->space);
  if (space) p->s.space = true;
  return space;
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
      return list_add(&p->s.states, &p->s.state_count, &p->s.state_capacity,
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
  return list_add(&p->s.states, &p->s.state_count, &p->s.state_capacity, n) ? n
                                                                            : 0;
}

static bool use_state(page* p, const char* key, double alpha,
                      const char* name) {
  uint32_t n = graphics_state(p, key, alpha, name);
  if (!n) return false;
  rmr_pdf_text_format(&p->s.content, "/RMRg%u gs\n", n);
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
  if (ca != p->s.ca) {
    if (!use_state(p, "ca", ca, nullptr)) return false;
    p->s.ca = ca;
  }
  if (stroke_alpha != p->s.stroke_alpha) {
    if (!use_state(p, "CA", stroke_alpha, nullptr)) return false;
    p->s.stroke_alpha = stroke_alpha;
  }
  if (blend != p->s.blend) {
    if (!use_state(p, "BM", 0, blend_name(blend))) return false;
    p->s.blend = blend;
  }
  return true;
}

static bool set_color(page* p, rmr_color c, bool stroke) {
  uint32_t space = color_space(p);
  if (!space) return false;
  rmr_pdf_text_format(&p->s.content, "/RMRc%u %s ", space,
                      stroke ? "CS" : "cs");
  exact(p, c.red);
  exact(p, c.green);
  exact(p, c.blue);
  put(p, stroke ? "SC\n" : "sc\n");
  return true;
}

static bool outer_color(page* p, rmr_color c, bool stroke) {
  bool* has = stroke ? &p->s.has_stroke : &p->s.has_fill;
  double* now = stroke ? p->s.stroke : p->s.fill;
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
  if (d->line_width != p->s.line_width) {
    exact(p, d->line_width);
    put(p, "w\n");
    p->s.line_width = d->line_width;
  }
  if (cap != p->s.cap) {
    rmr_pdf_text_format(&p->s.content, "%d J\n", cap);
    p->s.cap = cap;
  }
  if (join != p->s.join) {
    rmr_pdf_text_format(&p->s.content, "%d j\n", join);
    p->s.join = join;
  }
  if (miter != p->s.miter) {
    exact(p, miter);
    put(p, "M\n");
    p->s.miter = miter;
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

/* A path being mapped into device space, as CoreGraphics keeps it. */
typedef struct {
  CGMutablePathRef path;
  CGAffineTransform t;
} device_mapper;

/* CGPathApply callback adding an element in device space, a quadratic
 * segment as it is. */
static void device_element(void* info, const CGPathElement* e) {
  device_mapper* m = info;
  const CGPoint* q = e->points;
  CGPoint a, b, c;
  switch (e->type) {
    case kCGPathElementMoveToPoint:
      a = device(m->t, q[0].x, q[0].y);
      CGPathMoveToPoint(m->path, nullptr, a.x, a.y);
      break;
    case kCGPathElementAddLineToPoint:
      a = device(m->t, q[0].x, q[0].y);
      CGPathAddLineToPoint(m->path, nullptr, a.x, a.y);
      break;
    case kCGPathElementAddQuadCurveToPoint:
      a = device(m->t, q[0].x, q[0].y);
      b = device(m->t, q[1].x, q[1].y);
      CGPathAddQuadCurveToPoint(m->path, nullptr, a.x, a.y, b.x, b.y);
      break;
    case kCGPathElementAddCurveToPoint:
      a = device(m->t, q[0].x, q[0].y);
      b = device(m->t, q[1].x, q[1].y);
      c = device(m->t, q[2].x, q[2].y);
      CGPathAddCurveToPoint(m->path, nullptr, a.x, a.y, b.x, b.y, c.x, c.y);
      break;
    case kCGPathElementCloseSubpath:
      CGPathCloseSubpath(m->path);
      break;
  }
}

/* What CoreGraphics records of a draw inside a transparency layer, the
 * layer's BBox being the union of its members': the bounds of the path in
 * device space (erased, when erasers cut it); a stroke's grown on every side
 * by its width, or by its width times half its miter limit for mitered joins
 * when that is more; for a grain mask, its image's rectangle within the
 * bounds of its clip's control points. image is that rectangle's corner,
 * set for a mask. False on allocation failure. */
static bool member_box(const page* p, const rmr_draw* d, CGPathRef erased,
                       CGPoint image, CGRect* out) {
  device_mapper m = {CGPathCreateMutable(), p->ctm};
  if (!m.path) return false;
  if (erased) {
    CGPathApply(erased, &m, device_element);
  } else {
    CGPathRef path = native_path(d->path);
    if (!path) {
      CGPathRelease(m.path);
      return false;
    }
    CGPathApply(path, &m, device_element);
    CGPathRelease(path);
  }
  CGRect r = d->mask.bytes ? CGPathGetBoundingBox(m.path)
                           : CGPathGetPathBoundingBox(m.path);
  CGPathRelease(m.path);
  if (d->mode == RMR_STROKE && !erased) {
    double limit = d->miter_limit != 0 ? d->miter_limit : 10;
    double grow = d->line_width *
                  (d->join == RMR_JOIN_MITER ? fmax(limit / 2, 1) : 1) *
                  p->ctm.a;
    r = CGRectInset(r, -grow, -grow);
  }
  if (d->mask.bytes)
    r = CGRectIntersection(CGRectStandardize(CGRectMake(
                               image.x, image.y, p->ctm.a * d->mask.box.width,
                               -p->ctm.d * d->mask.box.height)),
                           r);
  *out = r;
  return true;
}

/* The first eight bytes of a digest, where the image table starts looking. */
static size_t slot(const rmr_pdf_digest* d) {
  uint64_t v = 0;
  for (unsigned k = 0; k < 8; k++) v |= (uint64_t)d->bytes[k] << (8 * k);
  return (size_t)v;
}

/* The entry for the mask's bytes, added with its dimensions when they were
 * not seen before (*found false); null on allocation failure. */
static image_entry* image_lookup(catalog* c, const rmr_mask_view* m,
                                 bool* found) {
  rmr_pdf_digest digest = rmr_pdf_hash(m->bytes, m->stride * m->height);
  if (c->image_count + 1 > c->image_capacity / 4 * 3) {
    size_t capacity = c->image_capacity ? c->image_capacity * 2 : 256;
    image_entry* grown = calloc(capacity, sizeof *grown);
    if (!grown) return nullptr;
    for (size_t i = 0; i < c->image_capacity; i++)
      if (c->images[i].width) {
        size_t at = slot(&c->images[i].digest) & (capacity - 1);
        while (grown[at].width) at = (at + 1) & (capacity - 1);
        grown[at] = c->images[i];
      }
    free(c->images);
    c->images = grown;
    c->image_capacity = capacity;
  }

  size_t at = slot(&digest) & (c->image_capacity - 1);
  for (; c->images[at].width; at = (at + 1) & (c->image_capacity - 1))
    if (!memcmp(c->images[at].digest.bytes, digest.bytes,
                sizeof digest.bytes)) {
      *found = true;
      return &c->images[at];
    }
  *found = false;
  c->images[at] = (image_entry){digest, (uint32_t)m->width, (uint32_t)m->height,
                                (uint32_t)m->stride, 0};
  c->image_count++;
  return &c->images[at];
}

/* The entry for a template form of digest d, added unnumbered when it was
 * not seen before; null on allocation failure. */
static form_entry* form_lookup(catalog* c, rmr_pdf_digest d) {
  if (c->form_count + 1 > c->form_capacity / 4 * 3) {
    size_t capacity = c->form_capacity ? c->form_capacity * 2 : 16;
    form_entry* grown = calloc(capacity, sizeof *grown);
    if (!grown) return nullptr;
    for (size_t i = 0; i < c->form_capacity; i++)
      if (c->forms[i].number) {
        size_t at = slot(&c->forms[i].digest) & (capacity - 1);
        while (grown[at].number) at = (at + 1) & (capacity - 1);
        grown[at] = c->forms[i];
      }
    free(c->forms);
    c->forms = grown;
    c->form_capacity = capacity;
  }

  size_t at = slot(&d) & (c->form_capacity - 1);
  for (; c->forms[at].number; at = (at + 1) & (c->form_capacity - 1))
    if (!memcmp(c->forms[at].digest.bytes, d.bytes, sizeof d.bytes))
      return &c->forms[at];
  c->forms[at].digest = d;
  return &c->forms[at];
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
  return list_add(&p->s.xobjects, &p->s.xobject_count, &p->s.xobject_capacity,
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

/* "q 0 0 <width> <height> re W n": the media box, as CoreGraphics clips an
 * overlay to its sheet and every transparency layer to the page. */
static void media_clip(page* p) {
  put(p, "q\n0 0 ");
  exact(p, p->width);
  exact(p, p->height);
  put(p, "re\nW n\n");
}

/* Writes the resource dictionary of what s names, after the objects'
 * numbers. */
static void resource_names(rmr_pdf_text* d, const stream* s, uint32_t space) {
  rmr_pdf_text_string(d, "<<");
  if (s->space)
    rmr_pdf_text_format(d, " /ColorSpace << /RMRc%u %u 0 R >>", space, space);
  if (s->state_count) {
    rmr_pdf_text_string(d, " /ExtGState <<");
    for (size_t i = 0; i < s->state_count; i++)
      rmr_pdf_text_format(d, " /RMRg%u %u 0 R", s->states[i], s->states[i]);
    rmr_pdf_text_string(d, " >>");
  }
  if (s->xobject_count) {
    rmr_pdf_text_string(d, " /XObject <<");
    for (size_t i = 0; i < s->xobject_count; i++)
      rmr_pdf_text_format(d, " /RMRx%u %u 0 R", s->xobjects[i], s->xobjects[i]);
    rmr_pdf_text_string(d, " >>");
  }
  rmr_pdf_text_string(d, " >>");
}

/* Starts writing a Form XObject's content in place of the page's. */
static void form_begin(page* p) {
  p->outer = p->s;
  p->s = (stream){0};
  initial(&p->s);
}

/* Stops writing the form's content, returned, and goes back to the page's. */
static stream form_end(page* p) {
  stream form = p->s;
  p->s = p->outer;
  p->outer = (stream){0};
  return form;
}

/* Opens a transparency group as rmr_apple.c's BeginTransparencyLayer does. */
static void open_group(page* p, const rmr_draw* d) {
  form_begin(p);
  /* Inside the group alpha and blend mode start from their initial values;
   * the rest comes from where the group is painted, and is set again. */
  p->s.line_width = p->s.miter = NAN;
  p->s.cap = p->s.join = -1;
  p->group = d->group;
  p->group_alpha = d->group_alpha;
  p->group_blend = d->group_blend;
  p->box = CGRectNull;
  media_clip(p);
}

/* Closes the open group, writing it as a transparency group Form XObject and
 * painting it under its alpha and blend mode, as EndTransparencyLayer does
 * for a layer that reaches the media box; false on allocation failure. */
static bool close_group(page* p) {
  put(p, "Q\n");
  stream form = form_end(p);
  p->group = 0;
  bool ok = !form.content.failed;
  if (ok &&
      CGRectIntersectsRect(p->box, CGRectMake(0, 0, p->width, p->height))) {
    catalog* c = p->resources;
    uint32_t space = icc_space(p, p->writer->group_profile, &c->group_profile,
                               &c->group_space);
    uint32_t n = space ? number(p) : 0;
    rmr_pdf_object* o = n ? rmr_pdf_batch_add(p->batch, n, true) : nullptr;
    if (!o) {
      ok = false;
    } else {
      rmr_pdf_text* d = &o->dict;
      char box[4][RMR_PDF_NUMBER_SIZE];
      size_t length[4] = {rmr_pdf_number(box[0], CGRectGetMinX(p->box)),
                          rmr_pdf_number(box[1], CGRectGetMinY(p->box)),
                          rmr_pdf_number(box[2], CGRectGetMaxX(p->box)),
                          rmr_pdf_number(box[3], CGRectGetMaxY(p->box))};
      rmr_pdf_text_string(d,
                          "/Type /XObject /Subtype /Form /FormType 1 /BBox [");
      for (size_t i = 0; i < 4; i++) {
        rmr_pdf_text_append(d, box[i], length[i]);
        rmr_pdf_text_string(d, i < 3 ? " " : "]");
      }
      rmr_pdf_text_format(d,
                          " /Group << /S /Transparency /CS %u 0 R /I true "
                          "/K false >> /Resources ",
                          space);
      resource_names(d, &form, c->space);
      o->data = form.content.bytes;
      o->size = form.content.size;
      form.content = (rmr_pdf_text){0};
      /* Inside q, as CoreGraphics paints a layer: its alpha and blend mode
       * end at Q. Left set after it, CoreGraphics' own renderer takes them
       * for set no longer once the group is painted. */
      double ca = p->s.ca;
      rmr_blend blend = p->s.blend;
      put(p, "q\n");
      ok = outer_state(p, p->group_alpha, p->s.stroke_alpha, p->group_blend) &&
           list_add(&p->s.xobjects, &p->s.xobject_count, &p->s.xobject_capacity,
                    n);
      rmr_pdf_text_format(&p->s.content, "/RMRx%u Do\nQ\n", n);
      p->s.ca = ca;
      p->s.blend = blend;
    }
  }
  stream_free(&form);
  return ok;
}

/* rmr_backend begin: nothing; serialize clips an overlay itself, around the
 * template's form too. */
static rmr_status begin(void* context, rmr_rect viewport) {
  (void)context;
  (void)viewport;
  return RMR_OK;
}

static bool native_only(const rmr_draw* d);
static bool paints(const rmr_draw* d);

/* rmr_backend draw: one fill, stroke, combined fill and stroke or grain mask,
 * after its erasers, in or out of a transparency group opened and closed as
 * the draws' group changes; a draw only CoreGraphics can write is left out
 * when it paints nothing, and refused otherwise. */
static rmr_status draw(void* context, const rmr_draw* d) {
  page* p = context;
  if (d->group != p->group) {
    if (p->group && !close_group(p)) return RMR_ERR_OUT_OF_MEMORY;
    if (d->group) open_group(p, d);
  }
  if (native_only(d)) return paints(d) ? RMR_ERR_UNSUPPORTED : RMR_OK;
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
  CGPoint image = {0};
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
      uint32_t mask = mask_image(p, &d->mask);
      if (!mask) ok = false;
      put(p, "q\n");
      if (erased)
        CGPathApply(erased, &w, native_element);
      else
        plan_path(p, d->path, false);
      put(p, "W n\n/Perceptual ri\n");
      /* Inside q: color and state changes end at Q. */
      double ca = p->s.ca;
      rmr_blend blend = p->s.blend;
      ok = ok && set_color(p, opaque, false) &&
           outer_state(p, d->color.alpha, p->s.stroke_alpha, d->blend);
      p->s.ca = ca;
      p->s.blend = blend;

      CGAffineTransform t = p->ctm;
      const rmr_mask_view* m = &d->mask;
      double top = m->box.y + m->box.height;
      double x = fma(t.c, top, fma(t.a, m->box.x, t.tx));
      double y = fma(t.d, top, fma(t.b, m->box.x, t.ty));
      double a = t.a * 1, b = t.b * 1, c = t.c * -1, e = t.d * -1;
      double zero = 0;
      image = CGPointMake(x, y);
      put(p, "q ");
      exact(p, fma(a, m->box.width, c * zero));
      exact(p, fma(b, m->box.width, e * zero));
      exact(p, fma(a, zero, c * m->box.height));
      exact(p, fma(b, zero, e * m->box.height));
      exact(p, x + fma(a, zero, c * zero));
      exact(p, y + fma(b, zero, e * zero));
      rmr_pdf_text_format(&p->s.content, "cm\n/RMRx%u Do\nQ\nQ\n", mask);
    } else {
      ok = outer_color(p, opaque, false) &&
           outer_state(p, d->color.alpha, p->s.stroke_alpha, d->blend);
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
         outer_state(p, p->s.ca, d->color.alpha, d->blend);
    transform(p);
    plan_path(p, d->path, true);
    put(p, "S\nQ\n");
  }

  if (ok && p->group) {
    CGRect r;
    ok = member_box(p, d, erased, image, &r);
    if (ok) p->box = CGRectUnion(p->box, r);
  }
  if (erased) CGPathRelease(erased);
  /* The output limit is the document's, checked as pages are appended. */
  return ok && !p->s.content.failed ? RMR_OK : RMR_ERR_OUT_OF_MEMORY;
}

/* rmr_backend end: closes the open transparency group, if any. */
static rmr_status end(void* context) {
  page* p = context;
  if (p->group && !close_group(p)) return RMR_ERR_OUT_OF_MEMORY;
  return p->s.content.failed ? RMR_ERR_OUT_OF_MEMORY : RMR_OK;
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
  return d->mode == RMR_TEXT || d->mode == RMR_IMAGE || d->mode == RMR_PDF;
}

/* CGPDFScanner callback: an operator that paints was met. */
static void painting(CGPDFScannerRef scanner, void* info) {
  (void)scanner;
  *(bool*)info = true;
}

/* Whether a draw only CoreGraphics can write may paint. An embedded PDF
 * page whose content holds no painting operator (no stroke or fill, text,
 * XObject, shading or inline image) paints nothing, whatever its graphics
 * state or clip: a text layer without glyphs is one. Anything else, and any
 * page that does not scan, may. */
static bool paints(const rmr_draw* d) {
  if (d->mode != RMR_PDF || !d->image.bytes || !d->image.size ||
      d->pdf_page == SIZE_MAX)
    return true;
  CGDataProviderRef provider = CGDataProviderCreateWithData(
      nullptr, d->image.bytes, d->image.size, nullptr);
  CGPDFDocumentRef document =
      provider ? CGPDFDocumentCreateWithProvider(provider) : nullptr;
  if (provider) CGDataProviderRelease(provider);
  CGPDFPageRef page = document && !CGPDFDocumentIsEncrypted(document)
                          ? CGPDFDocumentGetPage(document, d->pdf_page + 1)
                          : nullptr;
  bool painted = true;
  CGPDFOperatorTableRef table = page ? CGPDFOperatorTableCreate() : nullptr;
  CGPDFContentStreamRef content =
      table ? CGPDFContentStreamCreateWithPage(page) : nullptr;
  if (content) {
    static const char* const operators[] = {
        "S",  "s",  "f",  "F", "f*", "B",  "B*", "b",  "b*", "sh",
        "Do", "Tj", "TJ", "'", "\"", "BI", "EI", "d0", "d1"};
    for (size_t i = 0; i < sizeof operators / sizeof *operators; i++)
      CGPDFOperatorTableSetCallback(table, operators[i], painting);
    painted = false;
    CGPDFScannerRef scanner = CGPDFScannerCreate(content, table, &painted);
    if (!scanner || !CGPDFScannerScan(scanner)) painted = true;
    if (scanner) CGPDFScannerRelease(scanner);
    CGPDFContentStreamRelease(content);
  }
  if (table) CGPDFOperatorTableRelease(table);
  if (document) CGPDFDocumentRelease(document);
  return painted;
}

size_t rmr_apple_writer_split(const rmr_plan* plan) {
  for (size_t i = rmr_plan_count(plan); i-- > 0;) {
    const rmr_draw* d = rmr_plan_draw(plan, i);
    if (native_only(d) && paints(d)) return i + 1;
  }
  return 0;
}

/* The plan's template draws, clamped to the plan. */
static size_t background(const rmr_plan* plan) {
  size_t count = rmr_plan_count(plan), leading = rmr_plan_background(plan);
  return leading < count ? leading : count;
}

size_t rmr_apple_writer_lead(const rmr_plan* plan) {
  size_t end = background(plan);
  for (size_t i = 0; i < end; i++) {
    const rmr_draw* d = rmr_plan_draw(plan, i);
    if (native_only(d) && paints(d)) return i;
  }
  return end;
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
  CGColorSpaceRef generic = CGColorSpaceCreateWithName(kCGColorSpaceGenericRGB);
  w->group_profile = generic ? CGColorSpaceCopyICCData(generic) : nullptr;
  if (generic) CGColorSpaceRelease(generic);
  rmr_status s = !w->pipeline || !w->profile || !w->group_profile
                     ? RMR_ERR_OUT_OF_MEMORY
                     : RMR_OK;
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
  if (w->group_profile) CFRelease(w->group_profile);
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

/* Executes draws first to end - 1 of plan into page state p. */
static rmr_status execute(page* p, const rmr_plan* plan, size_t first,
                          size_t end, rmr_cancel_fn cancel, void* context,
                          rmr_error* e) {
  rmr_backend b = backend(p);
  return rmr_plan_execute_range(plan, first, end, &b, cancel, context, e);
}

/* Draws 0 to end - 1 of plan, the page's template, as a Form XObject and
 * paints it: the form is written once per catalog for templates that draw
 * the same. Like the page's own, its content starts from PDF's initial
 * graphics state: it is painted where the page's is, before anything else.
 * Its box reaches a page beyond each side, so that it clips nothing a
 * renderer could show. */
static rmr_status template_form(page* p, const rmr_plan* plan, size_t end,
                                rmr_cancel_fn cancel, void* context,
                                rmr_error* e) {
  form_begin(p);
  rmr_status s = execute(p, plan, 0, end, cancel, context, e);
  stream form = form_end(p);
  if (s != RMR_OK || !form.content.size) {
    stream_free(&form);
    return s;
  }

  catalog* c = p->resources;
  rmr_pdf_text dict = {0};
  rmr_pdf_text_string(&dict,
                      "/Type /XObject /Subtype /Form /FormType 1 /BBox [");
  double box[] = {-p->width, -p->height, 2 * p->width, 2 * p->height};
  for (size_t i = 0; i < 4; i++) {
    rmr_pdf_text_number(&dict, box[i], 0);
    rmr_pdf_text_string(&dict, i < 3 ? " " : "]");
  }
  rmr_pdf_text_string(&dict, " /Resources ");
  resource_names(&dict, &form, c->space);

  rmr_sha256 h;
  rmr_sha256_init(&h);
  rmr_sha256_update(&h, dict.bytes, dict.size);
  rmr_sha256_update(&h, form.content.bytes, form.content.size);
  form_entry* entry = !dict.failed && !form.content.failed
                          ? form_lookup(c, rmr_sha256_final(&h))
                          : nullptr;
  bool ok = entry;
  if (entry && !entry->number) {
    uint32_t n = number(p);
    rmr_pdf_object* o = n ? rmr_pdf_batch_add(p->batch, n, true) : nullptr;
    if (o) {
      o->dict = dict;
      dict = (rmr_pdf_text){0};
      o->data = form.content.bytes;
      o->size = form.content.size;
      form.content = (rmr_pdf_text){0};
      entry->number = n;
      c->form_count++;
    } else {
      /* Unnumbered, the entry is free again. */
      entry->digest = (rmr_pdf_digest){0};
      ok = false;
    }
  }
  if (ok) {
    ok = list_add(&p->s.xobjects, &p->s.xobject_count, &p->s.xobject_capacity,
                  entry->number);
    rmr_pdf_text_format(&p->s.content, "/RMRx%u Do\n", entry->number);
  }
  rmr_pdf_text_free(&dict);
  stream_free(&form);
  return ok ? RMR_OK : RMR_ERR_OUT_OF_MEMORY;
}

/* Serializes draws first onwards of plan as page state p, already set up,
 * into p->batch, starting from PDF's initial graphics state: from the first,
 * its template as a form of its own, then the rest. */
static rmr_status serialize(page* p, const rmr_plan* plan, size_t first,
                            rmr_cancel_fn cancel, void* context, rmr_error* e) {
  initial(&p->s);
  if (p->clip) media_clip(p);
  size_t start = first ? first : background(plan);
  rmr_status s = RMR_OK;
  if (!first && start) s = template_form(p, plan, start, cancel, context, e);
  if (s == RMR_OK)
    s = execute(p, plan, start, rmr_plan_count(plan), cancel, context, e);
  if (s == RMR_OK && p->clip) put(p, "Q\n");
  return s == RMR_OK && p->s.content.failed ? RMR_ERR_OUT_OF_MEMORY : s;
}

static void page_free(page* p) {
  stream_free(&p->s);
  stream_free(&p->outer);
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

/* A content stream object holding text, or 0 on failure. */
static uint32_t content_object(page* p, rmr_pdf_text* text) {
  uint32_t n = rmr_pdf_builder_reserve(p->writer->builder);
  rmr_pdf_object* o = n ? rmr_pdf_batch_add(p->batch, n, true) : nullptr;
  if (!o || text->failed) return 0;
  o->data = text->bytes;
  o->size = text->size;
  *text = (rmr_pdf_text){0};
  return n;
}

rmr_status rmr_apple_writer_page(rmr_apple_writer* w, size_t index,
                                 const rmr_plan* plan, size_t lead,
                                 size_t first, const rmr_sheet* sheet,
                                 rmr_cancel_fn cancel, void* context,
                                 rmr_error* e) {
  if (!w->merged) return fail(e, RMR_ERR_INVALID_ARGUMENT, "not a document");
  /* As rmr_apple_draw_context checks. */
  if (!valid_sheet(sheet))
    return fail(e, RMR_ERR_INVALID_ARGUMENT, "invalid native output sheet");
  if (lead && lead >= first)
    return fail(e, RMR_ERR_INVALID_ARGUMENT, "invalid PDF writer range");
  rmr_transform t = sheet->transform;
  page p = {.writer = w,
            .resources = &w->shared,
            .batch = calloc(1, sizeof(rmr_pdf_batch)),
            .ctm = CGAffineTransformMake(t.a, t.b, t.c, t.d, t.tx, t.ty),
            .width = sheet->width,
            .height = sheet->height,
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

  /* The template ahead of CoreGraphics' share, in a stream of its own. */
  uint32_t before = 0;
  rmr_status s = RMR_OK;
  if (lead) {
    initial(&p.s);
    s = template_form(&p, plan, lead, cancel, context, e);
    if (s == RMR_OK && p.s.content.size &&
        !(before = content_object(&p, &p.s.content))) {
      page_free(&p);
      return fail(e, RMR_ERR_OUT_OF_MEMORY, "PDF page allocation failed");
    }
  }
  bool rest = first < rmr_plan_count(plan) || !lead;
  if (s == RMR_OK && rest) s = serialize(&p, plan, first, cancel, context, e);
  if (s != RMR_OK || (!rest && !before)) {
    /* A template painting nothing before all CoreGraphics drew. */
    page_free(&p);
    return s;
  }

  uint32_t contents = rest ? content_object(&p, &p.s.content) : 0;
  uint32_t* spaces = p.s.space ? malloc(sizeof *spaces) : nullptr;
  if ((rest && !contents) || (p.s.space && !spaces)) {
    free(spaces);
    page_free(&p);
    return fail(e, RMR_ERR_OUT_OF_MEMORY, "PDF page allocation failed");
  }
  if (spaces) spaces[0] = w->shared.space;

  w->pages[w->page_count++] =
      (rmr_pdf_page_objects){.page = index,
                             .contents = contents,
                             .xobjects = p.s.xobjects,
                             .states = p.s.states,
                             .spaces = spaces,
                             .xobject_count = p.s.xobject_count,
                             .state_count = p.s.state_count,
                             .space_count = spaces ? 1 : 0,
                             .before = before};
  p.s.xobjects = p.s.states = nullptr;
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
                                    const rmr_plan* plan, size_t first,
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
  /* The masks CoreGraphics drew of this page so far: a mask of the same
   * bytes takes the dimensions they were first seen with, as it would were
   * CoreGraphics drawing the whole page. */
  p.local.images = w->shared.images;
  p.local.image_count = w->shared.image_count;
  p.local.image_capacity = w->shared.image_capacity;
  w->shared.images = nullptr;
  w->shared.image_count = w->shared.image_capacity = 0;
  p.resources = &p.local;
  p.inverse = CGAffineTransformInvert(p.ctm);
  if (!p.batch) {
    page_free(&p);
    return fail(e, RMR_ERR_OUT_OF_MEMORY,
                "PDF output memory limit or allocation failure");
  }

  s = serialize(&p, plan, first, cancel, context, e);
  if (s != RMR_OK) {
    page_free(&p);
    return s;
  }

  /* Adding an object may move those added before it. */
  size_t at = p.batch->count;
  bool added = rmr_pdf_batch_add(p.batch, 1, false) &&
               rmr_pdf_batch_add(p.batch, 2, false) &&
               rmr_pdf_batch_add(p.batch, 3, false) &&
               rmr_pdf_batch_add(p.batch, 4, true);
  if (!added || p.s.content.failed) {
    page_free(&p);
    return fail(e, RMR_ERR_OUT_OF_MEMORY,
                "PDF output memory limit or allocation failure");
  }
  rmr_pdf_object *catalog = &p.batch->objects[at],
                 *tree = &p.batch->objects[at + 1],
                 *leaf = &p.batch->objects[at + 2],
                 *content = &p.batch->objects[at + 3];
  rmr_pdf_text_string(&catalog->dict, "<< /Type /Catalog /Pages 2 0 R >>");
  rmr_pdf_text_string(&tree->dict,
                      "<< /Type /Pages /Kids [ 3 0 R ] /Count 1 >>");
  rmr_pdf_text* d = &leaf->dict;
  rmr_pdf_text_string(d, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ");
  rmr_pdf_text_number(d, sheet->width, 0);
  rmr_pdf_text_string(d, " ");
  rmr_pdf_text_number(d, sheet->height, 0);
  rmr_pdf_text_string(d, "] /Contents 4 0 R /Resources ");
  resource_names(d, &p.s, p.local.space);
  rmr_pdf_text_string(d, " >>");
  content->data = p.s.content.bytes;
  content->size = p.s.content.size;
  p.s.content = (rmr_pdf_text){0};

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
