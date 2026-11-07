/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <limits.h>

#include "../common/rmr_unreadable.h"
#include "rmr_typography_internal.h"

typedef struct {
  PangoLayout *layout, *marker;
  PangoLayout** lines;
  rmr_type_metrics metrics;
  rm_list_family family;
  rm_checkbox_state checkbox;
} paragraph;

struct rmr_linux_layout {
  rmr_anchor_layout anchors;
  rmr_caret_provider caret;
  rmr_anchor_paragraph* paragraphs;
  paragraph* native;
  rmr_linux_fonts* fonts;
  size_t bytes;
  double bottom;
};

/* Counts the UTF-16 code units spanned by bytes of UTF-8 text; characters
 * outside the basic plane count as two units. */
static size_t units(const char* text, size_t bytes) {
  size_t n = 0;
  const char* end = text + bytes;
  while (text < end) {
    n += g_utf8_get_char(text) > 0xffff ? 2u : 1u;
    text = g_utf8_next_char(text);
  }
  return n;
}

/* Byte offset of the u-th UTF-16 code unit within text, or SIZE_MAX if u
 * lands past the end or inside a surrogate pair. */
static size_t offset(const char* text, size_t bytes, size_t u) {
  size_t at = 0, used = 0;
  while (at < bytes && used < u) {
    used += g_utf8_get_char(text + at) > 0xffff ? 2u : 1u;
    at = (size_t)(g_utf8_next_char(text + at) - text);
  }
  return used == u ? at : SIZE_MAX;
}

/* CoreText places trailing whitespace before the origin of an RTL line.
 * Pango includes it at the visual left; keep geometry and drawing in sync. */
static double rtl_trailing_width(PangoLayoutLine* line) {
  if (pango_layout_line_get_resolved_direction(line) != PANGO_DIRECTION_RTL)
    return 0;
  const char* text = pango_layout_get_text(line->layout);
  const char *end = text + line->start_index + line->length, *tail = end;
  while (tail > text + line->start_index) {
    const char* previous = g_utf8_prev_char(tail);
    if (!g_unichar_isspace(g_utf8_get_char(previous))) break;
    tail = previous;
  }

  if (tail == end) return 0;

  int x;
  pango_layout_line_index_to_x(line, (int)(tail - text), FALSE, &x);
  return (double)x / PANGO_SCALE;
}

/* True if the line's final logical character resolves to RTL direction. */
static bool rtl_logical_end(PangoLayoutLine* line) {
  if (!line->length) return false;
  const char* text = pango_layout_get_text(line->layout);
  const char* last = g_utf8_prev_char(text + line->start_index + line->length);
  return pango_layout_get_direction(line->layout, (int)(last - text)) ==
         PANGO_DIRECTION_RTL;
}

/* X position in Pango units of index within line; uses the trailing edge of
 * the last character when index falls at an RTL line's logical end. */
static int line_index_x(PangoLayoutLine* line, size_t index) {
  int x = 0;
  const char* text = pango_layout_get_text(line->layout);
  if (index >= (size_t)(line->start_index + line->length) &&
      rtl_logical_end(line)) {
    const char* last =
        g_utf8_prev_char(text + line->start_index + line->length);
    pango_layout_line_index_to_x(line, (int)(last - text), TRUE, &x);
  } else
    pango_layout_line_index_to_x(line, (int)index, FALSE, &x);
  return x;
}

/* A leading LTR segment in an RTL line shares the explicit gap with the
 * adjacent RTL segment. CoreText includes that half-gap in its start caret. */
static double start_adjustment(PangoLayoutLine* line) {
  if (pango_layout_line_get_resolved_direction(line) != PANGO_DIRECTION_RTL)
    return 0;

  const char* text = pango_layout_get_text(line->layout);
  if (!line->length) return 0;

  if (pango_layout_get_direction(line->layout, 0) == PANGO_DIRECTION_RTL) {
    /* A retained pair adjustment at a soft-wrap boundary affects the
     * first logical glyph's advance, but not CoreText's leading caret. */
    for (GSList* it = line->runs; it; it = it->next) {
      PangoGlyphItem* run = it->data;
      if (run->item->offset != 0) continue;
      PangoGlyphItemIter cluster;
      if (pango_glyph_item_iter_init_start(&cluster, run, text))
        return 2 * (rmr_pango_cluster_adjustment(run, cluster.start_index) -
                    rmr_pango_cluster_spacing(line->layout, 0,
                                              (size_t)cluster.end_index));
    }
    return 0;
  }

  for (const char* c = text; *c; c = g_utf8_next_char(c)) {
    size_t at = (size_t)(c - text);
    if (pango_layout_get_direction(line->layout, (int)at) ==
        PANGO_DIRECTION_RTL)
      return rmr_pango_cluster_spacing(line->layout, at,
                                       (size_t)(g_utf8_next_char(c) - text));
  }
  return 0;
}

/* Computes the caret x position for an interior index that may fall at a
 * bidi run boundary or inside a kerned glyph cluster, writing the base
 * position to *x and returning any extra pair-adjustment width to add. */
static double interior_adjustment(PangoLayoutLine* line, size_t index, int* x) {
  const char* text = pango_layout_get_text(line->layout);
  size_t previous = (size_t)(g_utf8_prev_char(text + index) - text);
  PangoGlyphItem *before = nullptr, *after = nullptr;
  for (GSList* it = line->runs; it; it = it->next) {
    PangoGlyphItem* run = it->data;
    size_t start = (size_t)run->item->offset,
           end = start + (size_t)run->item->length;
    if (previous >= start && previous < end) before = run;
    if (index >= start && index < end) after = run;
  }
  if (!before || !after) return 0;

  bool direction_change =
      before->item->analysis.level != after->item->analysis.level;
  int attr_count;
  const PangoLogAttr* attrs =
      pango_layout_get_log_attrs_readonly(line->layout, &attr_count);
  glong character = g_utf8_pointer_to_offset(text, text + index);
  bool cursor_stop = attrs && character >= 0 && character < attr_count &&
                     attrs[character].is_cursor_position;

  /* Preserve composed-character stops; at a bidi boundary CoreText's
   * primary offset follows the preceding edge in LTR and the left edge in RTL.
   */
  if (direction_change || !cursor_stop)
    pango_layout_line_index_to_x(line, (int)previous, TRUE, x);
  if (direction_change) {
    int next;
    pango_layout_line_index_to_x(line, (int)index, FALSE, &next);
    if (pango_layout_line_get_resolved_direction(line) == PANGO_DIRECTION_RTL &&
        next < *x)
      *x = next;
    return 0;
  }

  bool rtl = (before->item->analysis.level & 1) != 0;
  PangoGlyphItem* selected = rtl ? after : before;
  size_t at = rtl ? index : previous;
  PangoGlyphItemIter cluster;
  for (gboolean valid =
           pango_glyph_item_iter_init_start(&cluster, selected, text);
       valid; valid = pango_glyph_item_iter_next_cluster(&cluster))
    if (at >= (size_t)cluster.start_index && at < (size_t)cluster.end_index) {
      /* RTL marks may occupy a separate glyph cluster immediately after
       * their base. Their attached pair adjustment is not split. */
      bool marks = false;
      for (const char* c = text + cluster.start_index;
           c <= text + cluster.end_index && *c; c = g_utf8_next_char(c)) {
        GUnicodeType type = g_unichar_type(g_utf8_get_char(c));
        marks |= type == G_UNICODE_NON_SPACING_MARK ||
                 type == G_UNICODE_SPACING_MARK ||
                 type == G_UNICODE_ENCLOSING_MARK;
      }
      if (rtl && marks)
        return rmr_pango_cluster_spacing(line->layout,
                                         (size_t)cluster.start_index,
                                         (size_t)cluster.end_index);
      return rmr_pango_cluster_adjustment(
          selected, cluster.start_index - selected->item->offset);
    }
  return 0;
}

/* Paragraph shaping retains pair spacing across a soft wrap. CoreText's
 * terminal caret excludes that pair adjustment, while glyphs keep it. */
static double end_pair_adjustment(PangoLayoutLine* line) {
  if (!line->length || rtl_logical_end(line) ||
      pango_layout_line_get_resolved_direction(line) == PANGO_DIRECTION_RTL)
    return 0;

  const char* text = pango_layout_get_text(line->layout);
  size_t previous = (size_t)(g_utf8_prev_char(text + line->length) - text);
  for (GSList* it = line->runs; it; it = it->next) {
    PangoGlyphItem* run = it->data;
    if (previous < (size_t)run->item->offset ||
        previous >= (size_t)(run->item->offset + run->item->length))
      continue;
    PangoGlyphItemIter cluster;
    for (gboolean valid = pango_glyph_item_iter_init_start(&cluster, run, text);
         valid; valid = pango_glyph_item_iter_next_cluster(&cluster))
      if (previous >= (size_t)cluster.start_index &&
          previous < (size_t)cluster.end_index)
        return 2 * (rmr_pango_cluster_adjustment(
                        run, cluster.start_index - run->item->offset) -
                    rmr_pango_cluster_spacing(line->layout,
                                              (size_t)cluster.start_index,
                                              (size_t)cluster.end_index));
  }

  return 0;
}

/* Caret-provider callback: computes the primary and line-start x offsets
 * for a character index within paragraph pi's line li, after validating
 * that pi, li, and index are all in range. */
static rmr_status caret(void* ctx, size_t pi, size_t li, size_t index,
                        rmr_caret_offsets* out) {
  rmr_linux_layout* l = ctx;
  if (!l || !out || pi >= l->anchors.count ||
      li >= l->paragraphs[pi].line_count)
    return RMR_ERR_INVALID_ARGUMENT;

  const rmr_anchor_paragraph* p = &l->paragraphs[pi];
  size_t at = offset(p->text.bytes, p->text.length, index);
  if (at == SIZE_MAX || at > INT_MAX) return RMR_ERR_INVALID_ARGUMENT;
  PangoLayoutLine* line =
      pango_layout_get_line_readonly(l->native[pi].lines[li], 0);
  size_t first = offset(p->text.bytes, p->text.length, p->lines[li].location);
  if (at < first || index > p->lines[li].location + p->lines[li].length)
    return RMR_ERR_INVALID_ARGUMENT;
  size_t local = at - first;
  int x = line_index_x(line, local),
      start = line_index_x(line, (size_t)line->start_index);
  double trailing = rtl_trailing_width(line);

  if (local == (size_t)line->length && local &&
      pango_layout_line_get_resolved_direction(line) == PANGO_DIRECTION_RTL &&
      !rtl_logical_end(line)) {
    const char* text = pango_layout_get_text(line->layout);
    size_t previous = (size_t)(g_utf8_prev_char(text + local) - text);
    x = line_index_x(line, previous);
  }

  bool rtl_end = rtl_logical_end(line) && local >= (size_t)line->length;
  double adjustment =
      local > 0 && local < (size_t)line->length
          ? interior_adjustment(line, local, &x)
      : rtl_end || pango_layout_line_get_resolved_direction(line) ==
                       PANGO_DIRECTION_RTL
          ? 0
          : rmr_pango_caret_adjustment(l->native[pi].lines[li], local);
  if (local == (size_t)line->length) adjustment += end_pair_adjustment(line);
  if (local == (size_t)line->length && at < p->text.length &&
      (p->text.bytes[at] == '\n' || p->text.bytes[at] == '\r'))
    adjustment = 0;

  double leading = start_adjustment(line);
  *out = (rmr_caret_offsets){(double)x / PANGO_SCALE - trailing + adjustment +
                                 (local == 0 ? leading : 0),
                             (double)start / PANGO_SCALE - trailing + leading};
  return RMR_OK;
}

/* Frees a layout together with its native Pango objects, retained paragraph
 * text, and font reference. A null layout is a no-op. */
void rmr_linux_layout_free(rmr_linux_layout* l) {
  if (!l) return;

  for (size_t i = 0; i < l->anchors.count; i++) {
    if (l->native[i].lines) {
      for (size_t j = 0; j < l->paragraphs[i].line_count; j++)
        if (l->native[i].lines[j]) g_object_unref(l->native[i].lines[j]);
      free(l->native[i].lines);
    }
    if (l->native[i].layout) g_object_unref(l->native[i].layout);
    if (l->native[i].marker) g_object_unref(l->native[i].marker);
    free((void*)(uintptr_t)l->paragraphs[i].text.bytes);
    free((void*)(uintptr_t)l->paragraphs[i].lines);
  }

  free(l->paragraphs);
  free(l->native);
  rmr_linux_fonts_free(l->fonts);
  free(l);
}

/* Returns the layout's accounted byte size, or zero for a null layout. */
size_t rmr_linux_layout_bytes(const rmr_linux_layout* l) {
  return l ? l->bytes : 0;
}

/* Returns the layout's anchor/paragraph table, or null for a null layout. */
const rmr_anchor_layout* rmr_linux_layout_anchors(const rmr_linux_layout* l) {
  return l ? &l->anchors : nullptr;
}

/* Writes the layout's bottom y-coordinate to *out; returns false if the
 * layout is null or has no paragraphs. */
bool rmr_linux_layout_bottom(const rmr_linux_layout* l, double* out) {
  if (!l || !l->anchors.count || !out) return false;
  *out = l->bottom;
  return true;
}

/* Sets a's byte range to [start, end) and merges it into list, which takes
 * ownership of a. */
static void attr(PangoAttrList* list, PangoAttribute* a, size_t start,
                 size_t end) {
  a->start_index = (guint)start;
  a->end_index = (guint)end;
  pango_attr_list_change(list, a);
}

/* Builds and lays out the marker glyph for a list item: a bullet character,
 * a lowercase letter, a lowercase roman numeral, or a decimal number,
 * chosen by family and nesting level. */
static PangoLayout* marker(const rmr_linux_fonts* fonts, rmr_type_metrics m,
                           rm_list_family family, size_t ordinal, int level) {
  char label[128];
  if (family == RM_LIST_BULLET)
    snprintf(label, sizeof label, "%s",
             level == 1   ? "•"
             : level == 2 ? "◦"
                          : "–");
  else if (level == 2) {
    char reversed[32];
    size_t n = 0;
    do {
      ordinal--;
      reversed[n++] = (char)('a' + ordinal % 26);
      ordinal /= 26;
    } while (ordinal && n < sizeof reversed);
    for (size_t i = 0; i < n; i++) label[i] = reversed[n - i - 1];
    label[n] = '.';
    label[n + 1] = 0;
  } else if (level >= 3 && ordinal < 4000) {
    static const size_t values[] = {1000, 900, 500, 400, 100, 90, 50,
                                    40,   10,  9,   5,   4,   1};
    static const char* const letters[] = {
        "m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
    size_t at = 0;
    for (size_t i = 0; i < 13; i++)
      while (ordinal >= values[i]) {
        size_t n = strlen(letters[i]);
        memcpy(label + at, letters[i], n);
        at += n;
        ordinal -= values[i];
      }
    label[at++] = '.';
    label[at] = 0;
  } else
    snprintf(label, sizeof label, "%zu.", ordinal);

  return rmr_pango_layout(fonts, (rm_str){label, strlen(label)}, m.size, m.face,
                          false, false);
}

/* Pango clusters are monotone within a directional glyph run. Binary searches
 * avoid rescanning an entire long paragraph for each narrow output line. */
static int glyph_boundary(const PangoGlyphItem* run, size_t byte) {
  int lo = 0, hi = run->glyphs->num_glyphs;
  bool rtl = (run->item->analysis.level & 1) != 0;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    size_t at =
        (size_t)run->item->offset + (size_t)run->glyphs->log_clusters[mid];
    if (rtl ? at >= byte : at < byte)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

/* Preserve paragraph shaping across soft wraps. Re-shaping an Arabic slice
 * changes joining forms and pair spacing at its edges. Only reuse compatible
 * font/bidi runs and complete clusters selected by line_breaks. */
static rmr_status paragraph_glyphs(PangoLayout* paragraph_layout,
                                   PangoLayout* line_layout, size_t start,
                                   const rmr_limits* limits) {
  PangoLayoutLine* line = pango_layout_get_line_readonly(line_layout, 0);
  for (GSList* ri = line->runs; ri; ri = ri->next) {
    PangoGlyphItem* run = ri->data;
    size_t from = start + (size_t)run->item->offset,
           to = from + (size_t)run->item->length;
    PangoGlyphItem* source = nullptr;
    for (GSList* li = pango_layout_get_lines_readonly(paragraph_layout);
         li && !source; li = li->next)
      for (GSList* si = ((PangoLayoutLine*)li->data)->runs; si; si = si->next) {
        PangoGlyphItem* candidate = si->data;
        if ((size_t)candidate->item->offset <= from &&
            to <= (size_t)candidate->item->offset +
                      (size_t)candidate->item->length &&
            candidate->item->analysis.font == run->item->analysis.font &&
            candidate->item->analysis.level == run->item->analysis.level) {
          source = candidate;
          break;
        }
      }
    if (!source) continue;

    bool rtl = (source->item->analysis.level & 1) != 0;
    int first = glyph_boundary(source, rtl ? to : from);
    int end = glyph_boundary(source, rtl ? from : to);
    int count = end - first;
    if (count <= 0) continue;
    size_t cluster_from =
        (size_t)source->item->offset +
        (size_t)source->glyphs->log_clusters[rtl ? end - 1 : first];
    int after = rtl ? first - 1 : end;
    size_t cluster_to =
        after < 0 || after == source->glyphs->num_glyphs
            ? (size_t)source->item->offset + (size_t)source->item->length
            : (size_t)source->item->offset +
                  (size_t)source->glyphs->log_clusters[after];
    if (cluster_from != from || cluster_to != to) continue;

    if ((size_t)count >
        limits->maximum_plan_bytes / (sizeof(PangoGlyphInfo) + sizeof(int)))
      return RMR_ERR_LIMIT;
    pango_glyph_string_set_size(run->glyphs, count);
    for (int i = 0; i < count; i++) {
      run->glyphs->glyphs[i] = source->glyphs->glyphs[first + i];
      run->glyphs->log_clusters[i] = source->glyphs->log_clusters[first + i] +
                                     source->item->offset - (int)from;
    }
  }

  return RMR_OK;
}

/* Choose breaks using the same calibrated advances later drawn and measured. */
static rmr_status line_breaks(PangoLayout* layout, double column,
                              const rmr_limits* limits, size_t** ends_out,
                              size_t* count_out) {
  pango_layout_set_width(layout, -1);
  PangoDirection direction = pango_layout_line_get_resolved_direction(
      pango_layout_get_line_readonly(layout, 0));
  /* A wrapped slice must inherit the paragraph's base direction, including
   * slices containing only the opposite script or neutral characters. */
  pango_context_set_base_dir(pango_layout_get_context(layout), direction);
  pango_layout_set_auto_dir(layout, FALSE);

  rmr_status status = rmr_pango_calibrate(layout, limits);
  if (status != RMR_OK) return status;

  const char* text = pango_layout_get_text(layout);
  size_t count = (size_t)g_utf8_strlen(text, -1);
  if (count >= limits->maximum_plan_bytes /
                   (2 * sizeof(size_t) + sizeof(int) + sizeof(bool)) ||
      count >= limits->maximum_elements)
    return RMR_ERR_LIMIT;

  size_t *offsets = calloc(count + 1, sizeof *offsets),
         *ends = calloc(count + 1, sizeof *ends);
  int* widths = calloc(count + 1, sizeof *widths);
  bool* cluster_breaks = calloc(count + 1, sizeof *cluster_breaks);
  if (!offsets || !ends || !widths || !cluster_breaks) {
    free(offsets);
    free(ends);
    free(widths);
    free(cluster_breaks);
    return RMR_ERR_OUT_OF_MEMORY;
  }

  const char* cursor = text;
  for (size_t i = 0; i < count; i++) {
    offsets[i] = (size_t)(cursor - text);
    cursor = g_utf8_next_char(cursor);
  }
  offsets[count] = (size_t)(cursor - text);
  cluster_breaks[0] = cluster_breaks[count] = true;

  for (GSList* li = pango_layout_get_lines(layout); li; li = li->next) {
    PangoLayoutLine* line = li->data;
    for (GSList* ri = line->runs; ri; ri = ri->next) {
      PangoGlyphItem* run = ri->data;
      size_t lo = 0, hi = count;
      while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (offsets[mid] < (size_t)run->item->offset)
          lo = mid + 1;
        else
          hi = mid;
      }
      if ((size_t)run->item->num_chars > count - lo) {
        status = RMR_ERR_BACKEND;
        goto done;
      }
      pango_glyph_item_get_logical_widths(run, text, widths + lo);
      cluster_breaks[lo + (size_t)run->item->num_chars] = true;
      /* Grapheme boundaries alone permit splitting ff/ffi ligatures.
       * A wrap must also respect the shaped glyph-cluster boundary. */
      for (int glyph = 0; glyph < run->glyphs->num_glyphs; glyph++) {
        size_t byte = (size_t)run->item->offset +
                      (size_t)run->glyphs->log_clusters[glyph];
        size_t left = lo, right = lo + (size_t)run->item->num_chars;
        while (left < right) {
          size_t mid = left + (right - left) / 2;
          if (offsets[mid] < byte)
            left = mid + 1;
          else
            right = mid;
        }
        if (left <= count && offsets[left] == byte) cluster_breaks[left] = true;
      }
    }
  }

  int attr_count = 0;
  const PangoLogAttr* attrs =
      pango_layout_get_log_attrs_readonly(layout, &attr_count);
  if (!attrs || attr_count < 0 || (size_t)attr_count != count + 1) {
    status = RMR_ERR_BACKEND;
    goto done;
  }

  size_t lines = 0, start = 0;
  double maximum = column * PANGO_SCALE;
  while (start < count) {
    size_t word = start, character = start, end = count;
    int64_t width = 0;
    for (size_t i = start; i < count; i++) {
      gunichar c = g_utf8_get_char(text + offsets[i]);
      if (c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029) {
        end = i + 1;
        if (c == '\r' && end < count && text[offsets[end]] == '\n') end++;
        break;
      }
      width += widths[i];
      if ((double)width > maximum && !g_unichar_isspace(c)) {
        end = word > start ? word : character > start ? character : i + 1;
        /* Never split a combining/surrogate/grapheme cluster. */
        while (end < count &&
               (!attrs[end].is_char_break || !cluster_breaks[end]))
          end++;
        break;
      }
      if (attrs[i + 1].is_line_break && cluster_breaks[i + 1]) word = i + 1;
      if (attrs[i + 1].is_char_break && cluster_breaks[i + 1])
        character = i + 1;
    }

    ends[lines++] = offsets[end];
    start = end;
  }

  size_t* compact = realloc(ends, lines * sizeof *ends);
  if (!compact) {
    status = RMR_ERR_OUT_OF_MEMORY;
    goto done;
  }
  ends = compact;
  *ends_out = ends;
  *count_out = lines;
  ends = nullptr;

done:
  free(cluster_breaks);
  free(offsets);
  free(widths);
  free(ends);
  return status;
}

rmr_status rmr_linux_layout_create(const rm_text_block* text, double page_width,
                                   const rmr_linux_fonts* fonts,
                                   const rmr_limits* given,
                                   rmr_linux_layout** out, rmr_error* e) {
  rmr_limits limits = given ? *given : rmr_limits_default();
  if (!text || !fonts || !out || (!text->paragraphs && text->paragraph_count) ||
      !isfinite(page_width) || page_width <= 0 || !isfinite(text->origin_x) ||
      !isfinite(text->origin_y) ||
      fabs(text->origin_x) > limits.maximum_coordinate ||
      fabs(text->origin_y) > limits.maximum_coordinate)
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT, "invalid text layout");

  double column = text->has_column_width ? text->column_width
                  : text->origin_x < 0   ? -2 * text->origin_x
                                         : page_width;
  if (!isfinite(column) || column <= 0 || column > 1000000)
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT, "invalid text column");

  size_t count = text->paragraph_count, base = sizeof(rmr_linux_layout);
  if (base > limits.maximum_plan_bytes || count > limits.maximum_elements ||
      count > (limits.maximum_plan_bytes - base) /
                  (sizeof(paragraph) + sizeof(rmr_anchor_paragraph)))
    return rmr_linux_error(e, RMR_ERR_LIMIT, "paragraph limit");

  rmr_linux_layout* l = calloc(1, sizeof *l);
  if (!l) return rmr_linux_error(e, RMR_ERR_OUT_OF_MEMORY, "allocation failed");
  l->paragraphs = calloc(count ? count : 1, sizeof *l->paragraphs);
  l->native = calloc(count ? count : 1, sizeof *l->native);
  if (!l->paragraphs || !l->native) {
    free(l->paragraphs);
    free(l->native);
    free(l);
    return rmr_linux_error(e, RMR_ERR_OUT_OF_MEMORY, "allocation failed");
  }

  l->fonts = (rmr_linux_fonts*)(uintptr_t)fonts;
  atomic_fetch_add(&l->fonts->references, 1);
  l->bytes = base + count * (sizeof(paragraph) + sizeof(rmr_anchor_paragraph));
  l->caret = (rmr_caret_provider){RMR_CARET_PROVIDER_ABI,
                                  sizeof(rmr_caret_provider), l, caret};
  l->anchors = (rmr_anchor_layout){text->origin_x + 4, fmax(column - 8, 32),
                                   l->paragraphs, count, &l->caret};

  size_t ordinals[17] = {0}, work = count;
  rm_list_family families[17] = {0};
  rmr_type_metrics previous = {0};
  double baseline = text->origin_y;
  rmr_status s = RMR_OK;

  for (size_t i = 0; i < count && s == RMR_OK; i++) {
    const rm_paragraph* in = &text->paragraphs[i];
    rmr_anchor_paragraph* p = &l->paragraphs[i];
    paragraph* n = &l->native[i];
    const rm_paragraph_format* fmt = in->has_format ? &in->format : nullptr;

    s = rmr_lines_type_metrics(fmt, in->text, &limits, &n->metrics, e);
    if (s != RMR_OK) break;

    size_t length = in->text.length;
    if (length > INT_MAX || length >= limits.maximum_plan_bytes - l->bytes ||
        length > limits.maximum_elements - work) {
      s = RMR_ERR_LIMIT;
      break;
    }
    work += length;
    if (in->face_count > limits.maximum_elements - work ||
        (length &&
         in->face_count > (limits.maximum_elements - work) / length)) {
      s = RMR_ERR_LIMIT;
      break;
    }
    work += in->face_count * length;
    if ((!in->faces && in->face_count) || (!in->text.bytes && length) ||
        !g_utf8_validate(in->text.bytes ? in->text.bytes : "", (gssize)length,
                         nullptr)) {
      s = RMR_ERR_INVALID_ARGUMENT;
      break;
    }

    char* copy = malloc(length + 1);
    if (!copy) {
      s = RMR_ERR_OUT_OF_MEMORY;
      break;
    }
    if (length) memcpy(copy, in->text.bytes, length);
    copy[length] = 0;
    p->text = (rm_str){copy, length};
    l->bytes += length + 1;

    int level = fmt ? rm_paragraph_format_level(fmt) : 1;
    if (level < 1 || level > 16) {
      s = RMR_ERR_LIMIT;
      break;
    }
    n->family =
        fmt ? rm_paragraph_style_list_family(rm_paragraph_format_style(fmt))
            : RM_LIST_NONE;
    p->indent = n->family == RM_LIST_NONE ? 0 : 48 * (double)level;

    size_t ordinal = 0;
    if (n->family != RM_LIST_NONE) {
      for (int k = level + 1; k < 17; k++) {
        ordinals[k] = 0;
        families[k] = RM_LIST_NONE;
      }
      if (families[level] != n->family) {
        families[level] = n->family;
        ordinals[level] = 0;
      }
      ordinal = ++ordinals[level];
    } else {
      memset(ordinals, 0, sizeof ordinals);
      memset(families, 0, sizeof families);
    }

    baseline += i ? previous.descent + previous.after + n->metrics.before +
                        n->metrics.ascent
                  : n->metrics.first_baseline_drop;
    p->baseline = baseline;
    previous = n->metrics;

    n->checkbox = rm_text_block_checkbox_state(text, i);
    if (n->family == RM_LIST_BULLET || n->family == RM_LIST_NUMBERED)
      n->marker = marker(fonts, n->metrics, n->family, ordinal, level);
    if (n->marker) {
      s = rmr_pango_calibrate(n->marker, &limits);
      if (s != RMR_OK) break;
    }

    if (!length) {
      l->bottom = baseline + n->metrics.descent + 24;
      continue;
    }

    n->layout = rmr_pango_layout(fonts, p->text, n->metrics.size,
                                 n->metrics.face, false, false);
    pango_layout_set_width(
        n->layout, (int)(fmax(1, l->anchors.column - p->indent) * PANGO_SCALE));
    pango_layout_set_wrap(n->layout, PANGO_WRAP_WORD_CHAR);
    PangoAttrList* attrs =
        pango_attr_list_copy(pango_layout_get_attributes(n->layout));
    /* Explicit base attributes let Pango merge redundant face overlays
     * instead of breaking Arabic shaping at an unchanged style boundary. */
    PangoWeight base_weight = n->metrics.face == RMR_FACE_MEDIUM
                                  ? PANGO_WEIGHT_MEDIUM
                                  : PANGO_WEIGHT_NORMAL;
    attr(attrs, pango_attr_weight_new(base_weight), 0, length);
    attr(attrs, pango_attr_style_new(PANGO_STYLE_NORMAL), 0, length);
    for (size_t j = 0; j < in->face_count; j++) {
      const rm_face_run* f = &in->faces[j];
      if (f->start < 0 || f->end < f->start) continue;
      glong scalars = g_utf8_strlen(copy, (gssize)length);
      if (f->end > scalars) continue;
      size_t a = (size_t)(g_utf8_offset_to_pointer(copy, f->start) - copy),
             b = (size_t)(g_utf8_offset_to_pointer(copy, f->end) - copy);
      rmr_pango_attributes(fonts, attrs, p->text, a, b, n->metrics.size,
                           n->metrics.face, f->bold, f->italic);
      attr(attrs,
           pango_attr_weight_new(f->bold ? PANGO_WEIGHT_BOLD : base_weight), a,
           b);
      attr(attrs,
           pango_attr_style_new(f->italic ? PANGO_STYLE_ITALIC
                                          : PANGO_STYLE_NORMAL),
           a, b);
    }
    pango_layout_set_attributes(n->layout, attrs);
    pango_attr_list_unref(attrs);

    size_t lines = 0, *line_ends = nullptr;
    rmr_limits remaining = limits;
    remaining.maximum_plan_bytes -= l->bytes;
    s = line_breaks(n->layout, fmax(1, l->anchors.column - p->indent),
                    &remaining, &line_ends, &lines);
    if (s != RMR_OK) break;
    if (lines > limits.maximum_elements - work ||
        lines > (limits.maximum_plan_bytes - l->bytes) /
                    (sizeof(rmr_anchor_line) + sizeof(PangoLayout*) +
                     sizeof(size_t))) {
      free(line_ends);
      s = RMR_ERR_LIMIT;
      break;
    }
    work += lines;

    rmr_anchor_line* visual = calloc(lines, sizeof *visual);
    if (!visual) {
      free(line_ends);
      s = RMR_ERR_OUT_OF_MEMORY;
      break;
    }
    n->lines = calloc(lines, sizeof *n->lines);
    if (!n->lines) {
      free(line_ends);
      free(visual);
      s = RMR_ERR_OUT_OF_MEMORY;
      break;
    }
    p->lines = visual;
    p->line_count = lines;
    p->has_typesetter = true;
    l->bytes += lines * (sizeof *visual + sizeof *n->lines);

    remaining.maximum_plan_bytes =
        limits.maximum_plan_bytes - l->bytes - lines * sizeof *line_ends;
    rmr_type_metrics last = n->metrics;

    for (size_t j = 0; j < lines; j++) {
      size_t start = j ? line_ends[j - 1] : 0, end = line_ends[j];

      /* Shape each chosen line independently, retaining trailing spaces in
       * caret geometry (Pango's wrapping layout collapses them at soft breaks).
       */
      size_t visible_end = end;
      while (visible_end > start &&
             (copy[visible_end - 1] == '\n' || copy[visible_end - 1] == '\r'))
        visible_end--;

      n->lines[j] = pango_layout_copy(n->layout);
      pango_layout_set_width(n->lines[j], -1);
      pango_layout_set_single_paragraph_mode(n->lines[j], TRUE);
      pango_layout_set_text(n->lines[j], copy + start,
                            (int)(visible_end - start));

      PangoAttrList* line_attrs = pango_attr_list_new();
      GSList* all_attrs = pango_attr_list_get_attributes(
          rmr_pango_source_attributes(n->layout));
      for (GSList* it = all_attrs; it; it = it->next) {
        PangoAttribute* a = it->data;
        if (a->start_index < visible_end && a->end_index > start) {
          a->start_index =
              (guint)(a->start_index > start ? a->start_index - start : 0);
          a->end_index =
              (guint)(fmin(a->end_index, (double)visible_end) - (double)start);
          pango_attr_list_insert(line_attrs, a);
        } else
          pango_attribute_destroy(a);
      }
      g_slist_free(all_attrs);
      pango_layout_set_attributes(n->lines[j], line_attrs);
      pango_attr_list_unref(line_attrs);

      s = rmr_pango_calibrate(n->lines[j], &remaining);
      if (s == RMR_OK)
        s = paragraph_glyphs(n->layout, n->lines[j], start, &remaining);
      if (s != RMR_OK) break;
      s = rmr_lines_type_metrics(fmt, (rm_str){copy + start, end - start},
                                 &limits, &last, e);
      if (s != RMR_OK) break;

      if (j) baseline += last.height;
      visual[j] = (rmr_anchor_line){units(copy, start),
                                    units(copy + start, end - start), baseline};
      if (!j)
        p->right_to_left = pango_layout_line_get_resolved_direction(
                               pango_layout_get_line_readonly(
                                   n->lines[j], 0)) == PANGO_DIRECTION_RTL;
    }

    free(line_ends);
    l->bottom = baseline + last.descent + 24;
    if (!isfinite(l->bottom) || fabs(l->bottom) > limits.maximum_coordinate)
      s = RMR_ERR_LIMIT;
  }

  if (s != RMR_OK) {
    rmr_linux_layout_free(l);
    return rmr_linux_error(e, s, "paragraph layout failed");
  }

  *out = l;
  return RMR_OK;
}

/* Computes the visual start/end x and baseline of paragraph pi's line li,
 * accounting for RTL alignment within the column and trailing whitespace. */
rmr_status rmr_linux_layout_line(const rmr_linux_layout* l, size_t pi,
                                 size_t li, rmr_text_line* out, rmr_error* e) {
  if (!l || !out || pi >= l->anchors.count ||
      li >= l->paragraphs[pi].line_count)
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT,
                           "invalid text line index");

  const rmr_anchor_paragraph* p = &l->paragraphs[pi];
  rmr_anchor_line v = p->lines[li];
  PangoLayoutLine* line =
      pango_layout_get_line_readonly(l->native[pi].lines[li], 0);
  PangoRectangle logical;
  pango_layout_line_get_extents(line, nullptr, &logical);

  double width = (double)logical.width / PANGO_SCALE,
         start = (double)line_index_x(line, 0) / PANGO_SCALE -
                 rtl_trailing_width(line) + start_adjustment(line),
         x = p->right_to_left
                 ? l->anchors.left + l->anchors.column - p->indent - start
                 : l->anchors.left + p->indent;
  *out = (rmr_text_line){pi,         v.location, v.location + v.length,
                         v.baseline, x,          x + width};
  return RMR_OK;
}

typedef struct {
  rmr_sink sink;
  bool discard;
} layout_sink;

/* Cairo PDF write callback that forwards bytes to the sink unless the
 * layout output is being discarded. */
static cairo_status_t layout_write(void* closure, const unsigned char* data,
                                   unsigned int length) {
  layout_sink* output = closure;
  if (!output->discard) (void)rmr_linux_write(&output->sink, data, length);
  /* Preserve the sink error, but let Cairo finish releasing PDF resources.
   * Cairo's PDF error path can leak when a stream rejects finalization. */
  return CAIRO_STATUS_SUCCESS;
}

typedef struct {
  rmr_draw* draws;
  rmr_path* paths;
  size_t count, capacity, elements;
  rmr_limits limits;
} outline_recorder;

/* Paints cr's current path with its source color: filled or stroked onto
 * the text PDF, or, when o is given, recorded as an outline command. */
static rmr_status paint(cairo_t* cr, outline_recorder* o, rmr_draw_mode mode) {
  if (!o) {
    if (mode == RMR_FILL)
      cairo_fill(cr);
    else
      cairo_stroke(cr);
    return RMR_OK;
  }

  if (o->count == o->limits.maximum_draws) return RMR_ERR_LIMIT;
  if (o->count == o->capacity) {
    size_t capacity = o->capacity ? o->capacity * 2 : 16;
    rmr_draw* draws = realloc(o->draws, capacity * sizeof *draws);
    if (draws) o->draws = draws;
    rmr_path* paths =
        draws ? realloc(o->paths, capacity * sizeof *paths) : nullptr;
    if (!paths) return RMR_ERR_OUT_OF_MEMORY;
    o->paths = paths;
    o->capacity = capacity;
  }

  double red = 0, green = 0, blue = 0, alpha = 1;
  cairo_pattern_get_rgba(cairo_get_source(cr), &red, &green, &blue, &alpha);
  cairo_line_cap_t cap = cairo_get_line_cap(cr);
  cairo_line_join_t join = cairo_get_line_join(cr);
  o->draws[o->count] = (rmr_draw){
      .mode = mode,
      .color = {red, green, blue, alpha},
      .line_width = mode == RMR_STROKE ? cairo_get_line_width(cr) : 0,
      .cap = cap == CAIRO_LINE_CAP_ROUND    ? RMR_CAP_ROUND
             : cap == CAIRO_LINE_CAP_SQUARE ? RMR_CAP_SQUARE
                                            : RMR_CAP_BUTT,
      .join = join == CAIRO_LINE_JOIN_ROUND   ? RMR_JOIN_ROUND
              : join == CAIRO_LINE_JOIN_BEVEL ? RMR_JOIN_BEVEL
                                              : RMR_JOIN_MITER,
      .miter_limit = cairo_get_miter_limit(cr)};

  rmr_path* path = &o->paths[o->count++];
  *path = (rmr_path){0};
  rmr_limits limits = o->limits;
  limits.maximum_elements -= o->elements;
  rmr_status s = rmr_linux_path_capture(cr, &limits, path);
  o->elements += path->count;
  return s;
}

/* Draws every paragraph marker, checkbox, line and strike rule in page
 * coordinates on cr, or, when o is given, records the same geometry as
 * outline commands; retained tracks emoji bytes kept alive for the PDF. */
static rmr_status text_layer(const rmr_linux_layout* l, cairo_t* cr,
                             const rmr_limits* limits, size_t* retained,
                             outline_recorder* o, rmr_error* e) {
  rmr_status s = RMR_OK;
  cairo_set_source_rgb(cr, 0, 0, 0);
  for (size_t i = 0; s == RMR_OK && i < l->anchors.count; i++) {
    const rmr_anchor_paragraph* p = &l->paragraphs[i];
    const paragraph* n = &l->native[i];
    double x = l->anchors.left + p->indent, y = p->baseline;

    if (n->marker) {
      PangoRectangle logical;
      pango_layout_get_extents(n->marker, nullptr, &logical);
      cairo_move_to(
          cr,
          n->family == RM_LIST_BULLET
              ? x - 28
              : x - 21.5 - (double)logical.width / PANGO_SCALE,
          y - (double)pango_layout_get_baseline(n->marker) / PANGO_SCALE);
      if (o) {
        pango_cairo_layout_path(cr, n->marker);
        s = paint(cr, o, RMR_FILL);
      } else
        pango_cairo_show_layout(cr, n->marker);
    } else if (n->family == RM_LIST_CHECKBOX) {
      double bx = x - 41.33, by = y - 24.93;
      cairo_set_line_width(cr, 2.5);
      cairo_rectangle(cr, bx, by, 26.67, 26.67);
      if (n->checkbox == RM_CHECKBOX_TICKED) {
        s = paint(cr, o, RMR_FILL);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_move_to(cr, bx + 5.87, by + 13.34);
        cairo_line_to(cr, bx + 12.8, by + 19.74);
        cairo_line_to(cr, bx + 21.87, by + 6.4);
        if (s == RMR_OK) s = paint(cr, o, RMR_STROKE);
        cairo_set_source_rgb(cr, 0, 0, 0);
      } else {
        s = paint(cr, o, RMR_STROKE);
        if (s == RMR_OK && n->checkbox == RM_CHECKBOX_PARTIAL) {
          cairo_move_to(cr, bx + 6.67, by + 13.34);
          cairo_line_to(cr, bx + 20, by + 13.34);
          s = paint(cr, o, RMR_STROKE);
        }
      }
    }

    for (size_t j = 0; s == RMR_OK && j < p->line_count; j++) {
      rmr_text_line v;
      s = rmr_linux_layout_line(l, i, j, &v, e);
      if (s != RMR_OK) break;
      PangoLayoutLine* native_line =
          pango_layout_get_line_readonly(n->lines[j], 0);
      cairo_move_to(cr, v.start - rtl_trailing_width(native_line), v.baseline);
      if (o) {
        pango_cairo_layout_line_path(cr, native_line);
        s = paint(cr, o, RMR_FILL);
      } else
        s = rmr_pango_show_line(cr, native_line, limits, retained);
      if (s != RMR_OK) break;
      if (n->checkbox == RM_CHECKBOX_TICKED) {
        cairo_set_line_width(cr, fmax(n->metrics.size * .065, .6));
        cairo_move_to(cr, v.start, v.baseline - n->metrics.size * .3);
        cairo_line_to(cr, v.end, v.baseline - n->metrics.size * .3);
        s = paint(cr, o, RMR_STROKE);
      }
    }
  }

  return s;
}

rmr_status rmr_linux_layout_outline_plan(const rmr_linux_layout* l,
                                         rmr_rect viewport,
                                         const rmr_limits* given,
                                         rmr_plan** out, rmr_error* e) {
  if (!l || !out)
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT,
                           "missing layout/output");

  outline_recorder o = {.limits = given ? *given : rmr_limits_default()};
  rmr_plan* valid = nullptr;
  rmr_status s = rmr_plan_create(nullptr, 0, viewport, &o.limits, &valid, e);
  if (s != RMR_OK) return s;
  rmr_plan_free(valid);

  /* Recording surfaces, like PDF surfaces, keep glyph outlines unhinted. */
  cairo_surface_t* surface =
      cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, nullptr);
  cairo_t* cr = cairo_create(surface);
  size_t retained = 0;
  s = text_layer(l, cr, &o.limits, &retained, &o, e);
  if (s == RMR_OK && cairo_status(cr) != CAIRO_STATUS_SUCCESS)
    s = RMR_ERR_BACKEND;
  cairo_destroy(cr);
  cairo_surface_destroy(surface);

  /* Whitespace-only lines have no outlines; drop their empty fills. */
  size_t kept = 0;
  for (size_t i = 0; i < o.count; i++) {
    o.draws[i].path = (rmr_path_view){o.paths[i].elements, o.paths[i].count};
    if (o.paths[i].count) o.draws[kept++] = o.draws[i];
  }

  if (s == RMR_OK)
    s = rmr_plan_create(o.draws, kept, viewport, &o.limits, out, e);
  else
    rmr_linux_error(e, s, "text outline failed");

  for (size_t i = 0; i < o.count; i++) rmr_path_free(&o.paths[i]);
  free(o.paths);
  free(o.draws);
  return s;
}

rmr_status rmr_linux_layout_plan(const rmr_linux_layout* l, rmr_rect viewport,
                                 const rmr_limits* given, rmr_plan** out,
                                 rmr_error* e) {
  if (!l || !out)
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT,
                           "missing layout/output");

  rmr_limits limits = given ? *given : rmr_limits_default();
  rmr_plan* valid = nullptr;
  rmr_status s = rmr_plan_create(nullptr, 0, viewport, &limits, &valid, e);
  if (s != RMR_OK) return s;
  rmr_plan_free(valid);

  if (!l->anchors.count)
    return rmr_plan_create(nullptr, 0, viewport, &limits, out, e);

  layout_sink output = {.sink.maximum = limits.maximum_pdf_bytes <
                                                limits.maximum_plan_bytes / 2
                                            ? limits.maximum_pdf_bytes
                                            : limits.maximum_plan_bytes / 2};
  rmr_sink* sink = &output.sink;
  cairo_surface_t* surface = cairo_pdf_surface_create_for_stream(
      layout_write, &output, viewport.width, viewport.height);
  cairo_t* cr = cairo_create(surface);
  cairo_translate(cr, -viewport.x, -viewport.y);
  size_t retained = 0;
  s = text_layer(l, cr, &limits, &retained, nullptr, e);

  output.discard = s != RMR_OK;
  cairo_show_page(cr);
  cairo_status_t cs = cairo_status(cr);
  cairo_destroy(cr);
  cairo_surface_finish(surface);
  if (cs == CAIRO_STATUS_SUCCESS) cs = cairo_surface_status(surface);
  cairo_surface_destroy(surface);
  if (sink->status != RMR_OK)
    s = sink->status;
  else if (cs != CAIRO_STATUS_SUCCESS)
    s = RMR_ERR_BACKEND;

  if (s == RMR_OK) {
    rmr_element clip[] = {
        {.kind = RMR_MOVE, .end = {viewport.x, viewport.y}},
        {.kind = RMR_LINE, .end = {viewport.x + viewport.width, viewport.y}},
        {.kind = RMR_LINE,
         .end = {viewport.x + viewport.width, viewport.y + viewport.height}},
        {.kind = RMR_LINE, .end = {viewport.x, viewport.y + viewport.height}},
        {.kind = RMR_CLOSE}};
    rmr_draw d = {.mode = RMR_PDF,
                  .path = {clip, 5},
                  .color = {0, 0, 0, 1},
                  .image = {sink->data.bytes,
                            sink->data.size,
                            {viewport.width, 0, 0, viewport.height, viewport.x,
                             viewport.y}}};
    limits.maximum_plan_bytes -= sink->data.size;
    s = rmr_plan_create(&d, 1, viewport, &limits, out, e);
  }

  rmr_buffer_free(&sink->data);
  return s == RMR_OK ? s : rmr_linux_error(e, s, "text PDF rendering failed");
}

/* Measures the Pango-shaped width of text; when less_rightmost is set, the
 * advance of whichever glyph extends furthest to the right is excluded. */
rmr_status rmr_linux_text_width(rm_str text, double size, rmr_type_face face,
                                bool bold, bool italic, bool less_rightmost,
                                const rmr_linux_fonts* fonts,
                                const rmr_limits* given, double* out,
                                rmr_error* e) {
  rmr_limits l = given ? *given : rmr_limits_default();
  if (!fonts || !out || (!text.bytes && text.length) || !isfinite(size) ||
      size <= 0 || size > 100000 || face > RMR_FACE_SERIF)
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT,
                           "invalid text measurement");

  if (text.length > INT_MAX || text.length > l.maximum_elements ||
      text.length > l.maximum_plan_bytes / 4)
    return rmr_linux_error(e, RMR_ERR_LIMIT, "text measurement limit");

  if (!g_utf8_validate(text.bytes ? text.bytes : "", (gssize)text.length,
                       nullptr))
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT, "invalid UTF-8");

  PangoLayout* p = rmr_pango_layout(fonts, text, size, face, bold, italic);
  pango_layout_set_single_paragraph_mode(p, TRUE);
  rmr_status status = rmr_pango_calibrate(p, &l);
  if (status != RMR_OK) {
    g_object_unref(p);
    return rmr_linux_error(e, status, "text calibration limit");
  }

  PangoRectangle logical;
  pango_layout_get_extents(p, nullptr, &logical);
  double width = (double)logical.width / PANGO_SCALE;

  if (less_rightmost) {
    PangoLayoutLine* line = pango_layout_get_line_readonly(p, 0);
    double right = 0, advance = 0, x = 0;
    for (GSList* it = line->runs; it; it = it->next) {
      PangoGlyphItem* run = it->data;
      for (int j = 0; j < run->glyphs->num_glyphs; j++) {
        double step =
            (double)run->glyphs->glyphs[j].geometry.width / PANGO_SCALE;
        if (step > 0 && x + step > right) {
          right = x + step;
          advance = step;
        }
        x += step;
      }
    }
    width -= advance;
  }

  g_object_unref(p);
  *out = width;
  return RMR_OK;
}

rmr_status rmr_linux_unreadable_plan(rmr_rect page, rmr_rect viewport,
                                     const rmr_linux_fonts* fonts,
                                     bool outlines, const rmr_limits* given,
                                     rmr_plan** out, rmr_error* e) {
  if (!out)
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT, "missing label output");
  rmr_limits limits = given ? *given : rmr_limits_default();
  double size = rmr_unreadable_size(page);
  if (!(size > 0) || !isfinite(size))
    return rmr_plan_create(nullptr, 0, viewport, &limits, out, e);
  if (!fonts)
    return rmr_linux_error(e, RMR_ERR_UNSUPPORTED,
                           "document font provider required");

  rm_str label = {RMR_UNREADABLE_LABEL, sizeof RMR_UNREADABLE_LABEL - 1};
  PangoLayout* text =
      rmr_pango_layout(fonts, label, size, RMR_FACE_SANS, false, false);
  pango_layout_set_single_paragraph_mode(text, TRUE);
  rmr_status s = rmr_pango_calibrate(text, &limits);
  if (s != RMR_OK) {
    g_object_unref(text);
    return rmr_linux_error(e, s, "label calibration limit");
  }
  PangoRectangle logical;
  pango_layout_get_extents(text, nullptr, &logical);
  double ascent = (double)(pango_layout_get_baseline(text) - logical.y) /
                  PANGO_SCALE,
         descent = (double)logical.height / PANGO_SCALE - ascent;
  rmr_unreadable_layout place = rmr_unreadable_place(
      page, size, (double)logical.width / PANGO_SCALE, ascent, descent);
  PangoLayoutLine* line = pango_layout_get_line_readonly(text, 0);
  rmr_rect box = place.box;
  rmr_element outline[10];
  rmr_unreadable_outline(box, size, outline);
  rmr_element clip[] = {
      {.kind = RMR_MOVE, .end = {box.x, box.y}},
      {.kind = RMR_LINE, .end = {box.x + box.width, box.y}},
      {.kind = RMR_LINE, .end = {box.x + box.width, box.y + box.height}},
      {.kind = RMR_LINE, .end = {box.x, box.y + box.height}},
      {.kind = RMR_CLOSE}};
  rmr_draw draws[2] = {rmr_unreadable_box(outline, size)};
  rmr_path glyphs = {0};
  layout_sink output = {.sink.maximum = limits.maximum_pdf_bytes <
                                                limits.maximum_plan_bytes / 2
                                            ? limits.maximum_pdf_bytes
                                            : limits.maximum_plan_bytes / 2};

  /* Recording surfaces, like PDF surfaces, keep glyph outlines unhinted. The
   * text alone becomes a PDF of the box, imported as typed text is, so the
   * output embeds its face the same way. */
  cairo_surface_t* surface =
      outlines
          ? cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, nullptr)
          : cairo_pdf_surface_create_for_stream(layout_write, &output,
                                                box.width, box.height);
  cairo_t* cr = cairo_create(surface);
  if (!outlines) cairo_translate(cr, -box.x, -box.y);
  cairo_set_source_rgb(cr, RMR_UNREADABLE_GREY, RMR_UNREADABLE_GREY,
                       RMR_UNREADABLE_GREY);
  cairo_move_to(cr, place.origin.x, place.origin.y);
  if (outlines) {
    pango_cairo_layout_line_path(cr, line);
    s = rmr_linux_path_capture(cr, &limits, &glyphs);
  } else {
    pango_cairo_show_layout_line(cr, line);
    cairo_show_page(cr);
  }
  cairo_status_t cs = cairo_status(cr);
  cairo_destroy(cr);
  cairo_surface_finish(surface);
  if (cs == CAIRO_STATUS_SUCCESS) cs = cairo_surface_status(surface);
  cairo_surface_destroy(surface);
  g_object_unref(text);
  if (s == RMR_OK && output.sink.status != RMR_OK) s = output.sink.status;
  if (s == RMR_OK && cs != CAIRO_STATUS_SUCCESS) s = RMR_ERR_BACKEND;

  draws[1] =
      outlines
          ? (rmr_draw){.mode = RMR_FILL,
                       .path = {glyphs.elements, glyphs.count},
                       .color = {RMR_UNREADABLE_GREY, RMR_UNREADABLE_GREY,
                                 RMR_UNREADABLE_GREY, 1}}
          : (rmr_draw){.mode = RMR_PDF,
                       .path = {clip, 5},
                       .color = {0, 0, 0, 1},
                       .image = {output.sink.data.bytes,
                                 output.sink.data.size,
                                 {box.width, 0, 0, box.height, box.x, box.y}}};
  if (s == RMR_OK) {
    limits.maximum_plan_bytes -= output.sink.data.size;
    s = rmr_plan_create(draws, 2, viewport, &limits, out, e);
  } else {
    rmr_linux_error(e, s, "label drawing failed");
  }
  rmr_path_free(&glyphs);
  rmr_buffer_free(&output.sink.data);
  return s;
}
