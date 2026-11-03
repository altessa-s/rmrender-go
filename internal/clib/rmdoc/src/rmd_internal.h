/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMD_INTERNAL_H
#define RMD_INTERNAL_H
#include <assert.h>
#include <limits.h>
#include <rmdoc/rmdoc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct rmd_name_node {
  struct rmd_name_node *left, *right;
  uint32_t* key;
  size_t length;
  int height;
} rmd_name_node;

typedef struct {
  size_t payload, start, end;
} rmd_location;

struct rmd_reader {
  rmd_bytes data;
  rmd_limits limits;
  rmd_entry* entries;
  rmd_location* locations;
  size_t count;
  bool mapped;
};

/* Allocates n bytes (at least one); NULL on failure or injected test
 * failure. */
void* rmd_alloc(size_t n);

/* Zero-initializes n elements of size bytes; NULL on overflow, injected
 * test failure, or allocation failure. */
void* rmd_calloc(size_t n, size_t size);

/* Resizes p to at least one byte; on failure (including an injected test
 * failure) returns NULL and leaves p unchanged. */
void* rmd_realloc(void* p, size_t n);

#ifdef RMDOC_TESTING
/* Testing hook: makes the count-th allocation after this call fail;
 * SIZE_MAX (the default) disables injected failures. */
void rmd_test_fail_after(size_t count);
#endif

/* Fills *e (if not NULL) with code, the exceeded value/limit and a
 * truncated copy of name, then returns code. message defaults to the
 * status text. */
rmd_status rmd_fail(rmd_error* e, rmd_status code, rmd_str name,
                    uint64_t actual, uint64_t limit, const char* message);

/* Shorthand for rmd_fail with no name or limit values; use when name,
 * actual and limit do not apply. */
#define RMD_FAIL(e, code, message) \
  rmd_fail(e, code, (rmd_str){0}, 0, 0, message)

/* Decodes one UTF-8 code point at byte offset at into *cp, rejecting
 * overlong encodings and surrogates. Returns the offset just past it, or 0
 * on malformed input or end of string. */
size_t rmd_utf8_next(rmd_str s, size_t at, uint32_t* cp);

/* Reports whether s is entirely valid UTF-8, scalar by scalar. */
bool rmd_utf8_valid(rmd_str s);

/* Inserts name, keyed by its NFD-normalized code points, into the
 * balanced tree for duplicate detection. Returns RMD_ERR_DUPLICATE_NAME if
 * an equivalent name is already present. */
rmd_status rmd_names_insert(rmd_name_node** tree, rmd_str name);

/* Frees every node of the tree, recursively. */
void rmd_names_free(rmd_name_node* tree);

/* Reports RMD_ERR_DUPLICATE_NAME if name (compared by NFD-normalized code
 * points) is already in tree, without inserting it. */
rmd_status rmd_names_check(const rmd_name_node* tree, rmd_str name);

/* Inflates a raw deflate stream into a newly allocated buffer of exactly
 * expected bytes; fails if the stream is malformed or decompresses to a
 * different size. */
rmd_status rmd_inflate(rmd_bytes data, uint32_t expected, rmd_buffer* out);

/* Raw-deflates data into a newly allocated buffer, but only when the
 * result is smaller; otherwise leaves *out empty so the caller stores the
 * original. Fails only on allocation failure. */
rmd_status rmd_deflate(rmd_bytes data, rmd_buffer* out);

/* CRC-32 of data, continuing from a prior checksum (0 to start fresh). */
uint32_t rmd_crc32(uint32_t crc, rmd_bytes data);

/* Decompresses (or copies, if stored) entry index's payload into a newly
 * allocated buffer and verifies it against the directory's CRC-32. */
rmd_status rmd_contents(const rmd_reader* r, size_t index, rmd_buffer* out,
                        rmd_error* e);

/* Opens path (UTF-8, even on Windows) for binary reading or, if write,
 * truncating binary writing. */
FILE* rmd_fopen(const char* path, bool write);

/* Seeks file to an absolute byte position, with 64-bit offsets on every
 * platform. */
int rmd_seek(FILE* file, uint64_t position);

/* Unmaps a region previously produced by a memory-mapped file open. No-op
 * on an empty mapping. */
void rmd_unmap(rmd_bytes data);

/* Little-endian 16-bit integer read from p. */
uint16_t rmd_u16(const uint8_t* p);

/* Little-endian 32-bit integer read from p. */
uint32_t rmd_u32(const uint8_t* p);

/* Writes x into p as a little-endian 16-bit integer. */
void rmd_put16(uint8_t* p, uint16_t x);

/* Writes x into p as a little-endian 32-bit integer. */
void rmd_put32(uint8_t* p, uint32_t x);
#endif
