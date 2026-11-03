/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Per-result arena; derived from the same ownership approach as RMLines. */

#include <stdlib.h>

#include "rmm_internal.h"

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

typedef struct rmm_arena_chunk {
  struct rmm_arena_chunk* next;
  size_t used;
  size_t capacity;
  alignas(max_align_t) unsigned char data[];
} rmm_arena_chunk;

struct rmm_arena {
  rmm_arena_chunk* head;
};

/* Big enough that an ordinary page is one or two chunks, small enough that a
 * page with nothing on it does not cost a megabyte. */
constexpr size_t RMM_ARENA_CHUNK = 4096u;

#ifdef RMMARKS_TESTING
static _Thread_local size_t rmm_test_arena_remaining = SIZE_MAX;
static _Thread_local bool rmm_test_arena_failed;

void rmm_test_fail_after(size_t count) {
  rmm_test_arena_remaining = count;
  rmm_test_arena_failed = false;
}

bool rmm_test_did_fail(void) { return rmm_test_arena_failed; }
#endif

/* True once per forced failure armed by rmm_test_fail_after, for tests. */
static bool rmm_should_fail(void) {
#ifdef RMMARKS_TESTING
  if (rmm_test_arena_remaining != SIZE_MAX) {
    if (rmm_test_arena_remaining == 0) {
      rmm_test_arena_remaining = SIZE_MAX;
      rmm_test_arena_failed = true;
      return true;
    }
    rmm_test_arena_remaining -= 1;
  }
#endif
  return false;
}

rmm_arena* rmm_arena_create(void) {
  if (rmm_should_fail()) return nullptr;
  rmm_arena* arena = calloc(1, sizeof *arena);
  return arena;
}

void rmm_arena_destroy(rmm_arena* arena) {
  if (arena == nullptr) return;
  rmm_arena_chunk* chunk = arena->head;
  while (chunk != nullptr) {
    rmm_arena_chunk* next = chunk->next;
    free(chunk);
    chunk = next;
  }
  free(arena);
}

/* False when rounding up would carry past what a `size_t` holds: the mask
 * below would then answer a number *smaller* than the request, and the caller
 * would hand out less memory than it was asked for without noticing. */
static bool rmm_align_up(size_t value, size_t* out) {
  constexpr size_t align = alignof(max_align_t);
  if (value > SIZE_MAX - (align - 1u)) return false;
  *out = (value + (align - 1u)) & ~(align - 1u);
  return true;
}

void* rmm_arena_alloc(rmm_arena* arena, size_t size) {
  if (rmm_should_fail()) return nullptr;
  if (arena == nullptr) return nullptr;
  if (size == 0) size = 1;
  if (!rmm_align_up(size, &size)) return nullptr;

  rmm_arena_chunk* head = arena->head;
  if (head == nullptr || head->capacity - head->used < size) {
    size_t capacity = size > RMM_ARENA_CHUNK ? size : RMM_ARENA_CHUNK;
    if (capacity > SIZE_MAX - sizeof(rmm_arena_chunk)) return nullptr;
    rmm_arena_chunk* chunk = malloc(sizeof(rmm_arena_chunk) + capacity);
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

void* rmm_arena_calloc(rmm_arena* arena, size_t count, size_t size) {
  if (count != 0 && size > SIZE_MAX / count) return nullptr;
  size_t total = count * size;
  void* result = rmm_arena_alloc(arena, total);
  if (result != nullptr) memset(result, 0, total == 0 ? 1 : total);
  return result;
}

char* rmm_arena_strndup(rmm_arena* arena, const char* bytes, size_t length) {
  if (length == SIZE_MAX) return nullptr;
  char* copy = rmm_arena_alloc(arena, length + 1u);
  if (copy == nullptr) return nullptr;
  if (length != 0) memcpy(copy, bytes, length);
  copy[length] = '\0';
  return copy;
}
