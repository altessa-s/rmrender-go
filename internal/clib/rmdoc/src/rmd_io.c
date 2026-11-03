/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#endif
#include "rmd_internal.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <share.h>
#include <wchar.h>
#include <windows.h>

/* Converts a UTF-8 path to a newly allocated wide string for the Win32
 * wide-char APIs; NULL on conversion failure. */
static wchar_t* wide_path(const char* path) {
  int n =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
  if (!n) return nullptr;
  wchar_t* p = rmd_calloc((size_t)n, sizeof *p);
  if (!p) return nullptr;
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, p, n)) {
    free(p);
    return nullptr;
  }
  return p;
}

FILE* rmd_fopen(const char* path, bool write) {
  wchar_t* p = wide_path(path);
  if (!p) return nullptr;
  FILE* f = _wfsopen(p, write ? L"wb" : L"rb", _SH_DENYNO);
  free(p);
  return f;
}

int rmd_seek(FILE* f, uint64_t position) {
  return _fseeki64(f, (int64_t)position, SEEK_SET);
}

void rmd_unmap(rmd_bytes data) {
  if (data.size) UnmapViewOfFile(data.bytes);
}

/* Memory-maps path read-only into *out (empty if the file is zero bytes);
 * fails for anything that is not a regular, readable file. */
static rmd_status map_file(const char* path, rmd_bytes* out) {
  wchar_t* p = wide_path(path);
  if (!p) return RMD_ERR_IO;
  HANDLE file = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  free(p);
  if (file == INVALID_HANDLE_VALUE) return RMD_ERR_IO;
  LARGE_INTEGER n;
  if (!GetFileSizeEx(file, &n) || n.QuadPart < 0 ||
      (uint64_t)n.QuadPart > SIZE_MAX) {
    CloseHandle(file);
    return RMD_ERR_IO;
  }
  if (!n.QuadPart) {
    CloseHandle(file);
    *out = (rmd_bytes){0};
    return RMD_OK;
  }
  HANDLE mapping =
      CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  CloseHandle(file);
  if (!mapping) return RMD_ERR_IO;
  const uint8_t* data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  CloseHandle(mapping);
  if (!data) return RMD_ERR_IO;
  *out = (rmd_bytes){data, (size_t)n.QuadPart};
  return RMD_OK;
}
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

FILE* rmd_fopen(const char* path, bool write) {
  return fopen(path, write ? "wb" : "rb");
}

int rmd_seek(FILE* f, uint64_t position) {
  return fseeko(f, (off_t)position, SEEK_SET);
}

void rmd_unmap(rmd_bytes data) {
  if (data.size) munmap((void*)(uintptr_t)data.bytes, data.size);
}

/* Memory-maps path read-only into *out (empty if the file is zero bytes);
 * fails for anything that is not a regular, readable file. */
static rmd_status map_file(const char* path, rmd_bytes* out) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return RMD_ERR_IO;
  struct stat st;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
      (uint64_t)st.st_size > SIZE_MAX) {
    close(fd);
    return RMD_ERR_IO;
  }
  size_t n = (size_t)st.st_size;
  if (!n) {
    close(fd);
    *out = (rmd_bytes){0};
    return RMD_OK;
  }
  void* p = mmap(nullptr, n, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (p == MAP_FAILED) return RMD_ERR_IO;
  *out = (rmd_bytes){p, n};
  return RMD_OK;
}
#endif

rmd_status rmd_reader_open_file(const char* path, const rmd_limits* limits,
                                rmd_reader** out, rmd_error* e) {
  if (!path || !out) return RMD_FAIL(e, RMD_ERR_INVALID_ARGUMENT, nullptr);
  rmd_bytes data = {0};
  rmd_status status = map_file(path, &data);
  if (status != RMD_OK) return RMD_FAIL(e, status, "cannot map archive file");
  rmd_reader* r = nullptr;
  status = rmd_reader_open(data, limits, &r, e);
  if (status != RMD_OK) {
    rmd_unmap(data);
    return status;
  }
  r->mapped = true;
  *out = r;
  return RMD_OK;
}
