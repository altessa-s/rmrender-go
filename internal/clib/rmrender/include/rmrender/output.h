/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_OUTPUT_H
#define RMR_OUTPUT_H
#include "backend.h"

typedef struct {
  uint8_t* pixels;
  size_t width, height, stride;
} rmr_bitmap;

/* Premultiplied RGBA8, tightly packed, first row at top. */
/* Frees pixels and zeroes bitmap; a null bitmap is a no-op. */
RMR_API void rmr_bitmap_free(rmr_bitmap* bitmap);

#endif
