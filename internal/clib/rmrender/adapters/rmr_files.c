/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "rmr_files.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

rmr_status rmr_read_resource(const char* directory, const char* name,
                             size_t length, const char* extension,
                             size_t maximum, rmr_buffer* out) {
  if (!directory || (!name && length)) return RMR_ERR_INVALID_ARGUMENT;
  if (!length || memchr(name, 0, length) || memchr(name, '/', length) ||
      memchr(name, '\\', length))
    return RMR_ERR_INVALID_ARGUMENT;

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

  FILE* file = fopen(path, "rb");
  free(path);
  if (!file) {
    if (errno == ENOENT) {
      *out = (rmr_buffer){0};
      return RMR_OK;
    }
    return RMR_ERR_BACKEND;
  }

  rmr_status status = RMR_OK;
  if (fseek(file, 0, SEEK_END) != 0) status = RMR_ERR_BACKEND;
  long measured = status == RMR_OK ? ftell(file) : -1;
  if (measured < 0)
    status = RMR_ERR_BACKEND;
  else if ((unsigned long)measured > maximum - capacity)
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
