/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_RMLINES_H
#define RMR_RMLINES_H
#include <rmlines/types.h>

#include "grain.h"
#include "template.h"

typedef struct {
  rmr_color color;
  rmr_blend blend;
  rmr_cap cap;
  rmr_join join;
  rmr_grain grain;
  bool stroked, eraser;
  double line_width;
} rmr_stroke_style;

/* Includes calibrated widths, explicit ARGB, palettes and tool classification.
 * This query does not imply that every grain/compositing mode is implemented.
 */
[[nodiscard]] RMR_API rmr_status rmr_lines_style(const rm_stroke* stroke,
                                                 rmr_stroke_style* out,
                                                 rmr_error* error);

/* Local stroke geometry for ALL tools, including those with grain. It does not
 * resolve text anchors or apply the grain; callers must account for both. */
[[nodiscard]] RMR_API rmr_status rmr_lines_outline(const rm_stroke* stroke,
                                                   const rmr_limits* limits,
                                                   rmr_path* out,
                                                   rmr_error* error);

/* Ballpoint-1 scratch or ballpoint-2 streak mask; other grains return
 * UNSUPPORTED. Zero/one point produces an empty mask. Does not resolve anchors
 * or source groups. */
[[nodiscard]] RMR_API rmr_status rmr_lines_mask(const rm_stroke* stroke,
                                                const rmr_limits* limits,
                                                rmr_mask* out,
                                                rmr_error* error);

/* Solid and ballpoint ink, ordered erasers and contiguous shader source groups.
 * All grain is portable; text anchors are refused.
 * Tool 6 cuts only earlier intersecting ink; tool 8 paints/erases nothing but
 * is a group barrier. Source identity matters only for shader strokes. Group
 * alpha is the first fragment's. No page growth, templates, text or images.
 * Empty commands preserve group boundaries. Candidate eraser pairs are limited
 * by maximum_elements; reference storage and deep copies share plan limits.
 * Caller supplies .rm viewport (e.g. {-702,0,1404,1872}) and intended draw
 * order. */
[[nodiscard]] RMR_API rmr_status rmr_lines_plan(const rm_stroke* strokes,
                                                size_t count, rmr_rect viewport,
                                                const rmr_limits* limits,
                                                rmr_plan** out,
                                                rmr_error* error);

/* All grain families. The optional raster is validated but not invoked;
 * pass nullptr. */
[[nodiscard]] RMR_API rmr_status rmr_lines_mask_with_raster(
    const rm_stroke* stroke, const rmr_unit_raster* raster,
    const rmr_limits* limits, rmr_mask* out, rmr_error* error);

/* Like rmr_lines_plan, but accepts a unit raster for grain families that
 * need one (validated, not invoked); pass nullptr for all-grain support
 * without a raster. */
[[nodiscard]] RMR_API rmr_status rmr_lines_plan_with_raster(
    const rm_stroke* strokes, size_t count, rmr_rect viewport,
    const rmr_unit_raster* raster, const rmr_limits* limits, rmr_plan** out,
    rmr_error* error);

/* Resolve an attachment by its length-delimited filename. Borrowed bytes remain
 * valid until scene_plan returns. RMR_OK with {nullptr,0} means missing: retain
 * the image's ordering barrier without painting. Any other failure aborts.
 * No I/O is done by the adapter itself. */
typedef struct {
  const uint8_t* bytes;
  size_t size;
} rmr_asset_view;

typedef rmr_status (*rmr_asset_resolve_fn)(void* context, rm_str file,
                                           rmr_asset_view* out);

/* Compose the parser's flattened scene order: strokes plus placed images.
 * Images never receive erasers, and close shader groups even if missing or
 * degenerate. Later erasers still cut earlier ink across an image insertion.
 * Image positions must be nondecreasing; positions beyond the stroke count
 * follow all ink. Anchors, typed text and highlights return UNSUPPORTED.
 * Templates/PDF backgrounds and page growth are outside this scene API; the
 * caller supplies the viewport. Resolve is required only if images exist.
 * Scratch stroke plan + commands + final copies share maximum_plan_bytes.
 * Corrupt/unsupported image data fails during backend execution, atomically. */
[[nodiscard]] RMR_API rmr_status rmr_lines_scene_plan(
    const rm_page* page, rmr_rect viewport, rmr_asset_resolve_fn resolve,
    void* context, const rmr_unit_raster* raster, const rmr_limits* limits,
    rmr_plan** out, rmr_error* error);

/* Bounds policy. Ribbon uses the calibrated width,
 * trims overhang when the centreline stays within an edge and grows down only
 * past 47 units. Uncut retains overhang and adds the same foot for any bottom
 * crossing. Both use a centred square on sideways centreline/image overflow.
 * Centreline ignores width and adds margin only along edges that grew. */
typedef enum rmr_ink_edge : uint32_t {
  RMR_EDGE_RIBBON,
  RMR_EDGE_UNCUT,
  RMR_EDGE_CENTRELINE
} rmr_ink_edge;

/* Canvas dimensions are explicit .rm units; its origin is {-width/2,0}.
 * Returns a .rm viewport, always containing the original canvas. No rounding
 * or output-scale conversion. Erasers do not grow it; image vertices do, even
 * for missing assets. Text/anchors/highlights need a layout provider and are
 * rejected here (see rmr_apple_page_bounds). maximum_points bounds the total
 * inspected stroke points plus four vertices per image; maximum_draws bounds
 * strokes plus images.
 * margin must be finite/nonnegative and zero outside CENTRELINE mode. */
[[nodiscard]] RMR_API rmr_status rmr_lines_content_bounds(
    const rm_page* page, double canvas_width, double canvas_height,
    rmr_ink_edge edge, double margin, const rmr_limits* limits, rmr_rect* out,
    rmr_error* error);

/* Scene plan using the measured viewport. Canvas size is supplied by the
 * document layer (e.g. page.paper_size after applying orientation policy).
 * Output sizing is chosen separately (paper_sheet/sheet_create). */
[[nodiscard]] RMR_API rmr_status rmr_lines_page_plan(
    const rm_page* page, double canvas_width, double canvas_height,
    rmr_ink_edge edge, double margin, rmr_asset_resolve_fn resolve,
    void* context, const rmr_unit_raster* raster, const rmr_limits* limits,
    rmr_plan** out, rmr_error* error);

/* Document canvas: first plausible page.paper_size in the supplied order,
 * then content_size, then 1404x1872. Plausible means finite positive dimensions
 * <=100000. Null page entries and absent/implausible sizes are skipped.
 * Landscape swaps only an upright result; portrait never swaps a wide
 * declaration. Template orientation does not participate. No allocation or I/O.
 * count is bounded by maximum_elements; .content parsing/page order belong to
 * the host. */
[[nodiscard]] RMR_API rmr_status rmr_lines_document_canvas(
    const rm_page* const* pages, size_t count, rmr_point content_size,
    bool landscape, const rmr_limits* limits, rmr_point* out, rmr_error* error);

/* Borrowed resolved model must live unchanged until composed_page_plan returns.
 * OK with *out=nullptr means a missing template. Failure aborts construction.
 * The host owns discovery, JSON documents/caches and any associated I/O. */
typedef rmr_status (*rmr_template_resolve_fn)(void* context, rm_str name,
                                              const rmr_template** out);

typedef struct {
  rmr_asset_resolve_fn image;
  void* image_context;
  rmr_template_resolve_fn template;
  void* template_context;
  const rmr_unit_raster* raster;
  const rmr_text_provider* text;
} rmr_page_resources;

/* Measures content, builds the ordered scene, then prepends the named visible
 * template. Hidden/absent/empty template names never invoke the resolver.
 * Visible nonempty names require a resolver; a missing model leaves only ink
 * and images. No implicit singleton/store, template orientation or PDF import.
 * Canvas must be plausible and already oriented (normally document_canvas).
 * Template repeats cover the grown right/bottom extent, capped at 100000;
 * template constants retain the original canvas dimensions. Ink erasers never
 * affect the template. Typed document text/anchors/highlights remain
 * UNSUPPORTED. All intermediate/final plans and scratch share
 * maximum_plan_bytes; borrowed assets/models and callback-owned allocations are
 * excluded. Out is unchanged on failure. Resolve callbacks run synchronously,
 * at most once per template. Use plan_viewport with paper_sheet/sheet_create
 * for explicit output sizing. */
[[nodiscard]] RMR_API rmr_status rmr_lines_composed_page_plan(
    const rm_page* page, rmr_point canvas, rmr_ink_edge edge, double margin,
    const rmr_page_resources* resources, const rmr_limits* limits,
    rmr_plan** out, rmr_error* error);

/* Resolve a text anchor to its final pen position in .rm coordinates (y down).
 * The host/layout provider resolves paragraph/scalar offset, line baseline and
 * declared origin_x precedence. No native handles cross this interface.
 * Callback inputs are borrowed, output is used only on OK; failures propagate.
 * Results must be deterministic. Callback-owned allocations are outside core
 * budgets. */
#define RMR_ANCHOR_PROVIDER_ABI 1u
typedef struct {
  uint32_t abi_version;
  size_t struct_size;
  void* context;
  rmr_status (*resolve)(void* context, const rm_text_anchor* anchor,
                        rmr_point* out);
} rmr_anchor_provider;

typedef struct rmr_placed_scene rmr_placed_scene;

/* Geometry-only placement, not a full page renderer: typed text/highlights are
 * neither accepted nor discarded by this API. Inputs are raw parser widths;
 * normal stroke calibration still happens later in the rendering adapter.
 * Copies all strokes, points, image vertices and filename bytes. Anchored
 * strokes add Float(pen.x/y) to Float coordinates; images add Double. Resolved
 * anchor flags/data are cleared, so placing the result again is inert.
 * Identical anchors across strokes/images resolve once per call. Unanchored
 * geometry never calls the provider; a null provider is valid only without
 * anchors. Missing positions must be reported as errors, never silently left
 * untranslated. Counts use maximum_draws/maximum_points; cache probes use
 * maximum_elements; retained copies and temporary cache share
 * maximum_plan_bytes. Coordinates and resolved pen positions obey
 * maximum_coordinate; widths/paint remain unchanged. Native/callback resources
 * are borrowed. Output is unchanged on any failure. */
[[nodiscard]] RMR_API rmr_status rmr_lines_place_scene(
    const rm_stroke* strokes, size_t stroke_count, const rm_page_image* images,
    size_t image_count, const rmr_anchor_provider* provider,
    const rmr_limits* limits, rmr_placed_scene** out, rmr_error* error);

/* Releases scene and its owned strokes, points, images and filenames; a
 * null pointer is a no-op. */
RMR_API void rmr_placed_scene_free(rmr_placed_scene* scene);

/* Accounted owned heap bytes; null scene returns 0. */
RMR_API size_t rmr_placed_scene_bytes(const rmr_placed_scene* scene);

/* Number of strokes in the placed scene; null scene returns 0. */
RMR_API size_t rmr_placed_scene_stroke_count(const rmr_placed_scene* scene);

/* Borrowed stroke array, valid until scene is freed; null scene returns
 * nullptr. */
RMR_API const rm_stroke* rmr_placed_scene_strokes(
    const rmr_placed_scene* scene);

/* Number of images in the placed scene; null scene returns 0. */
RMR_API size_t rmr_placed_scene_image_count(const rmr_placed_scene* scene);

/* Borrowed image array, valid until scene is freed; null scene returns
 * nullptr. */
RMR_API const rm_page_image* rmr_placed_scene_images(
    const rmr_placed_scene* scene);

/* Borrowed paragraph layout for anchor resolution. UTF-16 ranges are sorted,
 * nonoverlapping and must end on Unicode scalar boundaries. Baselines and
 * margins use .rm coordinates. A missing typesetter or zero lines uses the
 * paragraph's baseline/margin without examining its text or invoking a caret.
 */
typedef struct {
  size_t location, length;
  double baseline;
} rmr_anchor_line;

typedef struct {
  rm_str text;
  const rmr_anchor_line* lines;
  size_t line_count;
  double indent, baseline;
  bool right_to_left, has_typesetter;
} rmr_anchor_paragraph;

typedef struct {
  double at_index, at_start;
} rmr_caret_offsets;

#define RMR_CARET_PROVIDER_ABI 1u
/* Offset values are native caret positions, not typographic widths. The same
 * shaped line must supply both values: at UTF-16 index and at range.location.
 * The provider receives indices into the borrowed layout, not native objects.
 * Built-in Apple/Linux providers accept both line endpoints and scalar
 * boundaries within the line. They reject null context/output, invalid
 * paragraph/line indices, indices outside that line and indices inside a UTF-16
 * surrogate pair with RMR_ERR_INVALID_ARGUMENT, leaving a nonnull output
 * unchanged. Native offsets within a composed character are backend-dependent;
 * use grapheme boundaries for user-visible cursor stops. */
typedef struct {
  uint32_t abi_version;
  size_t struct_size;
  void* context;
  rmr_status (*measure)(void* context, size_t paragraph, size_t line,
                        size_t utf16_index, rmr_caret_offsets* out);
} rmr_caret_provider;

typedef struct {
  double left, column;
  const rmr_anchor_paragraph* paragraphs;
  size_t count;
  const rmr_caret_provider* caret;
} rmr_anchor_layout;

/* Clamp paragraph to [0,count-1]. Anchor offset is one-based Unicode scalars,
 * with <=0 selecting the start; clamp past-end to end. At a line boundary the
 * next visual line wins; end-of-text falls back to the last line. Explicit
 * origin_x overrides computed x but still selects the visual baseline. Without
 * paragraphs return {origin_x or left,0}. RTL aligns using the caret at line
 * start, retaining trailing-space behavior. Selected text/ranges and all
 * returned coordinates are validated; other paragraphs are not traversed. No
 * allocation. maximum_elements bounds paragraph count + selected UTF-8 bytes +
 * line count; maximum_coordinate bounds metrics/positions. Outputs unchanged on
 * failure. A caret provider is only required/validated if its result is needed.
 */
[[nodiscard]] RMR_API rmr_status rmr_lines_anchor_pen(
    const rmr_anchor_layout* layout, const rm_text_anchor* anchor,
    const rmr_limits* limits, rmr_point* out, rmr_error* error);

/* Borrowed adapter context for use with lines_place_scene. Context, layout,
 * limits and native caret state must remain valid/unchanged during placement.
 * A null limits pointer selects defaults. This factory does not validate
 * layout; errors are returned when an anchor is resolved. */
typedef struct {
  const rmr_anchor_layout* layout;
  const rmr_limits* limits;
} rmr_anchor_context;

/* Wraps context in an anchor_provider that resolves through
 * rmr_lines_anchor_pen using context's layout and limits. */
RMR_API rmr_anchor_provider
rmr_lines_layout_anchor_provider(rmr_anchor_context* context);

/* Laid-out text extent. Source geometry must already have resolved anchors.
 * Highlights do not grow the page. Text bottom includes descent
 * and paragraph spacing; the bounds policy adds its own 25-unit text foot. */
typedef struct {
  double origin_x, origin_y, bottom, column_width;
  bool has_column_width;
} rmr_text_extent;

/* Like rmr_lines_content_bounds, but also grows the viewport to cover a
 * laid-out text extent (and its ink-edge-specific text foot); callers with
 * text/highlights on the page must go through this entry point. */
[[nodiscard]] RMR_API rmr_status rmr_lines_content_bounds_with_text(
    const rm_page* page, double width, double height, rmr_ink_edge edge,
    double margin, const rmr_text_extent* text, const rmr_limits* limits,
    rmr_rect* out, rmr_error* error);

/* Opaque fill color for a highlight, derived from its stroke color/ARGB,
 * with the legacy yellow family remapped to a fixed highlight yellow. */
[[nodiscard]] RMR_API rmr_status rmr_lines_highlight_color(
    const rm_highlight* highlight, rmr_color* out, rmr_error* error);

/* Explicit PDF glyph highlights, multiply-blended before handwriting. */
[[nodiscard]] RMR_API rmr_status rmr_lines_highlight_plan(
    const rm_highlight* highlights, size_t count, rmr_rect viewport,
    const rmr_limits* limits, rmr_plan** out, rmr_error* error);

typedef struct rmr_template_store rmr_template_store;

/* Filesystem adapter for our generated template files. One borrowed decoded
 * model is retained until the next resolve/free. Use one store per synchronous
 * render; concurrent calls on a store are unsupported. Names cannot traverse
 * directories. Missing files return OK/null, malformed files return an error.
 */
[[nodiscard]] RMR_API rmr_status
rmr_template_store_create(const char* directory, const rmr_limits* limits,
                          rmr_template_store** out, rmr_error* error);

/* Ordered sources: first existing file wins.
 * A malformed override returns an error rather than falling through. Directory
 * strings are copied. count=0 is blank paper. Pass custom directories first and
 * the installed resources/templates directory last for bundled fallback.
 * The same borrowed-model lifetime and serialization rules apply. */
[[nodiscard]] RMR_API rmr_status rmr_template_store_create_layered(
    const char* const* directories, size_t count, const rmr_limits* limits,
    rmr_template_store** out, rmr_error* error);

/* Releases store, its directory copies and cached model; a null pointer is
 * a no-op. */
RMR_API void rmr_template_store_free(rmr_template_store* store);

/* rmr_template_resolve_fn callback: finds name's .template file across
 * store's layered directories, decodes it, and caches the decoded model,
 * replacing any previously resolved one. */
RMR_API rmr_status rmr_template_store_resolve(void* store, rm_str name,
                                              const rmr_template** out);

#endif
