/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_LINUX_MARKS_H
#define RMR_LINUX_MARKS_H
#include "linux_text.h"
#include "marks.h"

/* frame is the part of .rm space visible in the final rendered sheet, y down.
 * Resolves anchored handwriting through the same native text layout as drawing.
 * Results own text, identifiers and geometry and can be passed to RMMarks. */
[[nodiscard]] RMR_API rmr_status rmr_linux_page_marks(
    const rm_page* page, rmr_point canvas, rmr_rect frame, rm_str page_id,
    const rmr_linux_fonts* fonts, const rmr_limits* limits,
    rmr_page_marks** out, rmr_error* error);

/* Stack clipped bitmap blocks with white margins and black separator rules.
 * Source/result use RGBA premultiplied alpha, row zero at top. */
[[nodiscard]] RMR_API rmr_status rmr_linux_mark_sheet(
    const rmr_bitmap* page, const rmm_page_box* blocks, size_t count,
    size_t margin, size_t rule, const rmr_limits* limits, rmr_mark_sheet* out,
    rmr_error* error);

#endif
