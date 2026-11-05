/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <hb-ot.h>
#include <limits.h>
#include <pango/pangofc-font.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/rmr_font_calibration.h"
#include "rmr_font_cache.h"
#include "rmr_hvar.h"
#include "rmr_typography_internal.h"
#ifdef __linux__
#include <sys/mman.h>
#endif
#ifndef _WIN32
#include <unistd.h>
#endif

/* Writes the calibration-table lookup name for this face/weight/script/size
 * combination into key and returns it. */
static const char* calibration_key(rmr_type_face face, bool bold, bool italic,
                                   int script, double size, char key[96]) {
  if (script >= 0) {
    static const char* const families[] = {
        "NotoSans",   "NotoSansHebrew", "NotoSansDevanagari", "NotoSansArabic",
        "NotoSansKR", "NotoSansJP",     "NotoSansThaiLooped"};
    snprintf(key, 96, "%s-%s", families[script],
             bold                                             ? "Bold"
             : face == RMR_FACE_MEDIUM                        ? "Medium"
             : face == RMR_FACE_SERIF && italic && size >= 64 ? "Light"
                                                              : "Regular");
    return key;
  }

  if (face == RMR_FACE_SERIF)
    return bold     ? "reMarkableSerifSmall-Bold"
           : italic ? "reMarkableSerifSmall-RegularItalic"
                    : "reMarkableSerifSmall-Regular";
  return bold                      ? "reMarkableSans-Bold"
         : italic                  ? "reMarkableSans-RegularItalic"
         : face == RMR_FACE_MEDIUM ? "reMarkableSans-Medium"
                                   : "reMarkableSans-Regular";
}

/* The calibrated kerning share for c under this face/weight/style/script, as
 * a fraction of the type size. */
static double calibrated_kern(uint32_t c, rmr_type_face face, bool bold,
                              bool italic, int script) {
  if (c > UINT16_MAX) return 0;
  uint16_t u = (uint16_t)c;

  if (script >= 0) return script == 2 ? 0 : rmr_noto_glyph_share(u);
  if (face == RMR_FACE_SERIF)
    return bold     ? rmr_serif_bold_glyph_share(u)
           : italic ? rmr_serif_italic_glyph_share(u)
                    : rmr_serif_glyph_share(u);
  if (face == RMR_FACE_MEDIUM && !bold)
    return italic ? 0 : rmr_sans_medium_glyph_share(u);
  return bold     ? rmr_sans_bold_glyph_share(u)
         : italic ? rmr_sans_italic_glyph_share(u)
                  : rmr_sans_glyph_share(u);
}

/* Fontconfig default-substitute callback: rewrites family, weight, slant,
 * variation axis and synthetic matrix for the RMR-* alias family encoded in
 * pattern, or swaps in the real emoji family; other patterns are untouched.
 */
static void calibrated_pattern(FcPattern* pattern, void* context) {
  const rmr_linux_fonts* fonts = context;
  FcChar8* family = nullptr;
  int face, bold, italic, script;
  if (fonts->has_emoji &&
      FcPatternGetString(pattern, FC_FAMILY, 0, &family) == FcResultMatch &&
      !strcmp((const char*)family, "emoji")) {
    FcPatternDel(pattern, FC_FAMILY);
    FcPatternAddString(pattern, FC_FAMILY, (const FcChar8*)"Noto Emoji");
    return;
  }

  if (FcPatternGetString(pattern, FC_FAMILY, 0, &family) != FcResultMatch ||
      sscanf((const char*)family, "RMR-%d-%d-%d-%d", &face, &bold, &italic,
             &script) != 4 ||
      face < 0 || face > 2 || script < -1 || script > 6)
    return;

  bool base = strstr((const char*)family, "-base") != nullptr;
  double size = 32;
  FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &size);
  char key[96];
  const char* name = calibration_key((rmr_type_face)face, bold != 0,
                                     italic != 0, script, size, key);

  double width = 1, nearest = INFINITY;
  for (size_t i = 0; i < rmr_stretch_count; i++) {
    double distance = fabs(size - rmr_stretches[i].size);
    if (!strcmp(name, rmr_stretches[i].key) && distance < nearest) {
      nearest = distance;
      width = rmr_stretches[i].width;
    }
  }

  static const char* const families[] = {"Noto Sans",
                                         "Noto Sans Hebrew",
                                         "Noto Sans Devanagari",
                                         "Noto Sans Arabic",
                                         "Noto Sans KR",
                                         "Noto Sans JP",
                                         "Noto Sans Thai Looped"};
  FcPatternDel(pattern, FC_FAMILY);
  FcPatternAddString(pattern, FC_FAMILY,
                     (const FcChar8*)(script >= 0              ? families[0]
                                      : face == RMR_FACE_SERIF ? "EB Garamond"
                                                               : "Noto Sans"));
  const unsigned cascade[] = {0, 3, 1, 2, 4, 5, 6};
  if (!base)
    for (size_t i = 0; i < 7; i++)
      FcPatternAddString(pattern, FC_FAMILY,
                         (const FcChar8*)families[cascade[i]]);
  if (!base)
    FcPatternAddString(pattern, FC_FAMILY, (const FcChar8*)"Noto Emoji");

  bool synthetic = italic && (script >= 0 || bold);
  bool light = (italic && size >= 64) || (script == 2 && size >= 64 && !bold);
  bool weighted_light =
      light && (script == 1 || script == 2 || script == 3 || script == 6 ||
                (script == 0 && face == RMR_FACE_SERIF && italic));
  if (base) weighted_light = face == RMR_FACE_SERIF && italic && size >= 64;

  FcPatternDel(pattern, FC_WEIGHT);
  FcPatternAddInteger(pattern, FC_WEIGHT,
                      bold ? FC_WEIGHT_BOLD
                      : face == RMR_FACE_MEDIUM && (!italic || script >= 0)
                          ? FC_WEIGHT_MEDIUM
                      : weighted_light ? FC_WEIGHT_LIGHT
                                       : FC_WEIGHT_REGULAR);

  if (script > 0 || (script < 0 && face == RMR_FACE_SERIF)) {
    char variation[32];
    snprintf(variation, sizeof variation, "wght=%d",
             bold                      ? 700
             : face == RMR_FACE_MEDIUM ? 500
             : weighted_light          ? 300
                                       : 400);
    FcPatternDel(pattern, FC_FONT_VARIATIONS);
    FcPatternAddString(pattern, FC_FONT_VARIATIONS, (const FcChar8*)variation);
  }

  FcPatternDel(pattern, FC_SLANT);
  FcPatternAddInteger(pattern, FC_SLANT,
                      italic && !synthetic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
  FcMatrix matrix = {width, synthetic ? .3 : 0, 0, 1};
  FcPatternDel(pattern, FC_MATRIX);
  FcPatternAddMatrix(pattern, FC_MATRIX, &matrix);
}

/* Internal script index for c: 0 default, 1 Hebrew, 2 Devanagari, 3 Arabic,
 * 4 Korean, 5 CJK, 6 Thai. */
static int character_script(gunichar c) {
  if ((c >= 0x590 && c <= 0x5ff) || (c >= 0xfb1d && c <= 0xfb4f)) return 1;
  if (c >= 0x900 && c <= 0x97f) return 2;
  if ((c >= 0x600 && c <= 0x6ff) || (c >= 0xfb50 && c <= 0xfeff)) return 3;
  if ((c >= 0x1100 && c <= 0x11ff) || (c >= 0x3130 && c <= 0x318f) ||
      (c >= 0xac00 && c <= 0xd7af))
    return 4;
  if ((c >= 0x3040 && c <= 0x30ff) || (c >= 0x3400 && c <= 0x4dbf) ||
      (c >= 0x4e00 && c <= 0x9fff))
    return 5;
  if (c >= 0xe00 && c <= 0xe7f) return 6;
  return 0;
}

/* True if c is a strong directional/script character that determines the
 * script used for calibration from this point on. */
static bool strong(gunichar c) {
  return g_unichar_isalpha(c) || (c >= 0x3040 && c <= 0x30ff);
}

/* True if c is a basic Latin or Latin-1/Extended-A letter, exempted from
 * script-specific calibration. */
static bool latin_character(gunichar c) {
  return (c >= 65 && c <= 90) || (c >= 97 && c <= 122) ||
         (c >= 0xc0 && c <= 0x24f) || (c >= 0x1e00 && c <= 0x1eff);
}

/* Spacing metadata is removed before Pango shapes the text. Applying it to
 * completed clusters preserves natural font kerning and joining. */
static PangoAttrClass spacing_class;

/* Duplicates a calibrated-spacing Pango attribute. */
static PangoAttribute* spacing_copy(const PangoAttribute* a) {
  PangoAttrInt* copy = g_new(PangoAttrInt, 1);
  *copy = *(const PangoAttrInt*)a;
  return &copy->attr;
}

/* Frees a calibrated-spacing Pango attribute. */
static void spacing_destroy(PangoAttribute* a) { g_free(a); }

/* True if two calibrated-spacing attributes carry the same value. */
static gboolean spacing_equal(const PangoAttribute* a,
                              const PangoAttribute* b) {
  return ((const PangoAttrInt*)a)->value == ((const PangoAttrInt*)b)->value;
}

/* The calibrated-spacing attribute class, registered on first use by any
 * thread. */
static const PangoAttrClass* spacing_klass(void) {
  static gsize initialized;
  if (g_once_init_enter(&initialized)) {
    spacing_class =
        (PangoAttrClass){pango_attr_type_register("rmrender-cluster-spacing"),
                         spacing_copy, spacing_destroy, spacing_equal};
    g_once_init_leave(&initialized, 1);
  }
  return &spacing_class;
}

/* Creates a calibrated-spacing Pango attribute carrying value. */
static PangoAttribute* spacing_new(int value) {
  PangoAttrInt* a = g_new0(PangoAttrInt, 1);
  pango_attribute_init(&a->attr, spacing_klass());
  a->value = value;
  return &a->attr;
}

/* Pango attribute filter matching calibrated-spacing attributes. */
static gboolean is_spacing(PangoAttribute* a, gpointer context) {
  (void)context;
  return a->klass == &spacing_class;
}

PangoAttrList* rmr_pango_source_attributes(PangoLayout* layout) {
  PangoAttrList* source =
      g_object_get_data(G_OBJECT(layout), "rmrender-source-attributes");
  return source ? source : pango_layout_get_attributes(layout);
}

/* CoreText shears GPOS advance adjustments along with the font matrix;
 * nominal horizontal advances remain unchanged. Cairo applies the shear only
 * to glyph outlines, so mirror the extra adjustment in document geometry. */
static rmr_status synthetic_advances(PangoGlyphItem* run, const char* text,
                                     size_t length) {
  PangoFont* font = run->item->analysis.font;
  if (!PANGO_IS_FC_FONT(font)) return RMR_OK;

  FcMatrix* matrix = nullptr;
  if (FcPatternGetMatrix(pango_fc_font_get_pattern(PANGO_FC_FONT(font)),
                         FC_MATRIX, 0, &matrix) != FcResultMatch ||
      !matrix || matrix->xy == 0 || matrix->xx <= 0)
    return RMR_OK;
  double ratio = matrix->xy / matrix->xx;

  /* HarfBuzz has already sheared attachment offsets in its y-up space.
   * Undo that contribution before applying CoreText's x adjustment. The
   * effective HB slant need not equal the requested matrix shear (Pango
   * normalizes the font matrix). */
  hb_font_t* hb_font = pango_font_get_hb_font(font);
  double slant = hb_font_get_synthetic_slant(hb_font);
  int x_scale, y_scale;
  hb_font_get_scale(hb_font, &x_scale, &y_scale);
  /* Synthetic slant is in font units; attachment offsets are already scaled. */
  double y_ratio = y_scale ? slant * x_scale / y_scale * (1 + ratio) : 0;

  PangoAnalysis analysis = run->item->analysis;
  /* Leave mark features enabled: disabling them can change Hebrew glyph
   * decomposition. Mark offsets below use the attachment vector directly. */
  PangoAttribute* no_positioning =
      pango_attr_font_features_new("kern=0,dist=0");
  analysis.extra_attrs =
      g_slist_append(g_slist_copy(analysis.extra_attrs), no_positioning);
  PangoGlyphString* nominal = pango_glyph_string_new();
  pango_shape_full(text + run->item->offset, run->item->length, text,
                   (int)length, &analysis, nominal);
  g_slist_free(analysis.extra_attrs);
  pango_attribute_destroy(no_positioning);

  bool compatible = nominal->num_glyphs == run->glyphs->num_glyphs;
  for (int i = 0; compatible && i < nominal->num_glyphs; i++)
    compatible = nominal->glyphs[i].glyph == run->glyphs->glyphs[i].glyph &&
                 nominal->log_clusters[i] == run->glyphs->log_clusters[i];

  rmr_status status = RMR_OK;
  double base_distance = 0;
  hb_face_t* hb_face = hb_font_get_face(pango_font_get_hb_font(font));
  if (compatible)
    for (int i = 0; i < run->glyphs->num_glyphs; i++) {
      PangoGlyphGeometry* g = &run->glyphs->glyphs[i].geometry;
      PangoGlyphGeometry n = nominal->glyphs[i].geometry;
      double width = g->width + (g->width - (double)n.width) * ratio;
      bool mark =
          hb_ot_layout_get_glyph_class(hb_face, run->glyphs->glyphs[i].glyph) ==
          HB_OT_LAYOUT_GLYPH_CLASS_MARK;
      /* LTR marks follow their base in visual order: their pen is already
       * past the base advance. RTL marks precede the base instead. */
      double attachment =
          mark && !(run->item->analysis.level & 1) ? base_distance : 0;
      if (!mark) base_distance = 0;
      base_distance += g->width;
      double offset = attachment * ratio + g->x_offset +
                      (g->x_offset - (mark ? 0.0 : n.x_offset)) * ratio +
                      (g->y_offset - (mark ? 0.0 : n.y_offset)) * y_ratio;
      if (!isfinite(width) || width > INT_MAX || width < INT_MIN ||
          !isfinite(offset) || offset > INT_MAX || offset < INT_MIN) {
        status = RMR_ERR_LIMIT;
        break;
      }
      g->width = (int)lrint(width);
      g->x_offset = (int)lrint(offset);
    }

  pango_glyph_string_free(nominal);
  return status;
}

/* Restore the fractional HVAR advance lost before HarfBuzz scales to Pango
 * units. Do this after synthetic GPOS adjustment: nominal metrics are not
 * kerns. */
static rmr_status variable_advances(PangoGlyphItem* run) {
  hb_font_t* font = pango_font_get_hb_font(run->item->analysis.font);
  unsigned count = 0;
  const int* coords = hb_font_get_var_coords_normalized(font, &count);
  if (!count) return RMR_OK;

  hb_face_t* face = hb_font_get_face(font);
  hb_font_t* base = hb_font_create(face);
  hb_ot_font_set_funcs(base);
  /* A cached face may carry the named instance selected on its first use.
   * HVAR deltas are relative to fvar defaults, never that named instance. */
  hb_font_set_var_coords_normalized(base, nullptr, 0);

  hb_blob_t* blob = hb_face_reference_table(face, HB_TAG('H', 'V', 'A', 'R'));
  unsigned length = 0;
  const char* bytes = hb_blob_get_data(blob, &length);
  rmr_ot_bytes table = {(const uint8_t*)bytes, length};
  int x_scale, y_scale;
  hb_font_get_scale(font, &x_scale, &y_scale);
  double scale = (double)x_scale / hb_face_get_upem(face);

  rmr_status status = RMR_OK;
  double base_correction = 0;
  for (int i = 0; i < run->glyphs->num_glyphs; i++) {
    PangoGlyphInfo* g = &run->glyphs->glyphs[i];
    double delta;
    if (g->glyph >= PANGO_GLYPH_UNKNOWN_FLAG) {
      base_correction = 0;
      continue;
    }

    if (hb_ot_layout_get_glyph_class(face, g->glyph) ==
        HB_OT_LAYOUT_GLYPH_CLASS_MARK) {
      /* Keep the attachment anchored to the base origin when its
       * fractional advance changes the pen preceding an LTR mark. */
      if (!(run->item->analysis.level & 1)) {
        double offset = g->geometry.x_offset - base_correction;
        if (!isfinite(offset) || offset > INT_MAX || offset < INT_MIN) {
          status = RMR_ERR_LIMIT;
          break;
        }
        g->geometry.x_offset = (int)lrint(offset);
      }
      continue;
    }

    base_correction = 0;
    if (!rmr_hvar_delta(table, g->glyph, coords, count, &delta)) continue;
    double advance =
        (hb_font_get_glyph_h_advance(base, g->glyph) + delta) * scale;
    double width = g->geometry.width + advance -
                   hb_font_get_glyph_h_advance(font, g->glyph);
    if (!isfinite(width) || width > INT_MAX || width < INT_MIN) {
      status = RMR_ERR_LIMIT;
      break;
    }
    base_correction = (double)lrint(width) - g->geometry.width;
    g->geometry.width = (int)lrint(width);
  }

  hb_blob_destroy(blob);
  hb_font_destroy(base);
  return status;
}

/* Keep variation selectors with their visible emoji in Cairo's text cluster.
 * Otherwise Pango emits an empty ActualText span that PDF readers discard. */
static void emoji_clusters(PangoGlyphItem* run, const char* text) {
  PangoFontDescription* description =
      pango_font_describe(run->item->analysis.font);
  bool emoji =
      !strcmp(pango_font_description_get_family(description), "Noto Emoji");
  pango_font_description_free(description);
  if (!emoji) return;

  bool rtl = (run->item->analysis.level & 1) != 0;
  for (int i = 0; i < run->glyphs->num_glyphs; i++) {
    int index = run->glyphs->log_clusters[i];
    gunichar c = g_utf8_get_char(text + run->item->offset + index);
    int base = rtl ? i + 1 : i - 1;
    if (run->glyphs->glyphs[i].glyph != PANGO_GLYPH_EMPTY ||
        (c != 0xfe0e && c != 0xfe0f && c != 0x200d) || base < 0 ||
        base >= run->glyphs->num_glyphs ||
        run->glyphs->log_clusters[base] >= index)
      continue;
    run->glyphs->log_clusters[i] = run->glyphs->log_clusters[base];
    run->glyphs->glyphs[i].attr.is_cluster_start = 0;

    /* An unsupported ZWJ ligature can retain several outlines. Keep its
     * following emoji in the same text cluster without moving any glyph. */
    int next = rtl ? i - 1 : i + 1;
    if (c == 0x200d && next >= 0 && next < run->glyphs->num_glyphs) {
      int following = run->glyphs->log_clusters[next];
      for (int g = next; g >= 0 && g < run->glyphs->num_glyphs &&
                         run->glyphs->log_clusters[g] == following;
           g += rtl ? -1 : 1) {
        run->glyphs->log_clusters[g] = run->glyphs->log_clusters[base];
        run->glyphs->glyphs[g].attr.is_cluster_start = 0;
      }
    }
  }
}

rmr_status rmr_pango_calibrate(PangoLayout* layout, const rmr_limits* limits) {
  guint serial = pango_layout_get_serial(layout);
  if (GPOINTER_TO_UINT(
          g_object_get_data(G_OBJECT(layout), "rmrender-calibrated")) == serial)
    return RMR_OK;

  const char* text = pango_layout_get_text(layout);
  size_t length = strlen(text);
  if (length >= limits->maximum_plan_bytes / sizeof(int64_t))
    return RMR_ERR_LIMIT;
  int64_t* prefix = calloc(length + 1, sizeof *prefix);
  if (!prefix) return RMR_ERR_OUT_OF_MEMORY;

  PangoAttrList* original =
      pango_attr_list_ref(rmr_pango_source_attributes(layout));
  PangoAttrList* clean = pango_attr_list_copy(original);
  PangoAttrList* removed = pango_attr_list_filter(clean, is_spacing, nullptr);
  if (removed) pango_attr_list_unref(removed);

  /* Optional ligatures do not cross a change of explicit spacing in CoreText.
   * Keep them enabled within equal-spacing spans, including ff before i. */
  PangoAttrList* spacing_spans = pango_attr_list_copy(original);
  GSList* spans = pango_attr_list_get_attributes(spacing_spans);
  for (GSList* it = spans; it; it = it->next) {
    PangoAttribute* a = it->data;
    if (a->klass == &spacing_class) {
      size_t end = a->end_index < length ? a->end_index : length;
      bool multiple =
          a->start_index < end &&
          (size_t)(g_utf8_next_char(text + a->start_index) - text) < end;
      PangoAttribute* feature = pango_attr_font_features_new(
          multiple ? "liga=1,clig=1" : "liga=0,clig=0");
      feature->start_index = a->start_index;
      feature->end_index = a->end_index;
      pango_attr_list_change(clean, feature);
    }
    pango_attribute_destroy(a);
  }
  g_slist_free(spans);
  pango_attr_list_unref(spacing_spans);

  pango_layout_set_attributes(layout, clean);
  pango_attr_list_unref(clean);
  g_object_set_data_full(G_OBJECT(layout), "rmrender-source-attributes",
                         original, (GDestroyNotify)pango_attr_list_unref);
  serial = pango_layout_get_serial(layout);

  GSList* attrs = pango_attr_list_get_attributes(original);
  for (GSList* it = attrs; it; it = it->next) {
    PangoAttribute* a = it->data;
    if (a->klass == &spacing_class) {
      size_t end = a->end_index < length ? a->end_index : length;
      for (size_t at = a->start_index; at < end;
           at = (size_t)(g_utf8_next_char(text + at) - text))
        prefix[at + 1] = ((PangoAttrInt*)a)->value;
    }
    pango_attribute_destroy(a);
  }
  g_slist_free(attrs);

  for (size_t i = 1; i <= length; i++) prefix[i] += prefix[i - 1];

  for (GSList* line_it = pango_layout_get_lines(layout); line_it;
       line_it = line_it->next) {
    PangoLayoutLine* line = line_it->data;
    for (GSList* it = line->runs; it; it = it->next) {
      PangoGlyphItem* run = it->data;
      emoji_clusters(run, text);
      rmr_status status = synthetic_advances(run, text, length);
      if (status == RMR_OK) status = variable_advances(run);
      if (status != RMR_OK) {
        free(prefix);
        return status;
      }

      PangoGlyphItemIter cluster;
      for (gboolean valid =
               pango_glyph_item_iter_init_start(&cluster, run, text);
           valid; valid = pango_glyph_item_iter_next_cluster(&cluster)) {
        int glyph = cluster.start_glyph < cluster.end_glyph
                        ? cluster.end_glyph - 1
                        : cluster.end_glyph + 1;
        int64_t width = run->glyphs->glyphs[glyph].geometry.width;
        int64_t spacing =
            prefix[cluster.start_index + 1] - prefix[cluster.start_index];
        bool uniform = true;
        for (const char* at = text + cluster.start_index;
             at < text + cluster.end_index; at = g_utf8_next_char(at)) {
          size_t index = (size_t)(at - text);
          uniform &= prefix[index + 1] - prefix[index] == spacing;
        }
        width += uniform
                     ? spacing
                     : prefix[cluster.end_index] - prefix[cluster.start_index];
        if (width > INT_MAX || width < INT_MIN) {
          free(prefix);
          return RMR_ERR_LIMIT;
        }
        run->glyphs->glyphs[glyph].geometry.width = (int)width;
      }
    }
  }

  free(prefix);
  g_object_set_data(G_OBJECT(layout), "rmrender-calibrated",
                    GUINT_TO_POINTER(serial));
  return RMR_OK;
}

double rmr_pango_cluster_spacing(PangoLayout* layout, size_t from, size_t to) {
  const char* text = pango_layout_get_text(layout);
  PangoAttrIterator* it =
      pango_attr_list_get_iterator(rmr_pango_source_attributes(layout));
  int first = 0;
  int64_t sum = 0;
  bool uniform = true;

  for (const char* at = text + from; at < text + to;
       at = g_utf8_next_char(at)) {
    size_t index = (size_t)(at - text);
    int start, end, spacing = 0;
    pango_attr_iterator_range(it, &start, &end);
    while (index >= (size_t)end && pango_attr_iterator_next(it))
      pango_attr_iterator_range(it, &start, &end);
    PangoAttribute* a = pango_attr_iterator_get(it, spacing_klass()->type);
    if (a) spacing = ((PangoAttrInt*)a)->value;
    if (at == text + from) first = spacing;
    uniform &= spacing == first;
    sum += spacing;
  }

  pango_attr_iterator_destroy(it);
  return -.5 * (double)(uniform ? first : sum) / PANGO_SCALE;
}

/* Carets split the gap between nominal cluster advances, including GPOS and
 * explicit spacing. Use fractional HVAR metrics, as in variable_advances(). */
double rmr_pango_cluster_adjustment(PangoGlyphItem* run, int cluster) {
  hb_font_t* font = pango_font_get_hb_font(run->item->analysis.font);
  hb_face_t* face = hb_font_get_face(font);
  unsigned count = 0, length = 0;
  const int* coords = hb_font_get_var_coords_normalized(font, &count);
  hb_font_t* base = nullptr;
  hb_blob_t* blob = nullptr;
  rmr_ot_bytes table = {0};
  int x_scale, y_scale;
  hb_font_get_scale(font, &x_scale, &y_scale);
  double scale = (double)x_scale / hb_face_get_upem(face), delta_width = 0;
  if (count) {
    base = hb_font_create(face);
    hb_ot_font_set_funcs(base);
    hb_font_set_var_coords_normalized(base, nullptr, 0);
    blob = hb_face_reference_table(face, HB_TAG('H', 'V', 'A', 'R'));
    const char* bytes = hb_blob_get_data(blob, &length);
    table = (rmr_ot_bytes){(const uint8_t*)bytes, length};
  }

  for (int i = 0; i < run->glyphs->num_glyphs; i++) {
    PangoGlyphInfo g = run->glyphs->glyphs[i];
    if (run->glyphs->log_clusters[i] != cluster ||
        g.glyph >= PANGO_GLYPH_UNKNOWN_FLAG)
      continue;
    double nominal = hb_font_get_glyph_h_advance(font, g.glyph), delta;
    if (hb_ot_layout_get_glyph_class(face, g.glyph) ==
        HB_OT_LAYOUT_GLYPH_CLASS_MARK)
      nominal = 0;
    else if (count && rmr_hvar_delta(table, g.glyph, coords, count, &delta))
      nominal = (hb_font_get_glyph_h_advance(base, g.glyph) + delta) * scale;
    delta_width += g.geometry.width - nominal;
  }

  if (blob) hb_blob_destroy(blob);
  if (base) hb_font_destroy(base);
  return -.5 * delta_width / PANGO_SCALE;
}

double rmr_pango_caret_adjustment(PangoLayout* layout, size_t index) {
  const char* text = pango_layout_get_text(layout);
  size_t length = strlen(text);
  if (!index || index > length) return 0;
  const char* previous = g_utf8_find_prev_char(text, text + index);
  if (!previous) return 0;

  size_t at = (size_t)(previous - text);
  PangoAttrIterator* it =
      pango_attr_list_get_iterator(rmr_pango_source_attributes(layout));
  double adjustment = 0;
  do {
    int start, end;
    pango_attr_iterator_range(it, &start, &end);
    if (start >= 0 && (size_t)start <= at && end >= 0 && at < (size_t)end) {
      PangoAttribute* a = pango_attr_iterator_get(it, spacing_klass()->type);
      if (a)
        adjustment = -(double)((PangoAttrInt*)a)->value / PANGO_SCALE *
                     (index == length ? 1 : .5);
      break;
    }
  } while (pango_attr_iterator_next(it));

  pango_attr_iterator_destroy(it);
  return pango_layout_get_direction(layout, (int)at) == PANGO_DIRECTION_RTL
             ? -adjustment
             : adjustment;
}

void rmr_pango_attributes(const rmr_linux_fonts* fonts, PangoAttrList* attrs,
                          rm_str text, size_t start, size_t end, double size,
                          rmr_type_face face, bool bold, bool italic) {
  int script = -1;
  for (size_t at = start; at < end;
       at = (size_t)(g_utf8_next_char(text.bytes + at) - text.bytes)) {
    gunichar c = g_utf8_get_char(text.bytes + at);
    if (strong(c)) {
      script = latin_character(c) ? -1 : character_script(c);
      break;
    }
  }

  /* Keep the previous layer stable while the face attributes are overlaid. */
  PangoAttrList* previous_attrs = pango_attr_list_copy(attrs);
  PangoAttrIterator* previous_it = pango_attr_list_get_iterator(previous_attrs);
  for (size_t at = start; at < end;) {
    gunichar c = g_utf8_get_char(text.bytes + at);
    size_t next = (size_t)(g_utf8_next_char(text.bytes + at) - text.bytes);
    if (strong(c)) script = latin_character(c) ? -1 : character_script(c);

    char alias[64];
    unsigned file = bold                                             ? 1
                    : face == RMR_FACE_MEDIUM                        ? 3
                    : face == RMR_FACE_SERIF && italic && size >= 64 ? 4
                                                                     : 0;
    GUnicodeType type = g_unichar_type(c);
    bool base = script >= 0 && type != G_UNICODE_NON_SPACING_MARK &&
                type != G_UNICODE_ENCLOSING_MARK &&
                type != G_UNICODE_SPACING_MARK && type != G_UNICODE_FORMAT &&
                FcCharSetHasChar(fonts->base_characters[file], c);
    snprintf(alias, sizeof alias, "RMR-%u-%d-%d-%d%s", (unsigned)face, bold,
             italic, script, base ? "-base" : "");

    PangoAttribute* family = pango_attr_family_new(alias);
    family->start_index = (guint)at;
    family->end_index = (guint)next;
    pango_attr_list_change(attrs, family);

    double kern = calibrated_kern(c, face, bold, italic, script) * size;
    /* Apple overlays face attributes on the regular paragraph. A zero
     * adjustment leaves an existing kern attribute in place. */
    int applied = (int)lrint(kern * PANGO_SCALE);
    if (!applied) {
      int from, to;
      pango_attr_iterator_range(previous_it, &from, &to);
      while (to >= 0 && (size_t)to <= at &&
             pango_attr_iterator_next(previous_it))
        pango_attr_iterator_range(previous_it, &from, &to);
      PangoAttribute* previous =
          pango_attr_iterator_get(previous_it, spacing_klass()->type);
      if (previous) applied = ((PangoAttrInt*)previous)->value;
    }

    PangoAttribute* spacing = spacing_new(applied);
    spacing->start_index = (guint)at;
    spacing->end_index = (guint)next;
    pango_attr_list_change(attrs, spacing);
    at = next;
  }

  pango_attr_iterator_destroy(previous_it);
  pango_attr_list_unref(previous_attrs);

  /* Keycaps without VS16 are not emoji-presenting to Pango. Select one
   * font for the complete sequence and discard text-digit calibration. */
  if (fonts->has_emoji)
    for (size_t at = start; at < end;) {
      gunichar c = g_utf8_get_char(text.bytes + at);
      size_t next = (size_t)(g_utf8_next_char(text.bytes + at) - text.bytes);
      if ((c >= '0' && c <= '9') || c == '#' || c == '*') {
        size_t tail = next;
        if (tail < end && g_utf8_get_char(text.bytes + tail) == 0xfe0f)
          tail = (size_t)(g_utf8_next_char(text.bytes + tail) - text.bytes);
        if (tail < end && g_utf8_get_char(text.bytes + tail) == 0x20e3) {
          next = (size_t)(g_utf8_next_char(text.bytes + tail) - text.bytes);
          PangoAttribute* family = pango_attr_family_new("Noto Emoji");
          family->start_index = (guint)at;
          family->end_index = (guint)next;
          pango_attr_list_change(attrs, family);
          PangoAttribute* spacing = spacing_new(0);
          spacing->start_index = (guint)at;
          spacing->end_index = (guint)next;
          pango_attr_list_change(attrs, spacing);
        }
      }
      at = next;
    }
}

static const char* const names[] = {"NotoSans-Regular",
                                    "NotoSans-Bold",
                                    "NotoSans-Italic",
                                    "NotoSans-Medium",
                                    "NotoSans-Light",
                                    "EBGaramond-VariableFont_wght",
                                    "EBGaramond-Italic-VariableFont_wght",
                                    "NotoSansHebrew-Variable",
                                    "NotoSansArabic-Variable",
                                    "NotoSansDevanagari-Variable",
                                    "NotoSansKR-Variable",
                                    "NotoSansJP-Variable",
                                    "NotoSansThaiLooped-Variable",
                                    "NotoEmoji-Variable"};

static_assert(sizeof names / sizeof *names ==
              sizeof((rmr_linux_fonts*)0)->fds /
                  sizeof((rmr_linux_fonts*)0)->fds[0]);

/* Releases fonts once its reference count reaches zero, shutting down the
 * font map and closing all font file descriptors. */
void rmr_linux_fonts_free(rmr_linux_fonts* f) {
  if (!f || atomic_fetch_sub(&f->references, 1) != 1) return;

  if (f->map) {
    pango_fc_font_map_shutdown(PANGO_FC_FONT_MAP(f->map));
    g_object_unref(f->map);
  }

  for (size_t i = 0; i < 5; i++)
    if (f->base_characters[i]) FcCharSetDestroy(f->base_characters[i]);

  if (f->config) FcConfigDestroy(f->config);

  for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
    if (f->fds[i] >= 0) g_close(f->fds[i], nullptr);

#ifndef __linux__
    if (f->paths[i]) {
#ifndef _WIN32
      /* Private temporary files here; Windows keeps its shared cache. There
       * is no caller to report to and the library logs nothing by default,
       * so a file left behind shows only with G_MESSAGES_DEBUG. */
      if (g_unlink(f->paths[i]) != 0 && errno != ENOENT)
        g_debug("rmrender: cannot remove font file %s: %s", f->paths[i],
                g_strerror(errno));
#endif
      g_free(f->paths[i]);
    }
#endif
  }

  free(f);
}

/* Builds a private Fontconfig/Pango font map from resolve's bundled fonts,
 * including per-face base-character sets used to pick calibration script. */
rmr_status rmr_linux_fonts_create(rmr_font_resolve_fn resolve, void* context,
                                  const rmr_limits* given,
                                  rmr_linux_fonts** out, rmr_error* e) {
  if (!resolve || !out)
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT,
                           "missing font resolver/output");

  rmr_limits l = given ? *given : rmr_limits_default();
  rmr_linux_fonts* f = calloc(1, sizeof *f);
  if (!f) return rmr_linux_error(e, RMR_ERR_OUT_OF_MEMORY, "font allocation");
  atomic_init(&f->references, 1);
  for (size_t i = 0; i < sizeof names / sizeof *names; i++) f->fds[i] = -1;

  /* Private configuration: do not initialize or scan the process-global
   * system font configuration. All matching uses this explicit config. */
  f->config = FcConfigCreate();
  rmr_status s = f->config ? RMR_OK : RMR_ERR_OUT_OF_MEMORY;

  /* Names the step that failed, so a failure seen only elsewhere, such as on
   * a Windows runner, can be told apart. */
  char failure[sizeof((rmr_error*)0)->message] =
      "font resources could not be loaded";
#ifdef _WIN32
  /* %LOCALAPPDATA%\rmrender\fonts, or under XDG_DATA_HOME when set. */
  char* cache = nullptr;
  if (s == RMR_OK) {
    char* directory =
        g_build_filename(g_get_user_data_dir(), "rmrender", "fonts", nullptr);
    cache = rmr_font_cache_directory(directory);
    g_free(directory);
    if (!cache) {
      s = RMR_ERR_BACKEND;
      snprintf(failure, sizeof failure, "%s",
               "font cache directory unavailable or without an ASCII path");
    }
  }
#endif

  size_t total = 0;
  for (size_t i = 0; i < sizeof names / sizeof *names && s == RMR_OK; i++) {
    rmr_asset_view v = {0};
    s = resolve(context, names[i], &v);
    /* Optional for existing resolvers and resource directories. Other
     * errors, including malformed or oversized fonts, remain failures. */
    if (i == 13 &&
        (s == RMR_ERR_UNSUPPORTED || (s == RMR_OK && !v.bytes && !v.size))) {
      s = RMR_OK;
      continue;
    }

    if (s != RMR_OK) break;
    if (!v.bytes || !v.size) {
      s = RMR_ERR_UNSUPPORTED;
      break;
    }
    if (v.size > l.maximum_plan_bytes - total || v.size > PTRDIFF_MAX) {
      s = RMR_ERR_LIMIT;
      break;
    }
    total += v.size;

#ifdef _WIN32
    /* Fontconfig reopens files lazily and FreeType keeps them mapped, so a
     * per-map file could never be deleted; reuse the content-addressed one. */
    GError* error = nullptr;
    f->paths[i] = rmr_font_cache_file(cache, v.bytes, v.size, &error);
    if (!f->paths[i]) {
      snprintf(failure, sizeof failure, "font %s: cache file: %s", names[i],
               error ? error->message : "unknown failure");
      g_clear_error(&error);
      s = RMR_ERR_BACKEND;
      break;
    }
#else
#ifdef __linux__
    f->fds[i] = memfd_create(names[i], MFD_CLOEXEC);
#else
    /* Fontconfig reopens these files lazily; keep the private file until map
     * shutdown. The descriptor is GLib's and this libc's alike. */
    f->fds[i] = g_file_open_tmp("rmrender-font-XXXXXX", &f->paths[i], nullptr);
#endif
    if (f->fds[i] < 0) {
      s = RMR_ERR_BACKEND;
      break;
    }

    size_t at = 0;
    while (at < v.size) {
      ssize_t n = write(f->fds[i], v.bytes + at, v.size - at);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) {
        s = RMR_ERR_BACKEND;
        break;
      }
      at += (size_t)n;
    }
    if (s != RMR_OK) break;
#ifndef __linux__
    /* Named files need only their paths to stay alive after the write. */
    if (!g_close(f->fds[i], nullptr)) s = RMR_ERR_BACKEND;
    f->fds[i] = -1;
    if (s != RMR_OK) break;
#endif
#endif

#ifdef __linux__
    char path[64];
    snprintf(path, sizeof path, "/proc/self/fd/%d", f->fds[i]);
#else
#ifdef _WIN32
    /* Fontconfig stores FC_FILE with forward slashes. Match that spelling
     * when identifying the face whose charset drives our calibration. */
    for (char* c = f->paths[i]; *c; c++)
      if (*c == '\\') *c = '/';
#endif
    const char* path = f->paths[i];
#endif

    if (!FcConfigAppFontAddFile(f->config, (const FcChar8*)path)) {
      s = RMR_ERR_BACKEND;
      /* Fontconfig gives no reason; whether the file opens is the usual one. */
      int fd = g_open(path, O_RDONLY, 0);
      int code = errno;
      if (fd >= 0) g_close(fd, nullptr);
      snprintf(failure, sizeof failure, "font %s: Fontconfig cannot add it%s%s",
               names[i], fd >= 0 ? "" : ": ", fd >= 0 ? "" : g_strerror(code));
    } else if (i < 5) {
      FcFontSet* set = FcConfigGetFonts(f->config, FcSetApplication);
      for (int j = 0; set && j < set->nfont; j++) {
        FcChar8* file = nullptr;
        FcCharSet* characters = nullptr;
        if (FcPatternGetString(set->fonts[j], FC_FILE, 0, &file) ==
                FcResultMatch &&
            !strcmp((const char*)file, path) &&
            FcPatternGetCharSet(set->fonts[j], FC_CHARSET, 0, &characters) ==
                FcResultMatch) {
          f->base_characters[i] = FcCharSetCopy(characters);
          break;
        }
      }
      if (!f->base_characters[i]) {
        snprintf(failure, sizeof failure,
                 "font %s: no Fontconfig face of that file with a charset",
                 names[i]);
        s = RMR_ERR_BACKEND;
      }
    }
  }

#ifdef __linux__
  f->has_emoji = f->fds[13] >= 0;
#else
  f->has_emoji = f->paths[13] != nullptr;
#endif
#ifdef _WIN32
  g_free(cache);
#endif
  if (s == RMR_OK) {
    f->map = pango_cairo_font_map_new_for_font_type(CAIRO_FONT_TYPE_FT);
    if (!f->map) {
      snprintf(failure, sizeof failure, "%s", "Pango font map unavailable");
      s = RMR_ERR_BACKEND;
    } else {
      pango_fc_font_map_set_config(PANGO_FC_FONT_MAP(f->map), f->config);
      pango_fc_font_map_set_default_substitute(PANGO_FC_FONT_MAP(f->map),
                                               calibrated_pattern, f, nullptr);
      pango_cairo_font_map_set_resolution(PANGO_CAIRO_FONT_MAP(f->map), 72);
    }
  }

  if (s != RMR_OK) {
    rmr_linux_fonts_free(f);
    return rmr_linux_error(e, s, failure);
  }

  *out = f;
  return RMR_OK;
}

#include "../../adapters/rmr_files.h"

typedef struct {
  const char* directory;
  rmr_buffer bytes;
  size_t maximum;
} directory_fonts;

/* Font resolver reading name as a .ttf file from ctx's directory into its
 * reusable buffer. */
static rmr_status resolve_directory(void* ctx, const char* name,
                                    rmr_asset_view* out) {
  directory_fonts* d = ctx;
  rmr_buffer_free(&d->bytes);
  rmr_status s = rmr_read_resource(d->directory, name, strlen(name), ".ttf",
                                   d->maximum, &d->bytes);

  if (s == RMR_OK) *out = (rmr_asset_view){d->bytes.bytes, d->bytes.size};
  return s;
}

rmr_status rmr_linux_fonts_create_directory(const char* directory,
                                            const rmr_limits* l,
                                            rmr_linux_fonts** out,
                                            rmr_error* e) {
  if (!directory)
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT,
                           "missing font directory");
  directory_fonts d = {.directory = directory,
                       .maximum = l ? l->maximum_plan_bytes
                                    : rmr_limits_default().maximum_plan_bytes};

  rmr_status s = rmr_linux_fonts_create(resolve_directory, &d, l, out, e);
  rmr_buffer_free(&d.bytes);
  return s;
}

PangoLayout* rmr_pango_layout(const rmr_linux_fonts* fonts, rm_str text,
                              double size, rmr_type_face face, bool bold,
                              bool italic) {
  PangoContext* context = pango_font_map_create_context(fonts->map);
  pango_context_set_round_glyph_positions(context, FALSE);
  cairo_font_options_t* options = cairo_font_options_create();
  cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
  cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_NONE);
  pango_cairo_context_set_font_options(context, options);
  cairo_font_options_destroy(options);
  PangoLayout* p = pango_layout_new(context);
  g_object_unref(context);

  PangoFontDescription* f = pango_font_description_new();
  pango_font_description_set_family(
      f,
      face == RMR_FACE_SERIF
          ? "EB Garamond,Noto Sans,Noto Sans Arabic,Noto Sans Hebrew,Noto Sans "
            "Devanagari,Noto Sans KR,Noto Sans JP,Noto Sans Thai Looped,Noto "
            "Emoji"
          : "Noto Sans,Noto Sans Arabic,Noto Sans Hebrew,Noto Sans "
            "Devanagari,Noto Sans "
            "KR,Noto Sans JP,Noto Sans Thai Looped,Noto Emoji");
  pango_font_description_set_absolute_size(f, size * PANGO_SCALE);
  pango_font_description_set_weight(f, bold ? PANGO_WEIGHT_BOLD
                                       : face == RMR_FACE_MEDIUM
                                           ? PANGO_WEIGHT_MEDIUM
                                           : PANGO_WEIGHT_NORMAL);
  pango_font_description_set_style(
      f, italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
  pango_layout_set_font_description(p, f);
  pango_font_description_free(f);
  pango_layout_set_text(p, text.bytes ? text.bytes : "", (int)text.length);

  PangoAttrList* attrs = pango_attr_list_new();
  pango_attr_list_insert(attrs, pango_attr_font_features_new("liga=1,clig=1"));
  rmr_pango_attributes(fonts, attrs, text, 0, text.length, size, face, bold,
                       italic);
  pango_layout_set_attributes(p, attrs);
  pango_attr_list_unref(attrs);

  return p;
}
