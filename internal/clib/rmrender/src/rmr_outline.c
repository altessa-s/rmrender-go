/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Variable-width stroke outlines: caps, joins, run reduction and stationary
 * dots. */
#include "rmr_internal.h"

static constexpr double pi = 3.14159265358979323846264338327950288;

typedef struct {
  double x, y;
  bool valid;
} direction;

typedef struct {
  rmr_path path;
  size_t capacity, maximum, bytes;
  rmr_status status;
} builder;

/* Unit direction from a to b; invalid if they coincide or the length isn't
 * finite. */
static direction between(rmr_point a, rmr_point b) {
  double x = b.x - a.x, y = b.y - a.y, length = sqrt(x * x + y * y);
  return length > 0 && isfinite(length)
             ? (direction){x / length, y / length, true}
             : (direction){0};
}

/* Direction d rotated 90 degrees, giving the rightward normal in a
 * y-down space. */
static direction right(direction d) { return (direction){-d.y, d.x, d.valid}; }

/* Angle in radians between two unit directions. */
static double angle(direction a, direction b) {
  return acos(rmr_clamp(a.x * b.x + a.y * b.y, -1, 1));
}

/* Point p displaced by distance along direction d. */
static rmr_point moved(rmr_point p, direction d, double distance) {
  return (rmr_point){p.x + d.x * distance, p.y + d.y * distance};
}

/* Appends element e to b's path, failing b with RMR_ERR_LIMIT if the
 * element or byte limit would be exceeded. */
static void emit(builder* b, rmr_element e) {
  if (b->status != RMR_OK) return;
  if (b->path.count == b->maximum || b->path.count >= b->bytes / sizeof e) {
    b->status = RMR_ERR_LIMIT;
    return;
  }

  if (b->path.count == b->capacity) {
    size_t n = b->capacity ? b->capacity * 2 : 64;
    if (n > b->maximum) n = b->maximum;
    if (n > b->bytes / sizeof e) n = b->bytes / sizeof e;
    if (n <= b->capacity || n > SIZE_MAX / sizeof e) {
      b->status = RMR_ERR_LIMIT;
      return;
    }
    void* p = rmr_realloc(b->path.elements, n * sizeof e);
    if (!p) {
      b->status = RMR_ERR_OUT_OF_MEMORY;
      return;
    }
    b->path.elements = p;
    b->capacity = n;
  }
  b->path.elements[b->path.count++] = e;
}

/* Emits a closed polygon from corners; if wind, the winding is reversed
 * when the input is counterclockwise so output orientation is
 * consistent. */
static void polygon(builder* b, rmr_point* corners, size_t n, bool wind) {
  if (n < 3) return;

  double area = 0;
  if (wind)
    for (size_t i = 0; i < n; i++) {
      rmr_point a = corners[i], c = corners[(i + 1) % n];
      area += a.x * c.y - c.x * a.y;
    }
  bool reverse = area > 0;
  for (size_t i = 0; i < n; i++)
    emit(b, (rmr_element){.kind = i ? RMR_LINE : RMR_MOVE,
                          .end = corners[reverse ? n - i - 1 : i]});
  emit(b, (rmr_element){.kind = RMR_CLOSE});
}

/* Emits a full circle of radius centered at p, approximated by four
 * Bezier curves. */
static void disc(builder* b, rmr_point p, double radius) {
  if (radius <= 0) return;

  double k = radius * 0.5522847498307933, x = p.x, y = p.y;
  emit(b, (rmr_element){.kind = RMR_MOVE, .end = {x + radius, y}});
  emit(b, (rmr_element){RMR_CURVE,
                        {x, y - radius},
                        {x + radius, y - k},
                        {x + k, y - radius}});
  emit(b, (rmr_element){RMR_CURVE,
                        {x - radius, y},
                        {x - k, y - radius},
                        {x - radius, y - k}});
  emit(b, (rmr_element){RMR_CURVE,
                        {x, y + radius},
                        {x - radius, y + k},
                        {x - k, y + radius}});
  emit(b, (rmr_element){RMR_CURVE,
                        {x + radius, y},
                        {x + k, y + radius},
                        {x + radius, y + k}});
  emit(b, (rmr_element){.kind = RMR_CLOSE});
}

/* Number of polygon segments approximating an arc of radius and sweep
 * within a fixed chord-error tolerance, clamped to [1, 64]. */
static size_t arc_steps(double radius, double sweep) {
  if (radius <= 0.1) return 1;
  double steps = ceil(fabs(sweep) / (2 * acos(1 - 0.1 / radius)));
  return (size_t)fmin(fmax(steps, 1), 64);
}

/* Emits an end cap of the given kind at p, spanning half-width r, facing
 * outward; RMR_CAP_BUTT or a nonpositive radius emits nothing. */
static void cap(builder* b, rmr_cap kind, rmr_point p, double r,
                direction outward) {
  if (kind == RMR_CAP_BUTT || r <= 0) return;

  rmr_point corners[65];
  size_t n = 0;
  if (kind == RMR_CAP_SQUARE) {
    double ox = outward.x * r, oy = outward.y * r, rx = -oy, ry = ox;
    corners[0] = (rmr_point){p.x + rx, p.y + ry};
    corners[1] = (rmr_point){p.x + rx + ox, p.y + ry + oy};
    corners[2] = (rmr_point){p.x - rx + ox, p.y - ry + oy};
    corners[3] = (rmr_point){p.x - rx, p.y - ry};
    n = 4;
  } else {
    size_t steps = arc_steps(r, pi);
    if (steps < 2) steps = 2;
    double heading = atan2(outward.y, outward.x);
    for (size_t i = 0; i <= steps; i++) {
      double a = heading + pi / 2 - pi * (double)i / (double)steps;
      corners[n++] = (rmr_point){p.x + cos(a) * r, p.y + sin(a) * r};
    }
  }
  polygon(b, corners, n, true);
}

/* Emits a corner join at p bridging before/after at half-width r in the
 * given style; emits nothing for a zero radius or a negligible turn. */
static void join(builder* b, rmr_point p, double r, direction before,
                 direction after, rmr_join kind) {
  double turn = angle(before, after);
  if (r <= 0 || turn < 1e-6) return;

  double cross = before.x * after.y - before.y * after.x,
         side = cross < 0 ? 1 : -1;
  direction n0 = right(before), n1 = right(after);
  n0.x *= side;
  n0.y *= side;
  n1.x *= side;
  n1.y *= side;
  rmr_point corners[66];
  corners[0] = p;
  size_t n = 1;
  if (kind != RMR_JOIN_ROUND) {
    corners[n++] = moved(p, n0, r);
    double reach = 1 / cos(turn / 2);
    direction bisector =
        between((rmr_point){0}, (rmr_point){n0.x + n1.x, n0.y + n1.y});
    if (kind == RMR_JOIN_MITER && isfinite(reach) && reach <= 2 &&
        bisector.valid)
      corners[n++] = moved(p, bisector, r * reach);
    corners[n++] = moved(p, n1, r);
  } else {
    double start = atan2(n0.y, n0.x), sweep = cross < 0 ? -turn : turn;
    size_t steps = arc_steps(r, turn);
    for (size_t i = 0; i <= steps; i++) {
      double a = start + sweep * (double)i / (double)steps;
      corners[n++] = (rmr_point){p.x + r * cos(a), p.y + r * sin(a)};
    }
  }
  polygon(b, corners, n, true);
}

/* True if the points strictly between from and to deviate from the
 * straight line and linear width taper between them by no more than a
 * fixed tolerance. */
static bool fits(const rmr_sample* points, size_t from, size_t to) {
  rmr_point start = points[from].position, end = points[to].position;
  direction along = between(start, end), normal = right(along);
  if (!along.valid) return false;
  double span = (end.x - start.x) * along.x + (end.y - start.y) * along.y;
  if (span <= 0) return false;

  for (size_t i = from + 1; i < to; i++) {
    double dx = points[i].position.x - start.x,
           dy = points[i].position.y - start.y;
    if (fabs(dx * normal.x + dy * normal.y) > 0.1) return false;
    double travelled = (dx * along.x + dy * along.y) / span;
    double expected =
        points[from].width +
        (points[to].width - points[from].width) * rmr_clamp(travelled, 0, 1);
    if (fabs(points[i].width - expected) > 0.25) return false;
  }

  return true;
}

rmr_status rmr_outline(const rmr_sample* points, size_t count, rmr_cap end,
                       rmr_join joining, const rmr_limits* given, rmr_path* out,
                       rmr_error* e) {
  rmr_limits l = given ? *given : rmr_limits_default();
  if (!out || (!points && count) || end > RMR_CAP_BUTT ||
      joining > RMR_JOIN_MITER || !isfinite(l.maximum_coordinate) ||
      l.maximum_coordinate <= 0 || l.maximum_coordinate > 1e12 ||
      !isfinite(l.maximum_width) || l.maximum_width <= 0 ||
      l.maximum_width > 1e6)
    return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, 0,
                    "invalid outline arguments");
  if (count > l.maximum_points || count > SIZE_MAX / sizeof(direction))
    return rmr_fail(e, RMR_ERR_LIMIT, 0, "too many stroke points");

  for (size_t i = 0; i < count; i++) {
    if (!rmr_point_valid(points[i].position, l.maximum_coordinate) ||
        !isfinite(points[i].width) || points[i].width < 0 ||
        points[i].width > l.maximum_width)
      return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, i, "invalid stroke point");
  }

  if (count < 2) {
    *out = (rmr_path){0};
    return RMR_OK;
  }

  /* Bound scratch allocation as well as the growing output. */
  size_t stride = sizeof(size_t) + sizeof(direction);
  if (count > l.maximum_plan_bytes / stride)
    return rmr_fail(e, RMR_ERR_LIMIT, 0, "outline scratch limit");
  size_t* anchors = rmr_calloc(count, sizeof *anchors);
  direction* directions = rmr_calloc(count, sizeof *directions);
  if (!anchors || !directions) {
    free(anchors);
    free(directions);
    return rmr_fail(e, RMR_ERR_OUT_OF_MEMORY, 0, nullptr);
  }

  size_t used = 1, head = 0, tail = 1;
  while (tail < count) {
    if (tail + 1 < count && tail - head < 24 && fits(points, head, tail + 1)) {
      tail++;
      continue;
    }
    anchors[used++] = tail;
    head = tail;
    tail++;
  }

  size_t bands = used - 1;
  for (size_t i = 0; i < bands; i++) {
    rmr_point a = points[anchors[i]].position,
              z = points[anchors[i + 1]].position;
    directions[i] = between(a, z);
  }

  builder b = {.maximum = l.maximum_elements,
               .bytes = l.maximum_plan_bytes - count * stride};
  direction previous = {0}, first = {0}, last = {0};
  for (size_t i = 0; i < bands && b.status == RMR_OK; i++) {
    direction d = directions[i];
    if (!d.valid) continue;
    if (!first.valid) first = d;
    last = d;
    rmr_point from = points[anchors[i]].position,
              to = points[anchors[i + 1]].position;
    double fh = points[anchors[i]].width / 2,
           th = points[anchors[i + 1]].width / 2;
    direction normal = right(d);
    rmr_point corners[] = {moved(from, normal, fh), moved(to, normal, th),
                           moved(to, normal, -th), moved(from, normal, -fh)};
    polygon(&b, corners, 4, true);
    if (previous.valid) join(&b, from, fh, previous, d, joining);
    previous = d;
  }

  if (!first.valid) {
    disc(&b, points[0].position, points[0].width / 2);
    disc(&b, points[count - 1].position, points[count - 1].width / 2);
  } else {
    cap(&b, end, points[0].position, points[0].width / 2,
        (direction){-first.x, -first.y, true});
    cap(&b, end, points[count - 1].position, points[count - 1].width / 2, last);
  }

  free(anchors);
  free(directions);

  if (b.status != RMR_OK) {
    rmr_path_free(&b.path);
    return rmr_fail(e, b.status, 0, "outline allocation or element limit");
  }

  if (b.capacity > b.path.count && b.path.count) {
    rmr_element* exact =
        rmr_realloc(b.path.elements, b.path.count * sizeof(*exact));
    if (!exact) {
      rmr_path_free(&b.path);
      return rmr_fail(e, RMR_ERR_OUT_OF_MEMORY, 0, "outline result allocation");
    }
    b.path.elements = exact;
  }

  *out = b.path;
  return RMR_OK;
}
