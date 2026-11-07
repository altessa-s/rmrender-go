/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <limits.h>

#include "../common/rmr_unreadable.h"
#include "rmr_alphabetic.h"
#include "rmr_apple_internal.h"
#include "rmr_typography_internal.h"

typedef struct {
  CTTypesetterRef typesetter;
  CTLineRef *lines, marker;
  rmr_type_metrics metrics;
  rm_checkbox_state checkbox;
  rm_list_family family;
} native_paragraph;

struct rmr_apple_layout {
  rmr_anchor_layout anchors;
  rmr_caret_provider caret;
  rmr_anchor_paragraph* paragraphs;
  native_paragraph* native;
  double bottom;
  size_t bytes, work;
};

typedef struct {
  const rmr_apple_fonts* source;
  CTFontRef fonts[4][3][2][2][8];
} font_cache;

/* Retained font for size/face/bold/italic/script, loading and caching it in
 * cache on first use. */
static CTFontRef cached_font(font_cache* cache, double size, rmr_type_face face,
                             bool bold, bool italic, int script) {
  size_t index = size == 32 ? 0 : size == 36 ? 1 : size == 46 ? 2 : 3;
  CTFontRef* slot = &cache->fonts[index][face][bold][italic][script + 1];
  if (!*slot)
    *slot = rmr_ct_font(cache->source, size, face, bold, italic, script);
  return *slot ? (CTFontRef)CFRetain(*slot) : nullptr;
}

/* Release every font cached_font retained into cache. */
static void clear_fonts(font_cache* cache) {
  for (size_t s = 0; s < 4; s++)
    for (size_t f = 0; f < 3; f++)
      for (size_t b = 0; b < 2; b++)
        for (size_t i = 0; i < 2; i++)
          for (size_t c = 0; c < 8; c++)
            if (cache->fonts[s][f][b][i][c])
              CFRelease(cache->fonts[s][f][b][i][c]);
}

/* Whether c lies in the inclusive range [a, b]. */
static bool between(uint32_t c, uint32_t a, uint32_t b) {
  return c >= a && c <= b;
}

/* Whether c is a basic, Latin-1 Supplement or Latin Extended letter. */
static bool latin(uint32_t c) {
  return between(c, 65, 90) || between(c, 97, 122) || between(c, 0xc0, 0x24f) ||
         between(c, 0x1e00, 0x1eff);
}

/* Non-Latin Noto script bucket for c (see cached_font), or 0 for none. */
static int script_of(uint32_t c) {
  if (between(c, 0x590, 0x5ff) || between(c, 0xfb1d, 0xfb4f)) return 1;
  if (between(c, 0x900, 0x97f)) return 2;
  if (between(c, 0x600, 0x6ff) || between(c, 0xfb50, 0xfeff)) return 3;
  if (between(c, 0x1100, 0x11ff) || between(c, 0x3130, 0x318f) ||
      between(c, 0xac00, 0xd7af))
    return 4;
  if (between(c, 0x3040, 0x30ff) || between(c, 0x3400, 0x4dbf) ||
      between(c, 0x4e00, 0x9fff))
    return 5;
  if (between(c, 0xe00, 0xe7f)) return 6;
  return 0;
}

/* Whether the first strongly-directional scalar in u is right-to-left. */
static bool rtl(const UniChar* u, size_t n) {
  for (size_t i = 0; i < n; i++) {
    uint32_t c = u[i];
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < n)
      c = 0x10000 + ((c - 0xd800) << 10) + (u[++i] - 0xdc00);
    if (between(c, 0x590, 0x7bf) || between(c, 0xfb1d, 0xfdff) ||
        between(c, 0xfe70, 0xfeff))
      return true;
    if (rmr_alphabetic(c)) return false;
  }
  return false;
}

/* UTF-16 offset of the count-th Unicode scalar in u, or SIZE_MAX if u has
 * fewer than count scalars. */
static size_t scalar_index(const UniChar* u, size_t n, size_t count) {
  size_t i = 0;
  while (i < n && count) {
    bool pair = u[i] >= 0xd800 && u[i] <= 0xdbff;
    i += pair ? 2 : 1;
    count--;
  }
  return count ? SIZE_MAX : i;
}

/* Whether c is a Hangul syllable or compatibility jamo. */
static bool hangul(UniChar c) {
  return between(c, 0xac00, 0xd7a3) || between(c, 0x1100, 0x11ff) ||
         between(c, 0x3130, 0x318f);
}

/* Apply one face range, then contextual Noto script runs. */
static rmr_status attributes(CFMutableAttributedStringRef text,
                             const UniChar* u, size_t start, size_t end,
                             rmr_type_metrics m, bool bold, bool italic,
                             font_cache* fonts) {
  if (start == end) return RMR_OK;

  CTFontRef base = cached_font(fonts, m.size, m.face, bold, italic, -1);
  if (!base) return RMR_ERR_OUT_OF_MEMORY;
  CFAttributedStringSetAttribute(
      text, CFRangeMake((CFIndex)start, (CFIndex)(end - start)),
      kCTFontAttributeName, base);
  CFRelease(base);

  /* Strong Noto/Latin classification works on UTF-16 units. */
  int first = -1;
  for (size_t i = start; i < end; i++)
    if (rmr_alphabetic(u[i]) || between(u[i], 0x3040, 0x30ff)) {
      first = latin(u[i]) ? 0 : 1;
      break;
    }

  int current = first;
  size_t at = start;

  while (at < end) {
    if (rmr_alphabetic(u[at]) || between(u[at], 0x3040, 0x30ff))
      current = latin(u[at]) ? 0 : 1;
    size_t stop = at + 1;
    for (; stop < end; stop++)
      if (rmr_alphabetic(u[stop]) || between(u[stop], 0x3040, 0x30ff)) {
        if ((latin(u[stop]) ? 0 : 1) != current) break;
      }

    if (current != 1) {
      for (size_t i = at; i < stop; i++) {
        double kern = rmr_ct_kern(u[i], m.face, bold, italic, -1) * m.size;
        if (kern == 0) continue;
        CFNumberRef value =
            CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &kern);
        if (!value) return RMR_ERR_OUT_OF_MEMORY;
        CFAttributedStringSetAttribute(text, CFRangeMake((CFIndex)i, 1),
                                       kCTKernAttributeName, value);
        CFRelease(value);
      }
    } else {
      int script = 0;
      for (size_t i = at; i < stop; i++)
        if (rmr_alphabetic(u[i])) {
          script = script_of(u[i]);
          break;
        }

      for (size_t from = at; from < stop;) {
        if (rmr_alphabetic(u[from])) script = script_of(u[from]);
        size_t to = from + 1;
        while (to < stop &&
               (!rmr_alphabetic(u[to]) || script_of(u[to]) == script))
          to++;

        CTFontRef font =
            cached_font(fonts, m.size, m.face, bold, italic, script);
        if (!font) return RMR_ERR_OUT_OF_MEMORY;

        CFAttributedStringSetAttribute(
            text, CFRangeMake((CFIndex)from, (CFIndex)(to - from)),
            kCTFontAttributeName, font);
        CFRelease(font);

        for (size_t i = from; i < to; i++) {
          double kern =
              rmr_ct_kern(u[i], m.face, bold, italic, script) * m.size;
          if (kern == 0) continue;
          CFNumberRef value =
              CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &kern);
          if (!value) return RMR_ERR_OUT_OF_MEMORY;
          CFAttributedStringSetAttribute(text, CFRangeMake((CFIndex)i, 1),
                                         kCTKernAttributeName, value);
          CFRelease(value);
        }
        from = to;
      }
    }

    at = stop;
  }

  return RMR_OK;
}

/* Paragraph text was validated on layout creation. Check UTF-16 scalar
 * boundaries without allocating a CFString for each anchor query. */
static bool scalar_boundary(rm_str text, size_t index) {
  size_t units = 0;
  for (size_t at = 0; at < text.length && units < index;) {
    unsigned char lead = (unsigned char)text.bytes[at];
    units += lead >= 0xf0 ? 2u : 1u;
    at += lead >= 0xf0 ? 4u : lead >= 0xe0 ? 3u : lead >= 0xc0 ? 2u : 1u;
  }
  return units == index;
}

/* rmr_caret_provider callback: caret x offsets for index within the given
 * paragraph/line, relative to the line's string start. */
static rmr_status caret(void* context, size_t paragraph, size_t line,
                        size_t index, rmr_caret_offsets* out) {
  rmr_apple_layout* l = context;
  if (!l || !out || paragraph >= l->anchors.count ||
      line >= l->paragraphs[paragraph].line_count || index > LONG_MAX)
    return RMR_ERR_INVALID_ARGUMENT;
  const rmr_anchor_paragraph* p = &l->paragraphs[paragraph];
  rmr_anchor_line range = p->lines[line];
  if (index < range.location || index - range.location > range.length ||
      !scalar_boundary(p->text, index))
    return RMR_ERR_INVALID_ARGUMENT;

  CTLineRef native = l->native[paragraph].lines[line];
  CFRange r = CTLineGetStringRange(native);
  *out = (rmr_caret_offsets){
      CTLineGetOffsetForStringIndex(native, (CFIndex)index, nullptr),
      CTLineGetOffsetForStringIndex(native, r.location, nullptr)};
  return RMR_OK;
}

/* Release layout and every paragraph, native line and font retain it holds.
 */
void rmr_apple_layout_free(rmr_apple_layout* l) {
  if (!l) return;

  for (size_t i = 0; i < l->anchors.count; i++) {
    native_paragraph* n = &l->native[i];
    rmr_anchor_paragraph* p = &l->paragraphs[i];

    if (n->typesetter) CFRelease(n->typesetter);
    if (n->marker) CFRelease(n->marker);

    for (size_t j = 0; j < p->line_count; j++)
      if (n->lines[j]) CFRelease(n->lines[j]);
    free(n->lines);

    /* These pointers refer to our owned allocations, exposed only as borrowed
     * views. */
    free((void*)(uintptr_t)p->text.bytes);
    free((void*)(uintptr_t)p->lines);
  }

  free(l->paragraphs);
  free(l->native);
  free(l);
}

/* Bytes layout counts against maximum_plan_bytes, or 0 for nullptr. */
size_t rmr_apple_layout_bytes(const rmr_apple_layout* l) {
  return l ? l->bytes : 0;
}

/* Borrowed public anchors view into layout, or nullptr for nullptr. */
const rmr_anchor_layout* rmr_apple_layout_anchors(const rmr_apple_layout* l) {
  return l ? &l->anchors : nullptr;
}

/* Baseline-following y below the last paragraph, if layout has any. */
bool rmr_apple_layout_bottom(const rmr_apple_layout* l, double* out) {
  if (!l || !l->anchors.count || !out) return false;
  *out = l->bottom;
  return true;
}

/* Build a list marker line (bullet, numeral, letter or roman numeral,
 * depending on family and level) for ordinal. */
static CTLineRef marker(font_cache* fonts, rmr_type_metrics m,
                        rm_list_family family, size_t ordinal, int level) {
  size_t label_size = ordinal / 1000 + 64;
  char* label = malloc(label_size);
  if (!label) return nullptr;

  if (family == RM_LIST_BULLET)
    snprintf(label, label_size, "%s",
             level == 1   ? "•"
             : level == 2 ? "◦"
                          : "–");
  else if (level == 1)
    snprintf(label, label_size, "%zu.", ordinal);
  else if (level == 2) {
    char reversed[32];
    size_t count = 0;
    do {
      ordinal--;
      reversed[count++] = (char)('a' + ordinal % 26);
      ordinal /= 26;
    } while (ordinal && count < sizeof reversed);
    for (size_t i = 0; i < count; i++) label[i] = reversed[count - i - 1];
    label[count] = '.';
    label[count + 1] = 0;
  } else {
    static const size_t values[] = {1000, 900, 500, 400, 100, 90, 50,
                                    40,   10,  9,   5,   4,   1};
    static const char* const letters[] = {
        "m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
    size_t at = 0;
    {
      for (size_t i = 0; i < 13; i++)
        while (ordinal >= values[i]) {
          size_t len = strlen(letters[i]);
          memcpy(label + at, letters[i], len);
          at += len;
          ordinal -= values[i];
        }
      label[at++] = '.';
      label[at] = 0;
    }
  }

  CFStringRef string = CFStringCreateWithCString(kCFAllocatorDefault, label,
                                                 kCFStringEncodingUTF8);
  free(label);

  CTFontRef font = cached_font(fonts, m.size, m.face, false, false, -1);
  CGColorRef black = CGColorCreateSRGB(0, 0, 0, 1);
  const void *keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName},
             *vals[] = {font, black};
  CFDictionaryRef attrs =
      font && black ? CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 2,
                                         &kCFTypeDictionaryKeyCallBacks,
                                         &kCFTypeDictionaryValueCallBacks)
                    : nullptr;
  CFAttributedStringRef a =
      attrs && string
          ? CFAttributedStringCreate(kCFAllocatorDefault, string, attrs)
          : nullptr;

  CTLineRef line = a ? CTLineCreateWithAttributedString(a) : nullptr;
  if (a) CFRelease(a);
  if (attrs) CFRelease(attrs);
  if (font) CFRelease(font);
  if (black) CGColorRelease(black);
  if (string) CFRelease(string);

  return line;
}

rmr_status rmr_apple_layout_create(const rm_text_block* text, double page_width,
                                   const rmr_apple_fonts* fonts,
                                   const rmr_limits* given,
                                   rmr_apple_layout** out, rmr_error* e) {
  rmr_limits limits = given ? *given : rmr_limits_default();
  if (!text || !fonts || !out || (!text->paragraphs && text->paragraph_count) ||
      !isfinite(page_width) || page_width <= 0 ||
      !isfinite(limits.maximum_coordinate) || limits.maximum_coordinate <= 0 ||
      limits.maximum_coordinate > 1e12 || !isfinite(text->origin_x) ||
      !isfinite(text->origin_y) ||
      fabs(text->origin_x) > limits.maximum_coordinate ||
      fabs(text->origin_y) > limits.maximum_coordinate)
    return rmr_apple_text_error(e, RMR_ERR_INVALID_ARGUMENT,
                                "invalid layout input");

  double column = text->has_column_width ? text->column_width
                  : text->origin_x < 0   ? -2 * text->origin_x
                                         : page_width;
  if (!isfinite(column) || column <= 0 || column > limits.maximum_coordinate)
    return rmr_apple_text_error(e, RMR_ERR_INVALID_ARGUMENT,
                                "invalid text column");

  size_t count = text->paragraph_count, base = sizeof(rmr_apple_layout);
  if (count > limits.maximum_elements || base > limits.maximum_plan_bytes ||
      count > (limits.maximum_plan_bytes - base) /
                  (sizeof(rmr_anchor_paragraph) + sizeof(native_paragraph)))
    return rmr_apple_text_error(e, RMR_ERR_LIMIT, "paragraph storage limit");

  rmr_apple_layout* l = calloc(1, sizeof *l);
  if (!l)
    return rmr_apple_text_error(e, RMR_ERR_OUT_OF_MEMORY,
                                "layout allocation failed");

  l->paragraphs = count ? calloc(count, sizeof *l->paragraphs) : nullptr;
  l->native = count ? calloc(count, sizeof *l->native) : nullptr;
  if (count && (!l->paragraphs || !l->native)) {
    free(l->paragraphs);
    free(l->native);
    free(l);
    return rmr_apple_text_error(e, RMR_ERR_OUT_OF_MEMORY,
                                "paragraph allocation failed");
  }

  l->bytes =
      base + count * (sizeof(rmr_anchor_paragraph) + sizeof(native_paragraph));
  l->work = count;
  l->caret = (rmr_caret_provider){RMR_CARET_PROVIDER_ABI,
                                  sizeof(rmr_caret_provider), l, caret};
  l->anchors = (rmr_anchor_layout){text->origin_x + 4, fmax(column - 8, 32),
                                   l->paragraphs, count, &l->caret};

  size_t ordinals[17] = {0};
  rm_list_family families[17];
  for (size_t i = 0; i < 17; i++) families[i] = RM_LIST_NONE;
  double baseline = text->origin_y;
  rmr_type_metrics previous = {0};
  rmr_status s = RMR_OK;
  font_cache cache = {.source = fonts};

  for (size_t i = 0; i < count; i++) {
    const rm_paragraph* input = &text->paragraphs[i];
    rmr_anchor_paragraph* p = &l->paragraphs[i];
    native_paragraph* n = &l->native[i];
    const rm_paragraph_format* format =
        input->has_format ? &input->format : nullptr;

    s = rmr_lines_type_metrics(format, input->text, &limits, &n->metrics, e);
    if (s != RMR_OK) break;

    size_t length = input->text.length;
    if (length > LONG_MAX || length > limits.maximum_elements - l->work ||
        length > (limits.maximum_plan_bytes - l->bytes) / 5 ||
        input->face_count > limits.maximum_elements - l->work - length) {
      s = RMR_ERR_LIMIT;
      break;
    }

    if (!input->faces && input->face_count) {
      s = RMR_ERR_INVALID_ARGUMENT;
      break;
    }

    l->work += length + input->face_count;
    if (length &&
        input->face_count > (limits.maximum_elements - l->work) / length) {
      s = RMR_ERR_LIMIT;
      break;
    }
    l->work += length * input->face_count;

    char* copy = length ? malloc(length) : nullptr;
    if (length && !copy) {
      s = RMR_ERR_OUT_OF_MEMORY;
      break;
    }

    if (length) memcpy(copy, input->text.bytes, length);
    p->text = (rm_str){copy, length};
    l->bytes += length;

    n->family =
        format
            ? rm_paragraph_style_list_family(rm_paragraph_format_style(format))
            : RM_LIST_NONE;
    int level = format ? rm_paragraph_format_level(format) : 1;
    if (level < 1 || level > 16) {
      s = RMR_ERR_LIMIT;
      break;
    }
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
    } else
      for (size_t k = 0; k < 17; k++) {
        ordinals[k] = 0;
        families[k] = RM_LIST_NONE;
      }

    baseline += i ? previous.descent + previous.after + n->metrics.before +
                        n->metrics.ascent
                  : n->metrics.first_baseline_drop;
    p->baseline = baseline;
    previous = n->metrics;

    n->checkbox = rm_text_block_checkbox_state(text, i);
    if (n->family == RM_LIST_BULLET || n->family == RM_LIST_NUMBERED) {
      n->marker = marker(&cache, n->metrics, n->family, ordinal, level);
      if (!n->marker) {
        s = RMR_ERR_OUT_OF_MEMORY;
        break;
      }
    }

    if (!length) {
      l->bottom = baseline + n->metrics.descent + 24;
      continue;
    }

    CFStringRef string =
        CFStringCreateWithBytes(kCFAllocatorDefault, (const UInt8*)copy,
                                (CFIndex)length, kCFStringEncodingUTF8, false);
    if (!string) {
      s = RMR_ERR_INVALID_ARGUMENT;
      break;
    }

    size_t units = (size_t)CFStringGetLength(string);
    UniChar* u = malloc(units * sizeof *u);
    char* part = malloc(length);
    CFMutableAttributedStringRef a =
        CFAttributedStringCreateMutable(kCFAllocatorDefault, 0);
    if (!u || !part || !a) {
      free(u);
      free(part);
      if (a) CFRelease(a);
      CFRelease(string);
      s = RMR_ERR_OUT_OF_MEMORY;
      break;
    }

    CFStringGetCharacters(string, CFRangeMake(0, (CFIndex)units), u);
    p->right_to_left = rtl(u, units);
    CFAttributedStringReplaceString(a, CFRangeMake(0, 0), string);

    CTWritingDirection direction = p->right_to_left
                                       ? kCTWritingDirectionRightToLeft
                                       : kCTWritingDirectionLeftToRight;
    CTParagraphStyleSetting setting = {
        kCTParagraphStyleSpecifierBaseWritingDirection, sizeof direction,
        &direction};
    CTParagraphStyleRef style = CTParagraphStyleCreate(&setting, 1);
    CGColorRef black = CGColorCreateSRGB(0, 0, 0, 1);
    if (!style || !black)
      s = RMR_ERR_OUT_OF_MEMORY;
    else {
      CFAttributedStringSetAttribute(a, CFRangeMake(0, (CFIndex)units),
                                     kCTParagraphStyleAttributeName, style);
      CFAttributedStringSetAttribute(a, CFRangeMake(0, (CFIndex)units),
                                     kCTForegroundColorAttributeName, black);
      s = attributes(a, u, 0, units, n->metrics, false, false, &cache);
    }
    if (style) CFRelease(style);
    if (black) CGColorRelease(black);

    for (size_t f = 0; s == RMR_OK && f < input->face_count; f++) {
      const rm_face_run* run = &input->faces[f];
      if (run->start < 0 || run->end < run->start) continue;
      size_t from = scalar_index(u, units, (size_t)run->start),
             to = scalar_index(u, units, (size_t)run->end);
      if (from == SIZE_MAX || to == SIZE_MAX) continue;
      s = attributes(a, u, from, to, n->metrics, run->bold, run->italic,
                     &cache);
    }

    if (s == RMR_OK) {
      n->typesetter = CTTypesetterCreateWithAttributedString(a);
      if (!n->typesetter) s = RMR_ERR_OUT_OF_MEMORY;
    }

    CFRelease(a);
    p->has_typesetter = n->typesetter != nullptr;

    size_t capacity = 0;
    rmr_anchor_line* lines = nullptr;
    rmr_type_metrics last = n->metrics;
    for (size_t start = 0; s == RMR_OK && start < units;) {
      CFIndex suggested = CTTypesetterSuggestLineBreak(
          n->typesetter, (CFIndex)start, l->anchors.column - p->indent);
      size_t taken = suggested > 0 ? (size_t)suggested : 1;
      if (taken > units - start) taken = units - start;
      if (start + taken < units && u[start + taken - 1] >= 0xd800 &&
          u[start + taken - 1] <= 0xdbff)
        taken++;
      if (start + taken < units && hangul(u[start + taken - 1]) &&
          hangul(u[start + taken])) {
        for (size_t k = start + taken - 1; k > start; k--)
          if (u[k] == 32) {
            taken = k - start + 1;
            break;
          }
      }

      CFRange range = CFRangeMake((CFIndex)start, (CFIndex)taken);
      CFIndex used = 0;
      CFIndex converted =
          CFStringGetBytes(string, range, kCFStringEncodingUTF8, 0, false,
                           (UInt8*)part, (CFIndex)length, &used);
      if (converted != (CFIndex)taken) {
        s = RMR_ERR_BACKEND;
        break;
      }
      s = rmr_lines_type_metrics(format, (rm_str){part, (size_t)used}, &limits,
                                 &last, e);
      if (s != RMR_OK) break;

      if (p->line_count) baseline += last.height;
      if (!isfinite(baseline) || fabs(baseline) > limits.maximum_coordinate) {
        s = RMR_ERR_LIMIT;
        break;
      }
      if (l->work >= limits.maximum_elements) {
        s = RMR_ERR_LIMIT;
        break;
      }

      l->work++;
      if (p->line_count == capacity) {
        size_t next = capacity ? capacity * 2 : 8;
        size_t growth = next - capacity;
        if (next < capacity ||
            growth > (limits.maximum_plan_bytes - l->bytes - length * 3) /
                         (sizeof *lines + sizeof *n->lines)) {
          s = RMR_ERR_LIMIT;
          break;
        }
        rmr_anchor_line* grown = realloc(lines, next * sizeof *lines);
        if (!grown) {
          s = RMR_ERR_OUT_OF_MEMORY;
          break;
        }
        lines = grown;
        p->lines = lines;
        CTLineRef* native = realloc(n->lines, next * sizeof *n->lines);
        if (!native) {
          s = RMR_ERR_OUT_OF_MEMORY;
          break;
        }
        n->lines = native;
        capacity = next;
        l->bytes += growth * (sizeof *lines + sizeof *n->lines);
      }

      CTLineRef line = CTTypesetterCreateLine(n->typesetter, range);
      if (!line) {
        s = RMR_ERR_OUT_OF_MEMORY;
        break;
      }
      lines[p->line_count] = (rmr_anchor_line){start, taken, baseline};
      n->lines[p->line_count++] = line;
      start += taken;
    }

    l->bottom = baseline + last.descent + 24;
    free(u);
    free(part);
    CFRelease(string);

    if (s != RMR_OK) break;
  }

  clear_fonts(&cache);
  if (s == RMR_OK &&
      (!isfinite(l->bottom) || fabs(l->bottom) > limits.maximum_coordinate ||
       fabs(l->anchors.left) > limits.maximum_coordinate))
    s = RMR_ERR_LIMIT;

  if (s != RMR_OK) {
    rmr_apple_layout_free(l);
    return rmr_apple_text_error(e, s, "paragraph layout failed");
  }

  *out = l;
  return RMR_OK;
}

typedef struct {
  rmr_buffer buffer;
  size_t capacity, maximum;
  rmr_status status;
} pdf_bytes;

/* CGDataConsumer callback: append bytes to the output buffer, enforcing its
 * configured maximum size. */
static size_t put_pdf(void* context, const void* bytes, size_t size) {
  pdf_bytes* p = context;
  if (p->status != RMR_OK) return 0;

  if (size > p->maximum - p->buffer.size) {
    p->status = RMR_ERR_LIMIT;
    return 0;
  }

  size_t need = p->buffer.size + size;
  if (need > p->capacity) {
    size_t next =
        p->capacity && p->capacity <= p->maximum / 2 ? p->capacity * 2 : need;
    if (next < need) next = need;
    uint8_t* data = realloc(p->buffer.bytes, next);
    if (!data) {
      p->status = RMR_ERR_OUT_OF_MEMORY;
      return 0;
    }
    p->buffer.bytes = data;
    p->capacity = next;
  }

  memcpy(p->buffer.bytes + p->buffer.size, bytes, size);
  p->buffer.size = need;
  return size;
}

/* Draw a checkbox glyph at (x, y), filled and ticked when state requires. */
static void checkbox(CGContextRef cg, rm_checkbox_state state, double x,
                     double y) {
  CGRect box = CGRectMake(x, y - 1.74, 26.67, 26.67);

  CGContextSaveGState(cg);
  CGContextSetRGBFillColor(cg, 0, 0, 0, 1);
  CGContextSetRGBStrokeColor(cg, 0, 0, 0, 1);
  CGContextSetLineWidth(cg, 2.5);

  if (state == RM_CHECKBOX_TICKED) {
    CGContextFillRect(cg, box);
    CGContextSetRGBStrokeColor(cg, 1, 1, 1, 1);
    CGContextMoveToPoint(cg, x + 26.67 * .22, box.origin.y + 26.67 * .50);
    CGContextAddLineToPoint(cg, x + 26.67 * .48, box.origin.y + 26.67 * .26);
    CGContextAddLineToPoint(cg, x + 26.67 * .82, box.origin.y + 26.67 * .76);
    CGContextStrokePath(cg);
  } else {
    CGContextStrokeRect(cg, box);
    if (state == RM_CHECKBOX_PARTIAL) {
      CGContextMoveToPoint(cg, x + 26.67 * .25, CGRectGetMidY(box));
      CGContextAddLineToPoint(cg, x + 26.67 * .75, CGRectGetMidY(box));
      CGContextStrokePath(cg);
    }
  }

  CGContextRestoreGState(cg);
}

typedef struct {
  rmr_draw* draws;
  rmr_path* paths;
  size_t count, capacity, elements;
  rmr_rect viewport;
  rmr_limits limits;
  rmr_status status;
} outline_recorder;

/* Appends a command with an empty owned path, returning the path, or
 * nullptr once the recorder has failed. */
static rmr_path* outline_add(outline_recorder* o, rmr_draw draw) {
  if (o->status != RMR_OK) return nullptr;

  if (o->count == o->limits.maximum_draws) {
    o->status = RMR_ERR_LIMIT;
    return nullptr;
  }

  if (o->count == o->capacity) {
    size_t capacity = o->capacity ? o->capacity * 2 : 16;
    rmr_draw* draws = realloc(o->draws, capacity * sizeof *draws);
    if (draws) o->draws = draws;
    rmr_path* paths =
        draws ? realloc(o->paths, capacity * sizeof *paths) : nullptr;
    if (!paths) {
      o->status = RMR_ERR_OUT_OF_MEMORY;
      return nullptr;
    }
    o->paths = paths;
    o->capacity = capacity;
  }

  o->draws[o->count] = draw;
  o->paths[o->count] = (rmr_path){0};
  return &o->paths[o->count++];
}

/* Maps a text-PDF point (viewport origin, y up) to page coordinates. */
static rmr_point outline_point(const outline_recorder* o, double x, double y) {
  return (rmr_point){x + o->viewport.x, o->viewport.y + o->viewport.height - y};
}

/* Records line's glyph outlines at text position (x, y) as a black fill. */
static void outline_line(outline_recorder* o, CTLineRef line, double x,
                         double y) {
  rmr_path* path =
      outline_add(o, (rmr_draw){.mode = RMR_FILL, .color = {0, 0, 0, 1}});
  if (!path) return;

  rmr_limits limits = o->limits;
  limits.maximum_elements -= o->elements;
  rmr_point origin = outline_point(o, x, y);
  o->status = rmr_apple_line_outline(
      line, CGAffineTransformMake(1, 0, 0, -1, origin.x, origin.y), &limits,
      path);
  o->elements += path->count;
}

/* Records a polyline through count text-PDF points as a fill, or as a stroke
 * of width with butt caps and miter joins like CoreGraphics' defaults. */
static void outline_polyline(outline_recorder* o, const CGPoint* points,
                             size_t count, bool closed, rmr_draw_mode mode,
                             double gray, double width) {
  rmr_path* path = outline_add(o, (rmr_draw){.mode = mode,
                                             .color = {gray, gray, gray, 1},
                                             .line_width = width,
                                             .cap = RMR_CAP_BUTT,
                                             .join = RMR_JOIN_MITER});
  if (!path) return;

  if (count + 1 > o->limits.maximum_elements - o->elements) {
    o->status = RMR_ERR_LIMIT;
    return;
  }

  path->elements = calloc(count + 1, sizeof *path->elements);
  if (!path->elements) {
    o->status = RMR_ERR_OUT_OF_MEMORY;
    return;
  }

  for (size_t i = 0; i < count; i++)
    path->elements[path->count++] =
        (rmr_element){.kind = i ? RMR_LINE : RMR_MOVE,
                      .end = outline_point(o, points[i].x, points[i].y)};
  if (closed) path->elements[path->count++] = (rmr_element){.kind = RMR_CLOSE};
  o->elements += path->count;
}

/* Records the checkbox glyph at (x, y) like checkbox() draws it. */
static void outline_checkbox(outline_recorder* o, rm_checkbox_state state,
                             double x, double y) {
  double side = 26.67, bottom = y - 1.74;
  CGPoint box[] = {{x, bottom},
                   {x + side, bottom},
                   {x + side, bottom + side},
                   {x, bottom + side}};

  if (state == RM_CHECKBOX_TICKED) {
    CGPoint tick[] = {{x + side * .22, bottom + side * .50},
                      {x + side * .48, bottom + side * .26},
                      {x + side * .82, bottom + side * .76}};
    outline_polyline(o, box, 4, true, RMR_FILL, 0, 0);
    outline_polyline(o, tick, 3, false, RMR_STROKE, 1, 2.5);
  } else {
    outline_polyline(o, box, 4, true, RMR_STROKE, 0, 2.5);
    if (state == RM_CHECKBOX_PARTIAL) {
      CGPoint bar[] = {{x + side * .25, bottom + side / 2},
                       {x + side * .75, bottom + side / 2}};
      outline_polyline(o, bar, 2, false, RMR_STROKE, 0, 2.5);
    }
  }
}

/* Draws every paragraph marker, checkbox, line and strike rule in text-PDF
 * coordinates (viewport origin, y up) into cg or, when o is given, records
 * the same geometry as page-space outline commands. */
static void text_layer(const rmr_apple_layout* l, rmr_rect viewport,
                       CGContextRef cg, outline_recorder* o) {
  for (size_t i = 0; i < l->anchors.count; i++) {
    const rmr_anchor_paragraph* p = &l->paragraphs[i];
    const native_paragraph* n = &l->native[i];
    double x = l->anchors.left + p->indent - viewport.x;
    double y = viewport.height - (p->baseline - viewport.y);

    if (n->marker) {
      double marker_x = n->family == RM_LIST_BULLET
                            ? x - 28
                            : x - 21.5 -
                                  CTLineGetTypographicBounds(n->marker, nullptr,
                                                             nullptr, nullptr);
      if (o)
        outline_line(o, n->marker, marker_x, y);
      else {
        CGContextSetTextPosition(cg, marker_x, y);
        CTLineDraw(n->marker, cg);
      }
    } else if (n->family == RM_LIST_CHECKBOX) {
      if (o)
        outline_checkbox(o, n->checkbox, x - 41.33, y);
      else
        checkbox(cg, n->checkbox, x - 41.33, y);
    }

    for (size_t j = 0; j < p->line_count; j++) {
      CTLineRef line = n->lines[j];
      double margin =
          p->right_to_left
              ? l->anchors.left + l->anchors.column - p->indent -
                    CTLineGetOffsetForStringIndex(
                        line, (CFIndex)p->lines[j].location, nullptr)
              : l->anchors.left + p->indent;
      x = margin - viewport.x;
      y = viewport.height - (p->lines[j].baseline - viewport.y);

      if (o)
        outline_line(o, line, x, y);
      else {
        CGContextSetTextPosition(cg, x, y);
        CTLineDraw(line, cg);
      }

      if (n->checkbox == RM_CHECKBOX_TICKED) {
        double rule = y + n->metrics.size * .3;
        double width = fmax(n->metrics.size * .065, .6);
        double end =
            x + CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
        if (o) {
          CGPoint points[] = {{x, rule}, {end, rule}};
          outline_polyline(o, points, 2, false, RMR_STROKE, 0, width);
        } else {
          CGContextSetLineWidth(cg, width);
          CGContextSetRGBStrokeColor(cg, 0, 0, 0, 1);
          CGContextMoveToPoint(cg, x, rule);
          CGContextAddLineToPoint(cg, end, rule);
          CGContextStrokePath(cg);
        }
      }
    }
  }
}

rmr_status rmr_apple_layout_outline_plan(const rmr_apple_layout* l,
                                         rmr_rect viewport,
                                         const rmr_limits* given,
                                         rmr_plan** out, rmr_error* e) {
  if (!l || !out)
    return rmr_apple_text_error(e, RMR_ERR_INVALID_ARGUMENT,
                                "missing layout/output");

  outline_recorder o = {.viewport = viewport,
                        .limits = given ? *given : rmr_limits_default()};
  rmr_plan* validation = nullptr;
  rmr_status s =
      rmr_plan_create(nullptr, 0, viewport, &o.limits, &validation, e);
  if (s != RMR_OK) return s;
  rmr_plan_free(validation);

  text_layer(l, viewport, nullptr, &o);

  /* Whitespace-only lines have no outlines; drop their empty fills. */
  size_t kept = 0;
  for (size_t i = 0; i < o.count; i++) {
    o.draws[i].path = (rmr_path_view){o.paths[i].elements, o.paths[i].count};
    if (o.paths[i].count) {
      o.draws[kept] = o.draws[i];
      kept++;
    }
  }

  s = o.status;
  if (s == RMR_OK)
    s = rmr_plan_create(o.draws, kept, viewport, &o.limits, out, e);
  else
    rmr_apple_text_error(e, s, "text outline failed");

  for (size_t i = 0; i < o.count; i++) rmr_path_free(&o.paths[i]);
  free(o.paths);
  free(o.draws);
  return s;
}

rmr_status rmr_apple_layout_plan(const rmr_apple_layout* l, rmr_rect viewport,
                                 const rmr_limits* given, rmr_plan** out,
                                 rmr_error* e) {
  if (!l || !out)
    return rmr_apple_text_error(e, RMR_ERR_INVALID_ARGUMENT,
                                "missing layout/output");

  rmr_limits limits = given ? *given : rmr_limits_default();
  rmr_plan* validation = nullptr;
  rmr_status s = rmr_plan_create(nullptr, 0, viewport, &limits, &validation, e);
  if (s != RMR_OK) return s;

  rmr_plan_free(validation);
  if (!l->anchors.count)
    return rmr_plan_create(nullptr, 0, viewport, &limits, out, e);

  pdf_bytes data = {.maximum = limits.maximum_pdf_bytes};
  if (data.maximum > limits.maximum_plan_bytes / 2)
    data.maximum = limits.maximum_plan_bytes / 2;

  CGDataConsumerCallbacks callbacks = {put_pdf, nullptr};
  CGDataConsumerRef consumer = CGDataConsumerCreate(&data, &callbacks);
  if (!consumer)
    return rmr_apple_text_error(e, RMR_ERR_OUT_OF_MEMORY,
                                "PDF consumer allocation failed");

  CGRect box = CGRectMake(0, 0, viewport.width, viewport.height);
  CGContextRef cg = CGPDFContextCreate(consumer, &box, nullptr);
  CGDataConsumerRelease(consumer);
  if (!cg) {
    free(data.buffer.bytes);
    return rmr_apple_text_error(e, RMR_ERR_BACKEND, "PDF context failed");
  }

  CGPDFContextBeginPage(cg, nullptr);
  CGContextSetTextMatrix(cg, CGAffineTransformIdentity);
  text_layer(l, viewport, cg, nullptr);
  CGPDFContextEndPage(cg);
  CGPDFContextClose(cg);
  CGContextRelease(cg);

  if (data.status != RMR_OK) {
    free(data.buffer.bytes);
    return rmr_apple_text_error(e, data.status,
                                "text PDF exceeds output budget");
  }

  rmr_element clip[] = {
      {.kind = RMR_MOVE, .end = {viewport.x, viewport.y}},
      {.kind = RMR_LINE, .end = {viewport.x + viewport.width, viewport.y}},
      {.kind = RMR_LINE,
       .end = {viewport.x + viewport.width, viewport.y + viewport.height}},
      {.kind = RMR_LINE, .end = {viewport.x, viewport.y + viewport.height}},
      {.kind = RMR_CLOSE}};

  rmr_draw draw = {.mode = RMR_PDF,
                   .path = {clip, 5},
                   .color = {0, 0, 0, 1},
                   .image = {data.buffer.bytes,
                             data.buffer.size,
                             {viewport.width, 0, 0, viewport.height, viewport.x,
                              viewport.y}}};

  rmr_limits remaining = limits;
  remaining.maximum_plan_bytes -= data.capacity;
  s = rmr_plan_create(&draw, 1, viewport, &remaining, out, e);
  free(data.buffer.bytes);
  return s;
}

/* Paragraph-relative string range and pixel span of one visual line. */
rmr_status rmr_apple_layout_line(const rmr_apple_layout* l, size_t paragraph,
                                 size_t index, rmr_text_line* out,
                                 rmr_error* e) {
  if (!l || !out || paragraph >= l->anchors.count ||
      index >= l->paragraphs[paragraph].line_count)
    return rmr_apple_text_error(e, RMR_ERR_INVALID_ARGUMENT,
                                "invalid text line index");

  const rmr_anchor_paragraph* p = &l->paragraphs[paragraph];
  rmr_anchor_line visual = p->lines[index];
  CTLineRef line = l->native[paragraph].lines[index];

  double start = p->right_to_left
                     ? l->anchors.left + l->anchors.column - p->indent -
                           CTLineGetOffsetForStringIndex(
                               line, (CFIndex)visual.location, nullptr)
                     : l->anchors.left + p->indent;

  *out = (rmr_text_line){
      paragraph,
      visual.location,
      visual.location + visual.length,
      visual.baseline,
      start,
      start + CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr)};
  return RMR_OK;
}

/* Measured width of text, optionally excluding the trailing glyph's
 * advance beyond its ink (less_rightmost). */
rmr_status rmr_apple_text_width(rm_str text, double size, rmr_type_face face,
                                bool bold, bool italic, bool less_rightmost,
                                const rmr_apple_fonts* fonts,
                                const rmr_limits* given, double* out,
                                rmr_error* e) {
  rmr_limits l = given ? *given : rmr_limits_default();
  if (!fonts || !out || (!text.bytes && text.length) || !isfinite(size) ||
      size <= 0 || size > 100000 || face > RMR_FACE_SERIF)
    return rmr_apple_text_error(e, RMR_ERR_INVALID_ARGUMENT,
                                "invalid text measurement");

  if (text.length > l.maximum_elements || text.length > LONG_MAX ||
      text.length > l.maximum_plan_bytes / sizeof(UniChar))
    return rmr_apple_text_error(e, RMR_ERR_LIMIT, "text measurement limit");

  CFStringRef string = CFStringCreateWithBytes(
      kCFAllocatorDefault, (const UInt8*)(text.bytes ? text.bytes : ""),
      (CFIndex)text.length, kCFStringEncodingUTF8, false);
  if (!string)
    return rmr_apple_text_error(e, RMR_ERR_INVALID_ARGUMENT,
                                "invalid text UTF-8");

  CFIndex count = CFStringGetLength(string);
  UniChar* units = malloc((size_t)(count ? count : 1) * sizeof *units);
  CFMutableAttributedStringRef attributed =
      CFAttributedStringCreateMutable(kCFAllocatorDefault, 0);

  if (!units || !attributed) {
    free(units);
    if (attributed) CFRelease(attributed);
    CFRelease(string);
    return rmr_apple_text_error(e, RMR_ERR_OUT_OF_MEMORY,
                                "text allocation failed");
  }

  CFStringGetCharacters(string, CFRangeMake(0, count), units);
  CFAttributedStringReplaceString(attributed, CFRangeMake(0, 0), string);
  CFRelease(string);

  font_cache cache = {.source = fonts};
  rmr_status status = attributes(attributed, units, 0, (size_t)count,
                                 (rmr_type_metrics){.size = size, .face = face},
                                 bold, italic, &cache);

  CTLineRef line =
      status == RMR_OK ? CTLineCreateWithAttributedString(attributed) : nullptr;
  CFRelease(attributed);
  clear_fonts(&cache);
  if (status == RMR_OK && !line) status = RMR_ERR_OUT_OF_MEMORY;

  double width = 0;
  if (line) {
    width = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);

    if (less_rightmost) {
      CFArrayRef runs = CTLineGetGlyphRuns(line);
      double farthest = 0, advance = 0;
      size_t work = 0;
      for (CFIndex i = 0; i < CFArrayGetCount(runs); i++) {
        CTRunRef run = CFArrayGetValueAtIndex(runs, i);
        CFIndex n = CTRunGetGlyphCount(run);
        if ((size_t)n > l.maximum_elements - work) {
          status = RMR_ERR_LIMIT;
          break;
        }
        work += (size_t)n;
        for (CFIndex j = 0; j < n; j++) {
          CGPoint position;
          CGSize step;
          CFIndex index;
          CTRunGetPositions(run, CFRangeMake(j, 1), &position);
          CTRunGetAdvances(run, CFRangeMake(j, 1), &step);
          CTRunGetStringIndices(run, CFRangeMake(j, 1), &index);
          if (step.width <= 0 ||
              (index >= 0 && index < count && rmr_nonspacing(units[index])))
            continue;
          double end = position.x + step.width;
          if (end > farthest) {
            farthest = end;
            advance = step.width;
          }
        }
      }
      width -= advance;
    }

    CFRelease(line);
  }

  free(units);

  if (status != RMR_OK)
    return rmr_apple_text_error(e, status, "text measurement failed");

  *out = width;
  return RMR_OK;
}

rmr_status rmr_apple_unreadable_plan(rmr_rect page, rmr_rect viewport,
                                     const rmr_apple_fonts* fonts,
                                     bool outlines, const rmr_limits* given,
                                     rmr_plan** out, rmr_error* e) {
  if (!out)
    return rmr_apple_text_error(e, RMR_ERR_INVALID_ARGUMENT,
                                "missing label output");
  rmr_limits limits = given ? *given : rmr_limits_default();
  double size = rmr_unreadable_size(page);
  if (!(size > 0) || !isfinite(size))
    return rmr_plan_create(nullptr, 0, viewport, &limits, out, e);
  if (!fonts)
    return rmr_apple_text_error(e, RMR_ERR_UNSUPPORTED,
                                "document font provider required");

  CTFontRef font = rmr_ct_font(fonts, size, RMR_FACE_SANS, false, false, -1);
  if (!font)
    return rmr_apple_text_error(e, RMR_ERR_UNSUPPORTED,
                                "label font unavailable");
  const void* keys[] = {kCTFontAttributeName,
                        kCTForegroundColorFromContextAttributeName};
  const void* values[] = {font, kCFBooleanTrue};
  CFDictionaryRef style = CFDictionaryCreate(kCFAllocatorDefault, keys, values,
                                             2, &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
  CFRelease(font);
  CFAttributedStringRef text =
      style ? CFAttributedStringCreate(kCFAllocatorDefault,
                                       CFSTR(RMR_UNREADABLE_LABEL), style)
            : nullptr;
  if (style) CFRelease(style);
  CTLineRef line = text ? CTLineCreateWithAttributedString(text) : nullptr;
  if (text) CFRelease(text);
  if (!line)
    return rmr_apple_text_error(e, RMR_ERR_OUT_OF_MEMORY,
                                "label allocation failed");

  CGFloat ascent = 0, descent = 0;
  double width = CTLineGetTypographicBounds(line, &ascent, &descent, nullptr);
  rmr_unreadable_layout place =
      rmr_unreadable_place(page, size, width, ascent, descent);
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
  pdf_bytes data = {.maximum = limits.maximum_pdf_bytes};
  if (data.maximum > limits.maximum_plan_bytes / 2)
    data.maximum = limits.maximum_plan_bytes / 2;
  rmr_status s = RMR_OK;

  if (outlines) {
    /* Line space has its baseline origin at zero and y up. */
    s = rmr_apple_line_outline(
        line,
        CGAffineTransformMake(1, 0, 0, -1, place.origin.x, place.origin.y),
        &limits, &glyphs);
    draws[1] = (rmr_draw){.mode = RMR_FILL,
                          .path = {glyphs.elements, glyphs.count},
                          .color = {RMR_UNREADABLE_GREY, RMR_UNREADABLE_GREY,
                                    RMR_UNREADABLE_GREY, 1}};
  } else {
    /* The text alone as a PDF of the box, imported as typed text is, so the
     * output embeds its face the same way. */
    CGDataConsumerCallbacks callbacks = {put_pdf, nullptr};
    CGDataConsumerRef consumer = CGDataConsumerCreate(&data, &callbacks);
    CGRect media = CGRectMake(0, 0, box.width, box.height);
    CGContextRef cg =
        consumer ? CGPDFContextCreate(consumer, &media, nullptr) : nullptr;
    if (consumer) CGDataConsumerRelease(consumer);
    if (!cg) {
      s = RMR_ERR_BACKEND;
    } else {
      CGPDFContextBeginPage(cg, nullptr);
      CGContextSetTextMatrix(cg, CGAffineTransformIdentity);
      CGContextSetGrayFillColor(cg, RMR_UNREADABLE_GREY, 1);
      CGContextSetTextPosition(cg, place.origin.x - box.x,
                               box.y + box.height - place.origin.y);
      CTLineDraw(line, cg);
      CGPDFContextEndPage(cg);
      CGPDFContextClose(cg);
      CGContextRelease(cg);
      s = data.status;
    }
    draws[1] =
        (rmr_draw){.mode = RMR_PDF,
                   .path = {clip, 5},
                   .color = {0, 0, 0, 1},
                   .image = {data.buffer.bytes,
                             data.buffer.size,
                             {box.width, 0, 0, box.height, box.x, box.y}}};
  }
  CFRelease(line);

  if (s == RMR_OK) {
    rmr_limits remaining = limits;
    remaining.maximum_plan_bytes -= data.capacity;
    s = rmr_plan_create(draws, 2, viewport, &remaining, out, e);
  } else {
    rmr_apple_text_error(e, s, "label drawing failed");
  }
  rmr_path_free(&glyphs);
  free(data.buffer.bytes);
  return s;
}
