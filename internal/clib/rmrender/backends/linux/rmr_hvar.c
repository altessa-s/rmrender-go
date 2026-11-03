/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "rmr_hvar.h"

bool rmr_ot_has(rmr_ot_bytes b, size_t at, size_t n) {
  return at <= b.size && n <= b.size - at;
}

uint32_t rmr_ot_uint(rmr_ot_bytes b, size_t at, unsigned n) {
  uint32_t value = 0;
  for (unsigned i = 0; i < n; i++) value = (value << 8) | b.data[at + i];
  return value;
}

int32_t rmr_ot_int(rmr_ot_bytes b, size_t at, unsigned n) {
  uint32_t value = rmr_ot_uint(b, at, n);
  return (int32_t)(value << (32 - n * 8)) >> (32 - n * 8);
}

rmr_ot_bytes rmr_ot_tail(rmr_ot_bytes b, size_t at) {
  return at <= b.size ? (rmr_ot_bytes){b.data + at, b.size - at}
                      : (rmr_ot_bytes){0};
}

bool rmr_hvar_delta(rmr_ot_bytes b, uint32_t glyph, const int* coords,
                    unsigned count, double* result) {
  if (!rmr_ot_has(b, 0, 20) || rmr_ot_uint(b, 0, 4) != 0x10000) return false;

  uint32_t outer = 0, inner = glyph, map_offset = rmr_ot_uint(b, 8, 4);
  if (map_offset) {
    rmr_ot_bytes map = rmr_ot_tail(b, map_offset);
    if (!rmr_ot_has(map, 0, 4) || map.data[0] > 1 || (map.data[1] & 0xc0))
      return false;
    unsigned header = map.data[0] ? 6 : 4;
    if (!rmr_ot_has(map, 0, header)) return false;
    uint32_t entries = rmr_ot_uint(map, 2, header - 2);
    unsigned bytes = ((map.data[1] >> 4) & 3) + 1,
             bits = (map.data[1] & 15) + 1;
    if (!entries || entries > (map.size - header) / bytes) return false;
    size_t index = glyph < entries ? glyph : entries - 1;
    uint32_t entry = rmr_ot_uint(map, header + index * bytes, bytes);
    outer = entry >> bits;
    inner = entry & ((1u << bits) - 1);
  }

  if (outer == 0xffff && inner == 0xffff) {
    *result = 0;
    return true;
  }

  uint32_t store_offset = rmr_ot_uint(b, 4, 4);
  if (!store_offset) return false;
  rmr_ot_bytes store = rmr_ot_tail(b, store_offset);
  if (!rmr_ot_has(store, 0, 8) || rmr_ot_uint(store, 0, 2) != 1 ||
      outer >= rmr_ot_uint(store, 6, 2) ||
      !rmr_ot_has(store, 8, ((size_t)outer + 1) * 4))
    return false;

  rmr_ot_bytes regions = rmr_ot_tail(store, rmr_ot_uint(store, 2, 4));
  rmr_ot_bytes data =
      rmr_ot_tail(store, rmr_ot_uint(store, 8 + (size_t)outer * 4, 4));
  if (!rmr_ot_has(regions, 0, 4) || !rmr_ot_has(data, 0, 6)) return false;
  unsigned axes = rmr_ot_uint(regions, 0, 2),
           region_count = rmr_ot_uint(regions, 2, 2);
  if (!axes || axes != count || region_count >= 0x8000 ||
      (region_count && axes > (regions.size - 4) / 6 / region_count))
    return false;

  unsigned items = rmr_ot_uint(data, 0, 2), words = rmr_ot_uint(data, 2, 2);
  unsigned columns = rmr_ot_uint(data, 4, 2), large = (words & 0x8000) ? 4 : 2;
  words &= 0x7fff;
  if (inner >= items || words > columns ||
      !rmr_ot_has(data, 6, (size_t)columns * 2))
    return false;
  size_t row_size =
      (size_t)words * large + (size_t)(columns - words) * (large / 2);
  size_t row_start = 6 + (size_t)columns * 2;
  if ((items && row_size > (data.size - row_start) / items)) return false;

  size_t at = row_start + inner * row_size;
  double delta = 0;

  for (unsigned i = 0; i < columns; i++) {
    unsigned region = rmr_ot_uint(data, 6 + (size_t)i * 2, 2);
    if (region >= region_count) return false;
    double scalar = 1;
    for (unsigned axis = 0; axis < axes; axis++) {
      size_t p = 4 + ((size_t)region * axes + axis) * 6;
      int start = rmr_ot_int(regions, p, 2),
          peak = rmr_ot_int(regions, p + 2, 2);
      int end = rmr_ot_int(regions, p + 4, 2), value = coords[axis];
      if (!peak || start > peak || peak > end || (start < 0 && end > 0))
        continue;
      if (value == peak) continue;
      if (value <= start || value >= end) {
        scalar = 0;
        break;
      }
      scalar *= value < peak ? (double)(value - start) / (peak - start)
                             : (double)(end - value) / (end - peak);
    }
    unsigned bytes = i < words ? large : large / 2;
    delta += scalar * rmr_ot_int(data, at, bytes);
    at += bytes;
  }

  *result = delta;
  return true;
}
