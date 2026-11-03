/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* What backends/common/rmr_marks_common.c needs from the Linux backend: the
 * names it builds its entry points under and the one measurement that differs
 * per platform. */

#ifndef RMR_MARKS_PLATFORM_H
#define RMR_MARKS_PLATFORM_H
#include <rmrender/linux_marks.h>

#include "rmr_typography_internal.h"

#define mark_page_marks rmr_linux_page_marks
#define mark_mark_sheet rmr_linux_mark_sheet
#define mark_fonts rmr_linux_fonts
#define mark_layout rmr_linux_layout
#define mark_layout_create rmr_linux_layout_create
#define mark_layout_bytes rmr_linux_layout_bytes
#define mark_layout_anchors rmr_linux_layout_anchors
#define mark_layout_free rmr_linux_layout_free
#define mark_stroke_bounds rmr_linux_mark_stroke_bounds
#define mark_error rmr_linux_error

/* The box a highlighter stroke covers, drawn as the platform draws it. */
rmr_status mark_stroke_bounds(const rm_stroke* stroke,
                              const rmr_stroke_style* style, rmr_rect* out);

#endif
