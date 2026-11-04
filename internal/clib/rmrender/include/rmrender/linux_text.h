/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_LINUX_TEXT_H
#define RMR_LINUX_TEXT_H
#include "linux.h"
#include "typography.h"

/* Resolve a bundled open font by resource name, without the .ttf extension.
 * Bytes are borrowed for the callback's caller. No process-wide registration.
 * Missing required fonts return UNSUPPORTED. After the 13 text fonts, the
 * optional resource NotoEmoji-Variable is requested. Return UNSUPPORTED or an
 * empty view for an unavailable optional font; other errors are propagated.
 * The bundled monochrome emoji font is isolated from system font fallback. */
typedef struct rmr_linux_fonts rmr_linux_fonts;

/* Custom font resolver, called by name as each font is first needed. */
[[nodiscard]] RMR_API rmr_status rmr_linux_fonts_create(
    rmr_font_resolve_fn resolve, void* context, const rmr_limits* limits,
    rmr_linux_fonts** out, rmr_error* error);

/* Explicit directory containing the packaged open .ttf files. An absent
 * NotoEmoji-Variable.ttf preserves the original text-only resource contract. */
[[nodiscard]] RMR_API rmr_status rmr_linux_fonts_create_directory(
    const char* directory, const rmr_limits* limits, rmr_linux_fonts** out,
    rmr_error* error);

/* Releases fonts and its native font descriptors; a null pointer is a
 * no-op. */
RMR_API void rmr_linux_fonts_free(rmr_linux_fonts* fonts);

typedef struct rmr_linux_layout rmr_linux_layout;

/* Snapshot text and build immutable paragraph/line geometry with Pango.
 * The layout retains everything needed after fonts/input are freed. Native
 * allocations are excluded from maximum_plan_bytes; input, line count and
 * explicit owned buffers remain bounded. */
[[nodiscard]] RMR_API rmr_status rmr_linux_layout_create(
    const rm_text_block* text, double page_width, const rmr_linux_fonts* fonts,
    const rmr_limits* limits, rmr_linux_layout** out, rmr_error* error);

/* Accounted owned heap bytes; null layout returns 0. */
RMR_API size_t rmr_linux_layout_bytes(const rmr_linux_layout* layout);

/* Releases layout, its paragraphs and its fonts reference; a null pointer
 * is a no-op. */
RMR_API void rmr_linux_layout_free(rmr_linux_layout* layout);

/* Borrowed anchors view into layout, valid until it is freed; null layout
 * returns nullptr. */
RMR_API const rmr_anchor_layout* rmr_linux_layout_anchors(
    const rmr_linux_layout* layout);

/* Baseline-following y below the last paragraph; false if layout is null
 * or has no paragraphs, leaving out unchanged. */
RMR_API bool rmr_linux_layout_bottom(const rmr_linux_layout* layout,
                                     double* out);

/* Vector PDF commands keep the same shaped lines used for anchor measurement.
 * Output owns its bytes and outlives layout. Viewport uses page coordinates. */
[[nodiscard]] RMR_API rmr_status rmr_linux_layout_plan(
    const rmr_linux_layout* layout, rmr_rect viewport, const rmr_limits* limits,
    rmr_plan** out, rmr_error* error);

/* Complete notebook page foreground/template composition. Native fonts are
 * required only when typed text is present. Anchors without text fail
 * explicitly. Source PDF backgrounds belong to the document/PDF layer. */
[[nodiscard]] RMR_API rmr_status rmr_linux_page_plan(
    const rm_page* page, rmr_point canvas, rmr_ink_edge edge, double margin,
    const rmr_page_resources* resources, const rmr_linux_fonts* fonts,
    const rmr_limits* limits, rmr_plan** out, rmr_error* error);

/* One notebook page as a standalone SVG document on its paper sheet
 * (rmr_paper_sheet over the page viewport). Same composition as
 * rmr_linux_page_plan, except typed text: its shaped Pango lines become glyph
 * outline paths rather than an embedded text PDF, so the SVG matches the PDF
 * output without fonts. Rendered by rmr_linux_svg; see there for raster_dpi. */
[[nodiscard]] RMR_API rmr_status rmr_linux_page_svg(
    const rm_page* page, rmr_point canvas, rmr_ink_edge edge, double margin,
    const rmr_page_resources* resources, const rmr_linux_fonts* fonts,
    rmr_paper paper, double raster_dpi, const rmr_limits* limits,
    rmr_cancel_fn cancel, void* cancel_context, rmr_buffer* out,
    rmr_error* error);

/* Handwriting only, for OCR. Text still positions anchors and contributes to
 * bounds; typed glyphs, highlights, images and templates are not drawn. */
[[nodiscard]] RMR_API rmr_status rmr_linux_page_ink_plan(
    const rm_page* page, rmr_point canvas, rmr_ink_edge edge, double margin,
    const rmr_linux_fonts* fonts, const rmr_limits* limits, rmr_plan** out,
    rmr_error* error);

/* Native text + placed geometry measurement without constructing draw plans. */
[[nodiscard]] RMR_API rmr_status rmr_linux_page_bounds(
    const rm_page* page, rmr_point canvas, rmr_ink_edge edge, double margin,
    const rmr_linux_fonts* fonts, const rmr_limits* limits, rmr_rect* out,
    rmr_error* error);

/* Visual line geometry (UTF-16 range, baseline and start/end x) for a laid
 * out paragraph/line pair. Out-of-range indices fail, leaving out
 * unchanged. */
[[nodiscard]] RMR_API rmr_status
rmr_linux_layout_line(const rmr_linux_layout* layout, size_t paragraph,
                      size_t line, rmr_text_line* out, rmr_error* error);

/* Typographic width of one sans-serif line, matching rmr_linux_page_plan's
 * shaping. less_rightmost excludes trailing glyph overhang. */
[[nodiscard]] RMR_API rmr_status rmr_linux_text_width(
    rm_str text, double size, rmr_type_face face, bool bold, bool italic,
    bool less_rightmost, const rmr_linux_fonts* fonts, const rmr_limits* limits,
    double* out, rmr_error* error);

#endif
