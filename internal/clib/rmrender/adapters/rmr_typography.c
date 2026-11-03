/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <math.h>
#include <rmrender/typography.h>
#include <stdio.h>

/* Fill *e (if given) with status and message, and return status. */
static rmr_status fail(rmr_error* e, rmr_status status, const char* message) {
  if (e) {
    *e = (rmr_error){.code = status};
    snprintf(e->message, sizeof e->message, "%s", message);
  }
  return status;
}

/* Decode one scalar at *at, rejecting overlong encodings, surrogates and
 * truncated UTF-8; advances *at and sets *units to its UTF-16 length. */
static bool scalar(rm_str text, size_t* at, size_t* units) {
  unsigned c = (unsigned char)text.bytes[(*at)++];
  if (c < 128) {
    *units = 1;
    return true;
  }
  unsigned count = c >= 0xc2 && c <= 0xdf   ? 1
                   : c >= 0xe0 && c <= 0xef ? 2
                   : c >= 0xf0 && c <= 0xf4 ? 3
                                            : 0;
  if (!count || count > text.length - *at) return false;

  unsigned value = c & (0x7fu >> count);
  for (unsigned j = 0; j < count; j++) {
    unsigned b = (unsigned char)text.bytes[(*at)++];
    if ((b & 0xc0) != 0x80) return false;
    value = (value << 6) | (b & 63);
  }

  if ((count == 1 && value < 128) || (count == 2 && value < 2048) ||
      (count == 3 && value < 65536) || value > 0x10ffff ||
      (value >= 0xd800 && value <= 0xdfff))
    return false;
  *units = value >= 65536 ? 2 : 1;
  return true;
}

/* True if t is entirely well-formed UTF-8. */
static bool utf8(rm_str t) {
  for (size_t i = 0; i < t.length;) {
    size_t units;
    if (!scalar(t, &i, &units)) return false;
  }
  return true;
}

/* True if c lies within [lo, hi]. */
static bool in(uint32_t c, uint32_t lo, uint32_t hi) {
  return c >= lo && c <= hi;
}

/* True if c falls in an Arabic script block. */
static bool arabic(uint32_t c) {
  return in(c, 0x600, 0x7bf) || in(c, 0xfb50, 0xfdff) || in(c, 0xfe70, 0xfeff);
}

/* True if c falls in a CJK or Hangul script block. */
static bool cjk(uint32_t c) {
  return in(c, 0x2e80, 0x9fff) || in(c, 0xac00, 0xd7af) ||
         in(c, 0x1100, 0x11ff);
}

/* True if c falls in a Cyrillic or combining-mark block. */
static bool cyrillic(uint32_t c) {
  return in(c, 0x400, 0x52f) || in(c, 0x370, 0x3ff) || in(c, 0x1f00, 0x1fff);
}

rmr_status rmr_lines_type_metrics(const rm_paragraph_format* format,
                                  rm_str text, const rmr_limits* given,
                                  rmr_type_metrics* out, rmr_error* error) {
  rmr_limits limits = given ? *given : rmr_limits_default();
  if (!out || (!text.bytes && text.length))
    return fail(error, RMR_ERR_INVALID_ARGUMENT, "missing text or output");
  if (text.length > limits.maximum_elements)
    return fail(error, RMR_ERR_LIMIT, "text metrics work limit");
  if (!utf8(text))
    return fail(error, RMR_ERR_INVALID_ARGUMENT, "invalid text UTF-8");

  int level = format ? rm_paragraph_format_heading_level(format) : 0;
  /* Columns: Latin, Noto, CJK, Thai, Arabic, Hebrew. */
  static const double heights[4][6] = {
      {45.55, 51.55, 54.55, 59.55, 75.55, 51.55},
      {76, 88, 93, 103, 136, 88},
      {55, 63, 67, 74, 98, 63},
      {43, 50, 53, 58, 77, 49}};
  static const double sizes[4] = {32, 64, 46, 36}, before[4] = {0, 64, 32, 16};
  if (level < 0 || level > 3) level = 0;

  int dominant = 0;
  bool first_cyrillic = false;
  double height = heights[level][0];
  for (size_t i = 0; i < text.length;) {
    uint32_t c = (uint8_t)text.bytes[i++];
    unsigned rest = c < 0x80 ? 0 : c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
    if (rest) c &= (1u << (6 - rest)) - 1u;
    while (rest--) c = (c << 6) | ((uint8_t)text.bytes[i++] & 63u);
    bool cy = cyrillic(c);
    first_cyrillic |= cy;
    int script = arabic(c)             ? 4
                 : in(c, 0xe00, 0xe7f) ? 3
                 : cjk(c)              ? 2
                 : in(c, 0x590, 0x5ff) ? 5
                 : (cy || in(c, 0x900, 0x97f) || in(c, 0x1e00, 0x1eff) ||
                    in(c, 0x300, 0x36f))
                     ? 1
                     : 0;
    height = fmax(height, heights[level][script]);
    if (script == 5) script = 1;
    if (script > dominant) dominant = script;
  }

  double delta = height - heights[level][0], boost = 0;
  if (delta > 0) {
    boost = (dominant == 4 ? .48 : dominant == 2 ? .875 : .768) * delta;
    static const double measured[4][5] = {{0, 4.61, 0, 0, 14.36},
                                          {0, 9.22, 15.05, 20.80, 28.74},
                                          {0, 6.62, 10.81, 14.95, 20.66},
                                          {0, 0, 8.45, 11.70, 0}};
    rm_paragraph_style style =
        format ? rm_paragraph_format_style(format) : RM_PARA_BASIC;
    if ((level || style == RM_PARA_BASIC || style == RM_PARA_PLAIN) &&
        measured[level][dominant] != 0)
      boost = measured[level][dominant];
  }

  double size = sizes[level], ascent = .925 * size + boost;
  *out = (rmr_type_metrics){size,
                            height,
                            ascent,
                            height - ascent,
                            before[level],
                            24,
                            4 + (first_cyrillic ? 1.069 : .925) * size,
                            level == 1 || level == 2 ? RMR_FACE_SERIF
                            : level == 3             ? RMR_FACE_MEDIUM
                                                     : RMR_FACE_SANS};
  return RMR_OK;
}
