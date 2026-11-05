/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "rmr_files.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Whether the first three bytes of name spell prefix, ignoring ASCII case. */
static bool device_prefix(const char* name, const char* prefix) {
  for (size_t i = 0; i < 3; i++) {
    char c = name[i] >= 'a' && name[i] <= 'z' ? (char)(name[i] - 'a' + 'A')
                                              : name[i];
    if (c != prefix[i]) return false;
  }
  return true;
}

/* Whether Win32 would not open name as a plain file in the directory: drive or
 * stream colons, wildcard and reserved characters, control characters, a DOS
 * device basename (CON, PRN, AUX, NUL, COM0-9 or LPT0-9, also with a
 * superscript 1-3, before the first dot and in any case) or a trailing dot or
 * space, which path normalization strips. POSIX accepts all of these, so the
 * check is Windows-only. */
static bool reserved_name(const char* name, size_t length) {
  for (size_t i = 0; i < length; i++) {
    unsigned char c = (unsigned char)name[i];
    if (c < 0x20 || strchr(":<>\"|?*", c)) return true;
  }
  if (name[length - 1] == '.' || name[length - 1] == ' ') return true;

  /* Win32 also ignores spaces between a device name and its extension. */
  size_t base = 0;
  while (base < length && name[base] != '.') base++;
  while (base && name[base - 1] == ' ') base--;
  if (base == 3)
    return device_prefix(name, "CON") || device_prefix(name, "PRN") ||
           device_prefix(name, "AUX") || device_prefix(name, "NUL");
  if (base < 4 || base > 5 ||
      !(device_prefix(name, "COM") || device_prefix(name, "LPT")))
    return false;
  if (base == 4) return name[3] >= '0' && name[3] <= '9';
  /* U+00B9, U+00B2 and U+00B3 in UTF-8. */
  return name[3] == '\xC2' &&
         (name[4] == '\xB9' || name[4] == '\xB2' || name[4] == '\xB3');
}
#endif

rmr_status rmr_read_resource(const char* directory, const char* name,
                             size_t length, const char* extension,
                             size_t maximum, rmr_buffer* out) {
  if (!directory || (!name && length)) return RMR_ERR_INVALID_ARGUMENT;
  if (!length || memchr(name, 0, length) || memchr(name, '/', length) ||
      memchr(name, '\\', length))
    return RMR_ERR_INVALID_ARGUMENT;
#ifdef _WIN32
  if (reserved_name(name, length)) return RMR_ERR_INVALID_ARGUMENT;
#endif

  size_t root = strlen(directory), suffix = strlen(extension);
  if (root > maximum || length > maximum - root ||
      suffix > maximum - root - length || maximum - root - length - suffix < 2)
    return RMR_ERR_LIMIT;

  size_t capacity = root + length + suffix + 2;
  char* path = malloc(capacity);
  if (!path) return RMR_ERR_OUT_OF_MEMORY;
  memcpy(path, directory, root);
  path[root] = '/';
  memcpy(path + root + 1, name, length);
  memcpy(path + root + 1 + length, extension, suffix + 1);

#ifdef _WIN32
  /* The UTF-16 copy of the path lives only until the open, like the
   * conversion fopen does elsewhere, so it is not charged to maximum. */
  int chars =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
  if (!chars) {
    free(path);
    return RMR_ERR_INVALID_ARGUMENT;
  }
  wchar_t* wide = malloc((size_t)chars * sizeof(wchar_t));
  if (!wide) {
    free(path);
    return RMR_ERR_OUT_OF_MEMORY;
  }
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
                           chars)) {
    free(wide);
    free(path);
    return RMR_ERR_INVALID_ARGUMENT;
  }
  FILE* file = _wfopen(wide, L"rb");
  free(wide);
#else
  FILE* file = fopen(path, "rb");
#endif
  free(path);
  if (!file) {
    if (errno == ENOENT) {
      *out = (rmr_buffer){0};
      return RMR_OK;
    }
    return RMR_ERR_BACKEND;
  }

  rmr_status status = RMR_OK;
#ifdef _WIN32
  if (_fseeki64(file, 0, SEEK_END) != 0) status = RMR_ERR_BACKEND;
  int64_t measured = status == RMR_OK ? _ftelli64(file) : -1;
#else
  if (fseek(file, 0, SEEK_END) != 0) status = RMR_ERR_BACKEND;
  long measured = status == RMR_OK ? ftell(file) : -1;
#endif
  if (measured < 0)
    status = RMR_ERR_BACKEND;
  else if ((uint64_t)measured > maximum - capacity)
    status = RMR_ERR_LIMIT;
  if (status != RMR_OK) {
    fclose(file);
    return status;
  }

  size_t size = (size_t)measured;
  rewind(file);
  uint8_t* bytes = malloc(size ? size : 1);
  if (!bytes)
    status = RMR_ERR_OUT_OF_MEMORY;
  else if (fread(bytes, 1, size, file) != size)
    status = RMR_ERR_BACKEND;
  if (fclose(file) != 0) status = RMR_ERR_BACKEND;
  if (status != RMR_OK) {
    free(bytes);
    return status;
  }

  *out = (rmr_buffer){bytes, size};
  return RMR_OK;
}
