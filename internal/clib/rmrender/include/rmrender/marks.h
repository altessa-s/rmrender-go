/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_MARKS_H
#define RMR_MARKS_H
#include <rmmarks/types.h>

#include "output.h"

typedef struct rmr_page_marks rmr_page_marks;

/* Borrowed view, valid only while marks is alive. Null marks returns null. */
RMR_API const rmm_marked_page* rmr_page_marks_view(const rmr_page_marks* marks);

/* Frees marks's bands, highlights and owned strings; a null pointer is a
 * no-op. */
RMR_API void rmr_page_marks_free(rmr_page_marks* marks);

typedef struct {
  rmr_bitmap bitmap;
  const rmm_mark_strip* strips;
  size_t count;
} rmr_mark_sheet;

/* Frees bitmap and strips and zeroes sheet; a null pointer is a no-op. */
RMR_API void rmr_mark_sheet_free(rmr_mark_sheet* sheet);

/* Expands box by margin: twice horizontally, once vertically, clamped to
 * [0,1]. */
RMR_API rmm_page_box rmr_mark_around(rmm_page_box box, double margin);

#endif
