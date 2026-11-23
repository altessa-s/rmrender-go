/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <stdlib.h>

#include "rm_internal.h"
#include "rmlines/editor.h"
#include "rmlines/parser.h"

typedef struct text_rows {
  rm_reader root, lists, items, rows;
  size_t lists_start, items_start, rows_start;
  rm_arena* arena;
  rm_reader* raw;
  text_item* records;
  uint32_t* order;
  size_t count;
} text_rows;

static rm_status sub(rm_reader* r, uint32_t index, rm_reader* child) {
  RM_TRY(rm_expect_tag(r, index, RM_TAG_LENGTH4));
  return rm_read_subblock(r, child);
}

static rm_status row_value(rm_reader row, text_item* item, rm_arena* arena) {
  RM_TRY(rm_read_crdt(&row, 2, &item->id));
  RM_TRY(rm_read_crdt(&row, 3, &item->left));
  RM_TRY(rm_read_crdt(&row, 4, &item->right));
  RM_TRY(rm_expect_tag(&row, 5, RM_TAG_BYTE4));
  RM_TRY(rm_read_u32(&row, &item->deleted));
  if (!rm_reader_remaining(&row)) return RM_OK;
  rm_reader value;
  RM_TRY(sub(&row, 6, &value));
  rm_reader header = value;
  size_t declared;
  uint8_t encoding;
  RM_TRY(rm_read_count(&header, &declared));
  RM_TRY(rm_read_u8(&header, &encoding));
  if (encoding != 1 || declared > rm_reader_remaining(&header))
    return RM_ERR_UNVERIFIED;
  RM_TRY(rm_read_string(&value, value.size, arena, &item->text));
  if (rm_match_tag(&value, 2, RM_TAG_BYTE4)) {
    RM_TRY(rm_read_u32(&value, &item->marker));
    item->has_marker = item->marker != 0;
  }
  /* Splitting an extended record would give its unknown fields new owners. */
  if (rm_reader_remaining(&row) || rm_reader_remaining(&value))
    return RM_ERR_UNVERIFIED;
  return RM_OK;
}

static rm_status load_rows(text_rows* t, rm_reader root) {
  t->root = root;
  RM_TRY(rm_skip_crdt(&root, 1));
  t->lists_start = root.offset;
  RM_TRY(sub(&root, 2, &t->lists));
  rm_reader lists = t->lists;
  t->items_start = lists.offset;
  RM_TRY(sub(&lists, 1, &t->items));
  rm_reader items = t->items;
  t->rows_start = items.offset;
  RM_TRY(sub(&items, 1, &t->rows));
  rm_reader rows = t->rows;
  RM_TRY(rm_read_count(&rows, &t->count));
  if (t->count > rm_reader_remaining(&rows) / 5 || t->count > UINT32_MAX)
    return RM_ERR_MALFORMED_PAGE;
  if (t->count) {
    t->arena = rm_arena_create();
    if (!t->arena) return RM_ERR_OUT_OF_MEMORY;
    t->raw = calloc(t->count, sizeof *t->raw);
    t->records = calloc(t->count, sizeof *t->records);
    if (!t->raw || !t->records) return RM_ERR_OUT_OF_MEMORY;
  }
  for (size_t i = 0; i < t->count; ++i) {
    RM_TRY(sub(&rows, 0, &t->raw[i]));
    RM_TRY(row_value(t->raw[i], &t->records[i], t->arena));
  }
  if (rm_reader_remaining(&rows)) return RM_ERR_MALFORMED_PAGE;
  rm_vec parts;
  rm_vec_init(&parts, sizeof(text_item));
  if (!rm_split_text_runs(t->records, t->count, &parts)) {
    rm_vec_free(&parts);
    return RM_ERR_OUT_OF_MEMORY;
  }
  free(t->records);
  t->records = parts.data;
  t->count = parts.count;
  return rm_text_record_order(t->records, t->count, &t->order)
             ? RM_OK
             : RM_ERR_OUT_OF_MEMORY;
}

static rm_crdt_id advanced(rm_crdt_id id, size_t distance) {
  id.counter += distance;
  return id;
}

typedef struct rewritten {
  rm_buf bytes;
  size_t count;
  rm_idmap style_offsets;
} rewritten;

static void emit_row(rewritten* out, rm_crdt_id id, rm_crdt_id left,
                     rm_crdt_id right, uint32_t deleted, rm_str text) {
  rm_buf row = {0}, value = {0};
  rm_write_id(&row, 2, id);
  rm_write_id(&row, 3, left);
  rm_write_id(&row, 4, right);
  rm_write_tag(&row, 5, RM_TAG_BYTE4);
  rm_write_u32(&row, deleted);
  if (!deleted) {
    rm_write_varuint(&value, text.length);
    rm_buf_append_byte(&value, 1);
    rm_buf_append(&value, text.bytes, text.length);
    rm_write_block(&row, 6, value.bytes, value.count);
  }
  rm_write_block(&out->bytes, 0, row.bytes, row.count);
  if (row.failed || value.failed) out->bytes.failed = true;
  rm_buf_free(&row);
  rm_buf_free(&value);
  ++out->count;
}

static void copy_row(rewritten* out, const text_item* r, rm_reader raw) {
  if (r->fragment) {
    emit_row(out, r->id, r->left, r->right, r->deleted, r->text);
  } else {
    rm_write_block(&out->bytes, 0, raw.data + raw.offset,
                   rm_reader_remaining(&raw));
    ++out->count;
  }
}

static size_t byte_at(rm_str s, size_t scalar) {
  size_t n = 0;
  while (scalar-- && n < s.length)
    n = rm_utf8_next(s.bytes, s.length, n, nullptr);
  return n;
}

static rm_status rewrite_rows(const text_rows* t, size_t start, size_t end,
                              rm_str insertion, rm_crdt_id fresh,
                              rm_idmap* removed, rewritten* out) {
  size_t at = 0;
  bool inserted = false;
  size_t replacement_byte = 0, replacement_scalar = 0;
  rm_crdt_id previous = RM_CRDT_NONE;
  uint64_t insert_count =
      rm_utf8_scalar_count(insertion.bytes, insertion.length);
  for (size_t i = 0; i < t->count; ++i) {
    size_t n = t->order[i];
    const text_item* r = &t->records[n];
    rm_reader raw = t->raw[r->source];
    size_t length = r->deleted ? 0
                    : r->has_marker
                        ? (r->marker == UINT32_MAX)
                        : rm_utf8_scalar_count(r->text.bytes, r->text.length);
    uint64_t span = r->deleted ? r->deleted : length ? length : 1;
    if (span - 1 > UINT64_MAX - r->id.counter || length > SIZE_MAX - at)
      return RM_ERR_MALFORMED_PAGE;
    if (insert_count && fresh.author == r->id.author &&
        fresh.counter <= r->id.counter + span - 1 &&
        r->id.counter <= fresh.counter + (insert_count - 1))
      return RM_ERR_INVALID_ARGUMENT;
    size_t stop = at + length;
    size_t lo = start <= at ? 0 : start < stop ? start - at : length;
    size_t hi = end <= at ? 0 : end < stop ? end - at : length;
    bool insert_here =
        insertion.length && !inserted && length && start >= at && start < stop;
    if (!insert_here && hi == lo) {
      copy_row(out, r, raw);
    } else {
      size_t b0 = byte_at(r->text, lo), b1 = byte_at(r->text, hi);
      rm_crdt_id left = lo ? advanced(r->id, lo - 1) : r->left;
      if (lo)
        emit_row(out, r->id, r->left, r->right, 0, (rm_str){r->text.bytes, b0});
      if (insert_here) {
        emit_row(out, fresh, lo ? left : previous, advanced(r->id, lo), 0,
                 insertion);
        inserted = true;
      }
      if (hi > lo && !r->has_marker && replacement_byte < insertion.length) {
        size_t old_scalar = lo;
        for (size_t b = b0; b < b1 && replacement_byte < insertion.length;
             old_scalar++) {
          uint32_t ch;
          b = rm_utf8_next(r->text.bytes, r->text.length, b, &ch);
          if (ch != '\n') continue;
          /* Pair removed and inserted paragraph breaks in reading order. */
          while (replacement_byte < insertion.length) {
            uint32_t next;
            replacement_byte = rm_utf8_next(insertion.bytes, insertion.length,
                                            replacement_byte, &next);
            size_t offset = replacement_scalar++;
            if (next == '\n') {
              if (!rm_idmap_set(&out->style_offsets,
                                advanced(r->id, old_scalar), offset))
                return RM_ERR_OUT_OF_MEMORY;
              break;
            }
          }
        }
      }
      if (hi > lo) {
        for (size_t k = lo; k < hi; ++k) {
          if (!rm_idmap_set(removed, advanced(r->id, k), (at + k) - start))
            return RM_ERR_OUT_OF_MEMORY;
        }
        if (hi - lo > UINT32_MAX) return RM_ERR_TOO_MANY_TO_NUMBER;
        emit_row(out, advanced(r->id, lo), left, r->right, (uint32_t)(hi - lo),
                 (rm_str){0});
      }
      if (hi < length) {
        if (!hi) {
          copy_row(out, r, raw);
        } else {
          emit_row(out, advanced(r->id, hi), advanced(r->id, hi - 1), r->right,
                   0, (rm_str){r->text.bytes + b1, r->text.length - b1});
        }
      }
    }
    if (length) previous = advanced(r->id, (size_t)(span - 1));
    at = stop;
  }
  if (end > at) return RM_ERR_INVALID_ARGUMENT;
  if (insertion.length && !inserted)
    emit_row(out, fresh, previous, RM_CRDT_NONE, 0, insertion);
  return out->bytes.failed ? RM_ERR_OUT_OF_MEMORY : RM_OK;
}

/* All list headers are five bytes (a one-byte tag followed by a u32).
 * Preserve siblings and tails verbatim as each containing length changes. */
static void replace_child(rm_buf* out, rm_reader parent, size_t header,
                          rm_reader old, uint32_t index, const rm_buf* value) {
  rm_buf_append(out, parent.data + parent.offset, header - parent.offset);
  rm_write_block(out, index, value->bytes, value->count);
  rm_buf_append(out, parent.data + old.size, parent.size - old.size);
  if (value->failed) out->failed = true;
}

static rm_status remap_styles(rm_reader lists, rm_crdt_id fresh,
                              const rm_idmap* offsets, rm_buf* result) {
  size_t start = lists.offset;
  if (!rm_match_tag(&lists, 2, RM_TAG_LENGTH4)) {
    rm_buf_append(result, lists.data + start, lists.size - start);
    return RM_OK;
  }
  rm_reader styles, rows;
  RM_TRY(rm_read_subblock(&lists, &styles));
  rm_reader content = styles;
  RM_TRY(sub(&content, 1, &rows));
  size_t count;
  rm_reader input = rows;
  RM_TRY(rm_read_count(&input, &count));
  rm_buf changed = {0}, wrapped = {0};
  rm_buf_append(&changed, rows.data + rows.offset, input.offset - rows.offset);
  rm_status status = RM_OK;
  for (size_t i = 0; i < count; i++) {
    rm_crdt_id key;
    size_t key_start = input.offset;
    status = rm_read_bare_crdt(&input, &key);
    if (status != RM_OK) break;
    uint64_t offset;
    if (rm_idmap_get(offsets, key, &offset))
      rm_write_bare_id(&changed, advanced(fresh, (size_t)offset));
    else
      rm_buf_append(&changed, input.data + key_start, input.offset - key_start);
    size_t rest = input.offset;
    rm_reader value;
    status = rm_skip_crdt(&input, 1);
    if (status == RM_OK) status = sub(&input, 2, &value);
    if (status != RM_OK) break;
    rm_buf_append(&changed, input.data + rest, input.offset - rest);
  }
  if (status == RM_OK) {
    rm_buf_append(&changed, input.data + input.offset,
                  rm_reader_remaining(&input));
    replace_child(&wrapped, styles, styles.offset, rows, 1, &changed);
    rm_write_block(result, 2, wrapped.bytes, wrapped.count);
    rm_buf_append(result, lists.data + lists.offset,
                  rm_reader_remaining(&lists));
    if (wrapped.failed || result->failed) status = RM_ERR_OUT_OF_MEMORY;
  }
  rm_buf_free(&changed);
  rm_buf_free(&wrapped);
  return status;
}
static rm_status change_root(rm_reader root, size_t start, size_t end,
                             rm_str insertion, rm_crdt_id fresh,
                             rm_idmap* removed, rm_buf* result) {
  text_rows t = {0};
  rewritten rewritten_rows = {0};
  rm_idmap_init(&rewritten_rows.style_offsets);
  rm_status status = load_rows(&t, root);
  if (status == RM_OK)
    status = rewrite_rows(&t, start, end, insertion, fresh, removed,
                          &rewritten_rows);
  if (status == RM_OK) {
    rm_buf rows = {0}, items = {0}, lists = {0};
    rm_write_varuint(&rows, rewritten_rows.count);
    rm_buf_append(&rows, rewritten_rows.bytes.bytes,
                  rewritten_rows.bytes.count);
    replace_child(&items, t.items, t.rows_start, t.rows, 1, &rows);
    if (!rewritten_rows.style_offsets.count)
      replace_child(&lists, t.lists, t.items_start, t.items, 1, &items);
    else {
      rm_write_block(&lists, 1, items.bytes, items.count);
      if (items.failed) lists.failed = true;
      rm_reader tail = t.lists;
      tail.offset = t.items.size;
      status = remap_styles(tail, fresh, &rewritten_rows.style_offsets, &lists);
    }
    replace_child(result, t.root, t.lists_start, t.lists, 2, &lists);
    rm_buf_free(&rows);
    rm_buf_free(&items);
    rm_buf_free(&lists);
    if (result->failed) status = RM_ERR_OUT_OF_MEMORY;
  }
  rm_idmap_free(&rewritten_rows.style_offsets);
  rm_buf_free(&rewritten_rows.bytes);
  free(t.raw);
  rm_arena_destroy(t.arena);
  free(t.records);
  free(t.order);
  return status;
}

static rm_status page_text(const uint8_t* data, size_t size, rm_buf* flat,
                           rm_error* error) {
  rm_page* page = nullptr;
  RM_TRY(rm_parse_page(data, size, &page, error));
  for (size_t i = 0; page->has_text && i < page->text.paragraph_count; ++i) {
    if (i) rm_buf_append_byte(flat, '\n');
    rm_str s = page->text.paragraphs[i].text;
    rm_buf_append(flat, s.bytes, s.length);
  }
  rm_page_free(page);
  return flat->failed ? RM_ERR_OUT_OF_MEMORY : RM_OK;
}

static rm_status skip_stat_value(rm_reader* body, uint8_t type) {
  const uint8_t* bytes;
  rm_crdt_id id;
  rm_reader block;
  switch (type) {
    case RM_TAG_ID:
      return rm_read_bare_crdt(body, &id);
    case RM_TAG_LENGTH4:
      return rm_read_subblock(body, &block);
    case RM_TAG_BYTE1:
    case RM_TAG_BYTE4:
    case RM_TAG_BYTE8:
      return rm_read_bytes(body, type, &bytes);
    default:
      return RM_ERR_UNVERIFIED;
  }
}

/* Adjust only the existing PageStats fields; counts include the implicit
 * final character and the empty final paragraph. */
static rm_status update_stats(rm_reader body, const rm_buf* text, rm_buf* out) {
  rm_text_counts totals = {1, 1};
  rm_text_counts_add(&totals, (const char*)text->bytes, text->count);
  if (totals.chars > UINT32_MAX || totals.lines > UINT32_MAX)
    return RM_ERR_TOO_MANY_TO_NUMBER;
  while (rm_reader_remaining(&body)) {
    size_t start = body.offset;
    uint32_t tag;
    uint8_t type;
    if (rm_read_tag(&body, &tag, &type) != RM_OK ||
        skip_stat_value(&body, type) != RM_OK) {
      rm_buf_append(out, body.data + start, body.size - start);
      break;
    }
    if (type == RM_TAG_BYTE4 && (tag == 3 || tag == 4)) {
      rm_buf_append(out, body.data + start, body.offset - start - 4);
      rm_write_u32(out, (uint32_t)(tag == 3 ? totals.chars : totals.lines));
    } else
      rm_buf_append(out, body.data + start, body.offset - start);
  }
  return out->failed ? RM_ERR_OUT_OF_MEMORY : RM_OK;
}

typedef struct column_setting {
  rm_crdt_id stamp;
  uint64_t mode;
  uint32_t screen_width;
} column_setting;

static void read_column_setting(rm_reader body, column_setting* column) {
  while (rm_reader_remaining(&body)) {
    uint32_t index;
    uint8_t kind;
    rm_reader field;
    if (rm_read_tag(&body, &index, &kind) != RM_OK || kind != RM_TAG_LENGTH4 ||
        rm_read_subblock(&body, &field) != RM_OK)
      break;
    if (index == 5) {
      uint32_t width, height;
      if (rm_read_u32(&field, &width) == RM_OK &&
          rm_read_u32(&field, &height) == RM_OK && width && height)
        column->screen_width = width;
    }
    if (index != 9) continue;
    rm_reader value;
    uint64_t mode;
    rm_crdt_id stamp;
    if (rm_read_crdt(&field, 1, &stamp) == RM_OK &&
        sub(&field, 2, &value) == RM_OK &&
        rm_read_varuint(&value, &mode) == RM_OK &&
        !rm_stamp_newer(column->stamp, stamp)) {
      column->stamp = stamp;
      column->mode = mode;
    }
  }
}

static float preferred_width(const column_setting* column) {
  rm_text_column choice = RM_COLUMN_MEDIUM;
  if (!rm_crdt_equal(column->stamp, RM_CRDT_NONE)) {
    if (column->mode <= 2) choice = (rm_text_column)column->mode;
    if (column->mode == 4 && column->screen_width)
      choice = column->screen_width < 1000   ? RM_COLUMN_NARROW
               : column->screen_width < 1500 ? RM_COLUMN_MEDIUM
                                             : RM_COLUMN_WIDE;
  }
  return rm_text_column_width(choice);
}

static void empty_root(rm_buf* root, float width) {
  rm_buf rows = {0}, items = {0}, lists = {0}, box = {0};
  rm_write_varuint(&rows, 0);
  rm_write_block(&items, 1, rows.bytes, rows.count);
  rm_write_block(&lists, 1, items.bytes, items.count);
  rm_write_block(&lists, 2, items.bytes, items.count);
  rm_write_id(root, 1, RM_CRDT_NONE);
  rm_write_block(root, 2, lists.bytes, lists.count);
  rm_write_f64(&box, -width / 2.0);
  rm_write_f64(&box, 234);
  rm_write_block(root, 3, box.bytes, box.count);
  rm_write_tag(root, 4, RM_TAG_BYTE4);
  rm_write_f32(root, width);
  if (rows.failed || items.failed || lists.failed || box.failed)
    root->failed = true;
  rm_buf_free(&rows);
  rm_buf_free(&items);
  rm_buf_free(&lists);
  rm_buf_free(&box);
}

static rm_status rewrite_file(rm_reader r, size_t start, size_t end,
                              rm_str insertion, rm_crdt_id fresh,
                              const rm_buf* expected, rm_idmap* removed,
                              rm_buf* file) {
  bool found = false;
  column_setting column = {0};
  rm_buf_append(file, r.data, RM_HEADER_LENGTH);
  r.offset = RM_HEADER_LENGTH;
  while (rm_reader_remaining(&r) >= 8) {
    rm_block_header b;
    RM_TRY(rm_read_block_header(&r, &b));
    rm_reader body = r;
    body.size = b.end;
    if (b.type == 13) read_column_setting(body, &column);
    if (b.type == 7 || b.type == 10) {
      if (b.type == 7 && found) return RM_ERR_UNVERIFIED;
      rm_buf changed = {0};
      rm_status status = b.type == 7 ? change_root(body, start, end, insertion,
                                                   fresh, removed, &changed)
                                     : update_stats(body, expected, &changed);
      if (b.type == 7) found = true;
      if (status == RM_OK && changed.count > UINT32_MAX)
        status = RM_ERR_TOO_MANY_TO_NUMBER;
      if (status == RM_OK) {
        rm_write_u32(file, (uint32_t)changed.count);
        rm_buf_append(file, r.data + b.start + 4, 4);
        rm_buf_append(file, changed.bytes, changed.count);
      }
      rm_buf_free(&changed);
      if (status != RM_OK) return status;
    } else
      rm_buf_append(file, r.data + b.start, b.end - b.start);
    r.offset = b.end;
  }
  if (!found) {
    rm_buf root = {0}, changed = {0};
    empty_root(&root, preferred_width(&column));
    rm_status status =
        root.failed
            ? RM_ERR_OUT_OF_MEMORY
            : change_root((rm_reader){.data = root.bytes, .size = root.count},
                          start, end, insertion, fresh, removed, &changed);
    if (status == RM_OK)
      rm_write_top_block(file, 7, 1, 1, changed.bytes, changed.count);
    rm_buf_free(&root);
    rm_buf_free(&changed);
    if (status != RM_OK) return status;
  }
  rm_buf_append(file, r.data + r.offset, rm_reader_remaining(&r));
  return file->failed ? RM_ERR_OUT_OF_MEMORY : RM_OK;
}

/* Nodes can precede the root text on disk. Once the removed IDs are known,
 * patch their registers in the candidate, copying untouched blocks verbatim. */
typedef struct anchor_record {
  rm_anchor_field field;
  rm_block_header block;
} anchor_record;

static rm_status collect_anchors(rm_reader r, rm_vec* records,
                                 rm_idmap* winners) {
  while (rm_reader_remaining(&r) >= 8) {
    anchor_record record;
    RM_TRY(rm_read_block_header(&r, &record.block));
    rm_reader body = r;
    body.size = r.offset = record.block.end;
    if (record.block.type != 2) continue;
    RM_TRY(rm_read_node_anchor(body, &record.field));
    if (!record.field.field_end) continue;
    uint64_t index;
    if (rm_idmap_get(winners, record.field.node, &index)) {
      const anchor_record* old = rm_vec_at(records, index);
      if (rm_stamp_newer(old->field.stamp, record.field.stamp)) continue;
    }
    if (!rm_idmap_set(winners, record.field.node, records->count) ||
        !rm_vec_push(records, &record))
      return RM_ERR_OUT_OF_MEMORY;
  }
  return RM_OK;
}

static rm_status rewrite_anchors(rm_buf* file, const rm_idmap* removed,
                                 rm_crdt_id fresh, size_t inserted, rm_buf* out,
                                 const rm_vec* records,
                                 const rm_idmap* winners) {
  rm_reader r = {.data = file->bytes, .size = file->count};
  size_t copied = 0;
  for (size_t i = 0; i < records->count; ++i) {
    const anchor_record* record = rm_vec_at(records, i);
    rm_anchor_field anchor = record->field;
    rm_block_header b = record->block;
    uint64_t winning;
    if (!rm_idmap_get(winners, anchor.node, &winning) || i != winning) continue;
    uint64_t offset;
    if (!anchor.field_end || !rm_idmap_get(removed, anchor.target, &offset))
      continue;
    if (offset >= inserted) return RM_ERR_UNVERIFIED;

    rm_crdt_id stamp = advanced(fresh, inserted);
    if (!rm_stamp_newer(stamp, anchor.stamp)) return RM_ERR_INVALID_ARGUMENT;
    rm_buf id = {0};
    rm_write_id(&id, 1, stamp);
    rm_write_id(&id, 2, advanced(fresh, (size_t)offset));
    if (id.failed) {
      rm_buf_free(&id);
      return RM_ERR_OUT_OF_MEMORY;
    }
    size_t payload = anchor.length_at + 4;
    size_t old = anchor.target_end - payload;
    uint64_t body_size = b.end - b.body - old + id.count;
    if (body_size > UINT32_MAX) {
      rm_buf_free(&id);
      return RM_ERR_TOO_MANY_TO_NUMBER;
    }
    rm_buf_append(out, r.data + copied, b.start - copied);
    rm_write_u32(out, (uint32_t)body_size);
    size_t header = b.start + 4;
    rm_buf_append(out, r.data + header, anchor.length_at - header);
    rm_write_u32(out, (uint32_t)(anchor.field_end - payload - old + id.count));
    rm_buf_append(out, id.bytes, id.count);
    rm_buf_free(&id);
    copied = anchor.target_end;
  }
  if (copied) {
    rm_buf_append(out, r.data + copied, r.size - copied);
    if (out->failed) return RM_ERR_OUT_OF_MEMORY;
    rm_buf_free(file);
    *file = *out;
    *out = (rm_buf){0};
  }
  return RM_OK;
}

static rm_status remap_anchors(rm_buf* file, const rm_idmap* removed,
                               rm_crdt_id fresh, size_t inserted, rm_buf* out) {
  rm_vec records;
  rm_vec_init(&records, sizeof(anchor_record));
  rm_idmap winners = {0};
  rm_reader r = {
      .data = file->bytes, .size = file->count, .offset = RM_HEADER_LENGTH};
  rm_status status = collect_anchors(r, &records, &winners);
  if (status == RM_OK)
    status = rewrite_anchors(file, removed, fresh, inserted, out, &records,
                             &winners);
  rm_vec_free(&records);
  rm_idmap_free(&winners);
  return status;
}

rm_status rm_edit_replace_text(rm_edit_session* s, size_t start, size_t remove,
                               const char* utf8, rm_item_id id,
                               rm_error* error) {
  if (error) *error = (rm_error){0};
  rm_status status = RM_ERR_INVALID_ARGUMENT;
  rm_buf original = {0}, expected = {0}, candidate = {0}, actual = {0};
  rm_idmap removed = {0};
  rm_buf anchored = {0};
  char* valid = nullptr;
  if (!s || !utf8 || remove > SIZE_MAX - start) goto done;
  size_t bytes = strlen(utf8);
  size_t bound = rm_utf8_repair_bound(bytes);
  if (!bound) goto done;
  valid = malloc(bound);
  if (!valid) {
    status = RM_ERR_OUT_OF_MEMORY;
    goto done;
  }
  size_t length = rm_utf8_repair(utf8, bytes, valid);
  size_t count = rm_utf8_scalar_count(valid, length);
  /* One operation stamp serves every anchor register this edit remaps. */
  if (count == SIZE_MAX) goto done;
  size_t needed = count + (count && remove ? 1 : 0);
  size_t size;
  const uint8_t* data = rm_edit_bytes(s, &size);
  status = rm_edit_check_ids(data, size, id, needed);
  if (status != RM_OK) goto done;
  status = page_text(data, size, &original, error);
  if (status != RM_OK) goto done;
  rm_str flat = {(const char*)original.bytes, original.count};
  if (start + remove > rm_utf8_scalar_count(flat.bytes, flat.length)) {
    status = RM_ERR_INVALID_ARGUMENT;
    goto done;
  }
  if (!count && !remove) goto done;
  size_t left = byte_at(flat, start), right = byte_at(flat, start + remove);
  rm_buf_append(&expected, flat.bytes, left);
  rm_buf_append(&expected, valid, length);
  if (flat.length > right)
    rm_buf_append(&expected, flat.bytes + right, flat.length - right);
  if (expected.failed) {
    status = RM_ERR_OUT_OF_MEMORY;
    goto done;
  }
  status = rewrite_file((rm_reader){.data = data, .size = size}, start,
                        start + remove, (rm_str){valid, length}, rm_crdt_of(id),
                        &expected, &removed, &candidate);
  if (status == RM_OK && removed.count)
    status =
        remap_anchors(&candidate, &removed, rm_crdt_of(id), count, &anchored);
  if (status != RM_OK) goto done;
  status = page_text(candidate.bytes, candidate.count, &actual, error);
  if (status != RM_OK) goto done;
  if (actual.count != expected.count ||
      (actual.count && memcmp(actual.bytes, expected.bytes, actual.count))) {
    status = RM_ERR_UNVERIFIED;
    goto done;
  }
  uint8_t* output = rm_buf_take(&candidate, &size);
  status =
      output ? rm_edit_commit(s, output, size, error) : RM_ERR_OUT_OF_MEMORY;
done:
  rm_idmap_free(&removed);
  rm_buf_free(&anchored);
  free(valid);
  rm_buf_free(&original);
  rm_buf_free(&expected);
  rm_buf_free(&candidate);
  rm_buf_free(&actual);
  return rm_edit_result(error, status);
}
