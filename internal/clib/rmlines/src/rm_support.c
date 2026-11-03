/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* The arena, the two buffers, the UTF-8 repair, the identifier map and the
 * grouping — everything the format's own code is built on and nothing of the
 * format itself. */

#include <stdlib.h>

#include "rm_internal.h"

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

typedef struct rm_arena_chunk {
  struct rm_arena_chunk* next;
  size_t used;
  size_t capacity;
  alignas(max_align_t) unsigned char data[];
} rm_arena_chunk;

struct rm_arena {
  rm_arena_chunk* head;
};

/* Big enough that an ordinary page is one or two chunks, small enough that a
 * page with nothing on it does not cost a megabyte. */
constexpr size_t RM_ARENA_CHUNK = 64u * 1024u;

rm_arena* rm_arena_create(void) {
  rm_arena* arena = calloc(1, sizeof *arena);
  return arena;
}

void rm_arena_destroy(rm_arena* arena) {
  if (arena == nullptr) return;
  rm_arena_chunk* chunk = arena->head;
  while (chunk != nullptr) {
    rm_arena_chunk* next = chunk->next;
    free(chunk);
    chunk = next;
  }
  free(arena);
}

/* False when rounding up would carry past what a `size_t` holds: the mask
 * below would then answer a number *smaller* than the request, and the caller
 * would hand out less memory than it was asked for without noticing. */
static bool rm_align_up(size_t value, size_t* out) {
  constexpr size_t align = alignof(max_align_t);
  if (value > SIZE_MAX - (align - 1u)) return false;
  *out = (value + (align - 1u)) & ~(align - 1u);
  return true;
}

#ifdef RMLINES_TESTING
static _Thread_local size_t rm_test_arena_remaining = SIZE_MAX;
static _Thread_local bool rm_test_arena_failed;

void rm_test_arena_fail_after(size_t count) {
  rm_test_arena_remaining = count;
  rm_test_arena_failed = false;
}

bool rm_test_arena_did_fail(void) { return rm_test_arena_failed; }
#endif

void* rm_arena_alloc(rm_arena* arena, size_t size) {
#ifdef RMLINES_TESTING
  if (rm_test_arena_remaining != SIZE_MAX) {
    if (rm_test_arena_remaining == 0) {
      rm_test_arena_remaining = SIZE_MAX;
      rm_test_arena_failed = true;
      return nullptr;
    }
    rm_test_arena_remaining -= 1;
  }
#endif
  if (arena == nullptr) return nullptr;
  if (size == 0) size = 1;
  if (!rm_align_up(size, &size)) return nullptr;

  rm_arena_chunk* head = arena->head;
  if (head == nullptr || head->capacity - head->used < size) {
    size_t capacity = size > RM_ARENA_CHUNK ? size : RM_ARENA_CHUNK;
    if (capacity > SIZE_MAX - sizeof(rm_arena_chunk)) return nullptr;
    rm_arena_chunk* chunk = malloc(sizeof(rm_arena_chunk) + capacity);
    if (chunk == nullptr) return nullptr;
    chunk->next = arena->head;
    chunk->used = 0;
    chunk->capacity = capacity;
    arena->head = chunk;
    head = chunk;
  }
  void* result = head->data + head->used;
  head->used += size;
  return result;
}

void* rm_arena_calloc(rm_arena* arena, size_t count, size_t size) {
  if (count != 0 && size > SIZE_MAX / count) return nullptr;
  size_t total = count * size;
  void* result = rm_arena_alloc(arena, total);
  if (result != nullptr) memset(result, 0, total == 0 ? 1 : total);
  return result;
}

char* rm_arena_strndup(rm_arena* arena, const char* bytes, size_t length) {
  char* copy = rm_arena_alloc(arena, length + 1u);
  if (copy == nullptr) return nullptr;
  if (length != 0) memcpy(copy, bytes, length);
  copy[length] = '\0';
  return copy;
}

/* ------------------------------------------------------------------ */
/* Growable arrays                                                     */
/* ------------------------------------------------------------------ */

void rm_vec_init(rm_vec* vec, size_t elem_size) {
  *vec = (rm_vec){.elem = elem_size};
}

void rm_vec_free(rm_vec* vec) {
  free(vec->data);
  vec->data = nullptr;
  vec->count = 0;
  vec->capacity = 0;
}

bool rm_vec_reserve(rm_vec* vec, size_t capacity) {
  if (capacity <= vec->capacity) return true;
  if (capacity > SIZE_MAX / vec->elem) return false;
  void* grown = realloc(vec->data, capacity * vec->elem);
  if (grown == nullptr) return false;
  vec->data = grown;
  vec->capacity = capacity;
  return true;
}

bool rm_vec_push(rm_vec* vec, const void* element) {
  if (vec->count == vec->capacity) {
    size_t next = vec->capacity == 0 ? 8u : vec->capacity * 2u;
    if (!rm_vec_reserve(vec, next)) return false;
  }
  memcpy(rm_vec_at(vec, vec->count), element, vec->elem);
  vec->count += 1;
  return true;
}

void rm_buf_init(rm_buf* buf) { *buf = (rm_buf){0}; }

void rm_buf_free(rm_buf* buf) {
  free(buf->bytes);
  *buf = (rm_buf){0};
}

/* Make room for `extra` more bytes, growing the capacity by doubling. False,
 * and the buffer marked failed, when the growth cannot be satisfied. */
static bool rm_buf_grow(rm_buf* buf, size_t extra) {
  if (buf->failed) return false;
  if (buf->capacity - buf->count >= extra) return true;
  size_t needed = buf->count + extra;
  if (needed < buf->count) {
    buf->failed = true;
    return false;
  }
  size_t capacity = buf->capacity == 0 ? 256u : buf->capacity;
  while (capacity < needed) {
    if (capacity > SIZE_MAX / 2u) {
      buf->failed = true;
      return false;
    }
    capacity *= 2u;
  }
  uint8_t* grown = realloc(buf->bytes, capacity);
  if (grown == nullptr) {
    buf->failed = true;
    return false;
  }
  buf->bytes = grown;
  buf->capacity = capacity;
  return true;
}

void rm_buf_append(rm_buf* buf, const void* bytes, size_t length) {
  if (length == 0) return;
  if (!rm_buf_grow(buf, length)) return;
  memcpy(buf->bytes + buf->count, bytes, length);
  buf->count += length;
}

void rm_buf_append_byte(rm_buf* buf, uint8_t byte) {
  if (!rm_buf_grow(buf, 1)) return;
  buf->bytes[buf->count++] = byte;
}

uint8_t* rm_buf_take(rm_buf* buf, size_t* out_size) {
  if (buf->failed) {
    rm_buf_free(buf);
    if (out_size != nullptr) *out_size = 0;
    return nullptr;
  }
  /* An empty buffer still hands back a pointer: a caller that frees what it
   * was given must not have to know whether it got anything. */
  if (buf->bytes == nullptr) {
    buf->bytes = malloc(1);
    if (buf->bytes == nullptr) {
      if (out_size != nullptr) *out_size = 0;
      return nullptr;
    }
  }
  uint8_t* bytes = buf->bytes;
  if (out_size != nullptr) *out_size = buf->count;
  *buf = (rm_buf){0};
  return bytes;
}

/* ------------------------------------------------------------------ */
/* UTF-8                                                               */
/* ------------------------------------------------------------------ */

/* How long a sequence the lead byte promises, or 0 when it is not a lead.
 *
 * The ranges are Unicode\u2019s own table and not the bit patterns that look
 * like it: 0xC0 and 0xC1 lead only overlong forms of ASCII, and 0xF5 upwards
 * only scalars past U+10FFFF. Read as leads they would make an ill-formed pair
 * decode as one replacement where a conformant decoder gives two \u2014 and the
 * file numbers characters by scalar, so one replacement too few shifts every
 * anchor behind it. */
static int rm_utf8_lead_length(uint8_t byte) {
  if (byte < 0x80u) return 1;
  if (byte >= 0xC2u && byte <= 0xDFu) return 2;
  if (byte >= 0xE0u && byte <= 0xEFu) return 3;
  if (byte >= 0xF0u && byte <= 0xF4u) return 4;
  return 0;
}

/* The range a continuation byte may take, which narrows for the first one
 * after certain leads — that is what rules out the overlong forms and the
 * surrogates without decoding the scalar first. */
static bool rm_utf8_continuation_ok(uint8_t lead, int position, uint8_t byte) {
  uint8_t low = 0x80u;
  uint8_t high = 0xBFu;
  if (position == 1) {
    if (lead == 0xE0u)
      low = 0xA0u;
    else if (lead == 0xEDu)
      high = 0x9Fu;
    else if (lead == 0xF0u)
      low = 0x90u;
    else if (lead == 0xF4u)
      high = 0x8Fu;
  }
  return byte >= low && byte <= high;
}

size_t rm_utf8_repair(const char* bytes, size_t length, char* out) {
  const uint8_t* in = (const uint8_t*)bytes;
  size_t written = 0;
  size_t i = 0;
  while (i < length) {
    int wanted = rm_utf8_lead_length(in[i]);
    if (wanted == 0) {
      /* A continuation byte or one of the five-byte leads the format
       * never had: one replacement, one byte consumed. */
      out[written++] = (char)0xEFu;
      out[written++] = (char)0xBFu;
      out[written++] = (char)0xBDu;
      i += 1;
      continue;
    }
    /* The maximal subpart: consume as much of the sequence as is
     * well-formed, replace that whole run with one U+FFFD, and resume at
     * the first byte that did not belong to it. */
    size_t taken = 1;
    bool complete = true;
    while (taken < (size_t)wanted) {
      if (i + taken >= length ||
          !rm_utf8_continuation_ok(in[i], (int)taken, in[i + taken])) {
        complete = false;
        break;
      }
      taken += 1;
    }
    if (complete) {
      memcpy(out + written, in + i, taken);
      written += taken;
    } else {
      out[written++] = (char)0xEFu;
      out[written++] = (char)0xBFu;
      out[written++] = (char)0xBDu;
    }
    i += taken;
  }
  out[written] = '\0';
  return written;
}

size_t rm_utf8_scalar_count(const char* bytes, size_t length) {
  const uint8_t* in = (const uint8_t*)bytes;
  size_t count = 0;
  for (size_t i = 0; i < length; i += 1) {
    if ((in[i] & 0xC0u) != 0x80u) count += 1;
  }
  return count;
}

size_t rm_utf8_next(const char* bytes, size_t length, size_t offset,
                    uint32_t* out_scalar) {
  const uint8_t* in = (const uint8_t*)bytes;
  int wanted = rm_utf8_lead_length(in[offset]);
  if (wanted == 0 || offset + (size_t)wanted > length) {
    if (out_scalar != nullptr) *out_scalar = 0xFFFDu;
    return offset + 1u;
  }
  uint32_t scalar;
  switch (wanted) {
    case 1:
      scalar = in[offset];
      break;
    case 2:
      scalar = (uint32_t)(in[offset] & 0x1Fu) << 6 | (in[offset + 1] & 0x3Fu);
      break;
    case 3:
      scalar = (uint32_t)(in[offset] & 0x0Fu) << 12 |
               (uint32_t)(in[offset + 1] & 0x3Fu) << 6 |
               (in[offset + 2] & 0x3Fu);
      break;
    default:
      scalar = (uint32_t)(in[offset] & 0x07u) << 18 |
               (uint32_t)(in[offset + 1] & 0x3Fu) << 12 |
               (uint32_t)(in[offset + 2] & 0x3Fu) << 6 |
               (in[offset + 3] & 0x3Fu);
      break;
  }
  if (out_scalar != nullptr) *out_scalar = scalar;
  return offset + (size_t)wanted;
}

/* ------------------------------------------------------------------ */
/* Identifier map                                                      */
/* ------------------------------------------------------------------ */

typedef struct rm_idmap_slot {
  rm_crdt_id key;
  uint64_t value;
  bool used;
} rm_idmap_slot;

void rm_idmap_init(rm_idmap* map) { *map = (rm_idmap){0}; }

void rm_idmap_free(rm_idmap* map) {
  free(map->slots);
  *map = (rm_idmap){0};
}

/* splitmix64 over the counter, the author folded in. The counters of one
 * author run consecutively, which is exactly the input a weak hash smears
 * into one run of slots. */
static uint64_t rm_idmap_hash(rm_crdt_id key) {
  uint64_t x = key.counter + 0x9E3779B97F4A7C15u * (uint64_t)(key.author + 1u);
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9u;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBu;
  x ^= x >> 31;
  return x;
}

/* The slot `key` occupies, or the empty slot it would go in. Null only when
 * the map has no slots at all. */
static rm_idmap_slot* rm_idmap_find(const rm_idmap* map, rm_crdt_id key) {
  if (map->capacity == 0) return nullptr;
  size_t mask = map->capacity - 1u;
  size_t index = (size_t)rm_idmap_hash(key) & mask;
  for (;;) {
    rm_idmap_slot* slot = &map->slots[index];
    if (!slot->used) return slot;
    if (rm_crdt_equal(slot->key, key)) return slot;
    index = (index + 1u) & mask;
  }
}

/* Move every entry into a fresh table of `capacity` slots. False, and the
 * map left as it was, on allocation failure. */
static bool rm_idmap_rehash(rm_idmap* map, size_t capacity) {
  rm_idmap_slot* slots = calloc(capacity, sizeof *slots);
  if (slots == nullptr) return false;
  rm_idmap old = *map;
  map->slots = slots;
  map->capacity = capacity;
  map->count = 0;
  for (size_t i = 0; i < old.capacity; i += 1) {
    if (!old.slots[i].used) continue;
    rm_idmap_slot* slot = rm_idmap_find(map, old.slots[i].key);
    *slot = old.slots[i];
    map->count += 1;
  }
  free(old.slots);
  return true;
}

bool rm_idmap_set(rm_idmap* map, rm_crdt_id key, uint64_t value) {
  /* Grown at seven eighths rather than at a half: the probe is linear, the
   * hash is a good one, and a page's maps are built once and read many
   * times. */
  if (map->capacity == 0 || (map->count + 1u) * 8u > map->capacity * 7u) {
    size_t capacity = map->capacity == 0 ? 64u : map->capacity * 2u;
    if (!rm_idmap_rehash(map, capacity)) return false;
  }
  rm_idmap_slot* slot = rm_idmap_find(map, key);
  if (!slot->used) {
    slot->used = true;
    slot->key = key;
    map->count += 1;
  }
  slot->value = value;
  return true;
}

bool rm_idmap_get(const rm_idmap* map, rm_crdt_id key, uint64_t* out_value) {
  rm_idmap_slot* slot = rm_idmap_find(map, key);
  if (slot == nullptr || !slot->used) return false;
  if (out_value != nullptr) *out_value = slot->value;
  return true;
}

bool rm_idmap_take(rm_idmap* map, rm_crdt_id key, uint64_t* out_value) {
  rm_idmap_slot* slot = rm_idmap_find(map, key);
  if (slot == nullptr || !slot->used) return false;
  if (out_value != nullptr) *out_value = slot->value;
  /* Backward-shift deletion: a tombstone would need its own state and a
   * second pass in every probe, and the maps here are small enough that
   * moving the run is cheaper. */
  size_t mask = map->capacity - 1u;
  size_t hole = (size_t)(slot - map->slots);
  size_t scan = hole;
  for (;;) {
    scan = (scan + 1u) & mask;
    if (!map->slots[scan].used) break;
    size_t home = (size_t)rm_idmap_hash(map->slots[scan].key) & mask;
    /* Does `scan` belong at or before the hole, walking forward from its
     * home slot? If so it may move up into the hole. */
    size_t from_home = (scan - home) & mask;
    size_t hole_from_home = (hole - home) & mask;
    if (hole_from_home <= from_home) {
      map->slots[hole] = map->slots[scan];
      hole = scan;
    }
  }
  map->slots[hole].used = false;
  map->count -= 1;
  return true;
}

bool rm_idmap_next(const rm_idmap* map, size_t* cursor, rm_crdt_id* out_key,
                   uint64_t* out_value) {
  for (size_t i = *cursor; i < map->capacity; i += 1) {
    if (!map->slots[i].used) continue;
    if (out_key != nullptr) *out_key = map->slots[i].key;
    if (out_value != nullptr) *out_value = map->slots[i].value;
    *cursor = i + 1u;
    return true;
  }
  *cursor = map->capacity;
  return false;
}

/* ------------------------------------------------------------------ */
/* Opaque-key map                                                      */
/* ------------------------------------------------------------------ */

typedef struct rm_keymap_slot {
  uint8_t key[16];
  uint64_t value;
  bool used;
} rm_keymap_slot;

void rm_keymap_init(rm_keymap* map) { *map = (rm_keymap){0}; }

void rm_keymap_free(rm_keymap* map) {
  free(map->slots);
  *map = (rm_keymap){0};
}

/* splitmix64 over both halves of the key. The attachment keys are the
 * document index's own, and neighbouring assets of one page differ in a few
 * bytes — which is the input a hash that only folds them smears into one run
 * of slots. */
static uint64_t rm_keymap_hash(const uint8_t key[16]) {
  uint64_t x = 0;
  for (int half = 0; half < 2; half += 1) {
    uint64_t part = 0;
    for (int i = 0; i < 8; i += 1)
      part |= (uint64_t)key[half * 8 + i] << (8 * i);
    x ^= part + 0x9E3779B97F4A7C15u * (uint64_t)(half + 1);
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9u;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBu;
    x ^= x >> 31;
  }
  return x;
}

/* The slot `key` occupies, or the empty slot it would go in. Null only when
 * the map has no slots at all. */
static rm_keymap_slot* rm_keymap_find(const rm_keymap* map,
                                      const uint8_t key[16]) {
  if (map->capacity == 0) return nullptr;
  size_t mask = map->capacity - 1u;
  size_t index = (size_t)rm_keymap_hash(key) & mask;
  for (;;) {
    rm_keymap_slot* slot = &map->slots[index];
    if (!slot->used) return slot;
    if (memcmp(slot->key, key, 16) == 0) return slot;
    index = (index + 1u) & mask;
  }
}

/* Move every entry into a fresh table of `capacity` slots. False, and the
 * map left as it was, on allocation failure. */
static bool rm_keymap_rehash(rm_keymap* map, size_t capacity) {
  rm_keymap_slot* slots = calloc(capacity, sizeof *slots);
  if (slots == nullptr) return false;
  rm_keymap old = *map;
  map->slots = slots;
  map->capacity = capacity;
  map->count = 0;
  for (size_t i = 0; i < old.capacity; i += 1) {
    if (!old.slots[i].used) continue;
    rm_keymap_slot* slot = rm_keymap_find(map, old.slots[i].key);
    *slot = old.slots[i];
    map->count += 1;
  }
  free(old.slots);
  return true;
}

bool rm_keymap_set(rm_keymap* map, const uint8_t key[16], uint64_t value) {
  if (map->capacity == 0 || (map->count + 1u) * 8u > map->capacity * 7u) {
    size_t capacity = map->capacity == 0 ? 16u : map->capacity * 2u;
    if (!rm_keymap_rehash(map, capacity)) return false;
  }
  rm_keymap_slot* slot = rm_keymap_find(map, key);
  if (!slot->used) {
    slot->used = true;
    memcpy(slot->key, key, 16);
    map->count += 1;
  }
  slot->value = value;
  return true;
}

bool rm_keymap_get(const rm_keymap* map, const uint8_t key[16],
                   uint64_t* out_value) {
  rm_keymap_slot* slot = rm_keymap_find(map, key);
  if (slot == nullptr || !slot->used) return false;
  if (out_value != nullptr) *out_value = slot->value;
  return true;
}

/* ------------------------------------------------------------------ */
/* Grouping                                                            */
/* ------------------------------------------------------------------ */

void rm_grouping_free(rm_grouping* grouping) {
  rm_idmap_free(&grouping->index);
  free(grouping->starts);
  free(grouping->ends);
  free(grouping->items);
  *grouping = (rm_grouping){0};
}

bool rm_grouping_build(rm_grouping* grouping, size_t count,
                       rm_crdt_id (*key_of)(size_t index, void* ctx),
                       void* ctx) {
  *grouping = (rm_grouping){0};
  rm_idmap_init(&grouping->index);
  if (count == 0) return true;
  if (count > UINT32_MAX) return false;

  /* Count the groups and how big each one is, then lay the runs out and
   * fill them in a second pass. Two passes rather than an array per group:
   * the item order inside a group has to stay the order they were seen. */
  uint32_t* counts = calloc(count, sizeof *counts);
  if (counts == nullptr) return false;
  size_t groups = 0;
  for (size_t i = 0; i < count; i += 1) {
    rm_crdt_id key = key_of(i, ctx);
    uint64_t number;
    if (!rm_idmap_get(&grouping->index, key, &number)) {
      number = groups;
      if (!rm_idmap_set(&grouping->index, key, number)) {
        free(counts);
        goto fail;
      }
      groups += 1;
    }
    counts[number] += 1;
  }

  grouping->starts = calloc(groups, sizeof *grouping->starts);
  grouping->ends = calloc(groups, sizeof *grouping->ends);
  grouping->items = calloc(count, sizeof *grouping->items);
  if (grouping->starts == nullptr || grouping->ends == nullptr ||
      grouping->items == nullptr) {
    free(counts);
    goto fail;
  }
  uint32_t running = 0;
  for (size_t g = 0; g < groups; g += 1) {
    grouping->starts[g] = running;
    grouping->ends[g] = running;
    running += counts[g];
  }
  free(counts);

  for (size_t i = 0; i < count; i += 1) {
    /* Set on the first pass for every key, so the lookup cannot miss;
     * the initialiser is for gcc, which cannot see that far. */
    uint64_t number = 0;
    (void)rm_idmap_get(&grouping->index, key_of(i, ctx), &number);
    grouping->items[grouping->ends[number]] = (uint32_t)i;
    grouping->ends[number] += 1;
  }
  grouping->group_count = groups;
  grouping->item_count = count;
  return true;

fail:
  rm_grouping_free(grouping);
  return false;
}

bool rm_grouping_find(const rm_grouping* grouping, rm_crdt_id key,
                      const uint32_t** out_items, size_t* out_count) {
  uint64_t number;
  if (!rm_idmap_get(&grouping->index, key, &number)) {
    if (out_items != nullptr) *out_items = nullptr;
    if (out_count != nullptr) *out_count = 0;
    return false;
  }
  if (out_items != nullptr)
    *out_items = grouping->items + grouping->starts[number];
  if (out_count != nullptr) {
    *out_count = grouping->ends[number] - grouping->starts[number];
  }
  return true;
}

bool rm_grouping_number(const rm_grouping* grouping, rm_crdt_id key,
                        size_t* out_number) {
  uint64_t number;
  if (!rm_idmap_get(&grouping->index, key, &number)) return false;
  if (out_number != nullptr) *out_number = (size_t)number;
  return true;
}

void* rm_vec_at(const rm_vec* vec, size_t index) {
  return (unsigned char*)vec->data + index * vec->elem;
}

size_t rm_utf8_repair_bound(size_t length) { return length * 3u + 1u; }

bool rm_crdt_equal(rm_crdt_id a, rm_crdt_id b) {
  return a.counter == b.counter && a.author == b.author;
}

rm_crdt_id rm_crdt_make(uint8_t author, uint64_t counter) {
  return (rm_crdt_id){.counter = counter, .author = author};
}

bool rm_idmap_contains(const rm_idmap* map, rm_crdt_id key) {
  return rm_idmap_get(map, key, nullptr);
}

rm_item_id rm_item_id_of(rm_crdt_id id) {
  return (rm_item_id){.author = id.author, .counter = id.counter};
}

rm_crdt_id rm_crdt_of(rm_item_id id) {
  return rm_crdt_make(id.author, id.counter);
}
