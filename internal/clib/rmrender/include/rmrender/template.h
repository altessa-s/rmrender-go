/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_TEMPLATE_H
#define RMR_TEMPLATE_H
#include "rmrender.h"

/* Compiled template expressions and a borrowed vector-template model.
 * Immutable compiled programs own their source and can be evaluated
 * concurrently with independent environments. No global compilation cache. */
typedef struct rmr_expression rmr_expression;

typedef struct {
  const char* name;
  size_t length;
  double value;
} rmr_binding;

/* ASCII names ([A-Za-z_][A-Za-z_0-9]*) and decimal literals without exponent.
 * Operators, in precedence order: unary +/-; * /; + -; one comparison
 * (<= >= == != < >); &&; ||; right-associative ?:; parentheses.
 * Both operands/branches are evaluated eagerly.
 * Division by either signed zero returns +0. Boolean results are 0 or 1.
 * Nonfinite intermediate/results are retained for the caller's field policy.
 * Unicode source bytes return UNSUPPORTED. Unknown/malformed syntax returns
 * INVALID_ARGUMENT. error.index is a byte offset in the source.
 * Hard ceilings: 65,536 source bytes, 64 nested unary/parenthesis/ternary
 * levels. maximum_elements bounds instructions; maximum_plan_bytes bounds
 * program, source, instructions AND one evaluation stack. Other rmr_limits
 * fields are not used. All output/error ownership rules from rmrender.h apply.
 */
[[nodiscard]] RMR_API rmr_status rmr_expression_compile(
    const char* source, size_t length, const rmr_limits* limits,
    rmr_expression** out, rmr_error* error);

/* At most 256 bindings, valid ASCII names, last duplicate wins. Missing names
 * fail even in an unselected branch. Bindings/values are borrowed for this
 * call. One bounded stack is allocated per evaluation. No platform state is
 * changed. */
[[nodiscard]] RMR_API rmr_status rmr_expression_evaluate(
    const rmr_expression* expression, const rmr_binding* bindings, size_t count,
    double* out, rmr_error* error);

/* Releases expression, its source and compiled code; a null pointer is a
 * no-op. */
RMR_API void rmr_expression_free(rmr_expression* expression);

/* Borrowed vector-template model. Expressions must remain alive through plan
 * construction; nullptr selects the numeric value. Coordinates count from the
 * canvas top left. No asset store in this model. */
typedef struct {
  const rmr_expression* expression;
  double number;
} rmr_template_value;

typedef struct {
  const char* name;
  size_t length;
  rmr_template_value value;
} rmr_template_constant;

typedef struct {
  rmr_template_value x, y, width, height;
} rmr_template_box;

typedef enum rmr_repeat_kind : uint32_t {
  RMR_REPEAT_ONCE,
  RMR_REPEAT_COUNT,
  RMR_REPEAT_FORWARD,
  RMR_REPEAT_BACKWARD
} rmr_repeat_kind;

typedef struct {
  rmr_repeat_kind kind;
  rmr_template_value count;
} rmr_template_repeat;

typedef struct {
  rmr_element_kind kind;
  rmr_template_value x, y, control1_x, control1_y, control2_x, control2_y;
} rmr_template_element;

typedef struct rmr_template_item rmr_template_item;

typedef struct {
  rmr_template_box box;
  rmr_template_repeat rows, columns;
  const rmr_template_item* children;
  size_t count;
} rmr_template_group;

typedef struct {
  const rmr_template_element* elements;
  size_t count;
  bool has_fill, has_stroke_color, has_stroke_width;
  rmr_color fill, stroke;
  rmr_template_value stroke_width;
} rmr_template_path;

typedef enum rmr_template_kind : uint32_t {
  RMR_TEMPLATE_GROUP,
  RMR_TEMPLATE_PATH,
  RMR_TEMPLATE_TEXT
} rmr_template_kind;

typedef struct {
  const char* utf8;
  size_t size;
  bool has_font_size;
  rmr_template_value font_size;
  rmr_template_box position;
} rmr_template_text;

struct rmr_template_item {
  rmr_template_kind kind;
  union {
    rmr_template_group group;
    rmr_template_path path;
    rmr_template_text text;
  };
};

typedef struct {
  const rmr_template_constant* constants;
  size_t constant_count;
  const rmr_template_item* items;
  size_t count;
} rmr_template;

/* canvas and extent use x=width,y=height; viewport is in .rm coordinates.
 * Canvas supplies templateWidth/Height and initial parentWidth/Height. Extent
 * supplies forward repetition coverage; it never changes the canvas constants.
 * paperOriginX=(canvas.width-canvas.height)/2, paperOriginY=0. Constants
 * resolve in order and may overwrite built-ins. At most 250 constants, 32
 * nested groups. Groups inherit nonpositive box dimensions from parent; no
 * clipping to boxes. Counts truncate/clamp to 1..10000 (nonfinite => 1);
 * nonpositive step => once. Forward count=ceil((extent-start)/step);
 * backward=ceil(start/step)+1. Paths support explicit M/L/C/Z elements. Default
 * outline is grey .752941, width .5; explicit width without color is black.
 * Fill alone omits outline; fill plus explicit outline fields uses one
 * FILL_STROKE command. Square caps, miter joins (limit 10). Unknown
 * nodes/errors fail transactionally: this API never silently repairs malformed
 * paths. Maximum_elements bounds output geometry AND per-pass node/expression
 * work; maximum_draws bounds commands. Scratch, final plan and evaluation
 * stacks share maximum_plan_bytes; borrowed template/compiled-expression
 * storage is excluded. Two deterministic passes require inputs to remain
 * unchanged throughout the call. The result owns all geometry. Repeats do not
 * expand viewport automatically. */
[[nodiscard]] RMR_API rmr_status
rmr_template_plan(const rmr_template* value, rmr_point canvas, rmr_point extent,
                  rmr_rect viewport, const rmr_limits* limits, rmr_plan** out,
                  rmr_error* error);

/* Prepend the template to an existing scene. Ink erasers remain attached only
 * to their original draws, so never erase the background. The viewport and all
 * foreground commands are preserved; output owns new deep copies. If invisible
 * (e.g. rm_page.background_visible=false), template/canvas/extent are ignored
 * and only the foreground is copied. Intermediate background, command scratch
 * and final copies share the byte budget; borrowed foreground is excluded. */
[[nodiscard]] RMR_API rmr_status rmr_template_background_plan(
    const rmr_template* value, rmr_point canvas, rmr_point extent,
    const rmr_plan* foreground, bool visible, const rmr_limits* limits,
    rmr_plan** out, rmr_error* error);

typedef struct rmr_template_document rmr_template_document;

/* Owned JSON document. The input is borrowed only during decoding. Model and
 * metadata remain valid until document_free. Strings are UTF-8, NUL-terminated.
 * Missing orientation returns nullptr; categories may be empty. Unknown JSON
 * fields are ignored after validation; duplicate keys and embedded NUL strings
 * are rejected. Path commands other than explicit M/L/C/Z return
 * UNSUPPORTED. Invalid expressions/colors/schema fail without partial output.
 * At most 128 JSON nesting levels and 32 model group levels. maximum_elements
 * bounds JSON nodes and decoding work; maximum_plan_bytes bounds retained JSON,
 * model and compiled expressions plus compilation/evaluation stack allowance.
 * Input bytes are borrowed/excluded; input length is independently capped by
 * maximum_plan_bytes to bound scanning. Planning has a separate limit budget.
 * Error index is a JSON byte offset (expression failures point to their
 * string). Other rmr_limits fields apply when constructing a plan, not
 * decoding. */
[[nodiscard]] RMR_API rmr_status rmr_template_document_decode(
    const char* json, size_t length, const rmr_limits* limits,
    rmr_template_document** out, rmr_error* error);

/* Releases document, its compiled expressions and model storage; a null
 * pointer is a no-op. */
RMR_API void rmr_template_document_free(rmr_template_document* document);

/* Borrowed model view into document, valid until it is freed; null document
 * returns nullptr. */
RMR_API const rmr_template* rmr_template_document_model(
    const rmr_template_document* document);

/* Borrowed name string; null document returns nullptr. */
RMR_API const char* rmr_template_document_name(
    const rmr_template_document* document);

/* Borrowed orientation string, or nullptr if absent or document is null. */
RMR_API const char* rmr_template_document_orientation(
    const rmr_template_document* document);

/* Number of declared categories; null document returns 0. */
RMR_API size_t
rmr_template_document_category_count(const rmr_template_document* document);

/* Borrowed category string at index; out-of-range or null document returns
 * nullptr. */
RMR_API const char* rmr_template_document_category(
    const rmr_template_document* document, size_t index);

/* Text layout is explicit and platform-dependent. No native handles cross this
 * interface. A provider measures a Helvetica line (including fallback fonts),
 * returning typographic width and conservative y-down bounds relative to the
 * baseline. Inputs are borrowed; output is committed only on success. Callbacks
 * must be deterministic across the two construction passes and compatible with
 * the drawing backend. Native framework allocations are outside core budgets.
 * Caller serializes providers with mutable context. */
typedef struct {
  double width;
  rmr_rect bounds;
} rmr_text_metrics;

#define RMR_TEXT_PROVIDER_ABI 1u
typedef struct {
  uint32_t abi_version;
  size_t struct_size;
  void* context;
  rmr_status (*measure)(void* context, const char* utf8, size_t size,
                        double font_size, rmr_text_metrics* out);
} rmr_text_provider;

/* rmr_template_plan and rmr_template_background_plan use a null provider:
 * vector-only models render; text returns UNSUPPORTED. Size defaults to 24,
 * black paint. textWidth temporarily shadows a constant only while resolving
 * position. At most 256 active bindings including textWidth; a full environment
 * returns LIMIT if textWidth has to be inserted. Text bytes count toward work
 * limits. */
[[nodiscard]] RMR_API rmr_status rmr_template_plan_with_text(
    const rmr_template* value, rmr_point canvas, rmr_point extent,
    rmr_rect viewport, const rmr_text_provider* provider,
    const rmr_limits* limits, rmr_plan** out, rmr_error* error);

/* Like rmr_template_background_plan, but renders template text items
 * through provider instead of returning UNSUPPORTED for them. */
[[nodiscard]] RMR_API rmr_status rmr_template_background_plan_with_text(
    const rmr_template* value, rmr_point canvas, rmr_point extent,
    const rmr_plan* foreground, bool visible, const rmr_text_provider* provider,
    const rmr_limits* limits, rmr_plan** out, rmr_error* error);

/* Repeat coverage for a measured .rm viewport containing the original canvas.
 * width=min(max(canvas.x, viewport.x+viewport.width+canvas.x/2),100000),
 * height=min(max(canvas.y,viewport.y+viewport.height),100000). A top/left
 * extension does not shift the template origin. Canvas must be finite, positive
 * and <=100000; viewport must be finite and contain
 * {-canvas.x/2,0,canvas.x,canvas.y}, allowing eight machine epsilons of
 * coordinate-scale roundoff at unchanged edges. */
[[nodiscard]] RMR_API rmr_status rmr_template_page_extent(rmr_point canvas,
                                                          rmr_rect viewport,
                                                          rmr_point* out,
                                                          rmr_error* error);

#endif
