/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rm_internal.h"
#include "rmlines/rmlines.h"

typedef struct revision {
  uint8_t* bytes;
  size_t size;
  struct revision *before, *after;
} revision;

struct rm_edit_session {
  revision* current;
};

static rm_status report(rm_error* error, rm_status status) {
  if (error) *error = (rm_error){.status = status};
  return status;
}

static void discard(revision* r) {
  while (r) {
    revision* next = r->after;
    free(r->bytes);
    free(r);
    r = next;
  }
}

/* Takes ownership, including on failure. */
rm_status rm_edit_commit(rm_edit_session* s, uint8_t* bytes, size_t size,
                         rm_error* error) {
  revision* prior = s->current;
  if (prior && prior->size == size && !memcmp(prior->bytes, bytes, size)) {
    free(bytes);
    return report(error, RM_OK);
  }
  if (prior) {
    rm_page* check = nullptr;
    rm_status status = rm_parse_page(bytes, size, &check, error);
    if (status != RM_OK) {
      free(bytes);
      return status;
    }
    size_t skipped = check->skipped_blocks;
    rm_page_free(check);
    if (skipped) {
      free(bytes);
      if (error)
        *error = (rm_error){.status = RM_ERR_INCOMPLETE_PAGE,
                            .value = (long)skipped};
      return RM_ERR_INCOMPLETE_PAGE;
    }
  }
  revision* r = malloc(sizeof *r);
  if (!r) {
    free(bytes);
    return report(error, RM_ERR_OUT_OF_MEMORY);
  }
  *r = (revision){.bytes = bytes, .size = size, .before = prior};
  if (prior) {
    discard(prior->after);
    prior->after = r;
  }
  s->current = r;
  return report(error, RM_OK);
}

rm_status rm_edit_open(const uint8_t* data, size_t size, rm_edit_session** out,
                       rm_error* error) {
  if (!data || !out) return report(error, RM_ERR_INVALID_ARGUMENT);
  *out = nullptr;
  rm_page* page;
  RM_TRY(rm_parse_page(data, size, &page, error));
  size_t skipped = page->skipped_blocks;
  rm_page_free(page);
  if (skipped) {
    if (error)
      *error =
          (rm_error){.status = RM_ERR_INCOMPLETE_PAGE, .value = (long)skipped};
    return RM_ERR_INCOMPLETE_PAGE;
  }
  rm_edit_session* s = calloc(1, sizeof *s);
  uint8_t* copy = malloc(size);
  if (!s || !copy) {
    free(s);
    free(copy);
    return report(error, RM_ERR_OUT_OF_MEMORY);
  }
  memcpy(copy, data, size);
  rm_status status = rm_edit_commit(s, copy, size, error);
  if (status != RM_OK)
    free(s);
  else
    *out = s;
  return status;
}

void rm_edit_close(rm_edit_session* s) {
  if (!s) return;
  revision* first = s->current;
  while (first->before) first = first->before;
  discard(first);
  free(s);
}

const uint8_t* rm_edit_bytes(const rm_edit_session* s, size_t* size) {
  if (size) *size = s ? s->current->size : 0;
  return s ? s->current->bytes : nullptr;
}

bool rm_edit_undo(rm_edit_session* s) {
  if (!s || !s->current->before) return false;
  s->current = s->current->before;
  return true;
}

bool rm_edit_redo(rm_edit_session* s) {
  if (!s || !s->current->after) return false;
  s->current = s->current->after;
  return true;
}

rm_status rm_edit_erase(rm_edit_session* s, const rm_item_id* ids, size_t count,
                        rm_error* error) {
  if (!s) return report(error, RM_ERR_INVALID_ARGUMENT);
  if (!count) return report(error, RM_OK);
  uint8_t* bytes;
  size_t size;
  RM_TRY(rm_erase_items(s->current->bytes, s->current->size, ids, count, &bytes,
                        &size, error));
  return rm_edit_commit(s, bytes, size, error);
}

static rm_status node_field(rm_reader r, rm_item_id node, rm_item_id stamp,
                            uint32_t field, size_t* begin, size_t* end,
                            rm_crdt_id* previous) {
  rm_crdt_id id;
  RM_TRY(rm_read_crdt(&r, 1, &id));
  if (!rm_crdt_equal(id, rm_crdt_of(node))) return RM_ERR_ITEMS_NOT_FOUND;
  /* A node stores its numbered LWW fields in ascending order. Keep the
   * suffix opaque, including anchor records and future extensions. */
  bool found = false;
  while (rm_reader_remaining(&r)) {
    size_t start = r.offset;
    uint32_t index;
    uint8_t type;
    RM_TRY(rm_read_tag(&r, &index, &type));
    if (index > field) {
      if (!found) *begin = *end = start;
      return RM_OK;
    }
    if (type != RM_TAG_LENGTH4) return RM_ERR_MALFORMED_PAGE;
    rm_reader value;
    RM_TRY(rm_read_subblock(&r, &value));
    if (index == field) {
      rm_crdt_id candidate;
      RM_TRY(rm_read_crdt(&value, 1, &candidate));
      if (!rm_stamp_newer(rm_crdt_of(stamp), candidate))
        return RM_ERR_INVALID_ARGUMENT;
      if (!found || !rm_stamp_newer(*previous, candidate)) {
        found = true;
        *previous = candidate;
        *begin = start;
        *end = r.offset;
      }
    }
  }
  if (!found) *begin = *end = r.offset;
  return RM_OK;
}

static rm_status set_node(rm_edit_session* s, rm_item_id node, rm_item_id stamp,
                          uint32_t field, const rm_buf* replacement,
                          rm_error* error) {
  if (!s || !stamp.author || !stamp.counter)
    return report(error, RM_ERR_INVALID_ARGUMENT);
  if (replacement->failed) return report(error, RM_ERR_OUT_OF_MEMORY);
  revision* current = s->current;
  rm_reader r = {.data = current->bytes,
                 .size = current->size,
                 .offset = RM_HEADER_LENGTH};
  rm_block_header target = {0};
  size_t begin = 0, end = 0;
  rm_crdt_id winning = RM_CRDT_NONE;
  while (rm_reader_remaining(&r) >= 8) {
    rm_block_header b;
    rm_status status = rm_read_block_header(&r, &b);
    if (status != RM_OK) return report(error, status);
    if (b.type == 2) {
      rm_reader body = r;
      body.size = b.end;
      size_t start, stop;
      rm_crdt_id previous = RM_CRDT_NONE;
      status = node_field(body, node, stamp, field, &start, &stop, &previous);
      if (status == RM_OK) {
        if (!target.end || !rm_stamp_newer(winning, previous)) {
          winning = previous;
          target = b;
          begin = start;
          end = stop;
        }
      } else if (status != RM_ERR_ITEMS_NOT_FOUND)
        return report(error, status);
    }
    r.offset = b.end;
  }
  if (!target.end) return report(error, RM_ERR_ITEMS_NOT_FOUND);
  RM_TRY(rm_edit_check_location(r.data, r.size, node, (rm_item_id){0},
                                (rm_item_id){0}, nullptr, error));
  size_t kept = target.end - target.body - (end - begin);
  if (replacement->count > UINT32_MAX - kept)
    return report(error, RM_ERR_TOO_MANY_TO_NUMBER);
  rm_buf output = {0};
  rm_buf_append(&output, r.data, target.start);
  rm_write_u32(&output, (uint32_t)(kept + replacement->count));
  rm_buf_append(&output, r.data + target.start + 4, begin - target.start - 4);
  rm_buf_append(&output, replacement->bytes, replacement->count);
  rm_buf_append(&output, r.data + end, r.size - end);
  size_t size;
  uint8_t* bytes = rm_buf_take(&output, &size);
  if (!bytes) return report(error, RM_ERR_OUT_OF_MEMORY);
  return rm_edit_commit(s, bytes, size, error);
}

rm_status rm_edit_set_node_name(rm_edit_session* s, rm_item_id node,
                                rm_item_id stamp, const char* name,
                                rm_error* error) {
  if (!name) return report(error, RM_ERR_INVALID_ARGUMENT);
  rm_buf string = {0}, replacement = {0};
  rm_write_string(&string, name);
  rm_write_stamped_block(&replacement, 2, rm_crdt_of(stamp), string.bytes,
                         string.count);
  if (string.failed) replacement.failed = true;
  rm_buf_free(&string);
  rm_status status = set_node(s, node, stamp, 2, &replacement, error);
  rm_buf_free(&replacement);
  return status;
}

rm_status rm_edit_set_node_visible(rm_edit_session* s, rm_item_id node,
                                   rm_item_id stamp, bool visible,
                                   rm_error* error) {
  rm_buf replacement = {0};
  rm_write_stamped_u8(&replacement, 3, rm_crdt_of(stamp), visible);
  rm_status status = set_node(s, node, stamp, 3, &replacement, error);
  rm_buf_free(&replacement);
  return status;
}

static rm_status append_blocks(rm_edit_session* s, size_t offset,
                               const rm_buf* blocks, rm_error* error) {
  if (blocks->failed) return report(error, RM_ERR_OUT_OF_MEMORY);
  revision* base = s->current;
  rm_buf out = {0};
  rm_buf_append(&out, base->bytes, offset);
  rm_buf_append(&out, blocks->bytes, blocks->count);
  rm_buf_append(&out, base->bytes + offset, base->size - offset);
  size_t size;
  uint8_t* bytes = rm_buf_take(&out, &size);
  if (!bytes) return report(error, RM_ERR_OUT_OF_MEMORY);
  return rm_edit_commit(s, bytes, size, error);
}

static bool sequence_type(uint8_t t) {
  return (t >= 3 && t <= 6) || t == 8 || t == 15;
}

rm_status rm_edit_check_ids(const uint8_t* data, size_t size, rm_item_id first,
                            size_t count) {
  if (!count) return RM_OK;
  if (!first.author || !first.counter || count - 1 > UINT64_MAX - first.counter)
    return RM_ERR_INVALID_ARGUMENT;
  rm_reader r = {.data = data, .size = size, .offset = RM_HEADER_LENGTH};
  while (rm_reader_remaining(&r) >= 8) {
    rm_block_header h;
    RM_TRY(rm_read_block_header(&r, &h));
    rm_reader body = r;
    body.size = h.end;
    r.offset = h.end;
    if (sequence_type(h.type)) {
      rm_item_preamble item;
      RM_TRY(rm_parse_item_preamble(&body, &item));
      if (rm_id_overlaps(item.id, item.deleted ? item.deleted : 1, first,
                         count))
        return RM_ERR_INVALID_ARGUMENT;
    } else if (h.type == 1 || h.type == 2 || h.type == 13) {
      if (h.type != 13) {
        rm_crdt_id id;
        RM_TRY(rm_read_crdt(&body, 1, &id));
        if (rm_id_overlaps(id, 1, first, count)) return RM_ERR_INVALID_ARGUMENT;
      }
      if (h.type == 1) continue;
      while (rm_reader_remaining(&body)) {
        uint32_t tag;
        uint8_t kind;
        rm_reader field;
        rm_crdt_id stamp;
        if (rm_read_tag(&body, &tag, &kind) != RM_OK ||
            kind != RM_TAG_LENGTH4 || rm_read_subblock(&body, &field) != RM_OK)
          break;
        if (rm_read_crdt(&field, 1, &stamp) == RM_OK &&
            rm_id_overlaps(stamp, 1, first, count))
          return RM_ERR_INVALID_ARGUMENT;
      }
    } else if (h.type == 7)
      RM_TRY(rm_text_check_ids(body, first, count));
  }
  return RM_OK;
}
static bool zero_id(rm_item_id id) { return !id.author && !id.counter; }

rm_status rm_edit_insert_stroke(rm_edit_session* s, rm_item_id parent,
                                rm_item_id id, rm_item_id left,
                                rm_item_id right, const rm_draft_stroke* stroke,
                                rm_error* error) {
  if (!s || !stroke || !stroke->points || !stroke->point_count ||
      stroke->point_count > UINT32_MAX / 14u || !id.author || !id.counter ||
      zero_id(parent) || (!zero_id(left) && rm_item_id_equal(left, right)) ||
      !isfinite(stroke->thickness))
    return report(error, RM_ERR_INVALID_ARGUMENT);
  for (size_t i = 0; i < stroke->point_count; ++i) {
    if (!isfinite(stroke->points[i].x) || !isfinite(stroke->points[i].y))
      return report(error, RM_ERR_INVALID_ARGUMENT);
  }
  bool found_parent = false, found_left = zero_id(left),
       found_right = zero_id(right);
  rm_reader r = {.data = s->current->bytes,
                 .size = s->current->size,
                 .offset = RM_HEADER_LENGTH};
  while (rm_reader_remaining(&r) >= 8) {
    rm_block_header b;
    rm_status status = rm_read_block_header(&r, &b);
    if (status != RM_OK) return report(error, status);
    rm_reader body = r;
    body.size = b.end;
    if (b.type == 2) {
      rm_crdt_id node;
      status = rm_read_crdt(&body, 1, &node);
      if (status != RM_OK) return report(error, status);
      if (rm_crdt_equal(node, rm_crdt_of(id)))
        return report(error, RM_ERR_INVALID_ARGUMENT);
      if (rm_crdt_equal(node, rm_crdt_of(parent))) found_parent = true;
    } else if (sequence_type(b.type)) {
      rm_item_preamble item;
      status = rm_parse_item_preamble(&body, &item);
      if (status != RM_OK) return report(error, status);
      uint64_t span = item.deleted ? item.deleted : 1;
      if (item.id.author == id.author && item.id.counter <= id.counter &&
          id.counter - item.id.counter < span)
        return report(error, RM_ERR_INVALID_ARGUMENT);
      if (b.type != 8 && !item.deleted &&
          rm_match_tag(&body, 6, RM_TAG_LENGTH4) &&
          rm_crdt_equal(item.parent, rm_crdt_of(parent))) {
        if (rm_crdt_equal(item.id, rm_crdt_of(left))) found_left = true;
        if (rm_crdt_equal(item.id, rm_crdt_of(right))) found_right = true;
      }
    }
    r.offset = b.end;
  }
  if (!found_parent || !found_left || !found_right)
    return report(error, RM_ERR_ITEMS_NOT_FOUND);

  RM_TRY(rm_edit_check_location(r.data, r.size, parent, left, right, nullptr,
                                error));
  rm_buf payload = {0}, block = {0};
  rm_write_line_item(&payload, stroke, rm_crdt_of(parent), rm_crdt_of(id),
                     rm_crdt_of(left), rm_crdt_of(right));
  rm_write_top_block(&block, 5, 2, 2, payload.bytes, payload.count);
  if (payload.failed) block.failed = true;
  rm_buf_free(&payload);
  rm_status status = append_blocks(s, r.offset, &block, error);
  rm_buf_free(&block);
  return status;
}

typedef enum layer_action { ADD_LAYER, MOVE_LAYER, DELETE_LAYER } layer_action;

static void tree_move(rm_buf* out, rm_crdt_id event, rm_crdt_id node,
                      bool attach) {
  rm_buf body = {0}, parent = {0};
  rm_write_id(&body, 1, event);
  rm_write_id(&body, 2, node);
  rm_write_tag(&body, 3, RM_TAG_BYTE1);
  rm_buf_append_byte(&body, attach);
  rm_write_id(&parent, 1, attach ? rm_crdt_make(0, 1) : RM_CRDT_NONE);
  rm_write_block(&body, 4, parent.bytes, parent.count);
  rm_write_top_block(out, 1, 1, 1, body.bytes, body.count);
  if (body.failed || parent.failed) out->failed = true;
  rm_buf_free(&body);
  rm_buf_free(&parent);
}

static void layer_node(rm_buf* out, rm_crdt_id node, rm_crdt_id stamp,
                       const char* name) {
  rm_buf body = {0}, value = {0};
  rm_write_id(&body, 1, node);
  rm_write_string(&value, name);
  rm_write_stamped_block(&body, 2, stamp, value.bytes, value.count);
  rm_write_stamped_u8(&body, 3, RM_CRDT_NONE, true);
  rm_write_top_block(out, 2, 1, 2, body.bytes, body.count);
  if (body.failed || value.failed) out->failed = true;
  rm_buf_free(&body);
  rm_buf_free(&value);
}

static void layer_link(rm_buf* out, rm_crdt_id node, rm_crdt_id link,
                       rm_crdt_id left, rm_crdt_id right) {
  rm_buf body = {0}, value = {0};
  rm_write_id(&body, 1, rm_crdt_make(0, 1));
  rm_write_id(&body, 2, link);
  rm_write_id(&body, 3, left);
  rm_write_id(&body, 4, right);
  rm_write_tag(&body, 5, RM_TAG_BYTE4);
  rm_write_u32(&body, 0);
  rm_buf_append_byte(&value, 2);
  rm_write_id(&value, 2, node);
  rm_write_block(&body, 6, value.bytes, value.count);
  rm_write_top_block(out, 4, 1, 1, body.bytes, body.count);
  if (body.failed || value.failed) out->failed = true;
  rm_buf_free(&body);
  rm_buf_free(&value);
}

static void write_current(rm_buf* field, rm_crdt_id stamp, rm_item_id active) {
  rm_buf v = {0};
  rm_write_id(&v, 1, stamp);
  rm_write_id(&v, 2, rm_crdt_of(active));
  rm_write_block(field, 1, v.bytes, v.count);
  if (v.failed) field->failed = true;
  rm_buf_free(&v);
}

typedef struct current_layer_register {
  size_t block, begin, end;
  rm_crdt_id stamp, layer;
} current_layer_register;

/* The current layer is the leading SceneInfo register. Resolve its history
 * once, recording the precise field to replace in the winning block. */
static rm_status find_current_layer(rm_reader r,
                                    current_layer_register* current) {
  rm_reader winning = {0};
  while (rm_reader_remaining(&r) >= 8) {
    rm_block_header b;
    RM_TRY(rm_read_block_header(&r, &b));
    if (b.type == 13) {
      rm_reader body = r, field;
      body.size = b.end;
      if (!current->block)
        *current = (current_layer_register){
            .block = b.start, .begin = b.body, .end = b.body};
      for (;;) {
        size_t start = body.offset;
        if (!rm_match_tag(&body, 1, RM_TAG_LENGTH4)) break;
        rm_crdt_id stamp;
        RM_TRY(rm_read_subblock(&body, &field));
        RM_TRY(rm_read_crdt(&field, 1, &stamp));
        if (rm_stamp_newer(current->stamp, stamp)) continue;
        winning = field;
        *current = (current_layer_register){.block = b.start,
                                            .begin = start,
                                            .end = body.offset,
                                            .stamp = stamp};
      }
    }
    r.offset = b.end;
  }
  if (winning.data) {
    RM_TRY(rm_read_crdt(&winning, 2, &current->layer));
    if (rm_reader_remaining(&winning)) return RM_ERR_UNVERIFIED;
  }
  return RM_OK;
}

static rm_status scene_current(rm_buf* out, rm_reader body, rm_block_header b,
                               layer_action action, rm_item_id node,
                               rm_item_id active, rm_crdt_id stamp,
                               const rm_edit_layer* layers, size_t count,
                               const current_layer_register* reg) {
  rm_crdt_id current = reg->layer;
  if (action == DELETE_LAYER && !rm_crdt_equal(current, rm_crdt_of(node))) {
    if (!rm_crdt_equal(current, RM_CRDT_NONE)) {
      size_t i = 0;
      while (i < count && !rm_crdt_equal(rm_crdt_of(layers[i].node), current))
        ++i;
      if (i == count) return RM_ERR_UNVERIFIED;
    }
    rm_buf_append(out, body.data + b.start, b.end - b.start);
    return RM_OK;
  }
  if (!rm_stamp_newer(stamp, reg->stamp)) return RM_ERR_INVALID_ARGUMENT;
  rm_buf changed = {0};
  rm_buf_append(&changed, body.data + b.body, reg->begin - b.body);
  write_current(&changed, stamp, active);
  body.offset = reg->end;
  rm_buf_append(&changed, body.data + body.offset, rm_reader_remaining(&body));
  if (changed.count > UINT32_MAX) {
    rm_buf_free(&changed);
    return RM_ERR_TOO_MANY_TO_NUMBER;
  }
  rm_write_u32(out, (uint32_t)changed.count);
  rm_buf_append(out, body.data + b.start + 4, 4);
  rm_buf_append(out, changed.bytes, changed.count);
  if (changed.failed) out->failed = true;
  rm_buf_free(&changed);
  return RM_OK;
}

static rm_status change_layer(rm_edit_session* s, layer_action action,
                              rm_item_id node, size_t position,
                              const char* name, rm_item_id first,
                              rm_error* error) {
  if (error) *error = (rm_error){0};
  size_t needed = action == ADD_LAYER ? 4 : action == MOVE_LAYER ? 1 : 2;
  if (!s || !first.author || !first.counter ||
      needed - 1 > UINT64_MAX - first.counter || (action == ADD_LAYER && !name))
    return RM_ERR_INVALID_ARGUMENT;
  rm_edit_layer* layers;
  size_t count;
  RM_TRY(rm_edit_check_ids(s->current->bytes, s->current->size, first, needed));
  RM_TRY(rm_edit_layers(s, &layers, &count, error));
  size_t index = 0;
  while (index < count && !rm_item_id_equal(layers[index].node, node)) ++index;
  rm_buf out = {0};
  rm_status status = RM_ERR_INVALID_ARGUMENT;
  if (action == ADD_LAYER && (index < count || count >= 5 || position > count))
    goto done;
  if (action != ADD_LAYER && index == count) {
    status = RM_ERR_ITEMS_NOT_FOUND;
    goto done;
  }
  if (action == DELETE_LAYER && count < 2) goto done;
  if (action == MOVE_LAYER && position >= count) goto done;
  if (action == MOVE_LAYER && position == index) {
    status = RM_OK;
    goto done;
  }
  rm_item_id active = node, old_link = {0};
  if (action != ADD_LAYER) old_link = layers[index].link;
  if (action == DELETE_LAYER)
    active = layers[index + 1 < count ? index + 1 : index - 1].node;
  size_t expected_count = action == ADD_LAYER      ? count + 1
                          : action == DELETE_LAYER ? count - 1
                                                   : count;
  if (action == MOVE_LAYER) {
    memmove(layers + index, layers + index + 1,
            (count - index - 1) * sizeof *layers);
    --count;
  }
  rm_crdt_id next = rm_crdt_of(first), stamp = next;
  stamp.counter += needed - 1;
  rm_reader r = {.data = s->current->bytes,
                 .size = s->current->size,
                 .offset = RM_HEADER_LENGTH};
  rm_buf_append(&out, r.data, RM_HEADER_LENGTH);
  current_layer_register current = {0};
  if (action != MOVE_LAYER) {
    status = find_current_layer(r, &current);
    if (status != RM_OK) goto done;
  }
  bool removed = false;
  while (rm_reader_remaining(&r) >= 8) {
    rm_block_header b;
    status = rm_read_block_header(&r, &b);
    if (status != RM_OK) goto done;
    rm_reader body = r;
    body.size = b.end;
    bool copied = false;
    if (b.start == current.block) {
      status = scene_current(&out, body, b, action, node, active, stamp, layers,
                             count, &current);
      if (status != RM_OK) goto done;
      copied = true;
    }
    if (b.type == 4 && action == MOVE_LAYER && !zero_id(old_link)) {
      rm_item_preamble it;
      status = rm_parse_item_preamble(&body, &it);
      if (status != RM_OK) goto done;
      if (rm_crdt_equal(it.id, rm_crdt_of(old_link))) {
        if (removed || it.deleted) {
          status = RM_ERR_UNVERIFIED;
          goto done;
        }
        rm_write_u32(&out, (uint32_t)(body.offset - b.body));
        rm_buf_append(&out, r.data + b.start + 4, body.offset - b.start - 8);
        rm_write_u32(&out, 1);
        copied = removed = true;
      }
    }
    if (!copied) rm_buf_append(&out, r.data + b.start, b.end - b.start);
    r.offset = b.end;
  }
  if (action == MOVE_LAYER && !zero_id(old_link) && !removed) {
    status = RM_ERR_UNVERIFIED;
    goto done;
  }
  if (action == DELETE_LAYER)
    tree_move(&out, next, rm_crdt_of(node), false);
  else {
    if (action == ADD_LAYER) {
      tree_move(&out, next, RM_CRDT_NONE, true);
      ++next.counter;
      layer_node(&out, rm_crdt_of(node), next, name);
      ++next.counter;
    }
    rm_crdt_id left =
        position ? rm_crdt_of(layers[position - 1].link) : RM_CRDT_NONE;
    rm_crdt_id right =
        position < count ? rm_crdt_of(layers[position].link) : RM_CRDT_NONE;
    layer_link(&out, rm_crdt_of(node), next, left, right);
  }
  if (!current.block && action == ADD_LAYER) {
    rm_buf info = {0};
    write_current(&info, stamp, node);
    rm_write_top_block(&out, 13, 0, 1, info.bytes, info.count);
    if (info.failed) out.failed = true;
    rm_buf_free(&info);
  }
  rm_buf_append(&out, r.data + r.offset, rm_reader_remaining(&r));
  size_t size;
  if (out.failed) {
    status = RM_ERR_OUT_OF_MEMORY;
    goto done;
  }
  revision view = {.bytes = out.bytes, .size = out.count};
  rm_edit_session candidate = {.current = &view};
  rm_edit_layer* result = nullptr;
  size_t actual = 0;
  status = rm_edit_layers(&candidate, &result, &actual, error);
  if (status != RM_OK) goto done;
  if (actual != expected_count)
    status = RM_ERR_UNVERIFIED;
  else if (action != DELETE_LAYER &&
           !rm_item_id_equal(result[position].node, node))
    status = RM_ERR_UNVERIFIED;
  else if (action == DELETE_LAYER) {
    bool survivor = false;
    for (size_t i = 0; i < actual; i++) {
      if (rm_item_id_equal(result[i].node, node)) status = RM_ERR_UNVERIFIED;
      if (rm_item_id_equal(result[i].node, active)) survivor = true;
    }
    if (!survivor) status = RM_ERR_UNVERIFIED;
  }
  free(result);
  if (status != RM_OK) goto done;
  uint8_t* bytes = rm_buf_take(&out, &size);
  status = bytes ? rm_edit_commit(s, bytes, size, error) : RM_ERR_OUT_OF_MEMORY;
done:
  rm_buf_free(&out);
  free(layers);
  return status;
}

rm_status rm_edit_add_layer(rm_edit_session* s, size_t position,
                            const char* name, rm_item_id first,
                            rm_error* error) {
  return rm_edit_result(
      error, change_layer(s, ADD_LAYER, first, position, name, first, error));
}
rm_status rm_edit_move_layer(rm_edit_session* s, rm_item_id node,
                             size_t position, rm_item_id first,
                             rm_error* error) {
  return rm_edit_result(error, change_layer(s, MOVE_LAYER, node, position,
                                            nullptr, first, error));
}
rm_status rm_edit_delete_layer(rm_edit_session* s, rm_item_id node,
                               rm_item_id first, rm_error* error) {
  return rm_edit_result(
      error, change_layer(s, DELETE_LAYER, node, 0, nullptr, first, error));
}

void rm_edit_forget_previous(rm_edit_session* session) {
  revision* previous = session->current->before;
  if (!previous) return;
  previous->after = nullptr;
  session->current->before = nullptr;
  free(previous->bytes);
  free(previous);
}
