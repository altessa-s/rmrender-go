/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* The byte level of the format, both ways: little-endian scalars, varuints,
 * tags, identifiers and tagged subblocks.
 *
 * Every byte the writing half puts down is one the tablet itself writes, and
 * nothing below may change one of them unless the tablet is shown to accept
 * the change. */

#include <stdint.h>
#include <string.h>

#include "rm_internal.h"

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

void rm_reader_init(rm_reader* reader, const uint8_t* data, size_t size) {
  *reader = (rm_reader){.data = data, .size = size, .offset = 0};
}

rm_status rm_read_seek(rm_reader* reader, size_t position) {
  if (position > reader->size) return RM_ERR_UNEXPECTED_EOF;
  reader->offset = position;
  return RM_OK;
}

/* Claim `length` bytes and answer where they begin, so that a number is read
 * out of the bytes where they lie: a point costs four to six of these reads,
 * and a copy per read would be an allocation per coordinate of every stroke. */
static rm_status rm_advance(rm_reader* reader, size_t length,
                            const uint8_t** out) {
  if (length > reader->size - reader->offset) return RM_ERR_UNEXPECTED_EOF;
  *out = reader->data + reader->offset;
  reader->offset += length;
  return RM_OK;
}

rm_status rm_read_bytes(rm_reader* reader, size_t length, const uint8_t** out) {
  return rm_advance(reader, length, out);
}

rm_status rm_read_subblock(rm_reader* reader, rm_reader* out) {
  uint32_t length;
  RM_TRY(rm_read_u32(reader, &length));
  size_t end;
  if (!rm_fits_within(reader, length, reader->size, &end))
    return RM_ERR_MALFORMED_PAGE;
  *out = *reader;
  out->size = end;
  reader->offset = end;
  return RM_OK;
}

rm_status rm_read_block_header(rm_reader* reader, rm_block_header* out) {
  out->start = reader->offset;
  uint32_t length;
  RM_TRY(rm_read_u32(reader, &length));
  RM_TRY(rm_read_u8(reader, &out->unknown));
  RM_TRY(rm_read_u8(reader, &out->min_version));
  RM_TRY(rm_read_u8(reader, &out->current_version));
  RM_TRY(rm_read_u8(reader, &out->type));
  out->body = reader->offset;
  if (!rm_fits_within(reader, length, reader->size, &out->end))
    return RM_ERR_UNEXPECTED_EOF;
  return RM_OK;
}

rm_status rm_read_u8(rm_reader* reader, uint8_t* out) {
  const uint8_t* at;
  RM_TRY(rm_advance(reader, 1, &at));
  *out = at[0];
  return RM_OK;
}

rm_status rm_read_u16(rm_reader* reader, uint16_t* out) {
  const uint8_t* at;
  RM_TRY(rm_advance(reader, 2, &at));
  *out = (uint16_t)((uint16_t)at[0] | (uint16_t)at[1] << 8);
  return RM_OK;
}

rm_status rm_read_u32(rm_reader* reader, uint32_t* out) {
  const uint8_t* at;
  RM_TRY(rm_advance(reader, 4, &at));
  *out = (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 |
         (uint32_t)at[3] << 24;
  return RM_OK;
}

rm_status rm_read_f32(rm_reader* reader, float* out) {
  uint32_t pattern;
  RM_TRY(rm_read_u32(reader, &pattern));
  static_assert(sizeof(float) == sizeof(uint32_t), "a float32 is four bytes");
  memcpy(out, &pattern, sizeof pattern);
  return RM_OK;
}

rm_status rm_read_f64(rm_reader* reader, double* out) {
  const uint8_t* at;
  RM_TRY(rm_advance(reader, 8, &at));
  uint64_t pattern = 0;
  for (int i = 0; i < 8; i += 1) pattern |= (uint64_t)at[i] << (8 * i);
  static_assert(sizeof(double) == sizeof(uint64_t), "a float64 is eight bytes");
  memcpy(out, &pattern, sizeof pattern);
  return RM_OK;
}

rm_status rm_read_varuint(rm_reader* reader, uint64_t* out) {
  uint64_t result = 0;
  unsigned shift = 0;
  for (;;) {
    uint8_t byte;
    RM_TRY(rm_read_u8(reader, &byte));
    /* The tenth byte has one bit's worth of room left. A shift drops
     * whatever does not fit without a word, so a number too wide for the
     * type would arrive as a different number — refused instead of
     * quietly rewritten. */
    if (shift == 63 && (byte & 0x7Eu) != 0) return RM_ERR_MALFORMED_PAGE;
    result |= (uint64_t)(byte & 0x7Fu) << shift;
    shift += 7;
    if ((byte & 0x80u) == 0) break;
    if (shift >= 64) return RM_ERR_MALFORMED_PAGE;
  }
  *out = result;
  return RM_OK;
}

rm_status rm_read_count(rm_reader* reader, size_t* out) {
  uint64_t value;
  RM_TRY(rm_read_varuint(reader, &value));
  /* Refused against PTRDIFF_MAX and not against SIZE_MAX, which on a 64-bit
   * machine would let every varuint through. A count is an index into
   * memory, and a value no object can hold is not a count — it is a
   * corrupted field, and saying so here is what makes the list parsers fail
   * as a block they could not read rather than quietly return the empty
   * list they had got as far as. */
  if (value > (uint64_t)PTRDIFF_MAX) return RM_ERR_MALFORMED_PAGE;
  *out = (size_t)value;
  return RM_OK;
}

rm_status rm_read_tag(rm_reader* reader, uint32_t* out_index,
                      uint8_t* out_type) {
  uint64_t raw;
  RM_TRY(rm_read_varuint(reader, &raw));
  uint64_t index = raw >> 4;
  /* A field index past what a 32-bit number holds is still a tag: it names
   * no field anybody reads, and a loop over a
   * block's fields steps over it like any other unknown one. Held at the top
   * of the range, which no field of the format comes near — its own run to
   * single digits. */
  *out_index = index > UINT32_MAX ? UINT32_MAX : (uint32_t)index;
  *out_type = (uint8_t)(raw & 0xFu);
  return RM_OK;
}

bool rm_match_tag(rm_reader* reader, uint32_t index, uint8_t type) {
  size_t saved = reader->offset;
  if (rm_reader_remaining(reader) == 0) return false;
  uint32_t found_index;
  uint8_t found_type;
  if (rm_read_tag(reader, &found_index, &found_type) == RM_OK &&
      found_index == index && found_type == type) {
    return true;
  }
  reader->offset = saved;
  return false;
}

rm_status rm_expect_tag(rm_reader* reader, uint32_t index, uint8_t type) {
  return rm_match_tag(reader, index, type) ? RM_OK : RM_ERR_MALFORMED_PAGE;
}

rm_status rm_read_bare_crdt(rm_reader* reader, rm_crdt_id* out) {
  uint8_t author;
  uint64_t counter;
  RM_TRY(rm_read_u8(reader, &author));
  RM_TRY(rm_read_varuint(reader, &counter));
  *out = rm_crdt_make(author, counter);
  return RM_OK;
}

rm_status rm_read_crdt(rm_reader* reader, uint32_t index, rm_crdt_id* out) {
  RM_TRY(rm_expect_tag(reader, index, RM_TAG_ID));
  return rm_read_bare_crdt(reader, out);
}

rm_status rm_skip_crdt(rm_reader* reader, uint32_t index) {
  rm_crdt_id ignored;
  return rm_read_crdt(reader, index, &ignored);
}

rm_status rm_read_string(rm_reader* reader, size_t end, rm_arena* arena,
                         rm_str* out) {
  if (end > reader->size || reader->offset > end) return RM_ERR_MALFORMED_PAGE;
  rm_reader bounded = *reader;
  bounded.size = end;
  rm_reader* parent = reader;
  reader = &bounded;
  *out = (rm_str){.bytes = "", .length = 0};
  if (reader->offset >= end) return RM_OK;

  size_t byte_count;
  RM_TRY(rm_read_count(reader, &byte_count));
  if (reader->offset >= end) {
    parent->offset = reader->offset;
    return RM_OK;
  }
  uint8_t marker;
  RM_TRY(rm_read_u8(reader, &marker)); /* encoding marker */

  size_t room = end - reader->offset;
  size_t available = byte_count < room ? byte_count : room;
  const uint8_t* bytes;
  RM_TRY(rm_read_bytes(reader, available, &bytes));

  char* repaired = rm_arena_alloc(arena, rm_utf8_repair_bound(available));
  if (repaired == nullptr) return RM_ERR_OUT_OF_MEMORY;
  size_t length = rm_utf8_repair((const char*)bytes, available, repaired);
  *out = (rm_str){.bytes = repaired, .length = length};
  parent->offset = reader->offset;
  return RM_OK;
}

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

/* Whether a body fits behind the four-byte length prefix the format puts in
 * front of every block. Spelled as a function because on a machine whose
 * `size_t` is itself 32 bits the bound cannot be exceeded, and the comparison
 * written inline would be flagged as always false under `-Wextra`. */
static bool rm_length_fits_u32(size_t length) {
#if SIZE_MAX > UINT32_MAX
  return length <= UINT32_MAX;
#else
  (void)length;
  return true;
#endif
}

void rm_write_u16(rm_buf* buf, uint16_t value) {
  rm_buf_append_byte(buf, (uint8_t)(value & 0xFFu));
  rm_buf_append_byte(buf, (uint8_t)(value >> 8));
}

void rm_write_u32(rm_buf* buf, uint32_t value) {
  for (unsigned shift = 0; shift < 32u; shift += 8u) {
    rm_buf_append_byte(buf, (uint8_t)((value >> shift) & 0xFFu));
  }
}

void rm_write_f32(rm_buf* buf, float value) {
  uint32_t pattern;
  memcpy(&pattern, &value, sizeof pattern);
  rm_write_u32(buf, pattern);
}

void rm_write_f64(rm_buf* buf, double value) {
  uint64_t pattern;
  memcpy(&pattern, &value, sizeof pattern);
  for (unsigned shift = 0; shift < 64u; shift += 8u) {
    rm_buf_append_byte(buf, (uint8_t)((pattern >> shift) & 0xFFu));
  }
}

void rm_write_varuint(rm_buf* buf, uint64_t value) {
  uint64_t rest = value;
  do {
    uint8_t byte = (uint8_t)(rest & 0x7Fu);
    rest >>= 7;
    if (rest != 0) byte |= 0x80u;
    rm_buf_append_byte(buf, byte);
  } while (rest != 0);
}

void rm_write_tag(rm_buf* buf, uint32_t index, uint8_t type) {
  rm_write_varuint(buf, (uint64_t)index << 4 | (uint64_t)type);
}

void rm_write_bare_id(rm_buf* buf, rm_wid value) {
  rm_buf_append_byte(buf, value.author);
  rm_write_varuint(buf, value.counter);
}

void rm_write_id(rm_buf* buf, uint32_t index, rm_wid value) {
  rm_write_tag(buf, index, RM_TAG_ID);
  rm_write_bare_id(buf, value);
}

void rm_write_string(rm_buf* buf, const char* text) {
  size_t length = text == nullptr ? 0 : strlen(text);
  rm_write_varuint(buf, (uint64_t)length);
  rm_buf_append_byte(buf, 1); /* encoding: UTF-8 */
  rm_buf_append(buf, text, length);
}

/* The length prefix of a subblock is four bytes wide. A body too large for it
 * is refused rather than written behind a truncated prefix: the bytes would
 * parse back as a block nobody wrote, and a writer that quietly produces a
 * different page than it was given is the one failure this one must not have.
 * Reported the way every other failure here is — on the buffer, checked once
 * at the end. */
void rm_write_block(rm_buf* buf, uint32_t index, const uint8_t* body,
                    size_t length) {
  if (!rm_length_fits_u32(length)) {
    buf->failed = true;
    return;
  }
  rm_write_tag(buf, index, RM_TAG_LENGTH4);
  rm_write_u32(buf, (uint32_t)length);
  rm_buf_append(buf, body, length);
}

void rm_write_stamped_u8(rm_buf* buf, uint32_t index, rm_wid stamp,
                         uint8_t value) {
  rm_buf body;
  rm_buf_init(&body);
  rm_write_id(&body, 1, stamp);
  rm_write_tag(&body, 2, RM_TAG_BYTE1);
  rm_buf_append_byte(&body, value);
  rm_write_block(buf, index, body.bytes, body.count);
  if (body.failed) buf->failed = true;
  rm_buf_free(&body);
}

void rm_write_stamped_f32(rm_buf* buf, uint32_t index, rm_wid stamp,
                          float value) {
  rm_buf body;
  rm_buf_init(&body);
  rm_write_id(&body, 1, stamp);
  rm_write_tag(&body, 2, RM_TAG_BYTE4);
  rm_write_f32(&body, value);
  rm_write_block(buf, index, body.bytes, body.count);
  if (body.failed) buf->failed = true;
  rm_buf_free(&body);
}

void rm_write_stamped_block(rm_buf* buf, uint32_t index, rm_wid stamp,
                            const uint8_t* value, size_t length) {
  rm_buf body;
  rm_buf_init(&body);
  rm_write_id(&body, 1, stamp);
  rm_write_block(&body, 2, value, length);
  rm_write_block(buf, index, body.bytes, body.count);
  if (body.failed) buf->failed = true;
  rm_buf_free(&body);
}

void rm_write_top_block(rm_buf* file, uint8_t type, uint8_t min_version,
                        uint8_t current_version, const uint8_t* payload,
                        size_t length) {
  /* The same four-byte prefix, and the same refusal. */
  if (!rm_length_fits_u32(length)) {
    file->failed = true;
    return;
  }
  rm_write_u32(file, (uint32_t)length);
  rm_buf_append_byte(file, 0);
  rm_buf_append_byte(file, min_version);
  rm_buf_append_byte(file, current_version);
  rm_buf_append_byte(file, type);
  rm_buf_append(file, payload, length);
}

size_t rm_reader_remaining(const rm_reader* reader) {
  return reader->size - reader->offset;
}

bool rm_fits_within(const rm_reader* reader, size_t length, size_t limit,
                    size_t* out_end) {
  if (reader->offset > limit || length > limit - reader->offset) return false;
  *out_end = reader->offset + length;
  return true;
}
