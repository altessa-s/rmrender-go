/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* The label RMR_UNREADABLE_PLACEHOLDER draws on an unreadable page, shared by
 * the Apple and Cairo backends: what it says and where its box goes. They set
 * its text in their own way, from the face typed text is set in. Its
 * proportions are those of rmrender-swift's placeholder for a page whose
 * bytes did not arrive. */

#ifndef RMR_UNREADABLE_H
#define RMR_UNREADABLE_H
#include <math.h>
#include <rmrender/rmrender.h>

#define RMR_UNREADABLE_LABEL "Rendering error"

/* The size of the label's type against the width of its page. */
#define RMR_UNREADABLE_TYPE 0.05

/* The label's type size on page (.rm units, y down), or zero for a page with
 * no area, on which nothing is drawn. */
static inline double rmr_unreadable_size(rmr_rect page) {
  return page.width > 0 && page.height > 0 ? page.width * RMR_UNREADABLE_TYPE
                                           : 0;
}

/* Where the label goes on page, its text width wide and ascent plus descent
 * high at size: the box around it, in page coordinates, and the text's
 * baseline origin. */
typedef struct {
  rmr_rect box;
  rmr_point origin;
  double size;
} rmr_unreadable_layout;

static inline rmr_unreadable_layout rmr_unreadable_place(
    rmr_rect page, double size, double width, double ascent, double descent) {
  double x = page.x + page.width / 2, y = page.y + page.height / 2;
  double height = ascent + descent;
  return (rmr_unreadable_layout){
      .box = {x - width / 2 - size * .8, y - height / 2 - size * .5,
              width + size * 1.6, height + size},
      .origin = {x - width / 2, y + height / 2 - descent},
      .size = size};
}

/* The box's rounded outline into elements (ten of them). */
static inline void rmr_unreadable_outline(rmr_rect box, double size,
                                          rmr_element elements[10]) {
  double r = size * .35, k = r * .5522847498;
  double left = box.x, top = box.y, right = box.x + box.width,
         bottom = box.y + box.height;
  elements[0] = (rmr_element){.kind = RMR_MOVE, .end = {left + r, top}};
  elements[1] = (rmr_element){.kind = RMR_LINE, .end = {right - r, top}};
  elements[2] = (rmr_element){.kind = RMR_CURVE,
                              .end = {right, top + r},
                              .control1 = {right - r + k, top},
                              .control2 = {right, top + r - k}};
  elements[3] = (rmr_element){.kind = RMR_LINE, .end = {right, bottom - r}};
  elements[4] = (rmr_element){.kind = RMR_CURVE,
                              .end = {right - r, bottom},
                              .control1 = {right, bottom - r + k},
                              .control2 = {right - r + k, bottom}};
  elements[5] = (rmr_element){.kind = RMR_LINE, .end = {left + r, bottom}};
  elements[6] = (rmr_element){.kind = RMR_CURVE,
                              .end = {left, bottom - r},
                              .control1 = {left + r - k, bottom},
                              .control2 = {left, bottom - r + k}};
  elements[7] = (rmr_element){.kind = RMR_LINE, .end = {left, top + r}};
  elements[8] = (rmr_element){.kind = RMR_CURVE,
                              .end = {left + r, top},
                              .control1 = {left, top + r - k},
                              .control2 = {left + r - k, top}};
  elements[9] = (rmr_element){.kind = RMR_CLOSE};
}

/* The box drawn from outline: light grey, with a grey border 6% of the type
 * size wide, at least half a point. */
static inline rmr_draw rmr_unreadable_box(const rmr_element outline[10],
                                          double size) {
  return (rmr_draw){.mode = RMR_FILL_STROKE,
                    .path = {outline, 10},
                    .color = {.95, .95, .95, 1},
                    .stroke_color = {.7, .7, .7, 1},
                    .line_width = fmax(size * .06, .5 / RMR_POINTS_PER_UNIT),
                    .cap = RMR_CAP_BUTT,
                    .join = RMR_JOIN_MITER};
}

/* The grey of the label's text: dark. */
#define RMR_UNREADABLE_GREY .25

#endif
