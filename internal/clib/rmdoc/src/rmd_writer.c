/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "rmd_internal.h"

typedef struct {
  rmd_entry entry;
  uint32_t offset;
} write_record;

struct rmd_writer {
  FILE* file;
  rmd_compression compression;
  rmd_layout layout;
  write_record* records;
  size_t count, capacity;
  uint64_t offset, directory_size;
  rmd_name_node* names;
  bool finished;
};

/* Encodes a 30-byte ZIP local file header for r into p. */
static void local_header(uint8_t p[30], const write_record* r) {
  memset(p, 0, 30);
  rmd_put32(p, 0x04034b50);
  rmd_put16(p + 4, r->entry.method ? 20 : 10);
  rmd_put16(p + 8, r->entry.method);
  rmd_put16(p + 12, 0x21);
  rmd_put32(p + 14, r->entry.crc32);
  rmd_put32(p + 18, r->entry.compressed_size);
  rmd_put32(p + 22, r->entry.uncompressed_size);
  rmd_put16(p + 26, (uint16_t)r->entry.name.length);
}

/* Encodes a 46-byte ZIP central directory header for r into p. */
static void central_header(uint8_t p[46], const write_record* r) {
  memset(p, 0, 46);
  rmd_put32(p, 0x02014b50);
  rmd_put16(p + 4, 20);
  rmd_put16(p + 6, r->entry.method ? 20 : 10);
  rmd_put16(p + 10, r->entry.method);
  rmd_put16(p + 14, 0x21);
  rmd_put32(p + 16, r->entry.crc32);
  rmd_put32(p + 20, r->entry.compressed_size);
  rmd_put32(p + 24, r->entry.uncompressed_size);
  rmd_put16(p + 28, (uint16_t)r->entry.name.length);
  rmd_put32(p + 42, r->offset);
}

/* Encodes the 22-byte end-of-central-directory record into p. */
static void end_header(uint8_t p[22], size_t count, uint32_t size,
                       uint32_t offset) {
  memset(p, 0, 22);
  rmd_put32(p, 0x06054b50);
  rmd_put16(p + 8, (uint16_t)count);
  rmd_put16(p + 10, (uint16_t)count);
  rmd_put32(p + 12, size);
  rmd_put32(p + 16, offset);
}

rmd_status rmd_pack(const rmd_file* files, size_t count, rmd_buffer* out,
                    rmd_error* e) {
  if (!out || (!files && count))
    return RMD_FAIL(e, RMD_ERR_INVALID_ARGUMENT, nullptr);
  if (!count) return RMD_FAIL(e, RMD_ERR_EMPTY_ARCHIVE, nullptr);
  if (count > UINT16_MAX)
    return RMD_FAIL(e, RMD_ERR_ARCHIVE_TOO_LARGE, nullptr);
  rmd_name_node* names = nullptr;
  rmd_status status = RMD_OK;
  uint64_t body = 0, dir = 0;
  for (size_t i = 0; i < count; i++) {
    rmd_str name = files[i].name;
    status = rmd_validate_name(name, RMD_DEVICE, e);
    if (status != RMD_OK) break;
    status = rmd_names_insert(&names, name);
    if (status != RMD_OK) {
      rmd_fail(e, status, name, 0, 0, nullptr);
      break;
    }
    if (!files[i].data.bytes && files[i].data.size) {
      status = RMD_FAIL(e, RMD_ERR_INVALID_ARGUMENT, nullptr);
      break;
    }
    if (files[i].data.size > UINT32_MAX) {
      status = rmd_fail(e, RMD_ERR_FILE_TOO_LARGE, name, files[i].data.size,
                        UINT32_MAX, nullptr);
      break;
    }
    body += 30 + name.length + files[i].data.size;
    dir += 46 + name.length;
  }
  rmd_names_free(names);
  if (status != RMD_OK) return status;
  if (body > UINT32_MAX || dir > UINT32_MAX ||
      body + dir + 22 + (uint64_t)count * 16 > UINT32_MAX ||
      body + dir + 22 > SIZE_MAX)
    return RMD_FAIL(e, RMD_ERR_ARCHIVE_TOO_LARGE, nullptr);
  rmd_buffer result = {rmd_alloc((size_t)(body + dir + 22)),
                       (size_t)(body + dir + 22)};
  if (!result.bytes) return RMD_FAIL(e, RMD_ERR_OUT_OF_MEMORY, nullptr);
  size_t at = 0, central = (size_t)body;
  for (size_t i = 0; i < count; i++) {
    const rmd_file* file = &files[i];
    uint32_t size = (uint32_t)file->data.size;
    write_record r = {{file->name, size, size, rmd_crc32(0, file->data), 0},
                      (uint32_t)at};
    local_header(result.bytes + at, &r);
    at += 30;
    memcpy(result.bytes + at, file->name.bytes, file->name.length);
    at += file->name.length;
    assert(file->data.bytes || size == 0); /* Validated before allocation. */
    if (size) memcpy(result.bytes + at, file->data.bytes, size);
    at += size;
    central_header(result.bytes + central, &r);
    central += 46;
    memcpy(result.bytes + central, file->name.bytes, file->name.length);
    central += file->name.length;
  }
  end_header(result.bytes + central, count, (uint32_t)dir, (uint32_t)body);
  *out = result;
  return RMD_OK;
}

rmd_status rmd_writer_open(const char* path, rmd_compression compression,
                           rmd_layout layout, rmd_writer** out, rmd_error* e) {
  if (!path || !out ||
      (compression != RMD_STORE && compression != RMD_DEFLATE) ||
      (layout != RMD_DEVICE && layout != RMD_PACKAGE))
    return RMD_FAIL(e, RMD_ERR_INVALID_ARGUMENT, nullptr);
  rmd_writer* w = rmd_calloc(1, sizeof *w);
  if (!w) return RMD_FAIL(e, RMD_ERR_OUT_OF_MEMORY, nullptr);
  w->file = rmd_fopen(path, true);
  if (!w->file) {
    free(w);
    return RMD_FAIL(e, RMD_ERR_IO, "cannot open output file");
  }
  w->compression = compression;
  w->layout = layout;
  *out = w;
  return RMD_OK;
}

/* Validates name, grows the record array if needed, and fills *out with
 * a new record positioned at the writer's current offset; does not write
 * anything yet. */
static rmd_status prepare(rmd_writer* w, rmd_str name, write_record* out,
                          rmd_error* e) {
  if (!w) return RMD_FAIL(e, RMD_ERR_INVALID_ARGUMENT, nullptr);
  if (w->finished) return RMD_FAIL(e, RMD_ERR_ARCHIVE_FINISHED, nullptr);
  rmd_status status = rmd_validate_name(name, w->layout, e);
  if (status != RMD_OK) return status;
  if (w->count == UINT16_MAX || w->offset > UINT32_MAX ||
      w->directory_size + 46 + name.length > UINT32_MAX)
    return RMD_FAIL(e, RMD_ERR_ARCHIVE_TOO_LARGE, nullptr);
  if (w->count == w->capacity) {
    size_t n = w->capacity ? w->capacity * 2 : 16;
    void* p = rmd_realloc(w->records, n * sizeof *w->records);
    if (!p) return RMD_FAIL(e, RMD_ERR_OUT_OF_MEMORY, nullptr);
    w->records = p;
    w->capacity = n;
  }
  char* copy = rmd_alloc(name.length + 1);
  if (!copy) return RMD_FAIL(e, RMD_ERR_OUT_OF_MEMORY, nullptr);
  memcpy(copy, name.bytes, name.length);
  copy[name.length] = 0;
  status = rmd_names_insert(&w->names, name);
  if (status != RMD_OK) {
    free(copy);
    return rmd_fail(e, status, name, 0, 0, nullptr);
  }
  *out = (write_record){.entry = {.name = {copy, name.length}},
                        .offset = (uint32_t)w->offset};
  return RMD_OK;
}

/* Writes n bytes to the output file and advances w->offset; marks the
 * writer finished on overflow or I/O failure. */
static rmd_status append(rmd_writer* w, const void* p, size_t n, rmd_error* e) {
  if ((uint64_t)n > UINT32_MAX - w->offset) {
    w->finished = true;
    return RMD_FAIL(e, RMD_ERR_ARCHIVE_TOO_LARGE, nullptr);
  }
  if (n && fwrite(p, 1, n, w->file) != n) {
    w->finished = true;
    return RMD_FAIL(e, RMD_ERR_IO, "output write failed");
  }
  w->offset += n;
  return RMD_OK;
}

/* Writes r's local header followed by its name. */
static rmd_status start(rmd_writer* w, const write_record* r, rmd_error* e) {
  uint8_t header[30];
  local_header(header, r);
  rmd_status s = append(w, header, sizeof header, e);
  if (s == RMD_OK) s = append(w, r->entry.name.bytes, r->entry.name.length, e);
  return s;
}

/* Appends r to the writer's records and the pending directory size. */
static void remember(rmd_writer* w, write_record r) {
  w->records[w->count++] = r;
  w->directory_size += 46 + r.entry.name.length;
}

rmd_status rmd_writer_add(rmd_writer* w, rmd_file file,
                          rmd_compression compression, rmd_error* e) {
  if (!w || (!file.data.bytes && file.data.size) ||
      (compression != RMD_DEFAULT_COMPRESSION && compression != RMD_STORE &&
       compression != RMD_DEFLATE))
    return RMD_FAIL(e, RMD_ERR_INVALID_ARGUMENT, nullptr);
  if (w->finished) return RMD_FAIL(e, RMD_ERR_ARCHIVE_FINISHED, nullptr);
  rmd_status status = rmd_validate_name(file.name, w->layout, e);
  if (status != RMD_OK) return status;
  if (file.data.size > UINT32_MAX)
    return rmd_fail(e, RMD_ERR_FILE_TOO_LARGE, file.name, file.data.size,
                    UINT32_MAX, nullptr);
  status = rmd_names_check(w->names, file.name);
  if (status != RMD_OK) return rmd_fail(e, status, file.name, 0, 0, nullptr);
  rmd_buffer compressed = {0};
  if ((compression == RMD_DEFAULT_COMPRESSION ? w->compression : compression) ==
      RMD_DEFLATE) {
    status = rmd_deflate(file.data, &compressed);
    if (status != RMD_OK) return RMD_FAIL(e, status, nullptr);
  }
  write_record r = {0};
  status = prepare(w, file.name, &r, e);
  if (status != RMD_OK) {
    rmd_buffer_free(&compressed);
    return status;
  }
  rmd_bytes data = compressed.bytes
                       ? (rmd_bytes){compressed.bytes, compressed.size}
                       : file.data;
  r.entry.method = compressed.bytes ? 8 : 0;
  r.entry.crc32 = rmd_crc32(0, file.data);
  r.entry.compressed_size = (uint32_t)data.size;
  r.entry.uncompressed_size = (uint32_t)file.data.size;
  status = start(w, &r, e);
  if (status == RMD_OK) status = append(w, data.bytes, data.size, e);
  rmd_buffer_free(&compressed);
  if (status != RMD_OK) {
    free((void*)(uintptr_t)r.entry.name.bytes);
    w->finished = true;
    return status;
  }
  remember(w, r);
  return RMD_OK;
}

rmd_status rmd_writer_add_file(rmd_writer* w, rmd_str name, const char* path,
                               rmd_error* e) {
  if (!w || !path) return RMD_FAIL(e, RMD_ERR_INVALID_ARGUMENT, nullptr);
  if (w->finished) return RMD_FAIL(e, RMD_ERR_ARCHIVE_FINISHED, nullptr);
  rmd_status status = rmd_validate_name(name, w->layout, e);
  if (status != RMD_OK) return status;
  status = rmd_names_check(w->names, name);
  if (status != RMD_OK) return rmd_fail(e, status, name, 0, 0, nullptr);
  FILE* source = rmd_fopen(path, false);
  if (!source) return RMD_FAIL(e, RMD_ERR_IO, "cannot open source file");
  constexpr size_t chunk_size = 1u << 20;
  uint8_t* chunk = rmd_alloc(chunk_size);
  if (!chunk) {
    fclose(source);
    return RMD_FAIL(e, RMD_ERR_OUT_OF_MEMORY, nullptr);
  }
  write_record r = {0};
  status = prepare(w, name, &r, e);
  if (status != RMD_OK) {
    free(chunk);
    fclose(source);
    return status;
  }
  status = start(w, &r, e);
  uint64_t size = 0;
  uint32_t crc = 0;
  while (status == RMD_OK) {
    size_t n = fread(chunk, 1, chunk_size, source);
    if (ferror(source)) {
      status = RMD_FAIL(e, RMD_ERR_IO, "source read failed");
      break;
    }
    if (!n) break;
    size += n;
    if (size > UINT32_MAX) {
      status =
          rmd_fail(e, RMD_ERR_FILE_TOO_LARGE, name, size, UINT32_MAX, nullptr);
      break;
    }
    crc = rmd_crc32(crc, (rmd_bytes){chunk, n});
    status = append(w, chunk, n, e);
  }
  free(chunk);
  if (fclose(source) && status == RMD_OK)
    status = RMD_FAIL(e, RMD_ERR_IO, "source close failed");
  if (status == RMD_OK) {
    uint8_t patch[12];
    rmd_put32(patch, crc);
    rmd_put32(patch + 4, (uint32_t)size);
    rmd_put32(patch + 8, (uint32_t)size);
    if (rmd_seek(w->file, (uint64_t)r.offset + 14) ||
        fwrite(patch, 1, sizeof patch, w->file) != sizeof patch ||
        rmd_seek(w->file, w->offset))
      status = RMD_FAIL(e, RMD_ERR_IO, "header patch failed");
  }
  if (status != RMD_OK) {
    free((void*)(uintptr_t)r.entry.name.bytes);
    w->finished = true;
    return status;
  }
  r.entry.crc32 = crc;
  r.entry.compressed_size = (uint32_t)size;
  r.entry.uncompressed_size = (uint32_t)size;
  remember(w, r);
  return RMD_OK;
}

rmd_status rmd_writer_finish(rmd_writer* w, rmd_error* e) {
  if (!w) return RMD_FAIL(e, RMD_ERR_INVALID_ARGUMENT, nullptr);
  if (w->finished) return RMD_FAIL(e, RMD_ERR_ARCHIVE_FINISHED, nullptr);
  if (!w->count) return RMD_FAIL(e, RMD_ERR_EMPTY_ARCHIVE, nullptr);
  if (w->offset + w->directory_size + 22 > UINT32_MAX)
    return RMD_FAIL(e, RMD_ERR_ARCHIVE_TOO_LARGE, nullptr);
  w->finished = true;
  uint32_t offset = (uint32_t)w->offset;
  rmd_status status = RMD_OK;
  for (size_t i = 0; i < w->count; i++) {
    uint8_t header[46];
    central_header(header, &w->records[i]);
    status = append(w, header, sizeof header, e);
    if (status != RMD_OK) break;
    status = append(w, w->records[i].entry.name.bytes,
                    w->records[i].entry.name.length, e);
    if (status != RMD_OK) break;
  }
  if (status == RMD_OK) {
    uint8_t header[22];
    end_header(header, w->count, (uint32_t)w->directory_size, offset);
    status = append(w, header, sizeof header, e);
  }
  if (fclose(w->file) && status == RMD_OK)
    status = RMD_FAIL(e, RMD_ERR_IO, "output close failed");
  w->file = nullptr;
  return status;
}

void rmd_writer_close(rmd_writer* w) {
  if (!w) return;
  if (w->file) fclose(w->file);
  for (size_t i = 0; i < w->count; i++)
    free((void*)(uintptr_t)w->records[i].entry.name.bytes);
  free(w->records);
  rmd_names_free(w->names);
  free(w);
}
