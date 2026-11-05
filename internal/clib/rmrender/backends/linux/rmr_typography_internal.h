/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_LINUX_TYPOGRAPHY_INTERNAL_H
#define RMR_LINUX_TYPOGRAPHY_INTERNAL_H
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>
#include <rmrender/linux_text.h>
#include <stdatomic.h>

#include "rmr_linux_internal.h"

struct rmr_linux_fonts {
  atomic_size_t references;
  FcConfig* config;
  FcCharSet* base_characters[5];
  PangoFontMap* map;
  int fds[14];

#ifndef __linux__
  /* Font files Fontconfig loads: private temporary files removed with the
   * fonts, or on Windows shared cache files that are kept. */
  char* paths[14];
#endif
  bool has_emoji;
};

/* Builds a new Pango layout shaping text for face/size/bold/italic, with the
 * font-family cascade and calibration attributes applied. The caller owns
 * the returned layout. */
PangoLayout* rmr_pango_layout(const rmr_linux_fonts* fonts, rm_str text,
                              double size, rmr_type_face face, bool bold,
                              bool italic);

/* Adds per-character family and calibrated-spacing attributes to attrs for
 * text[start, end), selecting fonts by script and using a dedicated emoji
 * family for keycap sequences. */
void rmr_pango_attributes(const rmr_linux_fonts* fonts, PangoAttrList* attrs,
                          rm_str text, size_t start, size_t end, double size,
                          rmr_type_face face, bool bold, bool italic);

/* Applies synthetic-advance, variable-font and calibrated spacing
 * corrections to layout's glyph runs; a no-op once already applied for the
 * layout's current serial. */
rmr_status rmr_pango_calibrate(PangoLayout* layout, const rmr_limits* limits);

/* The attribute list as set before rmr_pango_calibrate() replaced it, or the
 * layout's current attributes if calibration has not run yet. */
PangoAttrList* rmr_pango_source_attributes(PangoLayout* layout);

/* Half the calibrated spacing over text[from, to), in points: the
 * per-character amount when uniform across the range, otherwise the sum. */
double rmr_pango_cluster_spacing(PangoLayout* layout, size_t from, size_t to);

/* Half the difference between cluster's glyph-run advances and their
 * nominal, HVAR-corrected widths, in points. */
double rmr_pango_cluster_adjustment(PangoGlyphItem* run, int cluster);

/* Caret offset from calibrated spacing just before index, in points, halved
 * mid-run and negated in right-to-left text; zero outside [1, length]. */
double rmr_pango_caret_adjustment(PangoLayout* layout, size_t index);

/* Draws line at cairo's current point, routing emoji runs through colored
 * glyph rendering; retained tracks bytes kept alive against limits. */
rmr_status rmr_pango_show_line(cairo_t* cr, PangoLayoutLine* line,
                               const rmr_limits* limits, size_t* retained);

/* rmr_linux_layout_plan's geometry with glyphs as page-space outline fills
 * instead of an embedded text PDF, for SVG output. */
rmr_status rmr_linux_layout_outline_plan(const rmr_linux_layout* layout,
                                         rmr_rect viewport,
                                         const rmr_limits* limits,
                                         rmr_plan** out, rmr_error* error);

/* rmr_linux_page_plan with typed text as outlines; exported for the Linux
 * document library, not part of the public API. */
RMR_API rmr_status rmr_linux_page_outline_plan(
    const rm_page* page, rmr_point canvas, rmr_ink_edge edge, double margin,
    const rmr_page_resources* resources, const rmr_linux_fonts* fonts,
    const rmr_limits* limits, rmr_plan** out, rmr_error* error);

/* rmr_linux_layout_plan's geometry with glyphs as page-space outline fills
 * instead of an embedded text PDF, for SVG output. */
rmr_status rmr_linux_layout_outline_plan(const rmr_linux_layout* layout,
                                         rmr_rect viewport,
                                         const rmr_limits* limits,
                                         rmr_plan** out, rmr_error* error);

/* rmr_linux_page_plan with typed text as outlines; exported for the Linux
 * document library, not part of the public API. */
RMR_API rmr_status rmr_linux_page_outline_plan(
    const rm_page* page, rmr_point canvas, rmr_ink_edge edge, double margin,
    const rmr_page_resources* resources, const rmr_linux_fonts* fonts,
    const rmr_limits* limits, rmr_plan** out, rmr_error* error);

#endif
