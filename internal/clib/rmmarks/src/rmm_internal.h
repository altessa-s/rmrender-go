/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMM_INTERNAL_H
#define RMM_INTERNAL_H
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rmmarks/rmmarks.h"

typedef struct rmm_arena rmm_arena;

/* Creates an empty arena. Returns nullptr on allocation failure. */
rmm_arena* rmm_arena_create(void);

/* Frees arena and everything allocated from it. Accepts nullptr. */
void rmm_arena_destroy(rmm_arena* arena);

/* Allocates size bytes from arena, aligned for any type. The memory is
 * not zeroed. Returns nullptr if arena is null or allocation fails. */
void* rmm_arena_alloc(rmm_arena* arena, size_t size);

/* Allocates count * size zeroed bytes from arena. Returns nullptr if the
 * product overflows size_t or allocation fails. */
void* rmm_arena_calloc(rmm_arena* arena, size_t count, size_t size);

/* Copies length bytes from bytes into a new NUL-terminated arena string.
 * Returns nullptr if length is SIZE_MAX or allocation fails. */
char* rmm_arena_strndup(rmm_arena* arena, const char* bytes, size_t length);

#ifdef RMMARKS_TESTING
/* Makes the arena fail its count-th allocation from now on, for tests. */
void rmm_test_fail_after(size_t count);

/* True if a pending rmm_test_fail_after threshold has been hit. */
bool rmm_test_did_fail(void);
#endif

/* True if data/count is a usable array: non-null whenever count is
 * nonzero, and count * element cannot overflow size_t. */
bool rmm_array_valid(const void* data, size_t count, size_t element);

/* True if box's coordinates and derived extents and sum are all finite. */
bool rmm_box_valid(rmm_page_box box);

/* Smallest box containing both a and b. */
rmm_page_box rmm_box_union(rmm_page_box a, rmm_page_box b);

/* Fills error (if non-null) with status and its message, then returns
 * status; used as the single return point for every public entry point. */
rmm_status rmm_finish(rmm_error* error, rmm_status status);

/* Validates that lines and every piece within them form well-formed
 * arrays with valid boxes and valid UTF-8 text. On success, writes the
 * total piece count across all lines to *pieces if pieces is non-null. */
rmm_status rmm_validate_lines(const rmm_line* lines, size_t count,
                              size_t* pieces);

/* True if text is well-formed UTF-8. */
bool rmm_utf8_valid(rmm_str text);

/* Decodes one UTF-8 scalar from text at offset into *scalar. Returns the
 * offset just past it, or 0 if offset is at the end or the bytes there
 * are not a valid encoding. */
size_t rmm_utf8_next(rmm_str text, size_t offset, uint32_t* scalar);

/* text with leading and trailing whitespace grapheme clusters removed.
 * The result is a view into text's own bytes, not a copy. */
rmm_str rmm_trim(rmm_str text);

/* Writes to *equal whether a and b are equal under canonical (NFD)
 * decomposition, using arena for scratch allocations. */
rmm_status rmm_text_equal(rmm_arena* arena, rmm_str a, rmm_str b, bool* equal);

#endif
