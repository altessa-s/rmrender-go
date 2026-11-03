/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <math.h>
#include <rmrender/rmlines.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rmr_placed_scene {
  rm_stroke* strokes;
  rm_stroke_point* points;
  rm_page_image* images;
  char* files;
  size_t stroke_count, image_count, bytes;
};

typedef struct {
  rm_text_anchor anchor;
  rmr_point pen;
  bool occupied;
} entry;

typedef struct {
  entry* cache;
  size_t capacity, work;
  rmr_limits limits;
  const rmr_anchor_provider* provider;
} placement;

/* Fill *e (if given) with status s, item index and message, and return s. */
static rmr_status fail(rmr_error* e, rmr_status s, size_t index,
                       const char* message) {
  if (e) {
    *e = (rmr_error){.code = s, .index = index};
    snprintf(e->message, sizeof e->message, "%s", message);
  }
  return s;
}

/* True if x and y are finite and within [-maximum, maximum]. */
static bool point_valid(double x, double y, double maximum) {
  return isfinite(x) && isfinite(y) && fabs(x) <= maximum && fabs(y) <= maximum;
}

/* Add count*size to *bytes if it stays within maximum; false if it would
 * overflow or exceed the limit, leaving *bytes unchanged. */
static bool reserve(size_t* bytes, size_t count, size_t size, size_t maximum) {
  if (*bytes > maximum || count > (maximum - *bytes) / size) return false;
  *bytes += count * size;
  return true;
}

/* FNV-1a hash of anchor's identifying fields, for the probe cache below. */
static uint64_t hash(rm_text_anchor anchor) {
  /* Hash values, not padding; +/-zero and absent origin_x compare equally. */
  uint64_t bits = 0;
  if (anchor.has_origin_x && anchor.origin_x != 0)
    memcpy(&bits, &anchor.origin_x, sizeof bits);

  uint64_t h = UINT64_C(14695981039346656037);
  uint64_t values[] = {(uint32_t)anchor.paragraph, (uint32_t)anchor.offset,
                       anchor.has_origin_x, bits};
  for (size_t i = 0; i < 4; i++)
    for (unsigned j = 0; j < 8; j++) {
      h ^= (values[i] >> (j * 8)) & 255;
      h *= UINT64_C(1099511628211);
    }

  return h;
}

/* True if a and b select the same anchor position. */
static bool equal(rm_text_anchor a, rm_text_anchor b) {
  return a.paragraph == b.paragraph && a.offset == b.offset &&
         a.has_origin_x == b.has_origin_x &&
         (!a.has_origin_x || a.origin_x == b.origin_x);
}

/* Resolve anchor to a pen position via the provider, caching by open-
 * addressed probing so repeated anchors resolve only once per call. */
static rmr_status pen(placement* p, rm_text_anchor anchor, size_t index,
                      rmr_point* out, rmr_error* e) {
  if (!p->provider)
    return fail(e, RMR_ERR_UNSUPPORTED, index, "anchor provider required");
  if (anchor.has_origin_x &&
      (!isfinite(anchor.origin_x) ||
       fabs(anchor.origin_x) > p->limits.maximum_coordinate))
    return fail(e, RMR_ERR_INVALID_ARGUMENT, index, "invalid anchor origin");

  size_t slot = (size_t)(hash(anchor) & (p->capacity - 1));
  for (;;) {
    if (p->work == p->limits.maximum_elements)
      return fail(e, RMR_ERR_LIMIT, index, "anchor cache work limit");
    p->work++;
    entry* v = &p->cache[slot];
    if (!v->occupied) {
      rmr_point position = {NAN, NAN};
      rmr_status s =
          p->provider->resolve(p->provider->context, &anchor, &position);
      if (s != RMR_OK) return fail(e, s, index, "anchor resolution failed");
      if (!point_valid(position.x, position.y, p->limits.maximum_coordinate))
        return fail(e, RMR_ERR_BACKEND, index, "invalid anchor position");
      *v = (entry){anchor, position, true};
      *out = position;
      return RMR_OK;
    }
    if (equal(v->anchor, anchor)) {
      *out = v->pen;
      return RMR_OK;
    }
    slot = (slot + 1) & (p->capacity - 1);
  }
}

/* Free a placed scene and its owned storage; s may be null. */
void rmr_placed_scene_free(rmr_placed_scene* s) {
  if (!s) return;
  free(s->strokes);
  free(s->points);
  free(s->images);
  free(s->files);
  free(s);
}

rmr_status rmr_lines_place_scene(const rm_stroke* strokes, size_t stroke_count,
                                 const rm_page_image* images,
                                 size_t image_count,
                                 const rmr_anchor_provider* provider,
                                 const rmr_limits* given,
                                 rmr_placed_scene** out, rmr_error* error) {
  rmr_limits l = given ? *given : rmr_limits_default();
  if (!out || (stroke_count && !strokes) || (image_count && !images) ||
      !isfinite(l.maximum_coordinate) || l.maximum_coordinate <= 0 ||
      l.maximum_coordinate > 1e12 ||
      (provider &&
       (provider->abi_version != RMR_ANCHOR_PROVIDER_ABI ||
        provider->struct_size < sizeof *provider || !provider->resolve)))
    return fail(error, RMR_ERR_INVALID_ARGUMENT, 0,
                "invalid placement input/provider");

  if (stroke_count > l.maximum_draws ||
      image_count > l.maximum_draws - stroke_count)
    return fail(error, RMR_ERR_LIMIT, 0, "placement item limit");

  size_t points = 0, files = 0, anchors = 0;
  for (size_t i = 0; i < stroke_count; i++) {
    if (strokes[i].point_count && !strokes[i].points)
      return fail(error, RMR_ERR_INVALID_ARGUMENT, i, "missing stroke points");
    if (strokes[i].point_count > l.maximum_points - points)
      return fail(error, RMR_ERR_LIMIT, i, "placement point limit");
    points += strokes[i].point_count;
    anchors += strokes[i].has_anchor;
  }

  if (image_count > (l.maximum_points - points) / 4)
    return fail(error, RMR_ERR_LIMIT, stroke_count, "placement vertex limit");

  for (size_t i = 0; i < image_count; i++) {
    if (images[i].file.length && !images[i].file.bytes)
      return fail(error, RMR_ERR_INVALID_ARGUMENT, stroke_count + i,
                  "missing image filename");
    if (images[i].file.length > l.maximum_plan_bytes - files)
      return fail(error, RMR_ERR_LIMIT, stroke_count + i,
                  "filename byte limit");
    files += images[i].file.length;
    anchors += images[i].has_anchor;
  }

  if (anchors && !provider)
    return fail(error, RMR_ERR_UNSUPPORTED, 0, "anchor provider required");

  size_t bytes = sizeof(rmr_placed_scene);
  if (!reserve(&bytes, stroke_count, sizeof(rm_stroke), l.maximum_plan_bytes) ||
      !reserve(&bytes, points, sizeof(rm_stroke_point), l.maximum_plan_bytes) ||
      !reserve(&bytes, image_count, sizeof(rm_page_image),
               l.maximum_plan_bytes) ||
      !reserve(&bytes, files, 1, l.maximum_plan_bytes))
    return fail(error, RMR_ERR_LIMIT, 0, "placement memory limit");

  size_t retained = bytes, capacity = 0;
  if (anchors) {
    if (anchors > SIZE_MAX / 2)
      return fail(error, RMR_ERR_LIMIT, 0, "anchor count overflow");
    capacity = 2;
    while (capacity < anchors * 2) {
      if (capacity > SIZE_MAX / 2)
        return fail(error, RMR_ERR_LIMIT, 0, "cache size overflow");
      capacity *= 2;
    }
    if (!reserve(&bytes, capacity, sizeof(entry), l.maximum_plan_bytes))
      return fail(error, RMR_ERR_LIMIT, 0, "anchor cache memory limit");
  }

  rmr_placed_scene* s = calloc(1, sizeof *s);
  if (!s)
    return fail(error, RMR_ERR_OUT_OF_MEMORY, 0, "placement allocation failed");

  s->strokes =
      stroke_count ? calloc(stroke_count, sizeof *s->strokes) : nullptr;
  s->points = points ? calloc(points, sizeof *s->points) : nullptr;
  s->images = image_count ? calloc(image_count, sizeof *s->images) : nullptr;
  s->files = files ? malloc(files) : nullptr;
  placement p = {.cache = capacity ? calloc(capacity, sizeof(entry)) : nullptr,
                 .capacity = capacity,
                 .limits = l,
                 .provider = provider};
  if ((stroke_count && !s->strokes) || (points && !s->points) ||
      (image_count && !s->images) || (files && !s->files) ||
      (capacity && !p.cache)) {
    free(p.cache);
    rmr_placed_scene_free(s);
    return fail(error, RMR_ERR_OUT_OF_MEMORY, 0, "placement allocation failed");
  }

  rmr_status status = RMR_OK;
  size_t at = 0, file_at = 0;
  for (size_t i = 0; i < stroke_count; i++) {
    const rm_stroke* source = &strokes[i];
    rm_stroke* dest = &s->strokes[i];
    *dest = *source;
    rmr_point position = {0};
    if (source->has_anchor) {
      status = pen(&p, source->anchor, i, &position, error);
      if (status != RMR_OK) goto done;
      dest->has_anchor = false;
      dest->anchor = (rm_text_anchor){0};
    }
    dest->points = source->point_count ? s->points + at : nullptr;
    float dx = (float)position.x, dy = (float)position.y;
    for (size_t j = 0; j < source->point_count; j++) {
      rm_stroke_point point = source->points[j];
      if (!point_valid(point.x, point.y, l.maximum_coordinate)) {
        status =
            fail(error, RMR_ERR_INVALID_ARGUMENT, i, "invalid source point");
        goto done;
      }
      if (source->has_anchor) {
        point.x += dx;
        point.y += dy;
      }
      if (!point_valid(point.x, point.y, l.maximum_coordinate)) {
        status = fail(error, RMR_ERR_LIMIT, i, "placed point out of range");
        goto done;
      }
      s->points[at++] = point;
    }
  }

  for (size_t i = 0; i < image_count; i++) {
    const rm_page_image* source = &images[i];
    rm_page_image* dest = &s->images[i];
    *dest = *source;
    dest->file.bytes = source->file.length ? s->files + file_at : nullptr;
    if (source->file.length)
      memcpy(s->files + file_at, source->file.bytes, source->file.length);
    file_at += source->file.length;
    rmr_point position = {0};
    if (source->has_anchor) {
      status = pen(&p, source->anchor, stroke_count + i, &position, error);
      if (status != RMR_OK) goto done;
      dest->has_anchor = false;
      dest->anchor = (rm_text_anchor){0};
    }
    for (size_t j = 0; j < 4; j++) {
      rm_image_vertex* v = &dest->vertices[j];
      if (!point_valid(v->x, v->y, l.maximum_coordinate) || !isfinite(v->u) ||
          !isfinite(v->v)) {
        status = fail(error, RMR_ERR_INVALID_ARGUMENT, stroke_count + i,
                      "invalid source vertex");
        goto done;
      }
      if (source->has_anchor) {
        v->x += position.x;
        v->y += position.y;
      }
      if (!point_valid(v->x, v->y, l.maximum_coordinate)) {
        status = fail(error, RMR_ERR_LIMIT, stroke_count + i,
                      "placed vertex out of range");
        goto done;
      }
    }
  }

  s->stroke_count = stroke_count;
  s->image_count = image_count;
  s->bytes = retained;
  *out = s;
  s = nullptr;

done:
  free(p.cache);
  rmr_placed_scene_free(s);
  return status;
}

/* Retained byte size of the placed scene, or 0 if s is null. */
size_t rmr_placed_scene_bytes(const rmr_placed_scene* s) {
  return s ? s->bytes : 0;
}

/* Number of strokes in the placed scene, or 0 if s is null. */
size_t rmr_placed_scene_stroke_count(const rmr_placed_scene* s) {
  return s ? s->stroke_count : 0;
}

/* Borrowed stroke array of the placed scene, or null if s is null. */
const rm_stroke* rmr_placed_scene_strokes(const rmr_placed_scene* s) {
  return s ? s->strokes : nullptr;
}

/* Number of images in the placed scene, or 0 if s is null. */
size_t rmr_placed_scene_image_count(const rmr_placed_scene* s) {
  return s ? s->image_count : 0;
}

/* Borrowed image array of the placed scene, or null if s is null. */
const rm_page_image* rmr_placed_scene_images(const rmr_placed_scene* s) {
  return s ? s->images : nullptr;
}
