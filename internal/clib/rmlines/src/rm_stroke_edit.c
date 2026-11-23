/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */
#include <float.h>
#include <math.h>
#include <stdlib.h>

#include "rm_internal.h"
#include "rmlines/editor.h"
#include "rmlines/parser.h"

typedef struct line {
  rm_item_preamble item;
  rm_reader value, points, tail, item_tail;
  uint32_t tool;
  rm_crdt_id timestamp, move, source;
  double thickness;
  float start;
  bool timestamp_present, move_present, argb_present, source_present;
  uint32_t argb;
  size_t thickness_offset, stride;
} line;

static rm_status decode_line(rm_reader body, rm_block_header header, line* l) {
  RM_TRY(rm_parse_item_preamble(&body, &l->item));
  if (l->item.deleted || !rm_match_tag(&body, 6, RM_TAG_LENGTH4))
    return RM_ERR_ITEMS_NOT_FOUND;
  RM_TRY(rm_read_subblock(&body, &l->value));
  l->item_tail = body;
  rm_reader r = l->value;
  uint8_t kind;
  uint32_t color;
  RM_TRY(rm_read_u8(&r, &kind));
  if (kind != 3 || !header.current_version || header.min_version > 2)
    return RM_ERR_UNVERIFIED;
  RM_TRY(rm_expect_tag(&r, 1, RM_TAG_BYTE4));
  RM_TRY(rm_read_u32(&r, &l->tool));
  RM_TRY(rm_expect_tag(&r, 2, RM_TAG_BYTE4));
  RM_TRY(rm_read_u32(&r, &color));
  RM_TRY(rm_expect_tag(&r, 3, RM_TAG_BYTE8));
  l->thickness_offset = r.offset;
  RM_TRY(rm_read_f64(&r, &l->thickness));
  RM_TRY(rm_expect_tag(&r, 4, RM_TAG_BYTE4));
  RM_TRY(rm_read_f32(&r, &l->start));
  RM_TRY(rm_expect_tag(&r, 5, RM_TAG_LENGTH4));
  RM_TRY(rm_read_subblock(&r, &l->points));
  l->stride = header.current_version == 1 ? 24 : 14;
  if (rm_reader_remaining(&l->points) % l->stride) return RM_ERR_MALFORMED_PAGE;
  if ((l->timestamp_present = rm_match_tag(&r, 6, RM_TAG_ID)))
    RM_TRY(rm_read_bare_crdt(&r, &l->timestamp));
  if ((l->move_present = rm_match_tag(&r, 7, RM_TAG_ID)))
    RM_TRY(rm_read_bare_crdt(&r, &l->move));
  if ((l->argb_present = rm_match_tag(&r, 8, RM_TAG_BYTE4)))
    RM_TRY(rm_read_u32(&r, &l->argb));
  if ((l->source_present = rm_match_tag(&r, 9, RM_TAG_ID)))
    RM_TRY(rm_read_bare_crdt(&r, &l->source));
  l->tail = r;
  return RM_OK;
}

static rm_status transform_points(const line* l, rm_reader r,
                                  rm_edit_transform m, double scale,
                                  rm_buf* out) {
  while (rm_reader_remaining(&r)) {
    float x, y;
    RM_TRY(rm_read_f32(&r, &x));
    RM_TRY(rm_read_f32(&r, &y));
    double xx = m.a * x + m.c * y + m.tx, yy = m.b * x + m.d * y + m.ty;
    if (!isfinite(xx) || fabs(xx) > FLT_MAX || !isfinite(yy) ||
        fabs(yy) > FLT_MAX)
      return RM_ERR_INVALID_ARGUMENT;
    rm_write_f32(out, (float)xx);
    rm_write_f32(out, (float)yy);
    size_t before = l->stride == 24 ? 8 : 2;
    const uint8_t* bytes;
    RM_TRY(rm_read_bytes(&r, before, &bytes));
    rm_buf_append(out, bytes, before);
    if (l->stride == 24) {
      float width;
      RM_TRY(rm_read_f32(&r, &width));
      if (!isfinite(width * scale) || fabs(width * scale) > FLT_MAX)
        return RM_ERR_INVALID_ARGUMENT;
      rm_write_f32(out, (float)(width * scale));
    } else {
      uint16_t width;
      RM_TRY(rm_read_u16(&r, &width));
      double w = round(width * scale);
      if (w > UINT16_MAX) return RM_ERR_INVALID_ARGUMENT;
      rm_write_u16(out, (uint16_t)w);
    }
    size_t remaining = l->stride == 24 ? 4 : 2;
    RM_TRY(rm_read_bytes(&r, remaining, &bytes));
    rm_buf_append(out, bytes, remaining);
  }
  return out->failed ? RM_ERR_OUT_OF_MEMORY : RM_OK;
}

static rm_status piece(rm_buf* file, rm_block_header h, const line* l,
                       rm_crdt_id id, rm_crdt_id left, size_t from, size_t to,
                       const rm_edit_transform* transform) {
  rm_reader points = l->points;
  points.offset += from * l->stride;
  points.size = l->points.offset + to * l->stride;
  double start = l->start, scale = 1;
  if (!transform && from) {
    rm_reader r = l->points;
    float x0 = 0, y0 = 0;
    for (size_t i = 0; i <= from; ++i) {
      float x, y;
      RM_TRY(rm_read_f32(&r, &x));
      RM_TRY(rm_read_f32(&r, &y));
      if (i) start += hypot((double)x - x0, (double)y - y0);
      x0 = x;
      y0 = y;
      r.offset += l->stride - 8;
    }
  }
  if (transform)
    scale =
        sqrt(fabs(transform->a * transform->d - transform->b * transform->c));
  if (!isfinite(start) || fabs(start) > FLT_MAX || !isfinite(scale) ||
      !isfinite(l->thickness * scale))
    return RM_ERR_INVALID_ARGUMENT;
  rm_buf samples = {0}, value = {0}, body = {0};
  rm_status status = RM_OK;
  if (transform)
    status = transform_points(l, points, *transform, scale, &samples);
  else
    rm_buf_append(&samples, points.data + points.offset,
                  rm_reader_remaining(&points));
  if (status == RM_OK) {
    rm_buf_append(&value, l->value.data + l->value.offset,
                  l->thickness_offset - l->value.offset);
    rm_write_f64(&value, l->thickness * scale);
    rm_write_tag(&value, 4, RM_TAG_BYTE4);
    rm_write_f32(&value, (float)start);
    rm_write_block(&value, 5, samples.bytes, samples.count);
    if (l->timestamp_present) rm_write_id(&value, 6, l->timestamp);
    if (transform)
      rm_write_id(&value, 7,
                  rm_crdt_equal(l->move, RM_CRDT_NONE) ? l->item.id : l->move);
    else if (l->move_present)
      rm_write_id(&value, 7, l->move);
    if (l->argb_present) {
      rm_write_tag(&value, 8, RM_TAG_BYTE4);
      rm_write_u32(&value, l->argb);
    }
    if (l->source_present || (!transform && l->tool == RM_TOOL_SHADER))
      rm_write_id(&value, 9, l->source_present ? l->source : l->item.id);
    rm_buf_append(&value, l->tail.data + l->tail.offset,
                  rm_reader_remaining(&l->tail));
    rm_write_id(&body, 1, l->item.parent);
    rm_write_id(&body, 2, id);
    rm_write_id(&body, 3, left);
    rm_write_id(&body, 4, l->item.right);
    rm_write_tag(&body, 5, RM_TAG_BYTE4);
    rm_write_u32(&body, 0);
    rm_write_block(&body, 6, value.bytes, value.count);
    rm_buf_append(&body, l->item_tail.data + l->item_tail.offset,
                  rm_reader_remaining(&l->item_tail));
    rm_write_top_block(file, 5, h.min_version, h.current_version, body.bytes,
                       body.count);
    if (samples.failed || value.failed || body.failed || file->failed)
      status = RM_ERR_OUT_OF_MEMORY;
  }
  rm_buf_free(&samples);
  rm_buf_free(&value);
  rm_buf_free(&body);
  return status;
}

static rm_status rework(rm_edit_session* s, rm_item_id id, rm_item_id fresh,
                        size_t first, size_t count,
                        const rm_edit_transform* transform, rm_error* error) {
  if (!s) return RM_ERR_INVALID_ARGUMENT;
  size_t size;
  const uint8_t* data = rm_edit_bytes(s, &size);
  rm_reader r = {.data = data, .size = size, .offset = RM_HEADER_LENGTH};
  rm_block_header target = {0};
  size_t deleted_at = 0;
  line original = {0};
  while (rm_reader_remaining(&r) >= 8) {
    rm_block_header h;
    RM_TRY(rm_read_block_header(&r, &h));
    rm_reader body = r;
    body.size = h.end;
    if (h.type == 5) {
      rm_item_preamble item;
      RM_TRY(rm_parse_item_preamble(&body, &item));
      if (rm_crdt_equal(item.id, rm_crdt_of(id))) {
        if (target.end) return RM_ERR_UNVERIFIED;
        deleted_at = body.offset - 4;
        body.offset = h.body;
        RM_TRY(decode_line(body, h, &original));
        target = h;
      }
    }
    r.offset = h.end;
  }
  if (!target.end) return RM_ERR_ITEMS_NOT_FOUND;
  /* Anchor every surviving piece between the original tombstone and the
   * next scene item, including erasers and non-stroke children. */
  RM_TRY(rm_edit_check_location(data, size, rm_item_id_of(original.item.parent),
                                id, (rm_item_id){0}, &original.item.right,
                                error));
  size_t points = rm_reader_remaining(&original.points) / original.stride;
  if (!transform && (first > points || count > points - first))
    return RM_ERR_INVALID_ARGUMENT;
  if (!transform && !count) return RM_OK;
  size_t needed =
      transform ? 1 : (size_t)(first != 0) + (first + count < points);
  RM_TRY(rm_edit_check_ids(data, size, fresh, needed));
  rm_buf out = {0};
  rm_buf_append(&out, data, target.start);
  rm_write_u32(&out, (uint32_t)(deleted_at - target.body + 4));
  rm_buf_append(&out, data + target.start + 4, deleted_at - target.start - 4);
  rm_write_u32(&out, 1);
  rm_buf_append(&out, data + target.end, r.offset - target.end);
  rm_status status = RM_OK;
  rm_crdt_id next = rm_crdt_of(fresh), left = original.item.id;
  if (transform)
    status = piece(&out, target, &original, next, left, 0, points, transform);
  else {
    if (first) {
      status = piece(&out, target, &original, next, left, 0, first, nullptr);
      left = next;
      ++next.counter;
    }
    if (status == RM_OK && first + count < points)
      status = piece(&out, target, &original, next, left, first + count, points,
                     nullptr);
  }
  rm_buf_append(&out, data + r.offset, size - r.offset);
  if (out.failed) status = RM_ERR_OUT_OF_MEMORY;
  if (status == RM_OK) {
    uint8_t* bytes = rm_buf_take(&out, &size);
    status =
        bytes ? rm_edit_commit(s, bytes, size, error) : RM_ERR_OUT_OF_MEMORY;
  }
  rm_buf_free(&out);
  return status;
}

rm_status rm_edit_transform_stroke(rm_edit_session* s, rm_item_id stroke,
                                   rm_item_id fresh,
                                   rm_edit_transform transform,
                                   rm_error* error) {
  if (error) *error = (rm_error){0};
  rm_status status = rework(s, stroke, fresh, 0, 0, &transform, error);
  return rm_edit_result(error, status);
}

rm_status rm_edit_cut_stroke(rm_edit_session* s, rm_item_id stroke,
                             rm_item_id fresh, size_t first, size_t count,
                             rm_error* error) {
  if (error) *error = (rm_error){0};
  rm_status status = rework(s, stroke, fresh, first, count, nullptr, error);
  return rm_edit_result(error, status);
}

/* Resolve every requested item on the immutable starting revision. */
static rm_status check_selection(const uint8_t* data, size_t size,
                                 const rm_item_id* ids, size_t count) {
  rm_idmap wanted;
  rm_idmap_init(&wanted);
  rm_status status = RM_OK;
  for (size_t i = 0; i < count; i++) {
    rm_crdt_id id = rm_crdt_of(ids[i]);
    if (rm_idmap_contains(&wanted, id)) {
      status = RM_ERR_ITEMS_NOT_FOUND;
      goto done;
    }
    if (!rm_idmap_set(&wanted, id, 1)) {
      status = RM_ERR_OUT_OF_MEMORY;
      goto done;
    }
  }
  size_t found = 0;
  rm_reader r = {.data = data, .size = size, .offset = RM_HEADER_LENGTH};
  while (rm_reader_remaining(&r) >= 8) {
    rm_block_header h;
    status = rm_read_block_header(&r, &h);
    if (status != RM_OK) goto done;
    if (h.type == 5) {
      rm_reader body = r;
      body.size = h.end;
      rm_item_preamble it;
      status = rm_parse_item_preamble(&body, &it);
      if (status != RM_OK) goto done;
      if (!it.deleted && rm_match_tag(&body, 6, RM_TAG_LENGTH4) &&
          rm_idmap_take(&wanted, it.id, nullptr))
        ++found;
    }
    r.offset = h.end;
  }
  if (found != count) status = RM_ERR_ITEMS_NOT_FOUND;
done:
  rm_idmap_free(&wanted);
  return status;
}
rm_status rm_edit_transform_strokes(rm_edit_session* session,
                                    const rm_item_id* ids, size_t count,
                                    rm_item_id fresh,
                                    rm_edit_transform transform,
                                    rm_error* error) {
  rm_status status = RM_OK;
  if (error) *error = (rm_error){0};
  if (!session || (!ids && count) ||
      (count && count - 1 > UINT64_MAX - fresh.counter))
    status = RM_ERR_INVALID_ARGUMENT;
  if (status == RM_OK && count == 1)
    return rm_edit_transform_stroke(session, ids[0], fresh, transform, error);
  if (status == RM_OK && count) {
    size_t size;
    const uint8_t* bytes = rm_edit_bytes(session, &size);
    rm_edit_session* draft = nullptr;
    status = check_selection(bytes, size, ids, count);
    if (status == RM_OK) status = rm_edit_check_ids(bytes, size, fresh, count);
    if (status == RM_OK) status = rm_edit_open(bytes, size, &draft, error);
    for (size_t i = 0; status == RM_OK && i < count; ++i, ++fresh.counter) {
      status = rework(draft, ids[i], fresh, 0, 0, &transform, error);
      if (status == RM_OK) rm_edit_forget_previous(draft);
    }
    if (status == RM_OK) {
      bytes = rm_edit_bytes(draft, &size);
      uint8_t* copy = malloc(size);
      if (copy) {
        memcpy(copy, bytes, size);
        status = rm_edit_commit(session, copy, size, error);
      } else
        status = RM_ERR_OUT_OF_MEMORY;
    }
    rm_edit_close(draft);
  }
  return rm_edit_result(error, status);
}
