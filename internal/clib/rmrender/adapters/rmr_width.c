/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "rmr_width.h"

#include <math.h>

double rmr_line_width(double raw) {
  double w = raw / 4;
  return isfinite(w) ? fmin(fmax(w, 0.7), 200) : 0.7;
}

float rmr_drawn_width(rm_pen_tool tool, rm_stroke_point p) {
  double scale = 1;
  if (tool == RM_TOOL_MARKER_2)
    scale = 0.74;
  else if (tool == RM_TOOL_PENCIL_2 || tool == RM_TOOL_MECHANICAL_PENCIL_2)
    scale = 0.69;
  else if (tool == RM_TOOL_PAINTBRUSH_2) {
    static const double table[][2] = {{0, 0},       {0.19, 0},    {0.22, 0.23},
                                      {0.28, 0.45}, {0.34, 0.68}, {0.41, 0.90},
                                      {0.47, 1},    {1, 1}};
    double pressure =
        isfinite(p.pressure) ? fmin(fmax((double)p.pressure / 255, 0), 1) : 0;
    double opening = 1;
    for (size_t i = 1; i < sizeof table / sizeof *table; i++)
      if (pressure < table[i][0]) {
        opening = table[i - 1][1] + (table[i][1] - table[i - 1][1]) *
                                        (pressure - table[i - 1][0]) /
                                        (table[i][0] - table[i - 1][0]);
        break;
      }
    scale = 0.72 * opening;
  }

  return scale == 1 ? p.width : p.width * (float)scale;
}
