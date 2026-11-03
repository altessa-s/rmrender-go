/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "rmd_internal.h"

/* Whether byte range [at, at+n) fits within end, without overflowing. */
static bool inside(size_t at, uint64_t n, size_t end) {
  return at <= end && n <= end - at;
}

/* Walks n bytes of extra-field tag/length records, rejecting a ZIP64
 * record (tag 1); strict also requires the records to exactly fill n. */
static rmd_status extra(const uint8_t* p, size_t n, bool strict) {
  size_t at = 0;
  while (n - at >= 4) {
    uint16_t tag = rmd_u16(p + at), length = rmd_u16(p + at + 2);
    at += 4;
    if (length > n - at) return RMD_ERR_MALFORMED_ARCHIVE;
    if (tag == 1) return RMD_ERR_ZIP64_UNSUPPORTED;
    at += length;
  }
  return strict && at != n ? RMD_ERR_MALFORMED_ARCHIVE : RMD_OK;
}

/* Whether the 12 bytes at at are a data descriptor matching entry's
 * crc32 and sizes. */
static bool descriptor(const uint8_t* p, size_t at, size_t end,
                       const rmd_entry* entry) {
  return inside(at, 12, end) && rmd_u32(p + at) == entry->crc32 &&
         rmd_u32(p + at + 4) == entry->compressed_size &&
         rmd_u32(p + at + 8) == entry->uncompressed_size;
}

/* Parses and fully validates the central directory record at *cursor
 * (and its matching local header), filling r->entries[index] and
 * r->locations[index], and advances *cursor past it. */
static rmd_status record(rmd_reader* r, size_t index, size_t* cursor,
                         size_t directory, size_t directory_end, rmd_error* e) {
  const uint8_t* p = r->data.bytes;
  size_t at = *cursor;
  rmd_str name = {0};
#define BAD(msg) return rmd_fail(e, RMD_ERR_MALFORMED_ARCHIVE, name, 0, 0, msg)
  if (!inside(at, 46, directory_end) || rmd_u32(p + at) != 0x02014b50)
    BAD("invalid directory record boundary");
  uint16_t flags = rmd_u16(p + at + 8), method = rmd_u16(p + at + 10);
  uint16_t nn = rmd_u16(p + at + 28), en = rmd_u16(p + at + 30),
           cn = rmd_u16(p + at + 32);
  if (!inside(at, 46u + (uint64_t)nn + en + cn, directory_end))
    BAD("directory record exceeds directory");
  if (nn > r->limits.maximum_name_length)
    return rmd_fail(e, RMD_ERR_NAME_TOO_LONG, name, nn,
                    r->limits.maximum_name_length, nullptr);
  name = (rmd_str){(const char*)p + at + 46, nn};
  if (!rmd_utf8_valid(name)) BAD("entry name is not UTF-8");
  rmd_status status = rmd_validate_name(name, RMD_DEVICE, e);
  if (status != RMD_OK) return status;
  status = extra(p + at + 46 + nn, en, true);
  if (status != RMD_OK)
    return rmd_fail(e, status, name, 0, 0, "invalid central extra field");
  if (rmd_u16(p + at + 34))
    return rmd_fail(e, RMD_ERR_SPANNED_ARCHIVE, name, 0, 0, nullptr);
  if (flags & 0x2041)
    return rmd_fail(e, RMD_ERR_ENCRYPTED_ENTRY, name, 0, 0, nullptr);
  if (method != 0 && method != 8)
    return rmd_fail(e, RMD_ERR_UNSUPPORTED_COMPRESSION, name, method, 0,
                    nullptr);
  rmd_entry entry = {name, rmd_u32(p + at + 20), rmd_u32(p + at + 24),
                     rmd_u32(p + at + 16), method};
  if (!method && entry.compressed_size != entry.uncompressed_size)
    BAD("stored entry sizes differ");
  if (method == 8 &&
      entry.uncompressed_size > (uint64_t)entry.compressed_size * 1032)
    BAD("impossible deflate ratio");
  size_t local = rmd_u32(p + at + 42);
  if (!inside(local, 30, r->data.size) || rmd_u32(p + local) != 0x04034b50)
    BAD("missing local header");
  uint16_t lf = rmd_u16(p + local + 6), lm = rmd_u16(p + local + 8);
  uint16_t ln = rmd_u16(p + local + 26), le = rmd_u16(p + local + 28);
  uint64_t extent = 30u + (uint64_t)ln + le + entry.compressed_size;
  if (!inside(local, extent, r->data.size) || !inside(local, extent, directory))
    BAD("payload outside body");
  size_t payload = local + 30u + ln + le, end = payload + entry.compressed_size;
  if (ln != nn || memcmp(p + local + 30, name.bytes, nn) != 0)
    BAD("local and central names differ");
  if (lm != method || (lf & 0x2041) || (lf & 8) != (flags & 8))
    BAD("local flags or compression disagree");
  status = extra(p + local + 30 + ln, le, false);
  if (status != RMD_OK)
    return rmd_fail(e, status, name, 0, 0, "invalid local extra field");
  uint32_t crc = rmd_u32(p + local + 14), cs = rmd_u32(p + local + 18),
           us = rmd_u32(p + local + 22);
  if (flags & 8) {
    if ((crc && crc != entry.crc32) || (cs && cs != entry.compressed_size) ||
        (us && us != entry.uncompressed_size))
      BAD("local sizes disagree before descriptor");
    if (inside(end, 4, directory) && rmd_u32(p + end) == 0x08074b50 &&
        descriptor(p, end + 4, directory, &entry))
      end += 16;
    else if (descriptor(p, end, directory, &entry))
      end += 12;
    else
      BAD("data descriptor disagrees");
  } else if (crc != entry.crc32 || cs != entry.compressed_size ||
             us != entry.uncompressed_size)
    BAD("local sizes or checksum disagree");
  char* copy = rmd_alloc((size_t)nn + 1);
  if (!copy) return RMD_FAIL(e, RMD_ERR_OUT_OF_MEMORY, nullptr);
  memcpy(copy, name.bytes, nn);
  copy[nn] = 0;
  entry.name.bytes = copy;
  r->entries[index] = entry;
  r->locations[index] = (rmd_location){payload, local, end};
  *cursor = at + 46u + nn + en + cn;
  return RMD_OK;
#undef BAD
}

/* qsort comparator ordering locations by their starting byte offset. */
static int by_region(const void* a, const void* b) {
  const rmd_location *x = a, *y = b;
  return (x->start > y->start) - (x->start < y->start);
}

/* Locates and validates the end-of-central-directory record, then parses
 * every central directory record into r->entries/locations and checks
 * that no two entries' payload regions overlap. */
static rmd_status directory(rmd_reader* r, rmd_error* e) {
  const uint8_t* p = r->data.bytes;
  size_t n = r->data.size;
  if (n < 22) return RMD_FAIL(e, RMD_ERR_NOT_AN_ARCHIVE, nullptr);
  size_t end = n - 22, lowest = end > UINT16_MAX ? end - UINT16_MAX : 0;
  for (;;) {
    if (rmd_u32(p + end) == 0x06054b50 && rmd_u16(p + end + 20) == n - end - 22)
      break;
    if (end == lowest) return RMD_FAIL(e, RMD_ERR_NOT_AN_ARCHIVE, nullptr);
    end--;
  }
  uint16_t count = rmd_u16(p + end + 10);
  uint32_t ds = rmd_u32(p + end + 12), offset = rmd_u32(p + end + 16);
  uint64_t de = (uint64_t)offset + ds;
  if (end >= 20 && rmd_u32(p + end - 20) == 0x07064b50 &&
      (ds == UINT32_MAX || offset == UINT32_MAX || end - 20 >= de))
    return RMD_FAIL(e, RMD_ERR_ZIP64_UNSUPPORTED, nullptr);
  if (rmd_u16(p + end + 4) || rmd_u16(p + end + 6) ||
      rmd_u16(p + end + 8) != count)
    return RMD_FAIL(e, RMD_ERR_SPANNED_ARCHIVE, nullptr);
  if (count > r->limits.maximum_entry_count)
    return rmd_fail(e, RMD_ERR_TOO_MANY_ENTRIES, (rmd_str){0}, count,
                    r->limits.maximum_entry_count, nullptr);
  if (de > end)
    return RMD_FAIL(e, RMD_ERR_MALFORMED_ARCHIVE,
                    "central directory outside file");
  if (ds > r->limits.maximum_directory_size)
    return rmd_fail(e, RMD_ERR_DIRECTORY_TOO_LARGE, (rmd_str){0}, ds,
                    r->limits.maximum_directory_size, nullptr);
  r->entries = rmd_calloc(count, sizeof *r->entries);
  r->locations = rmd_calloc(count, sizeof *r->locations);
  if (!r->entries || !r->locations)
    return RMD_FAIL(e, RMD_ERR_OUT_OF_MEMORY, nullptr);
  size_t cursor = offset;
  rmd_name_node* names = nullptr;
  rmd_status status = RMD_OK;
  for (size_t i = 0; i < count; i++) {
    status = record(r, i, &cursor, offset, (size_t)de, e);
    if (status != RMD_OK) break;
    r->count++;
    status = rmd_names_insert(&names, r->entries[i].name);
    if (status != RMD_OK) {
      rmd_fail(e, status, r->entries[i].name, 0, 0, nullptr);
      break;
    }
  }
  rmd_names_free(names);
  if (status != RMD_OK) return status;
  if (cursor != de)
    return RMD_FAIL(e, RMD_ERR_MALFORMED_ARCHIVE,
                    "directory has uncounted bytes");
  if (!count) return RMD_FAIL(e, RMD_ERR_EMPTY_ARCHIVE, nullptr);
  rmd_location* sorted = rmd_alloc((size_t)count * sizeof *sorted);
  if (!sorted) return RMD_FAIL(e, RMD_ERR_OUT_OF_MEMORY, nullptr);
  memcpy(sorted, r->locations, (size_t)count * sizeof *sorted);
  qsort(sorted, count, sizeof *sorted, by_region);
  for (size_t i = 1; i < count; i++)
    if (sorted[i - 1].end > sorted[i].start) {
      status = RMD_ERR_MALFORMED_ARCHIVE;
      break;
    }
  free(sorted);
  return status == RMD_OK ? RMD_OK : RMD_FAIL(e, status, "entries overlap");
}

rmd_status rmd_reader_open(rmd_bytes data, const rmd_limits* limits,
                           rmd_reader** out, rmd_error* e) {
  if (!out || (!data.bytes && data.size))
    return RMD_FAIL(e, RMD_ERR_INVALID_ARGUMENT, nullptr);
  rmd_reader* r = rmd_calloc(1, sizeof *r);
  if (!r) return RMD_FAIL(e, RMD_ERR_OUT_OF_MEMORY, nullptr);
  r->data = data;
  r->limits = limits ? *limits : rmd_limits_default();
  rmd_status status = directory(r, e);
  if (status != RMD_OK) {
    rmd_reader_close(r);
    return status;
  }
  *out = r;
  return RMD_OK;
}

void rmd_reader_close(rmd_reader* r) {
  if (!r) return;
  for (size_t i = 0; i < r->count; i++)
    free((void*)(uintptr_t)r->entries[i].name.bytes);
  free(r->entries);
  free(r->locations);
  if (r->mapped) rmd_unmap(r->data);
  free(r);
}

size_t rmd_reader_count(const rmd_reader* r) { return r ? r->count : 0; }

const rmd_entry* rmd_reader_entry(const rmd_reader* r, size_t index) {
  return r && index < r->count ? &r->entries[index] : nullptr;
}

/* Builds, in *out, which entries are selected (via select, or all if
 * NULL), then checks the selected set against the reader's resource
 * budgets before returning it. */
static rmd_status choose(const rmd_reader* r, rmd_select_fn select,
                         void* context, bool** out, rmd_error* e) {
  bool* chosen = rmd_calloc(r->count, sizeof *chosen);
  if (!chosen) return RMD_FAIL(e, RMD_ERR_OUT_OF_MEMORY, nullptr);
  rmd_status status = RMD_OK;
  for (size_t i = 0; i < r->count; i++) {
    chosen[i] = true;
    if (select && select(context, &r->entries[i], &chosen[i]) != RMD_OK) {
      status = rmd_fail(e, RMD_ERR_CALLBACK, r->entries[i].name, 0, 0,
                        "selection callback failed");
      goto done;
    }
  }
  const rmd_limits* l = &r->limits;
  uint64_t total = 0, compressed = 0;
  uint64_t ratio =
      l->maximum_compression_ratio ? l->maximum_compression_ratio : 1;
  for (size_t i = 0; i < r->count; i++)
    if (chosen[i]) {
      const rmd_entry* entry = &r->entries[i];
      uint64_t c = entry->compressed_size, u = entry->uncompressed_size;
#define LIMIT(check, code, actual, limit)                            \
  if (check) {                                                       \
    status = rmd_fail(e, code, entry->name, actual, limit, nullptr); \
    goto done;                                                       \
  }
      LIMIT(c > l->maximum_compressed_entry_size,
            RMD_ERR_COMPRESSED_ENTRY_TOO_LARGE, c,
            l->maximum_compressed_entry_size)
      compressed += c;
      LIMIT(compressed > l->maximum_compressed_total_size,
            RMD_ERR_COMPRESSED_TOTAL_SIZE_EXCEEDED, compressed,
            l->maximum_compressed_total_size)
      LIMIT(u > l->maximum_entry_size, RMD_ERR_ENTRY_TOO_LARGE, u,
            l->maximum_entry_size)
      LIMIT(u > l->ratio_threshold && (c == 0 || ratio <= UINT64_MAX / c) &&
                u > c * ratio,
            RMD_ERR_COMPRESSION_RATIO_EXCEEDED, u, ratio)
      total += u;
      LIMIT(total > l->maximum_total_size, RMD_ERR_TOTAL_SIZE_EXCEEDED, total,
            l->maximum_total_size)
#undef LIMIT
    }
  *out = chosen;
  return RMD_OK;
done:
  free(chosen);
  return status;
}

rmd_status rmd_contents(const rmd_reader* r, size_t i, rmd_buffer* out,
                        rmd_error* e) {
  const rmd_entry* entry = &r->entries[i];
  rmd_bytes input = {r->data.bytes + r->locations[i].payload,
                     entry->compressed_size};
  rmd_buffer b = {0};
  rmd_status status = RMD_OK;
  if (entry->method == 8)
    status = rmd_inflate(input, entry->uncompressed_size, &b);
  else {
    b.bytes = rmd_alloc(input.size);
    if (!b.bytes)
      status = RMD_ERR_OUT_OF_MEMORY;
    else {
      if (input.size) memcpy(b.bytes, input.bytes, input.size);
      b.size = input.size;
    }
  }
  if (status != RMD_OK) return rmd_fail(e, status, entry->name, 0, 0, nullptr);
  if (rmd_crc32(0, (rmd_bytes){b.bytes, b.size}) != entry->crc32) {
    rmd_buffer_free(&b);
    return rmd_fail(e, RMD_ERR_CHECKSUM_MISMATCH, entry->name, 0, 0, nullptr);
  }
  *out = b;
  return RMD_OK;
}

rmd_status rmd_reader_foreach(const rmd_reader* r, rmd_select_fn select,
                              void* select_context, rmd_file_fn body,
                              void* body_context, rmd_error* e) {
  if (!r || !body) return RMD_FAIL(e, RMD_ERR_INVALID_ARGUMENT, nullptr);
  bool* chosen = nullptr;
  rmd_status status = choose(r, select, select_context, &chosen, e);
  if (status != RMD_OK) return status;
  assert(chosen); /* choose publishes storage only on success. */
  for (size_t i = 0; i < r->count; i++)
    if (chosen[i]) {
      rmd_buffer b = {0};
      status = rmd_contents(r, i, &b, e);
      if (status != RMD_OK) break;
      rmd_status callback =
          body(body_context, &r->entries[i], (rmd_bytes){b.bytes, b.size});
      rmd_buffer_free(&b);
      if (callback != RMD_OK) {
        status = rmd_fail(e, RMD_ERR_CALLBACK, r->entries[i].name, 0, 0,
                          "file callback failed");
        break;
      }
    }
  free(chosen);
  return status;
}

rmd_status rmd_reader_unpack(const rmd_reader* r, rmd_select_fn select,
                             void* context, rmd_archive* out, rmd_error* e) {
  if (!r || !out) return RMD_FAIL(e, RMD_ERR_INVALID_ARGUMENT, nullptr);
  bool* chosen = nullptr;
  rmd_status status = choose(r, select, context, &chosen, e);
  if (status != RMD_OK) return status;
  rmd_file* files = rmd_calloc(r->count, sizeof *files);
  if (!files) {
    free(chosen);
    return RMD_FAIL(e, RMD_ERR_OUT_OF_MEMORY, nullptr);
  }
  rmd_archive a = {.files = files, ._storage = files};
  assert(chosen); /* choose publishes storage only on success. */
  for (size_t i = 0; i < r->count; i++)
    if (chosen[i]) {
      rmd_buffer b = {0};
      status = rmd_contents(r, i, &b, e);
      if (status != RMD_OK) break;
      rmd_str name = r->entries[i].name;
      char* copy = rmd_alloc(name.length + 1);
      if (!copy) {
        rmd_buffer_free(&b);
        status = RMD_FAIL(e, RMD_ERR_OUT_OF_MEMORY, nullptr);
        break;
      }
      memcpy(copy, name.bytes, name.length + 1);
      files[a.count++] = (rmd_file){{copy, name.length}, {b.bytes, b.size}};
    }
  free(chosen);
  if (status != RMD_OK) {
    rmd_archive_free(&a);
    return status;
  }
  *out = a;
  return RMD_OK;
}
