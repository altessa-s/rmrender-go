/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "rmd_internal.h"
#ifdef RMDOC_TESTING
static thread_local size_t remaining = SIZE_MAX;

void rmd_test_fail_after(size_t n) { remaining = n; }

/* Whether the next allocation should be forced to fail, consuming one
 * count set by rmd_test_fail_after. */
static bool fail(void) {
  if (remaining == SIZE_MAX) return false;
  if (!remaining) return true;
  remaining--;
  return false;
}
#else
/* Always false: failure injection is compiled out of non-test builds. */
static bool fail(void) { return false; }
#endif

void* rmd_alloc(size_t n) { return fail() ? nullptr : malloc(n ? n : 1); }

void* rmd_calloc(size_t n, size_t size) {
  if (size && n > SIZE_MAX / size) return nullptr;
  if (fail()) return nullptr;
  return calloc(n ? n : 1, size ? size : 1);
}

void* rmd_realloc(void* p, size_t n) {
  return fail() ? nullptr : realloc(p, n ? n : 1);
}

/* Stringizes a macro's expansion (two-step to force expansion before
 * stringizing). */
#define RMD_STRINGIFY_(x) #x
#define RMD_STRINGIFY(x) RMD_STRINGIFY_(x)
const char* rmd_version(void) {
  return RMD_STRINGIFY(RMDOC_VERSION_MAJOR) "." RMD_STRINGIFY(
      RMDOC_VERSION_MINOR) "." RMD_STRINGIFY(RMDOC_VERSION_PATCH);
}

const char* rmd_status_string(rmd_status status) {
  static const char* const names[] = {"ok",
                                      "invalid argument",
                                      "out of memory",
                                      "I/O error",
                                      "invalid name",
                                      "duplicate name",
                                      "empty archive",
                                      "file too large",
                                      "archive too large",
                                      "not an archive",
                                      "malformed archive",
                                      "unsupported compression",
                                      "checksum mismatch",
                                      "archive finished",
                                      "too many entries",
                                      "entry too large",
                                      "total size exceeded",
                                      "compression ratio exceeded",
                                      "compressed entry too large",
                                      "compressed total size exceeded",
                                      "directory too large",
                                      "name too long",
                                      "ZIP64 unsupported",
                                      "encrypted entry",
                                      "spanned archive",
                                      "callback failed"};
  return (size_t)status < sizeof names / sizeof *names ? names[status]
                                                       : "unknown status";
}

rmd_status rmd_fail(rmd_error* e, rmd_status code, rmd_str name,
                    uint64_t actual, uint64_t limit, const char* message) {
  if (e) {
    *e = (rmd_error){.code = code,
                     .actual = actual,
                     .limit = limit,
                     .name_length = name.length};
    size_t n =
        name.length < sizeof e->name - 1 ? name.length : sizeof e->name - 1;
    if (name.bytes && n) memcpy(e->name, name.bytes, n);
    snprintf(e->message, sizeof e->message, "%s",
             message ? message : rmd_status_string(code));
  }
  return code;
}

rmd_limits rmd_limits_default(void) {
  return (rmd_limits){UINT16_MAX, UINT32_MAX, INT64_MAX, 200,       1u << 20,
                      UINT32_MAX, INT64_MAX,  INT64_MAX, UINT16_MAX};
}

rmd_limits rmd_limits_format(void) {
  rmd_limits l = rmd_limits_default();
  l.maximum_compression_ratio = 1032;
  l.ratio_threshold = 0;
  return l;
}

void rmd_buffer_free(rmd_buffer* b) {
  if (b) {
    free(b->bytes);
    *b = (rmd_buffer){0};
  }
}

void rmd_archive_free(rmd_archive* a) {
  if (!a) return;
  rmd_file* files = a->_storage;
  for (size_t i = 0; i < a->count; i++) {
    /* Allocations are owned here; the public views are immutable. */
    free((void*)(uintptr_t)files[i].name.bytes);
    free((void*)(uintptr_t)files[i].data.bytes);
  }
  free(files);
  *a = (rmd_archive){0};
}

void rmd_strings_free(rmd_strings* s) {
  if (!s) return;
  rmd_str* values = s->_storage;
  for (size_t i = 0; i < s->count; i++) free((void*)(uintptr_t)values[i].bytes);
  free(values);
  *s = (rmd_strings){0};
}

rmd_status rmd_reader_document_ids(const rmd_reader* r, rmd_strings* out,
                                   rmd_error* e) {
  if (!r || !out) return RMD_FAIL(e, RMD_ERR_INVALID_ARGUMENT, nullptr);
  rmd_str* ids = rmd_calloc(r->count, sizeof *ids);
  if (!ids) return RMD_FAIL(e, RMD_ERR_OUT_OF_MEMORY, nullptr);
  rmd_strings result = {.values = ids, ._storage = ids};
  rmd_name_node* names = nullptr;
  rmd_status status = RMD_OK;
  for (size_t i = 0; i < r->count; i++) {
    rmd_str id = rmd_document_id(r->entries[i].name);
    status = rmd_names_insert(&names, id);
    if (status == RMD_ERR_DUPLICATE_NAME) {
      status = RMD_OK;
      continue;
    }
    if (status != RMD_OK) break;
    char* copy = rmd_alloc(id.length + 1);
    if (!copy) {
      status = RMD_ERR_OUT_OF_MEMORY;
      break;
    }
    memcpy(copy, id.bytes, id.length);
    copy[id.length] = 0;
    ids[result.count++] = (rmd_str){copy, id.length};
  }
  rmd_names_free(names);
  if (status != RMD_OK) {
    rmd_strings_free(&result);
    return RMD_FAIL(e, status, nullptr);
  }
  *out = result;
  return RMD_OK;
}

uint16_t rmd_u16(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

uint32_t rmd_u32(const uint8_t* p) {
  return (uint32_t)rmd_u16(p) | (uint32_t)rmd_u16(p + 2) << 16;
}

void rmd_put16(uint8_t* p, uint16_t x) {
  p[0] = (uint8_t)x;
  p[1] = (uint8_t)(x >> 8);
}

void rmd_put32(uint8_t* p, uint32_t x) {
  rmd_put16(p, (uint16_t)x);
  rmd_put16(p + 2, (uint16_t)(x >> 16));
}
