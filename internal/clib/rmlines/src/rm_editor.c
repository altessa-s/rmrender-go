/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Edits made to a page the tablet wrote, in place: the named items become
 * tombstones and every other byte goes out as it came in. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rm_internal.h"
#include "rmlines/editor.h"
#include "rmlines/parser.h"
#include "rmlines/writer.h"

/* Fill in `*out_error`, when the caller asked for one, with the status, the
 * numeric value and an optional message. */
static void fail_with(rm_error* out_error, rm_status status, long value,
                      const char* message) {
  if (out_error == nullptr) return;
  *out_error = (rm_error){.status = status, .value = value};
  if (message != nullptr) {
    snprintf(out_error->message, sizeof out_error->message, "%s", message);
  }
}

/* Where a living item's deleted length stands in the page, when the block the
 * reader is at is one of `wanted`; false for any other block. The reader is
 * bounded to the block, so the value's tag is looked for inside it and
 * nowhere else: a tombstone's block ends right behind its preamble, and the
 * next block's length of 108 spells 0x6C, the very tag of a value. */
static bool living_item(rm_reader* reader, const rm_idmap* wanted,
                        rm_crdt_id* out_id, size_t* out_deleted_at) {
  /* A block whose preamble does not read is not one we were asked about:
   * the parse that ran first counted it, and it goes out as it came in. */
  rm_item_preamble preamble;
  if (rm_parse_item_preamble(reader, &preamble) != RM_OK) return false;
  /* The preamble ends with the four bytes of the deleted length. */
  size_t deleted_at = reader->offset - 4u;
  if (!rm_idmap_contains(wanted, preamble.id)) return false;
  /* Already a tombstone — erased, or a range of erased items — is not a
   * living item, and the request that named it is not about this page as it
   * is. */
  if (preamble.deleted != 0 || !rm_match_tag(reader, 6, RM_TAG_LENGTH4))
    return false;
  *out_id = preamble.id;
  *out_deleted_at = deleted_at;
  return true;
}

/* Every stroke and highlight the page shows, by identifier, in order — less
 * the ones in `erased`, when it is given. */
static bool identifiers(const rm_page* page, const rm_idmap* erased,
                        rm_vec* out) {
  for (size_t i = 0; i < page->stroke_count; i += 1) {
    rm_item_id id = page->strokes[i].id;
    if (erased != nullptr && rm_idmap_contains(erased, rm_crdt_of(id)))
      continue;
    if (!rm_vec_push(out, &id)) return false;
  }
  for (size_t i = 0; i < page->highlight_count; i += 1) {
    rm_item_id id = page->highlights[i].id;
    if (erased != nullptr && rm_idmap_contains(erased, rm_crdt_of(id)))
      continue;
    if (!rm_vec_push(out, &id)) return false;
  }
  return true;
}

/* The edited page read back and held against the page it was made from. */
static rm_status verify(const uint8_t* edited, size_t size,
                        const rm_page* original, const rm_idmap* erased,
                        rm_error* out_error) {
  rm_page* page = nullptr;
  rm_error parse_error;
  rm_status status = rm_parse_page(edited, size, &page, &parse_error);
  if (status == RM_ERR_OUT_OF_MEMORY) {
    fail_with(out_error, status, 0, nullptr);
    return status;
  }
  if (status != RM_OK) {
    char message[160];
    snprintf(message, sizeof message, "the edited page does not parse: %s",
             rm_status_string(status));
    fail_with(out_error, RM_ERR_UNVERIFIED, 0, message);
    return RM_ERR_UNVERIFIED;
  }

  if (page->skipped_blocks != original->skipped_blocks) {
    char message[160];
    snprintf(message, sizeof message,
             "the edited page skips %zu blocks, the original %zu",
             page->skipped_blocks, original->skipped_blocks);
    rm_page_free(page);
    fail_with(out_error, RM_ERR_UNVERIFIED, 0, message);
    return RM_ERR_UNVERIFIED;
  }

  rm_vec before, after;
  rm_vec_init(&before, sizeof(rm_item_id));
  rm_vec_init(&after, sizeof(rm_item_id));
  status = RM_OK;
  if (!identifiers(original, erased, &before) ||
      !identifiers(page, nullptr, &after)) {
    status = RM_ERR_OUT_OF_MEMORY;
    fail_with(out_error, status, 0, nullptr);
  } else {
    bool same = before.count == after.count;
    for (size_t i = 0; same && i < before.count; i += 1) {
      same = rm_item_id_equal(*(rm_item_id*)rm_vec_at(&before, i),
                              *(rm_item_id*)rm_vec_at(&after, i));
    }
    if (!same) {
      status = RM_ERR_UNVERIFIED;
      fail_with(out_error, status, 0,
                "the edited page does not hold what the original held less the "
                "erased");
    }
  }
  rm_vec_free(&before);
  rm_vec_free(&after);
  rm_page_free(page);
  return status;
}

/* The identifiers asked for and not found, spelled out as far as the message
 * holds them. */
static void fail_not_found(rm_error* out_error, const rm_item_id* ids,
                           size_t id_count, const rm_idmap* erased,
                           size_t missing) {
  if (out_error == nullptr) return;
  char message[sizeof out_error->message] = "not on the page:";
  size_t length = strlen(message);
  const char* separator = " ";
  rm_idmap named;
  rm_idmap_init(&named);
  for (size_t i = 0; i < id_count; i += 1) {
    rm_crdt_id key = rm_crdt_of(ids[i]);
    if (rm_idmap_contains(erased, key) || rm_idmap_contains(&named, key))
      continue;
    /* A failed insert only means a name may be said twice. */
    (void)rm_idmap_set(&named, key, 1);
    char spelled[32];
    rm_item_id_format(ids[i], spelled, sizeof spelled);
    /* Room kept for the ", ..." that says the list goes on. */
    if (length + strlen(separator) + strlen(spelled) + 6u >= sizeof message) {
      snprintf(message + length, sizeof message - length, ", ...");
      break;
    }
    snprintf(message + length, sizeof message - length, "%s%s", separator,
             spelled);
    length = strlen(message);
    separator = ", ";
  }
  rm_idmap_free(&named);
  fail_with(out_error, RM_ERR_ITEMS_NOT_FOUND, (long)missing, message);
}

rm_status rm_erase_items(const uint8_t* data, size_t size,
                         const rm_item_id* ids, size_t id_count,
                         uint8_t** out_bytes, size_t* out_size,
                         rm_error* out_error) {
  if (out_error != nullptr) *out_error = (rm_error){.status = RM_OK};
  if (data == nullptr || out_bytes == nullptr || out_size == nullptr ||
      (ids == nullptr && id_count != 0)) {
    fail_with(out_error, RM_ERR_INVALID_ARGUMENT, 0, nullptr);
    return RM_ERR_INVALID_ARGUMENT;
  }

  /* Nothing asked, nothing changed — and nothing read either. */
  if (id_count == 0) {
    uint8_t* copy = malloc(size == 0 ? 1 : size);
    if (copy == nullptr) {
      fail_with(out_error, RM_ERR_OUT_OF_MEMORY, 0, nullptr);
      return RM_ERR_OUT_OF_MEMORY;
    }
    if (size != 0) memcpy(copy, data, size);
    *out_bytes = copy;
    *out_size = size;
    return RM_OK;
  }

  rm_page* original = nullptr;
  RM_TRY(rm_parse_page(data, size, &original, out_error));

  rm_status status = RM_ERR_OUT_OF_MEMORY;
  rm_idmap wanted, erased;
  rm_idmap_init(&wanted);
  rm_idmap_init(&erased);
  rm_buf output;
  rm_buf_init(&output);
  size_t wanted_count = 0;
  for (size_t i = 0; i < id_count; i += 1) {
    rm_crdt_id key = rm_crdt_of(ids[i]);
    if (rm_idmap_contains(&wanted, key)) continue;
    if (!rm_idmap_set(&wanted, key, 1)) goto fail;
    wanted_count += 1;
  }

  /* The parse above has checked the header; it is copied as it stands. */
  rm_reader reader;
  rm_reader_init(&reader, data, size);
  reader.offset = RM_HEADER_LENGTH;
  rm_buf_append(&output, data, RM_HEADER_LENGTH);

  while (rm_reader_remaining(&reader) >= 8) {
    /* The parse above accepted every envelope, so this cannot fail on a
     * page that reached here; it is checked all the same. */
    rm_block_header block;
    status = rm_read_block_header(&reader, &block);
    if (status != RM_OK) goto fail;

    rm_crdt_id id;
    size_t deleted_at;
    rm_reader inner = reader;
    inner.size = block.end;
    if ((block.type == RM_BLOCK_SCENE_LINE_ITEM ||
         block.type == RM_BLOCK_SCENE_GLYPH_ITEM) &&
        living_item(&inner, &wanted, &id, &deleted_at)) {
      status = RM_ERR_OUT_OF_MEMORY;
      if (!rm_idmap_set(&erased, id, 1)) goto fail;
      size_t kept = deleted_at - block.body;
      rm_write_u32(&output, (uint32_t)(kept + 4u));
      const uint8_t header[] = {block.unknown, block.min_version,
                                block.current_version, block.type};
      rm_buf_append(&output, header, sizeof header);
      rm_buf_append(&output, data + block.body, kept);
      rm_write_u32(&output, 1);
    } else {
      rm_buf_append(&output, data + block.start, block.end - block.start);
    }
    reader.offset = block.end;
  }
  /* Fewer bytes than a block header: not a block, and not ours to drop. */
  rm_buf_append(&output, data + reader.offset, rm_reader_remaining(&reader));
  status = RM_ERR_OUT_OF_MEMORY;
  if (output.failed) goto fail;

  if (erased.count < wanted_count) {
    status = RM_ERR_ITEMS_NOT_FOUND;
    fail_not_found(out_error, ids, id_count, &erased,
                   wanted_count - erased.count);
    goto cleanup;
  }

  status = verify(output.bytes, output.count, original, &erased, out_error);
  if (status != RM_OK) goto cleanup;

  size_t written = output.count;
  uint8_t* bytes = rm_buf_take(&output, nullptr);
  if (bytes == nullptr) {
    status = RM_ERR_OUT_OF_MEMORY;
    goto fail;
  }
  *out_bytes = bytes;
  *out_size = written;
  status = RM_OK;
  goto cleanup;

fail:
  fail_with(out_error, status, 0, nullptr);
cleanup:
  rm_buf_free(&output);
  rm_idmap_free(&erased);
  rm_idmap_free(&wanted);
  rm_page_free(original);
  return status;
}

rm_status rm_highlighter_items(const uint8_t* data, size_t size,
                               rm_item_id** out_ids, size_t* out_count,
                               rm_error* out_error) {
  if (out_error != nullptr) *out_error = (rm_error){.status = RM_OK};
  if (data == nullptr || out_ids == nullptr || out_count == nullptr) {
    fail_with(out_error, RM_ERR_INVALID_ARGUMENT, 0, nullptr);
    return RM_ERR_INVALID_ARGUMENT;
  }
  rm_vec ids;
  rm_vec_init(&ids, sizeof(rm_item_id));
  rm_status status = rm_scan_highlighter_items(data, size, &ids, out_error);
  if (status != RM_OK) {
    rm_vec_free(&ids);
    return status;
  }
  *out_ids = ids.count == 0 ? nullptr : ids.data;
  *out_count = ids.count;
  if (ids.count == 0) rm_vec_free(&ids);
  return RM_OK;
}

void rm_item_ids_free(rm_item_id* ids) { free(ids); }
