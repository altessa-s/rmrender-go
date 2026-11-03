/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Portable grain sampling: SplitMix64 lattice hashes and capsule pressure maps.
 */
#include "rmr_internal.h"
#include "rmr_tiles.h"

/* SplitMix64 finalizer: scrambles z into a well-distributed 64-bit value. */
static uint64_t mix(uint64_t z) {
  z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
  z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
  return z ^ (z >> 31);
}

/* Hash of lattice cell (x, y), salted by salt. */
static uint64_t hash(int64_t x, int64_t y, uint64_t salt) {
  return mix(mix((uint64_t)x + salt) ^ (uint64_t)y);
}

/* Hashed pseudo-random value in [0,1) for lattice cell (x, y), salted by
 * salt. */
static double unit(int64_t x, int64_t y, uint64_t salt) {
  return (double)(hash(x, y, salt) >> 11) * 0x1p-53;
}

double rmr_grain_noise(int64_t x, int64_t y) {
  return unit(x, y, UINT64_C(0x6772617068697465));
}

unsigned int rmr_stamp_variant(float x, float y) {
  if (!isfinite(x) || !isfinite(y) || fabs((double)x) > 1e12 ||
      fabs((double)y) > 1e12)
    return 0;
  uint64_t bits = (uint64_t)(int64_t)roundf(x) + (uint64_t)(int64_t)roundf(y);
  bits = (bits ^ (bits >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
  bits = (bits ^ (bits >> 27)) * UINT64_C(0x94D049BB133111EB);
  return (unsigned int)((bits ^ (bits >> 31)) & 3);
}

/* Pressure normalized to [0,1]; a nonfinite input is treated as zero. */
static double pressure(float p) {
  return isfinite(p) ? rmr_clamp((double)p / 255, 0, 1) : 0;
}

/* Ink coverage density for grain at normalized pressure p, interpolated
 * from the family's calibration table. */
static double density(rmr_grain grain, double p) {
  static const double pencil[][2] = {
      {0, .010},   {.09, .020}, {.16, .060}, {.22, .090}, {.28, .200},
      {.34, .230}, {.41, .230}, {.47, .350}, {.50, .460}, {.53, .560},
      {.59, .590}, {.66, .680}, {.72, .820}, {.78, .840}, {.84, .890},
      {.91, .960}, {1, .980}};
  static const double brush[][2] = {{0, 0},     {.19, 0},   {.22, .33},
                                    {.28, .39}, {.34, .60}, {.41, .78},
                                    {.47, .92}, {.53, 1},   {1, 1}};

  const double (*table)[2] = grain == RMR_GRAIN_BRISTLE ? brush : pencil;
  size_t count = grain == RMR_GRAIN_BRISTLE ? sizeof brush / sizeof *brush
                                            : sizeof pencil / sizeof *pencil;
  if (p <= table[0][0]) return table[0][1];

  for (size_t i = 1; i < count; i++)
    if (p < table[i][0])
      return table[i - 1][1] +
             (table[i][1] - table[i - 1][1]) *
                 ((p - table[i - 1][0]) / (table[i][0] - table[i - 1][0]));
  return table[count - 1][1];
}

/* Smoothstep of x between a and b, clamped to [0,1]. */
static double smooth(double a, double b, double x) {
  double u = rmr_clamp((x - a) / (b - a), 0, 1);
  return u * u * (3 - 2 * u);
}

/* Lattice cell index containing x, clamped to a safe integer range. */
static int64_t cell(double x) {
  return (int64_t)rmr_clamp(floor(x), -0x1p62, 0x1p62);
}

/* Interpolated noise offset at arc-length s on bristle track t, giving a
 * wavering hair line in [0,1). */
static double hair(double s, double t) {
  const uint64_t salt = UINT64_C(0x686169726C696E65);
  int64_t track = cell(t);
  double x = s / 40 + unit(track, -(INT64_C(1) << 40), salt);
  int64_t i = cell(x);
  double f = rmr_clamp(x - (double)i, 0, 1), g = f * f * (3 - 2 * f);
  double a = unit(track, i, salt), b = unit(track, i + 1, salt);
  return a + (b - a) * g;
}

/* Row or column range in [0, count) covering pixel centers in [low, high);
 * false (first/last unset) if the range is empty or invalid. */
static bool cells(double low, double high, size_t count, size_t* first,
                  size_t* last) {
  double a = fmax(ceil(low - .5), 0),
         b = fmin(floor(high - .5), (double)count - 1);
  if (!isfinite(a) || !isfinite(b) || a > b) return false;
  *first = (size_t)a;
  *last = (size_t)b;
  return true;
}

/* True if lattice position (x, y) is inked for grain at pressure byte
 * value (1..254), using the procedural tiles or density tables. */
static bool sample(rmr_grain grain, int64_t x, int64_t y, unsigned value,
                   double width) {
  double p = (double)(value - 1) / 253;
  if (grain == RMR_GRAIN_NOISE || grain == RMR_GRAIN_BRISTLE)
    return rmr_grain_noise(x, y) < density(grain, p);

  uint64_t salt = grain == RMR_GRAIN_STAMP ? UINT64_C(0x7374616D7074696C)
                                           : UINT64_C(0x62616C6C736B6970);
  uint64_t h = hash(cell((double)x / 64), cell((double)y / 64), salt);
  unsigned a = (unsigned)(h & 3), b = (a + 1 + (unsigned)((h >> 2) % 3)) & 3;
  size_t bit = (size_t)(((uint64_t)y & 63) * 64 + ((uint64_t)x & 63));
  uint8_t mask = (uint8_t)(0x80u >> (bit & 7));
  if (grain == RMR_GRAIN_STAMP) {
    size_t family = p < .6 ? 0 : p < .85 ? 1 : 2;
    return ((rmr_pencil_tiles[family * 4 + a][bit / 8] |
             rmr_pencil_tiles[family * 4 + b][bit / 8]) &
            mask) != 0;
  }

  double share = (1 - smooth(.5, .85, p)) * (width > 3 ? 3 / width : 1);
  return share <= 0 || (rmr_marker_tiles[a][bit / 8] & mask) ||
         unit(x, y, salt) >= share;
}

rmr_status rmr_grain_mask(const rmr_grain_point* points, size_t count,
                          rmr_grain grain, rmr_rect box,
                          const rmr_unit_raster* raster,
                          const rmr_limits* given, rmr_mask* out,
                          rmr_error* error) {
  if (grain == RMR_GRAIN_SOLID)
    return rmr_fail(error, RMR_ERR_UNSUPPORTED, 0, "solid ink has no mask");
  if (grain > RMR_GRAIN_SCRATCH || !out ||
      (raster && (raster->abi_version != RMR_UNIT_RASTER_ABI ||
                  raster->struct_size < sizeof *raster || !raster->render)))
    return rmr_fail(error, RMR_ERR_INVALID_ARGUMENT, 0,
                    "invalid grain arguments");

  rmr_limits limits = given ? *given : rmr_limits_default();
  rmr_status status = rmr_grain_validate(points, count, limits, error);
  if (status != RMR_OK) return status;

  rmr_mask m;
  status = rmr_mask_layout(box, limits, &m, error);
  if (status != RMR_OK) return status;

  size_t w = m.width / 2, h = m.height / 2, area = w * h,
         bytes = m.stride * m.height;
  if (area > limits.maximum_plan_bytes - bytes)
    return rmr_fail(error, RMR_ERR_LIMIT, 0, "grain map memory limit");

  uint8_t* map = rmr_calloc(area, 1);
  m.bytes = rmr_alloc(bytes);
  if (!map || !m.bytes) {
    free(map);
    free(m.bytes);
    return rmr_fail(error, RMR_ERR_OUT_OF_MEMORY, 0, nullptr);
  }

  memset(m.bytes, 255, bytes);
  double run = 0;
  size_t work = 0;
  bool moved = false;
  float sum = 0;
  for (size_t i = 0; i < count; i++) sum += (float)(points[i].width * 4);
  double mean =
      count ? rmr_clamp((double)sum / (double)count / 4, .7, 200) : .7;

  // The final iteration is the stationary streak's disc if no segment moved.
  for (size_t i = 1; i <= count; i++) {
    bool stationary = i == count;
    if (stationary && (grain != RMR_GRAIN_STREAK || moved)) break;

    rmr_grain_point a = points[stationary ? 0 : i - 1],
                    b = points[stationary ? 0 : i];
    double ax = a.x, ay = a.y, ex = (double)b.x - ax, ey = (double)b.y - ay;
    double length = sqrt(ex * ex + ey * ey);
    // Run length accumulates Float point differences; capsules use Double.
    double fx = (double)(b.x - a.x), fy = (double)(b.y - a.y);
    double advance = sqrt(fx * fx + fy * fy);
    if (grain == RMR_GRAIN_STREAK && !stationary && !(advance > 0)) continue;
    if (!stationary) moved = true;
    double radius =
        fmax(a.width, b.width) / 2 + (grain == RMR_GRAIN_STREAK ? 0 : 1);

    size_t top, bottom, left, right;
    if (!cells(fmin(ay, ay + ey) - radius - box.y,
               fmax(ay, ay + ey) + radius - box.y, h, &top, &bottom) ||
        !cells(fmin(ax, ax + ex) - radius - box.x,
               fmax(ax, ax + ex) + radius - box.x, w, &left, &right)) {
      run += advance;
      continue;
    }

    double dx = length > 0 ? ex / length : 1, dy = length > 0 ? ey / length : 0;
    double p0 = pressure(a.pressure), p1 = pressure(b.pressure);
    unsigned byte = 1 + (unsigned)round((p0 + p1) / 2 * 253);

    for (size_t row = top; row <= bottom; row++) {
      double cy = box.y + (double)row + .5 - ay;
      for (size_t col = left; col <= right; col++) {
        if (++work > limits.maximum_elements) {
          status =
              rmr_fail(error, RMR_ERR_LIMIT, work, "grain capsule work limit");
          goto done;
        }

        size_t at = row * w + col;
        double cx = box.x + (double)col + .5 - ax;
        if (grain == RMR_GRAIN_STREAK) {
          if (map[at]) continue;

          double along = cx * dx + cy * dy;
          double tau = length > 0 ? rmr_clamp(along / length, 0, 1) : 0;
          double qx = cx - dx * tau * length, qy = cy - dy * tau * length;
          double half = a.width / 2 + (b.width / 2 - a.width / 2) * tau;
          if (qx * qx + qy * qy > half * half) continue;

          double speed =
              (double)a.speed + ((double)b.speed - (double)a.speed) * tau;
          if (!isfinite(speed)) speed = 0;
          double threshold = (.55 + .45 * smooth(0, .5, p0 + (p1 - p0) * tau)) *
                             (1 - .15 * smooth(24, 160, speed));
          if (threshold >= 1 ||
              hair(run + along, dx * cy - dy * cx) < threshold)
            map[at] = 1;
        } else {
          double squared = ex * ex + ey * ey;
          double tau =
              squared > 0 ? rmr_clamp((cx * ex + cy * ey) / squared, 0, 1) : 0;
          double qx = cx - ex * tau, qy = cy - ey * tau;
          if (qx * qx + qy * qy <= radius * radius) map[at] = (uint8_t)byte;
        }
      }
    }

    run += advance;
  }

  for (size_t row = 0; row < h; row++)
    for (size_t col = 0; col < w; col++) {
      unsigned byte = map[row * w + col];
      bool ink = byte && (grain == RMR_GRAIN_STREAK ||
                          sample(grain, (int64_t)box.x + (int64_t)col,
                                 (int64_t)box.y + (int64_t)row, byte, mean));
      if (ink)
        for (size_t dy = 0; dy < 2; dy++)
          for (size_t dx = 0; dx < 2; dx++) {
            size_t pixel = col * 2 + dx;
            m.bytes[(row * 2 + dy) * m.stride + pixel / 8] &=
                (uint8_t)~(0x80u >> (pixel & 7));
          }
    }

  *out = m;
done:
  free(map);
  if (status != RMR_OK) free(m.bytes);
  return status;
}
