/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "rmd_internal.h"
#include "rmd_miniz.h"

/* miniz zalloc callback: forwards to rmd_calloc, ignoring context. */
static void* codec_alloc(void* context, size_t n, size_t size) {
  (void)context;
  return rmd_calloc(n, size);
}

/* miniz zfree callback: forwards to free, ignoring context. */
static void codec_free(void* context, void* p) {
  (void)context;
  free(p);
}

uint32_t rmd_crc32(uint32_t crc, rmd_bytes data) {
  return (uint32_t)mz_crc32(crc, data.bytes, data.size);
}

rmd_status rmd_inflate(rmd_bytes input, uint32_t expected, rmd_buffer* out) {
  if (!input.size) return RMD_ERR_MALFORMED_ARCHIVE;
  mz_stream s = {.zalloc = codec_alloc, .zfree = codec_free};
  int rc = mz_inflateInit2(&s, -MZ_DEFAULT_WINDOW_BITS);
  if (rc != MZ_OK)
    return rc == MZ_MEM_ERROR ? RMD_ERR_OUT_OF_MEMORY
                              : RMD_ERR_MALFORMED_ARCHIVE;
  s.next_in = input.bytes;
  s.avail_in = (unsigned int)input.size;
  rmd_buffer result = {0};
  size_t capacity = 0;
  uint8_t spare = 0;
  rmd_status status = RMD_ERR_MALFORMED_ARCHIVE;
  for (;;) {
    if (result.size == capacity && capacity < expected) {
      uint64_t grown = capacity ? (uint64_t)capacity * 2 : 256u * 1024u;
      if (grown > expected) grown = expected;
      void* p = rmd_realloc(result.bytes, (size_t)grown);
      if (!p) {
        status = RMD_ERR_OUT_OF_MEMORY;
        break;
      }
      result.bytes = p;
      capacity = (size_t)grown;
    }
    bool overrun = result.size == expected;
    s.next_out = overrun ? &spare : result.bytes + result.size;
    s.avail_out = overrun ? 1u : (unsigned int)(capacity - result.size);
    unsigned int before_in = s.avail_in, before_out = s.avail_out;
    rc = mz_inflate(&s, MZ_NO_FLUSH);
    size_t written = before_out - s.avail_out;
    if (overrun && written) break;
    if (!overrun) result.size += written;
    if (rc == MZ_STREAM_END) {
      if (!s.avail_in && result.size == expected) status = RMD_OK;
      break;
    }
    if (rc != MZ_OK || (s.avail_in == before_in && !written)) break;
  }
  mz_inflateEnd(&s);
  if (status != RMD_OK)
    rmd_buffer_free(&result);
  else
    *out = result;
  return status;
}

rmd_status rmd_deflate(rmd_bytes input, rmd_buffer* out) {
  /* Only a result smaller than the input is useful. A full buffer is a
   * valid reason to store the original; allocation failure remains explicit. */
  if (!input.size) {
    *out = (rmd_buffer){0};
    return RMD_OK;
  }
  mz_stream s = {.zalloc = codec_alloc, .zfree = codec_free};
  int rc = mz_deflateInit2(&s, MZ_DEFAULT_COMPRESSION, MZ_DEFLATED,
                           -MZ_DEFAULT_WINDOW_BITS, 8, MZ_DEFAULT_STRATEGY);
  if (rc != MZ_OK) return RMD_ERR_OUT_OF_MEMORY;
  uint8_t* p = rmd_alloc(input.size);
  if (!p) {
    mz_deflateEnd(&s);
    return RMD_ERR_OUT_OF_MEMORY;
  }
  s.next_in = input.bytes;
  s.avail_in = (unsigned int)input.size;
  s.next_out = p;
  s.avail_out = (unsigned int)input.size;
  rc = mz_deflate(&s, MZ_FINISH);
  size_t n = input.size - s.avail_out;
  bool useful = rc == MZ_STREAM_END && !s.avail_in && n < input.size;
  mz_deflateEnd(&s);
  if (!useful) {
    free(p);
    *out = (rmd_buffer){0};
  } else
    *out = (rmd_buffer){p, n};
  return RMD_OK;
}
