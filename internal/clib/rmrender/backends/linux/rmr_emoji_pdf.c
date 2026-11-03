/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "rmr_typography_internal.h"

/* Each shaped cluster becomes one PDF glyph with its original Unicode text.
 * This also supports clusters composed of several positioned native outlines.
 */
typedef struct {
  int first, count;
  double advance;
} emoji_alias;

typedef struct {
  cairo_scaled_font_t* native;
  cairo_matrix_t inverse;
  size_t count;
  emoji_alias* aliases;
  cairo_glyph_t* glyphs;
} emoji_font;

static const cairo_user_data_key_t emoji_key;

/* Returns the emoji_font user data attached to font's face. */
static emoji_font* font_data(cairo_scaled_font_t* font) {
  return cairo_font_face_get_user_data(cairo_scaled_font_get_font_face(font),
                                       &emoji_key);
}

/* Frees an emoji_font's retained native font and its alias/glyph arrays. */
static void font_free(void* data) {
  emoji_font* font = data;
  cairo_scaled_font_destroy(font->native);
  free(font->aliases);
  free(font->glyphs);
  free(font);
}

/* Cairo user-font init callback: converts the native scaled font's extents
 * into the emoji font's (pre-inverse) coordinate space. */
static cairo_status_t font_init(cairo_scaled_font_t* font, cairo_t* cr,
                                cairo_font_extents_t* extents) {
  (void)cr;
  emoji_font* data = font_data(font);
  cairo_scaled_font_extents(data->native, extents);
  double x = 0, y = extents->ascent;
  cairo_matrix_transform_distance(&data->inverse, &x, &y);
  extents->ascent = hypot(x, y);
  x = 0;
  y = extents->descent;
  cairo_matrix_transform_distance(&data->inverse, &x, &y);
  extents->descent = hypot(x, y);
  x = 0;
  y = extents->height;
  cairo_matrix_transform_distance(&data->inverse, &x, &y);
  extents->height = hypot(x, y);

  cairo_matrix_transform_distance(&data->inverse, &extents->max_x_advance,
                                  &extents->max_y_advance);
  return cairo_scaled_font_status(data->native);
}

/* Cairo user-font render callback: fills extents for the aliased cluster at
 * index and paints its underlying native glyphs at full native precision. */
static cairo_status_t font_render(cairo_scaled_font_t* font,
                                  unsigned long index, cairo_t* cr,
                                  cairo_text_extents_t* extents) {
  emoji_font* data = font_data(font);
  if (!index || index > data->count) return CAIRO_STATUS_USER_FONT_ERROR;

  emoji_alias alias = data->aliases[index - 1];
  cairo_glyph_t* glyphs = data->glyphs + alias.first;
  cairo_scaled_font_glyph_extents(data->native, glyphs, alias.count, extents);
  extents->x_advance = alias.advance;
  extents->y_advance = 0;

  double left = INFINITY, top = INFINITY, right = -INFINITY, bottom = -INFINITY;
  for (unsigned corner = 0; corner < 4; corner++) {
    double x = extents->x_bearing + ((corner & 1) ? extents->width : 0);
    double y = extents->y_bearing + ((corner & 2) ? extents->height : 0);
    cairo_matrix_transform_point(&data->inverse, &x, &y);
    left = fmin(left, x);
    right = fmax(right, x);
    top = fmin(top, y);
    bottom = fmax(bottom, y);
  }
  extents->x_bearing = left;
  extents->y_bearing = top;
  extents->width = right - left;
  extents->height = bottom - top;
  cairo_matrix_transform_distance(&data->inverse, &extents->x_advance,
                                  &extents->y_advance);

  /* Keep the native size while obtaining outlines. Recreating a one-unit
   * font loses FreeType precision before the PDF font matrix scales it up. */
  cairo_transform(cr, &data->inverse);
  cairo_set_scaled_font(cr, data->native);
  cairo_glyph_path(cr, glyphs, alias.count);
  cairo_fill(cr);
  return cairo_status(cr);
}

/* Cairo user-font unicode-to-glyph callback: always reports no glyph, since
 * glyphs are addressed only by explicit cluster index. */
static cairo_status_t no_unicode(cairo_scaled_font_t* font,
                                 unsigned long unicode, unsigned long* glyph) {
  (void)font;
  (void)unicode;
  *glyph = 0;
  return CAIRO_STATUS_SUCCESS;
}

/* Returns whether run's glyphs come from the Noto Emoji font. */
static bool is_emoji(PangoGlyphItem* run) {
  PangoFontDescription* description =
      pango_font_describe(run->item->analysis.font);
  bool result =
      !strcmp(pango_font_description_get_family(description), "Noto Emoji");
  pango_font_description_free(description);
  return result;
}

/* Draws one Pango glyph run as a cairo user font, so each cluster becomes a
 * single PDF glyph carrying its original text. Falls back to Pango's own
 * glyph-item renderer for clusters it cannot alias or past resource limits.
 */
static rmr_status show_emoji(cairo_t* cr, const char* text, PangoGlyphItem* run,
                             const rmr_limits* limits, size_t* retained) {
  bool rtl = (run->item->analysis.level & 1) != 0;
  PangoGlyphItemIter cluster;
  /* Keep Pango's general renderer for uncombined/unsupported sequences.
   * Clusters may contain several outlines plus optional empty glyphs. */
  for (gboolean valid = pango_glyph_item_iter_init_start(&cluster, run, text);
       valid; valid = pango_glyph_item_iter_next_cluster(&cluster)) {
    size_t visible = 0;
    for (int g = cluster.start_glyph; g != cluster.end_glyph;
         g += rtl ? -1 : 1) {
      PangoGlyph glyph = run->glyphs->glyphs[g].glyph;
      if (glyph != PANGO_GLYPH_EMPTY) {
        if (!glyph || glyph >= PANGO_GLYPH_UNKNOWN_FLAG) visible = SIZE_MAX;
        if (visible == SIZE_MAX) break;
        visible++;
      }
    }
    if (!visible || visible == SIZE_MAX) {
      pango_cairo_show_glyph_item(cr, text, run);
      return RMR_OK;
    }
  }

  size_t count = (size_t)run->glyphs->num_glyphs;
  if (!count) return RMR_OK;
  size_t per_glyph = sizeof(emoji_alias) + 2 * sizeof(cairo_glyph_t);
  if (count > limits->maximum_elements ||
      count > (SIZE_MAX - sizeof(emoji_font)) / per_glyph)
    return RMR_ERR_LIMIT;
  size_t owned = sizeof(emoji_font) +
                 count * (sizeof(emoji_alias) + sizeof(cairo_glyph_t));
  size_t available = limits->maximum_plan_bytes - *retained;
  if (owned > available || count * sizeof(cairo_glyph_t) > available - owned)
    return RMR_ERR_LIMIT;

  emoji_font* data = calloc(1, sizeof *data);
  cairo_glyph_t* positions = calloc(count, sizeof *positions);
  if (data) {
    data->aliases = calloc(count, sizeof *data->aliases);
    data->glyphs = calloc(count, sizeof *data->glyphs);
  }
  if (!data || !positions || !data->aliases || !data->glyphs) {
    if (data) {
      free(data->aliases);
      free(data->glyphs);
    }
    free(data);
    free(positions);
    return RMR_ERR_OUT_OF_MEMORY;
  }

  cairo_scaled_font_t* source = pango_cairo_font_get_scaled_font(
      PANGO_CAIRO_FONT(run->item->analysis.font));
  cairo_font_options_t* options = cairo_font_options_create();
  cairo_scaled_font_get_font_options(source, options);
  cairo_matrix_t matrix;
  cairo_scaled_font_get_font_matrix(source, &matrix);

  /* Normalize the first emitted cluster to a half-em horizontal advance.
   * Poppler estimates Type3 text size from that advance. Compensating in
   * the font matrix and its inverse preserves outlines and real advances. */
  (void)pango_glyph_item_iter_init_start(&cluster, run, text);
  double advance = 0;
  for (int g = cluster.start_glyph; g != cluster.end_glyph; g += rtl ? -1 : 1)
    advance += (double)run->glyphs->glyphs[g].geometry.width / PANGO_SCALE;
  if (advance > 0 && matrix.xx > 0) {
    double scale = 2 * advance / matrix.xx;
    matrix.xx *= scale;
    matrix.yx *= scale;
  }

  data->native = cairo_scaled_font_reference(source);
  data->inverse = matrix;
  if (cairo_matrix_invert(&data->inverse) != CAIRO_STATUS_SUCCESS) {
    font_free(data);
    cairo_font_options_destroy(options);
    free(positions);
    return RMR_ERR_BACKEND;
  }

  cairo_font_face_t* face = cairo_user_font_face_create();
  cairo_status_t cs =
      cairo_font_face_set_user_data(face, &emoji_key, data, font_free);
  if (cs != CAIRO_STATUS_SUCCESS) {
    font_free(data);
    cairo_font_face_destroy(face);
    cairo_font_options_destroy(options);
    free(positions);
    return cs == CAIRO_STATUS_NO_MEMORY ? RMR_ERR_OUT_OF_MEMORY
                                        : RMR_ERR_BACKEND;
  }
  *retained += owned;
  cairo_user_font_face_set_init_func(face, font_init);
  cairo_user_font_face_set_render_glyph_func(face, font_render);
  cairo_user_font_face_set_unicode_to_glyph_func(face, no_unicode);

  double x, y;
  cairo_get_current_point(cr, &x, &y);
  double pen = x;
  for (size_t g = 0; g < count; g++) {
    PangoGlyphGeometry geometry = run->glyphs->glyphs[g].geometry;
    positions[g] =
        (cairo_glyph_t){0, pen + (double)geometry.x_offset / PANGO_SCALE,
                        y + (double)geometry.y_offset / PANGO_SCALE};
    pen += (double)geometry.width / PANGO_SCALE;
  }

  cairo_save(cr);
  cairo_set_font_face(cr, face);
  cairo_set_font_matrix(cr, &matrix);
  cairo_set_font_options(cr, options);
  for (gboolean valid = pango_glyph_item_iter_init_start(&cluster, run, text);
       valid; valid = pango_glyph_item_iter_next_cluster(&cluster)) {
    int low = rtl ? cluster.end_glyph + 1 : cluster.start_glyph;
    int high = rtl ? cluster.start_glyph + 1 : cluster.end_glyph;
    cairo_glyph_t output = positions[low];
    double width = 0;
    int written = 0;
    for (int g = low; g < high; g++) {
      width += (double)run->glyphs->glyphs[g].geometry.width / PANGO_SCALE;
      if (run->glyphs->glyphs[g].glyph == PANGO_GLYPH_EMPTY) continue;
      data->glyphs[low + written++] =
          (cairo_glyph_t){run->glyphs->glyphs[g].glyph,
                          positions[g].x - output.x, positions[g].y - output.y};
    }
    data->aliases[data->count++] = (emoji_alias){low, written, width};
    output.index = (unsigned long)data->count;
    size_t start = (size_t)cluster.start_index,
           length = (size_t)(cluster.end_index - cluster.start_index);
    cairo_text_cluster_t span = {(int)length, 1};
    cairo_show_text_glyphs(cr, text + start, (int)length, &output, 1, &span, 1,
                           0);
  }

  cs = cairo_status(cr);
  cairo_restore(cr);
  cairo_font_face_destroy(face);
  cairo_font_options_destroy(options);
  free(positions);
  return cs == CAIRO_STATUS_SUCCESS     ? RMR_OK
         : cs == CAIRO_STATUS_NO_MEMORY ? RMR_ERR_OUT_OF_MEMORY
                                        : RMR_ERR_BACKEND;
}

rmr_status rmr_pango_show_line(cairo_t* cr, PangoLayoutLine* line,
                               const rmr_limits* limits, size_t* retained) {
  bool emoji = false;
  for (GSList* it = line->runs; it; it = it->next) emoji |= is_emoji(it->data);
  if (!emoji) {
    pango_cairo_show_layout_line(cr, line);
    return RMR_OK;
  }

  const char* text = pango_layout_get_text(line->layout);
  double x, y;
  cairo_get_current_point(cr, &x, &y);

  for (GSList* it = line->runs; it; it = it->next) {
    PangoGlyphItem* run = it->data;
    x += (double)run->start_x_offset / PANGO_SCALE;
    cairo_move_to(cr, x, y - (double)run->y_offset / PANGO_SCALE);
    if (is_emoji(run)) {
      rmr_status status = show_emoji(cr, text, run, limits, retained);
      if (status != RMR_OK) return status;
    } else
      pango_cairo_show_glyph_item(cr, text, run);
    x += (double)pango_glyph_string_get_width(run->glyphs) / PANGO_SCALE +
         (double)run->end_x_offset / PANGO_SCALE;
  }
  return RMR_OK;
}
