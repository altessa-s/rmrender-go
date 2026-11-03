/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_HVAR_H
#define RMR_HVAR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bounded OpenType HVAR reader. Keep interpolation fractional until scaling;
 * HarfBuzz's public advance API rounds variation deltas in design units.
 * https://learn.microsoft.com/en-us/typography/opentype/spec/otvarcommonformats
 */
typedef struct {
  const uint8_t* data;
  size_t size;
} rmr_ot_bytes;

/* True when n bytes at offset at fit within b, false on a range that
 * overflows or runs past its end. */
bool rmr_ot_has(rmr_ot_bytes b, size_t at, size_t n);

/* Big-endian unsigned value of the n bytes at offset at (n at most 4); the
 * caller must have checked bounds with rmr_ot_has. */
uint32_t rmr_ot_uint(rmr_ot_bytes b, size_t at, unsigned n);

/* Big-endian signed value of the n bytes at offset at, sign-extended from n
 * bytes; the caller must have checked bounds with rmr_ot_has. */
int32_t rmr_ot_int(rmr_ot_bytes b, size_t at, unsigned n);

/* Sub-range of b from offset at to its end, or an empty range if at is past
 * b's size. */
rmr_ot_bytes rmr_ot_tail(rmr_ot_bytes b, size_t at);

/* Interpolates the HVAR advance-width delta for glyph at the given design
 * coordinates (count values, one per axis) and writes it to result; returns
 * false if the table is malformed or the lookup falls outside it. */
bool rmr_hvar_delta(rmr_ot_bytes b, uint32_t glyph, const int* coords,
                    unsigned count, double* result);

#endif
