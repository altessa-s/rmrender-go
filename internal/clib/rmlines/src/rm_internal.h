/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* The plumbing the parser and the writer are built out of: an arena, two
 * growable buffers, a UTF-8 decoder that repairs by the Unicode "maximal
 * subpart" rule, a hash map on CRDT identifiers, and the byte-level reader and
 * writer of the format itself.
 *
 * Internal: nothing here is in the installed headers, and no symbol below is
 * exported from the shared library. */

#ifndef RM_INTERNAL_H
#define RM_INTERNAL_H

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rmlines/log.h"
#include "rmlines/types.h"

/* Return from the enclosing function on the first failure. The parser is a
 * long sequence of reads that each may run off the end, and spelling the
 * check out at every one of them would bury the shape of the format. */
#define RM_TRY(expr)                                    \
  do {                                                  \
    rm_status rm_try_status_ = (expr);                  \
    if (rm_try_status_ != RM_OK) return rm_try_status_; \
  } while (false)

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

/* A bump allocator in chunks. A parsed page is a graph of small arrays that
 * all live and die together, so it is one allocation policy and one free —
 * and a caller who loses track of a stroke's points cannot free them out from
 * under the page. */
typedef struct rm_arena rm_arena;

/* A fresh, empty arena, or null on allocation failure. */
rm_arena* rm_arena_create(void);

/* Free an arena and everything allocated from it. Null is accepted and
 * ignored. */
void rm_arena_destroy(rm_arena* arena);

/* Aligned to `max_align_t`, zeroed nowhere: callers that need zeroes ask. */
void* rm_arena_alloc(rm_arena* arena, size_t size);

/* Like `rm_arena_alloc`, zeroed, and null without allocating on an overflow of
 * `count * size`. */
void* rm_arena_calloc(rm_arena* arena, size_t count, size_t size);

#ifdef RMLINES_TESTING
/* Fail one arena allocation after this many successes; SIZE_MAX disables it. */
void rm_test_arena_fail_after(size_t count);

/* Whether the counted allocation has failed since the last `_fail_after`. */
bool rm_test_arena_did_fail(void);
#endif

/* A copy of `length` bytes with a NUL behind them, so the result reaches
 * `printf` unchanged. */
char* rm_arena_strndup(rm_arena* arena, const char* bytes, size_t length);

/* ------------------------------------------------------------------ */
/* Growable arrays                                                     */
/* ------------------------------------------------------------------ */

/* Scratch storage for a parse: the counts are the file's to choose, so the
 * arrays grow, and what survives is copied into the page's arena at the end.
 * Backed by malloc rather than by the arena because an arena cannot
 * reallocate. */
typedef struct rm_vec {
  void* data;
  size_t count;
  size_t capacity;
  size_t elem;
} rm_vec;

/* Ready an empty vector of elements `elem_size` bytes wide. */
void rm_vec_init(rm_vec* vec, size_t elem_size);

/* Release the backing storage and leave the vector empty. */
void rm_vec_free(rm_vec* vec);

/* False on allocation failure, and then the vector is untouched. */
[[nodiscard]] bool rm_vec_push(rm_vec* vec, const void* element);

/* Grow the backing storage to hold at least `capacity` elements. False and
 * the vector untouched on allocation failure. */
[[nodiscard]] bool rm_vec_reserve(rm_vec* vec, size_t capacity);

/* The element at `index`, unchecked. */
void* rm_vec_at(const rm_vec* vec, size_t index);

/* A growable byte buffer: what the writer builds a block in. */
typedef struct rm_buf {
  uint8_t* bytes;
  size_t count;
  size_t capacity;
  /* Sticky: once an allocation has failed the buffer stops growing and says
   * so here, so that a long chain of appends is checked once at the end
   * instead of at every call. */
  bool failed;
} rm_buf;

/* Ready an empty buffer. */
void rm_buf_init(rm_buf* buf);

/* Release the backing storage and leave the buffer empty. */
void rm_buf_free(rm_buf* buf);

/* Append `length` bytes, or mark the buffer failed on allocation failure. */
void rm_buf_append(rm_buf* buf, const void* bytes, size_t length);

/* Append one byte, or mark the buffer failed on allocation failure. */
void rm_buf_append_byte(rm_buf* buf, uint8_t byte);

/* Hand the bytes over to the caller and leave the buffer empty. Null when the
 * buffer had failed. */
uint8_t* rm_buf_take(rm_buf* buf, size_t* out_size);

/* ------------------------------------------------------------------ */
/* UTF-8                                                               */
/* ------------------------------------------------------------------ */

/* How many Unicode scalars `length` bytes of **valid** UTF-8 hold. */
size_t rm_utf8_scalar_count(const char* bytes, size_t length);

/* Copy `length` bytes into `out`, replacing every ill-formed sequence with
 * U+FFFD, and answer how many bytes were written.
 *
 * The replacement is the Unicode "maximal subpart" rule, which conformant
 * decoders follow — and matching it is not
 * cosmetic: the file numbers characters by scalar, so a byte sequence counted
 * differently here shifts every anchor and every style key behind it.
 *
 * `out` must hold at least `rm_utf8_repair_bound(length)` bytes. */
size_t rm_utf8_repair(const char* bytes, size_t length, char* out);

/* Three bytes out for every byte in is the worst case: a run of continuation
 * bytes is one U+FFFD each. */
size_t rm_utf8_repair_bound(size_t length);

/* Whether the scalar starting at `offset` is a newline, and where the next
 * one begins. The bytes must be valid UTF-8 — `rm_utf8_repair` has run. */
size_t rm_utf8_next(const char* bytes, size_t length, size_t offset,
                    uint32_t* out_scalar);

/* ------------------------------------------------------------------ */
/* Identifiers                                                         */
/* ------------------------------------------------------------------ */

/* The identifier of a scene element: the author of the edit and its counter. */
typedef struct rm_crdt_id {
  uint64_t counter;
  uint8_t author;
} rm_crdt_id;

/* Whether the two identifiers name the same record. */
bool rm_crdt_equal(rm_crdt_id a, rm_crdt_id b);

/* An identifier out of its two parts. */
rm_crdt_id rm_crdt_make(uint8_t author, uint64_t counter);

constexpr rm_crdt_id RM_CRDT_NONE = {.counter = 0, .author = 0};

/* An open-addressing map from an identifier to a 64-bit value — an index into
 * an array of items, a count, or a set's presence.
 *
 * A map and not a sorted array because the parse asks it O(n) times: the
 * children of a node, the bucket of a `left` link, the place of a character.
 * At a few thousand records a linear scan per question is the parse itself
 * hanging. */
typedef struct rm_idmap {
  struct rm_idmap_slot* slots;
  size_t capacity; /* a power of two, or zero */
  size_t count;
} rm_idmap;

/* Ready an empty map. */
void rm_idmap_init(rm_idmap* map);

/* Release the backing storage and leave the map empty. */
void rm_idmap_free(rm_idmap* map);

/* Set or replace the value under `key`. False on allocation failure, and
 * then the map is untouched. */
[[nodiscard]] bool rm_idmap_set(rm_idmap* map, rm_crdt_id key, uint64_t value);

/* The value under `key`, or false when there is none. */
bool rm_idmap_get(const rm_idmap* map, rm_crdt_id key, uint64_t* out_value);

/* Whether `key` is in the map. */
bool rm_idmap_contains(const rm_idmap* map, rm_crdt_id key);

/* Take the entry out and answer what it held. Used, among other things, to
 * mark a bucket spent. */
bool rm_idmap_take(rm_idmap* map, rm_crdt_id key, uint64_t* out_value);

/* Walk every entry. `*cursor` starts at 0 and is opaque. */
bool rm_idmap_next(const rm_idmap* map, size_t* cursor, rm_crdt_id* out_key,
                   uint64_t* out_value);

/* A map from a 16-byte opaque key to a 64-bit value.
 *
 * The attachment keys are not CRDT identifiers and do not fit `rm_idmap`:
 * they are sixteen opaque bytes the document's index hands out. A map and not
 * a scan for the same reason the identifiers have one — the page asks it once
 * per attachment while the list is being built and once per image afterwards,
 * and the number of attachments is the file's to choose. */
typedef struct rm_keymap {
  struct rm_keymap_slot* slots;
  size_t capacity; /* a power of two, or zero */
  size_t count;
} rm_keymap;

/* Ready an empty map. */
void rm_keymap_init(rm_keymap* map);

/* Release the backing storage and leave the map empty. */
void rm_keymap_free(rm_keymap* map);

/* Set or replace the value under `key`. False on allocation failure, and
 * then the map is untouched. */
[[nodiscard]] bool rm_keymap_set(rm_keymap* map, const uint8_t key[16],
                                 uint64_t value);

/* The value under `key`, or false when there is none. */
bool rm_keymap_get(const rm_keymap* map, const uint8_t key[16],
                   uint64_t* out_value);

/* ------------------------------------------------------------------ */
/* Grouping                                                            */
/* ------------------------------------------------------------------ */

/* Items grouped by an identifier, in a compressed-row layout: one array of
 * item indexes and, per group, where that group's run begins and ends.
 *
 * Order inside a group is the order the items were seen, and the chain walk
 * depends on it, because file order is the tie-break of last resort. */
typedef struct rm_grouping {
  /* key -> group number */
  rm_idmap index;
  /* group number -> [start, end) in `items` */
  uint32_t* starts;
  uint32_t* ends;
  uint32_t* items;
  size_t group_count;
  size_t item_count;
} rm_grouping;

/* Group `count` items, `key_of(i, ctx)` naming each one's group. */
[[nodiscard]] bool rm_grouping_build(rm_grouping* grouping, size_t count,
                                     rm_crdt_id (*key_of)(size_t index,
                                                          void* ctx),
                                     void* ctx);

/* Release the grouping and everything built for it. */
void rm_grouping_free(rm_grouping* grouping);

/* The run of items in the group named by `key`, or false when there is none. */
bool rm_grouping_find(const rm_grouping* grouping, rm_crdt_id key,
                      const uint32_t** out_items, size_t* out_count);

/* The group's number, for the callers that keep a cursor or a spent flag per
 * group rather than per key. */
bool rm_grouping_number(const rm_grouping* grouping, rm_crdt_id key,
                        size_t* out_number);

/* ------------------------------------------------------------------ */
/* The format's tags                                                   */
/* ------------------------------------------------------------------ */

enum rm_tag_type {
  RM_TAG_ID = 0xF,
  RM_TAG_LENGTH4 = 0xC,
  RM_TAG_BYTE8 = 0x8,
  RM_TAG_BYTE4 = 0x4,
  RM_TAG_BYTE1 = 0x1,
};

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

/* Low-level reading of the `.rm` format: little-endian numbers, varuints,
 * tags and subblocks. The bytes are read in place and never copied: a
 * notebook is parsed page by page in a loop, and a copy would be a per-page
 * allocation for nothing. */
typedef struct rm_reader {
  const uint8_t* data;
  size_t size;
  size_t offset;
} rm_reader;

/* Ready a reader positioned at the start of `data`. */
void rm_reader_init(rm_reader* reader, const uint8_t* data, size_t size);

/* How many bytes stand between the position and the reader's own end. */
size_t rm_reader_remaining(const rm_reader* reader);

/* Where `length` bytes starting at the position end, when they fit inside
 * `limit`, and false when they do not.
 *
 * The sum is formed only once it is known to fit. Written the other way round
 * — add, then compare — an offset and a length that are each in range can
 * still carry past what a `size_t` holds on a 32-bit machine, and the wrapped
 * end passes every bound below it. Where such an end is seeked to, it walks
 * the reader backwards, and the loop that reads fields until the block ends
 * does not terminate. */
bool rm_fits_within(const rm_reader* reader, size_t length, size_t limit,
                    size_t* out_end);

/* Move the position to `position`. RM_ERR_UNEXPECTED_EOF when it lies past
 * the reader's own end. */
[[nodiscard]] rm_status rm_read_seek(rm_reader* reader, size_t position);

/* One byte. */
[[nodiscard]] rm_status rm_read_u8(rm_reader* reader, uint8_t* out);

/* Two little-endian bytes. */
[[nodiscard]] rm_status rm_read_u16(rm_reader* reader, uint16_t* out);

/* Four little-endian bytes. */
[[nodiscard]] rm_status rm_read_u32(rm_reader* reader, uint32_t* out);

/* Four little-endian bytes, as the float32 they are the bit pattern of. */
[[nodiscard]] rm_status rm_read_f32(rm_reader* reader, float* out);

/* Eight little-endian bytes, as the float64 they are the bit pattern of. */
[[nodiscard]] rm_status rm_read_f64(rm_reader* reader, double* out);

/* `out` points into the source bytes and lives as long as they do. */
[[nodiscard]] rm_status rm_read_bytes(rm_reader* reader, size_t length,
                                      const uint8_t** out);

/* Read a length4 body after its tag. The child cannot read past that body;
 * the parent advances to its end, including any padding the child ignores. */
[[nodiscard]] rm_status rm_read_subblock(rm_reader* reader, rm_reader* out);

/* The envelope every top-level block comes in: the payload's length, an
 * unknown byte, the two versions and the type. */
typedef struct rm_block_header {
  size_t start; /* where the envelope begins */
  size_t body;  /* where the payload begins */
  size_t end;   /* where the payload ends */
  uint8_t unknown;
  uint8_t min_version;
  uint8_t current_version;
  uint8_t type;
} rm_block_header;

/* Read one envelope and leave the reader at the payload. A payload that
 * would run past the reader's end is RM_ERR_UNEXPECTED_EOF: one reading of
 * the envelope for the parser and the editor both, so the editor can never
 * walk a page differently from the parse that accepted it. */
[[nodiscard]] rm_status rm_read_block_header(rm_reader* reader,
                                             rm_block_header* out);

/* A variable-length integer: 7 bits per byte, the top bit meaning "more to
 * come". A number too wide for 64 bits is refused rather than quietly
 * rewritten into a different one. */
[[nodiscard]] rm_status rm_read_varuint(rm_reader* reader, uint64_t* out);

/* A count the file declares, as a `size_t`. A varuint is 64 bits wide and a
 * count is an index into memory; a value above what the machine can address
 * is an error rather than a wrap. */
[[nodiscard]] rm_status rm_read_count(rm_reader* reader, size_t* out);

/* A tag: a varuint whose upper bits are the field index and lower bits the
 * value type. */
[[nodiscard]] rm_status rm_read_tag(rm_reader* reader, uint32_t* out_index,
                                    uint8_t* out_type);

/* Check that the expected tag comes next without moving the position on a
 * mismatch: the format has optional fields.
 *
 * A question, not a read: a tag that is not there, and bytes that are not a
 * tag at all, are both answered false with the position restored. */
bool rm_match_tag(rm_reader* reader, uint32_t index, uint8_t type);

/* Like `rm_match_tag`, but RM_ERR_MALFORMED_PAGE instead of false. */
[[nodiscard]] rm_status rm_expect_tag(rm_reader* reader, uint32_t index,
                                      uint8_t type);

/* A CRDT identifier: a byte plus a varuint. We have no use for most of them,
 * but skipping one correctly is essential — otherwise the whole parse slides
 * out of place. */
[[nodiscard]] rm_status rm_read_crdt(rm_reader* reader, uint32_t index,
                                     rm_crdt_id* out);

/* Like `rm_read_crdt`, the value thrown away. */
[[nodiscard]] rm_status rm_skip_crdt(rm_reader* reader, uint32_t index);

/* An identifier written with no tag in front of it. The list of paragraph
 * styles is keyed that way: the key stands bare, and only the fields after it
 * carry tags. */
[[nodiscard]] rm_status rm_read_bare_crdt(rm_reader* reader, rm_crdt_id* out);

/* A string as the format spells one: a length in **bytes**, a one-byte
 * encoding marker, then the bytes themselves.
 *
 * The length is a byte count and not a count of characters, whatever the
 * field is called elsewhere: outside ASCII the two are different numbers. In
 * one place because it is read in three — the typed text, a highlight's
 * words, an attachment's file name — and three copies of this are three
 * chances to get that wrong in one of them.
 *
 * `end` bounds the read. The length in front of the bytes is the file's word
 * and not a measurement, so it is clamped to what the field it sits in can
 * actually hold. Nothing left to read is an empty string and not an error —
 * an empty record is how the format fences a bold stretch.
 *
 * The result is copied into `arena` with ill-formed UTF-8 repaired, and the
 * position is left where the bytes ended. */
[[nodiscard]] rm_status rm_read_string(rm_reader* reader, size_t end,
                                       rm_arena* arena, rm_str* out);

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

/* An identifier as the writer hands one out. */
typedef rm_crdt_id rm_wid;

/* Two bytes, little-endian. */
void rm_write_u16(rm_buf* buf, uint16_t value);

/* Four bytes, little-endian. */
void rm_write_u32(rm_buf* buf, uint32_t value);

/* The float32's bit pattern, little-endian. */
void rm_write_f32(rm_buf* buf, float value);

/* The float64's bit pattern, little-endian. */
void rm_write_f64(rm_buf* buf, double value);

/* A varuint: 7 bits per byte, the top bit saying "more to come". */
void rm_write_varuint(rm_buf* buf, uint64_t value);

/* A tag: the field index in the upper bits, the value type in the lower. */
void rm_write_tag(rm_buf* buf, uint32_t index, uint8_t type);

/* A tagged identifier: the tag, then the author and counter. */
void rm_write_id(rm_buf* buf, uint32_t index, rm_wid value);

/* An identifier with no tag in front of it: the author, then the counter. */
void rm_write_bare_id(rm_buf* buf, rm_wid value);

/* A string as the format spells one: the length in bytes, the encoding
 * marker, then the bytes. */
void rm_write_string(rm_buf* buf, const char* text);

/* A tagged subblock: the tag, the length of what follows, and the bytes. */
void rm_write_block(rm_buf* buf, uint32_t index, const uint8_t* body,
                    size_t length);

/* A last-writer-wins field: who set the value, then the value.
 *
 * A typed page carries six of them, in three shapes — a byte, a float, a
 * subblock — and only the shape of the value differs. Written out each time,
 * the stamp would be the field easiest to leave at the wrong id. */
void rm_write_stamped_u8(rm_buf* buf, uint32_t index, rm_wid stamp,
                         uint8_t value);

void rm_write_stamped_f32(rm_buf* buf, uint32_t index, rm_wid stamp,
                          float value);

void rm_write_stamped_block(rm_buf* buf, uint32_t index, rm_wid stamp,
                            const uint8_t* body, size_t length);

/* A top-level block: the 8-byte header, then the payload. The versions
 * default to what the tablet writes for most of its blocks; the two blocks
 * that carry other numbers say so at the call. */
void rm_write_top_block(rm_buf* file, uint8_t type, uint8_t min_version,
                        uint8_t current_version, const uint8_t* payload,
                        size_t length);

/* ------------------------------------------------------------------ */
/* What the parser shares with the editor                              */
/* ------------------------------------------------------------------ */

/* The two block types an erase rewrites: a stroke and a highlight over the
 * text of an imported PDF. */
constexpr uint8_t RM_BLOCK_SCENE_LINE_ITEM = 0x05;
constexpr uint8_t RM_BLOCK_SCENE_GLYPH_ITEM = 0x03;

/* The public spelling of an internal identifier. */
rm_item_id rm_item_id_of(rm_crdt_id id);

/* The internal spelling of a public identifier. */
rm_crdt_id rm_crdt_of(rm_item_id id);

/* The envelope every scene item starts with: the parent node, the item's own
 * identifier, its neighbours in the parent's list, and the count of what was
 * deleted here. The deleted count is the last four bytes it reads. */
typedef struct rm_item_preamble {
  rm_crdt_id parent;
  rm_crdt_id id;
  rm_crdt_id left;
  uint32_t deleted;
} rm_item_preamble;

/* Read one item's preamble. */
[[nodiscard]] rm_status rm_parse_item_preamble(rm_reader* reader,
                                               rm_item_preamble* out);

/* Every living mark of the highlighter on a page, into `out_ids`
 * (`rm_item_id`): the parser's block pass without the assembly, so that the
 * hidden layers the page leaves out are in it with the rest. The statuses are
 * those of `rm_parse_page`. */
[[nodiscard]] rm_status rm_scan_highlighter_items(const uint8_t* data,
                                                  size_t size, rm_vec* out_ids,
                                                  rm_error* out_error);

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

/* The message is built only when a sink is installed, which is what makes a
 * debug line on a hot parsing path free. */
bool rm_log_wanted(void);

/* `format(printf, ...)` is a GNU spelling. MSVC has no `__attribute__` at all
 * — unguarded it is a syntax error, not a warning — and the library declares
 * Windows a target in `export.h` and in the build. */
#if defined(__GNUC__) || defined(__clang__)
#define RM_PRINTF(format_index, first_argument) \
  __attribute__((format(printf, format_index, first_argument)))
#else
#define RM_PRINTF(format_index, first_argument)
#endif

/* Format a line and hand it to the installed sink. A no-op when none is
 * installed — callers guard with `rm_log_wanted` so the formatting itself is
 * skipped on the common path. */
void rm_log_writef(rm_log_level level, const char* format, ...) RM_PRINTF(2, 3);

#define RM_LOG_ERRORF(...)                                         \
  do {                                                             \
    if (rm_log_wanted()) rm_log_writef(RM_LOG_ERROR, __VA_ARGS__); \
  } while (false)
#define RM_LOG_DEBUGF(...)                                         \
  do {                                                             \
    if (rm_log_wanted()) rm_log_writef(RM_LOG_DEBUG, __VA_ARGS__); \
  } while (false)

#endif /* RM_INTERNAL_H */
