/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_GRAIN_H
#define RMR_GRAIN_H
#include "rmrender.h"

typedef enum rmr_grain : uint32_t {
  RMR_GRAIN_SOLID,
  RMR_GRAIN_STAMP,
  RMR_GRAIN_NOISE,
  RMR_GRAIN_BRISTLE,
  RMR_GRAIN_STREAK,
  RMR_GRAIN_SCRATCH
} rmr_grain;

/* Width is calibrated in page units; pressure is a byte-valued float (0..255),
 * speed is the raw .rm value. Float arithmetic uses a fixed operation order. */
typedef struct {
  float x, y;
  double width;
  float pressure, speed;
} rmr_grain_point;

/* One bit per pixel, MSB first, rows top down. 0 paints, 1 is paper.
 * Two pixels per page unit. Free with rmr_mask_free; views are borrowed. */
typedef struct {
  uint8_t* bytes;
  size_t width, height, stride;
  rmr_rect box;
} rmr_mask;

/* Releases mask's bytes and zeroes it; a null pointer is a no-op. */
RMR_API void rmr_mask_free(rmr_mask* mask);

/* Integer-aligned bounding box covering every point's half-width-plus-one
 * capsule. */
[[nodiscard]] RMR_API rmr_status rmr_grain_box(const rmr_grain_point* points,
                                               size_t count,
                                               const rmr_limits* limits,
                                               rmr_rect* out, rmr_error* error);

/* A backend rasterizes ordered opaque spans into a caller-owned gray map at
 * one pixel per page unit, without antialiasing. Page origin is box.x/y, rows
 * top-down. Replace covered bytes with span.value; leave other bytes untouched.
 * Inputs/output are borrowed for the call. On failure buffer contents are
 * unspecified; no mask is published. Each call has its own zeroed buffer.
 * The callback must not retain pointers or mutate spans. */
typedef struct {
  rmr_point from, to;
  double width;
  rmr_cap cap;
  uint8_t value;
} rmr_unit_span;

#define RMR_UNIT_RASTER_ABI 1u
typedef struct {
  uint32_t abi_version;
  size_t struct_size;
  void* context;
  rmr_status (*render)(void* context, const rmr_unit_span* spans, size_t count,
                       rmr_rect box, size_t width, size_t height,
                       uint8_t* bytes, size_t stride);
} rmr_unit_raster;

/* Deterministic pseudo-random value in [0,1) for an integer lattice point. */
RMR_API double rmr_grain_noise(int64_t x, int64_t y);

/* Nonfinite or |coordinate| > 1e12 returns variant zero. */
RMR_API unsigned int rmr_stamp_variant(float x, float y);

/* Portable capsule sampling for every grain family. STREAK draws directional
 * hairs; SCRATCH selects procedural marker tiles by lattice hash and skips a
 * pressure/width-dependent share; STAMP selects procedural pencil tiles by
 * pressure; NOISE and BRISTLE threshold hashed noise against pressure density
 * tables. SOLID returns UNSUPPORTED. Box must have integer origin and
 * dimensions, at most 16384 units per side and 2^23 units total. Invalid
 * positions/widths are rejected; nonfinite pressure and speed use zero.
 * Temporary unit maps and output share maximum_plan_bytes; capsule visits
 * are bounded by maximum_elements. The optional raster is validated but not
 * invoked; pass nullptr. Output is unchanged on failure. */
[[nodiscard]] RMR_API rmr_status
rmr_grain_mask(const rmr_grain_point* points, size_t count, rmr_grain grain,
               rmr_rect box, const rmr_unit_raster* raster,
               const rmr_limits* limits, rmr_mask* out, rmr_error* error);

#endif
