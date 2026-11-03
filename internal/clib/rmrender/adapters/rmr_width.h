/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_WIDTH_H
#define RMR_WIDTH_H
#include <rmlines/types.h>
#include <rmrender/export.h>

/* Shared by the RMLines adapter and the platform marks libraries, so exported
 * from the adapter; not part of the public API. */

/* The drawn line width for a raw stroke width, bounded to [0.7, 200]. */
RMR_API double rmr_line_width(double raw);

/* The width a stroke point is drawn at, after the tool's own scaling. */
RMR_API float rmr_drawn_width(rm_pen_tool tool, rm_stroke_point p);

#endif
