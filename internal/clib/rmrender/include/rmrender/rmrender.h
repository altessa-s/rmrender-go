/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMRENDER_H
#define RMRENDER_H
#include <stddef.h>
#include <stdint.h>

#include "export.h"
#define RMRENDER_VERSION_MAJOR 1
#define RMRENDER_VERSION_MINOR 4
#define RMRENDER_VERSION_PATCH 1
constexpr double RMR_POINTS_PER_UNIT = 0.317180616;

typedef enum rmr_status : uint32_t {
  RMR_OK,
  RMR_ERR_INVALID_ARGUMENT,
  RMR_ERR_OUT_OF_MEMORY,
  RMR_ERR_LIMIT,
  RMR_ERR_UNSUPPORTED,
  RMR_ERR_BACKEND,
  RMR_ERR_CANCELLED
} rmr_status;

typedef struct {
  rmr_status code;
  size_t index;
  char message[192];
} rmr_error;

typedef struct {
  double x, y;
} rmr_point;

typedef struct {
  double x, y, width, height;
} rmr_rect;

typedef struct {
  double a, b, c, d, tx, ty;
} rmr_transform;

typedef struct {
  double red, green, blue, alpha;
} rmr_color;

typedef enum rmr_cap : uint32_t {
  RMR_CAP_ROUND,
  RMR_CAP_SQUARE,
  RMR_CAP_BUTT
} rmr_cap;

typedef enum rmr_join : uint32_t {
  RMR_JOIN_ROUND,
  RMR_JOIN_BEVEL,
  RMR_JOIN_MITER
} rmr_join;

typedef enum rmr_blend : uint32_t {
  RMR_BLEND_NORMAL,
  RMR_BLEND_DARKEN,
  RMR_BLEND_MULTIPLY
} rmr_blend;

typedef enum rmr_element_kind : uint32_t {
  RMR_MOVE,
  RMR_LINE,
  RMR_CURVE,
  RMR_CLOSE
} rmr_element_kind;

typedef struct {
  rmr_element_kind kind;
  rmr_point end, control1, control2;
} rmr_element;

typedef struct {
  const rmr_element* elements;
  size_t count;
} rmr_path_view;

typedef struct {
  rmr_element* elements;
  size_t count;
} rmr_path;

/* Width is already calibrated, in page units; coordinates are .rm coordinates
 * (x from page centre, y down). Outline does NOT apply pressure or raw-width
 * scaling. */
typedef struct {
  rmr_point position;
  double width;
} rmr_sample;

typedef struct {
  size_t maximum_points, maximum_elements, maximum_draws, maximum_plan_bytes;
  size_t maximum_bitmap_bytes, maximum_pdf_bytes;
  uint32_t maximum_bitmap_side;
  double maximum_coordinate, maximum_width;
} rmr_limits;

typedef enum rmr_draw_mode : uint32_t {
  RMR_FILL,
  RMR_STROKE,
  RMR_IMAGE,
  RMR_FILL_STROKE,
  RMR_TEXT,
  RMR_PDF
} rmr_draw_mode;

/* Optional one-bit image mask: MSB first, row zero at top, 0 paints, 1 is
 * paper. A zero-initialized view disables masking. Nonempty masks are supported
 * only for RMR_FILL, clipped to its path, and deep-copied into the plan. */
typedef struct {
  const uint8_t* bytes;
  size_t width, height, stride;
  rmr_rect box;
} rmr_mask_view;

/* Encoded PNG/JPEG, copied into the plan. Transform maps top-down normalized
 * image coordinates (u,v) to page coordinates. RMR_IMAGE uses path as its
 * nonzero clip, color.alpha and blend as paint; RGB is ignored. Images cannot
 * carry masks, erasers or groups. Decoder and decoded-size limits belong to
 * the backend; plan byte limits bound the encoded data. */
typedef struct {
  const uint8_t* bytes;
  size_t size;
  rmr_transform transform;
} rmr_image_view;

/* One UTF-8 Helvetica line, font fallback/shaping supplied by the platform.
 * Baseline origin and relative ink/typographic bounds are y-down page units.
 * At most 65,536 UTF-8 bytes; plans deep-copy bytes (no terminator required).
 * Bounds come from the same text provider used by the drawing backend. No path,
 * mask, image, erasers or group on TEXT. Color/blend apply normally. */
typedef struct {
  const char* utf8;
  size_t size;
  double font_size;
  rmr_point origin;
  rmr_rect bounds;
} rmr_text_view;

typedef struct {
  rmr_path_view path;
  rmr_draw_mode mode;
  rmr_color color;
  rmr_blend blend;
  double line_width;
  rmr_cap cap;
  rmr_join join;
  rmr_mask_view mask;
  /* Subtract these nonzero-filled contours in order before painting. Stroke
   * mode first becomes its native stroked outline; mask applies afterward.
   * Plans own deep copies. Eraser references count toward maximum_elements. */
  const rmr_path_view* erasers;
  size_t eraser_count;
  /* Contiguous equal nonzero tokens form one isolated transparency group.
   * A different token (including zero) ends the group. Members must have
   * color.alpha=1 and blend=NORMAL; group style is identical within a run.
   * Tokens have no relationship across runs. Zero means ordinary drawing. */
  uint64_t group;
  double group_alpha;
  rmr_blend group_blend;
  rmr_image_view image;
  /* FILL_STROKE uses color for fill, stroke_color for outline, each with
   * its own alpha, in one native operation. No masks/erasers/groups on this
   * mode. Native MITER uses miter_limit (default 10); rmr_outline uses limit 2.
   */
  rmr_color stroke_color;
  rmr_text_view text;
  /* RMR_PDF: image.bytes contains an entire PDF, image.transform maps a
   * normalized top-down media box to page space. pdf_page is zero-based.
   * Path clips the source; no masks/erasers/groups. Source rotation is
   * honored. PDF input is copied, native decoding is backend-owned. */
  size_t pdf_page;
  /* Zero selects the backend default 10; explicit values must be >= 1. */
  double miter_limit;
} rmr_draw;

typedef struct rmr_plan rmr_plan;

typedef struct {
  uint8_t* bytes;
  size_t size;
} rmr_buffer;

/* One render call owns its context. Immutable plans may be shared between
 * calls. All output arguments are unchanged on failure. Error is optional and
 * unchanged on success. Owning results must not be copied; free before reusing
 * an output. */
/* Library version string (e.g. "1.2.0"), statically owned. */
RMR_API const char* rmr_version(void);

/* Static, null-terminated description of status, for logging. */
RMR_API const char* rmr_status_string(rmr_status status);

/* Built-in default limits, used wherever a limits pointer is null. */
RMR_API rmr_limits rmr_limits_default(void);

/* Releases path's elements and zeroes it; a null pointer is a no-op. */
RMR_API void rmr_path_free(rmr_path* path);

/* Releases buffer's bytes and zeroes it; a null pointer is a no-op. */
RMR_API void rmr_buffer_free(rmr_buffer* buffer);

/* Calibrated stroke outline from sampled centerline points and widths. */
[[nodiscard]] RMR_API rmr_status rmr_outline(const rmr_sample* points,
                                             size_t count, rmr_cap cap,
                                             rmr_join join,
                                             const rmr_limits* limits,
                                             rmr_path* out, rmr_error* error);

/* Deep copies commands and paths. Viewport crops; geometry is never silently
 * rescaled to fit it. Draw order is caller supplied. No automatic layer
 * grouping. */
[[nodiscard]] RMR_API rmr_status
rmr_plan_create(const rmr_draw* draws, size_t count, rmr_rect viewport,
                const rmr_limits* limits, rmr_plan** out, rmr_error* error);

/* Concatenate borrowed plans in order into an independent owned plan.
 * The caller accounts for borrowed input memory; scratch/result share limits.
 * Group tokens are kept distinct across plan boundaries. */
[[nodiscard]] RMR_API rmr_status
rmr_plan_concat(const rmr_plan* const* plans, size_t count, rmr_rect viewport,
                const rmr_limits* limits, rmr_plan** out, rmr_error* error);

/* Releases plan's draws, elements, masks and eraser storage; a null pointer
 * is a no-op. */
RMR_API void rmr_plan_free(rmr_plan* plan);

/* Accounted owned heap bytes; excludes allocator/native framework overhead. */
RMR_API size_t rmr_plan_bytes(const rmr_plan* plan);

/* Number of draw commands; null plan returns 0. */
RMR_API size_t rmr_plan_count(const rmr_plan* plan);

/* The viewport plan was created with, or {0,0,0,0} if plan is null. */
RMR_API rmr_rect rmr_plan_viewport(const rmr_plan* plan);

/* Borrowed draw at index, valid until plan is freed; out-of-range or null
 * plan returns nullptr. */
RMR_API const rmr_draw* rmr_plan_draw(const rmr_plan* plan, size_t index);

/* Bounds conservatively include curve control points and stroked caps/joins. */
RMR_API rmr_rect rmr_plan_bounds(const rmr_plan* plan);

/* Maps a .rm viewport to a top-left-origin, y-down PDF transform at
 * points_per_unit scale. */
[[nodiscard]] RMR_API rmr_status rmr_page_to_pdf(rmr_rect viewport,
                                                 double points_per_unit,
                                                 rmr_transform* out,
                                                 rmr_error* error);

/* Applies transform to point. */
RMR_API rmr_point rmr_transform_point(rmr_transform transform, rmr_point point);

/* Rounded output sheet and .rm -> bottom-left, y-up output transform.
 * Width/height are output units (PDF points, or pixels for a bitmap).
 * No native handles; constructing this value allocates nothing. */
typedef struct {
  double width, height;
  rmr_transform transform;
} rmr_sheet;

typedef enum rmr_paper : uint32_t {
  RMR_PAPER_A4,
  RMR_PAPER_LETTER,
  RMR_PAPER_TABLET
} rmr_paper;

/* A4/Letter turn with a landscape canvas; TABLET uses POINTS_PER_UNIT. */
[[nodiscard]] RMR_API rmr_status rmr_paper_points(rmr_paper paper,
                                                  double canvas_width,
                                                  double canvas_height,
                                                  rmr_point* out,
                                                  rmr_error* error);

/* Round max(floor, bounds.size*scale) to whole units, ties away from zero.
 * Centre horizontally and align the content's top; do not rescale after
 * rounding. bounds is in .rm coordinates (y down).
 * A rounded zero size or arithmetic overflow returns LIMIT. */
[[nodiscard]] RMR_API rmr_status rmr_sheet_create(rmr_rect bounds,
                                                  double floor_width,
                                                  double floor_height,
                                                  double scale, rmr_sheet* out,
                                                  rmr_error* error);

/* Paper floor with the fixed width-derived scale, for a measured page. */
[[nodiscard]] RMR_API rmr_status
rmr_paper_sheet(rmr_paper paper, double canvas_width, double canvas_height,
                rmr_rect bounds, rmr_sheet* out, rmr_error* error);

#endif
