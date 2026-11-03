/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <math.h>
#include <rmrender/rmlines.h>
#include <stdio.h>

/* Fill *e (if given) with status and message, and return status. */
static rmr_status fail(rmr_error* e, rmr_status status, const char* message) {
  if (e) {
    *e = (rmr_error){.code = status};
    snprintf(e->message, sizeof e->message, "%s", message);
  }
  return status;
}

/* True if n is finite and within [-maximum, maximum]. */
static bool valid(double n, double maximum) {
  return isfinite(n) && fabs(n) <= maximum;
}

/* Decode one scalar, rejecting overlong encodings, surrogates and truncated
 * UTF-8. */
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

/* Validate point against maximum and, if valid, write it to *out. */
static rmr_status result(rmr_point point, double maximum, rmr_point* out,
                         rmr_error* error) {
  if (!valid(point.x, maximum) || !valid(point.y, maximum))
    return fail(error, RMR_ERR_LIMIT, "anchor position out of range");
  *out = point;
  return RMR_OK;
}

rmr_status rmr_lines_anchor_pen(const rmr_anchor_layout* layout,
                                const rm_text_anchor* anchor,
                                const rmr_limits* given, rmr_point* out,
                                rmr_error* error) {
  rmr_limits l = given ? *given : rmr_limits_default();
  if (!layout || !anchor || !out || (layout->count && !layout->paragraphs) ||
      !isfinite(l.maximum_coordinate) || l.maximum_coordinate <= 0 ||
      l.maximum_coordinate > 1e12 ||
      !valid(layout->left, l.maximum_coordinate) ||
      !valid(layout->column, l.maximum_coordinate) || layout->column < 0 ||
      (anchor->has_origin_x && !valid(anchor->origin_x, l.maximum_coordinate)))
    return fail(error, RMR_ERR_INVALID_ARGUMENT, "invalid anchor layout/input");
  if (layout->count > l.maximum_elements)
    return fail(error, RMR_ERR_LIMIT, "anchor paragraph limit");
  if (!layout->count)
    return result(
        (rmr_point){anchor->has_origin_x ? anchor->origin_x : layout->left, 0},
        l.maximum_coordinate, out, error);

  size_t number = anchor->paragraph < 0 ? 0 : (size_t)anchor->paragraph;
  if (number >= layout->count) number = layout->count - 1;
  const rmr_anchor_paragraph* p = &layout->paragraphs[number];
  if (!valid(p->indent, l.maximum_coordinate) ||
      !valid(p->baseline, l.maximum_coordinate))
    return fail(error, RMR_ERR_INVALID_ARGUMENT, "invalid paragraph metrics");
  double margin = p->right_to_left ? layout->left + layout->column - p->indent
                                   : layout->left + p->indent;

  if (!p->has_typesetter || !p->line_count)
    return result((rmr_point){anchor->has_origin_x ? anchor->origin_x : margin,
                              p->baseline},
                  l.maximum_coordinate, out, error);

  size_t left = l.maximum_elements - layout->count;
  if (p->line_count > left || p->text.length > left - p->line_count)
    return fail(error, RMR_ERR_LIMIT, "anchor layout work limit");
  if (!p->lines || (p->text.length && !p->text.bytes))
    return fail(error, RMR_ERR_INVALID_ARGUMENT,
                "missing paragraph text/lines");

  size_t previous = 0;
  for (size_t i = 0; i < p->line_count; i++) {
    const rmr_anchor_line* line = &p->lines[i];
    if (line->location < previous || line->length > SIZE_MAX - line->location ||
        !valid(line->baseline, l.maximum_coordinate))
      return fail(error, RMR_ERR_INVALID_ARGUMENT,
                  "invalid visual line range/baseline");
    previous = line->location + line->length;
  }

  size_t target = anchor->offset > 0 ? (size_t)(anchor->offset - 1) : 0;
  size_t at = 0, count = 0, utf16 = 0, index = 0, edge_line = 0;
  bool end = false;
  while (at < p->text.length) {
    size_t units;
    if (!scalar(p->text, &at, &units))
      return fail(error, RMR_ERR_INVALID_ARGUMENT, "invalid paragraph UTF-8");
    /* Ranges may not split a surrogate pair. Visit sorted endpoints once. */
    while (edge_line < p->line_count) {
      const rmr_anchor_line* line = &p->lines[edge_line];
      size_t edge = line->location + (end ? line->length : 0);
      if (edge > utf16) {
        if (edge < utf16 + units)
          return fail(error, RMR_ERR_INVALID_ARGUMENT,
                      "line splits Unicode scalar");
        break;
      }
      if (end) {
        edge_line++;
        end = false;
      } else
        end = true;
    }
    utf16 += units;
    if (count < target) index = utf16;
    count++;
  }

  if (previous > utf16)
    return fail(error, RMR_ERR_INVALID_ARGUMENT, "visual line past text end");
  size_t visual = p->line_count - 1;
  for (size_t i = 0; i < p->line_count; i++)
    if (index < p->lines[i].location + p->lines[i].length) {
      visual = i;
      break;
    }

  double baseline = p->lines[visual].baseline;
  if (anchor->has_origin_x)
    return result((rmr_point){anchor->origin_x, baseline}, l.maximum_coordinate,
                  out, error);

  const rmr_caret_provider* caret = layout->caret;
  if (!caret)
    return fail(error, RMR_ERR_UNSUPPORTED, "caret provider required");
  if (caret->abi_version != RMR_CARET_PROVIDER_ABI ||
      caret->struct_size < sizeof *caret || !caret->measure)
    return fail(error, RMR_ERR_INVALID_ARGUMENT, "invalid caret provider ABI");

  rmr_caret_offsets offsets = {NAN, NAN};
  rmr_status status =
      caret->measure(caret->context, number, visual, index, &offsets);
  if (status != RMR_OK) return fail(error, status, "caret measurement failed");
  if (!valid(offsets.at_index, l.maximum_coordinate) ||
      (p->right_to_left && !valid(offsets.at_start, l.maximum_coordinate)))
    return fail(error, RMR_ERR_BACKEND, "invalid caret metrics");

  if (p->right_to_left) margin -= offsets.at_start;
  return result((rmr_point){margin + offsets.at_index, baseline},
                l.maximum_coordinate, out, error);
}

/* rmr_anchor_provider callback: resolves anchor using the bound context. */
static rmr_status resolve(void* context, const rm_text_anchor* anchor,
                          rmr_point* out) {
  rmr_anchor_context* c = context;
  if (!c) return RMR_ERR_INVALID_ARGUMENT;
  return rmr_lines_anchor_pen(c->layout, anchor, c->limits, out, nullptr);
}

rmr_anchor_provider rmr_lines_layout_anchor_provider(
    rmr_anchor_context* context) {
  return (rmr_anchor_provider){RMR_ANCHOR_PROVIDER_ABI,
                               sizeof(rmr_anchor_provider), context, resolve};
}
