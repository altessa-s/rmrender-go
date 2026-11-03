/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "rmm_internal.h"
#include "rmm_unicode_data.h"

/* Invalid UTF-8 is refused at the boundary rather than repaired in silence.
 * The caller decides whether OCR bytes should be repaired before this API. */
size_t rmm_utf8_next(rmm_str s, size_t at, uint32_t* cp) {
  if (at >= s.length) return 0;
  const unsigned char* p = (const unsigned char*)s.bytes;
  uint32_t lead = p[at], value;
  size_t n;
  if (lead < 0x80) {
    *cp = lead;
    return at + 1;
  }
  if (lead >= 0xC2 && lead <= 0xDF) {
    n = 2;
    value = lead & 0x1F;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    n = 3;
    value = lead & 0x0F;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    n = 4;
    value = lead & 7;
  } else
    return 0;
  if (n > s.length - at) return 0;
  for (size_t i = 1; i < n; i += 1) {
    uint32_t byte = p[at + i];
    if ((byte & 0xC0) != 0x80) return 0;
    value = (value << 6) | (byte & 0x3F);
  }
  if ((n == 3 && value < 0x800) || (n == 4 && value < 0x10000) ||
      value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
    return 0;
  *cp = value;
  return at + n;
}

bool rmm_utf8_valid(rmm_str s) {
  if (s.length && !s.bytes) return false;
  for (size_t at = 0; at < s.length;) {
    uint32_t cp;
    at = rmm_utf8_next(s, at, &cp);
    if (!at) return false;
  }
  return true;
}

/* Value of the range in table (sorted, n entries) that contains cp, or 0
 * if cp falls in no range. */
static uint8_t property(uint32_t cp, const rmm_unicode_range* table, size_t n) {
  size_t lo = 0, hi = n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (cp < table[mid].first)
      hi = mid;
    else if (cp > table[mid].last)
      lo = mid + 1;
    else
      return table[mid].value;
  }
  return 0;
}

/* True if cp is a Unicode space, line or paragraph separator treated as
 * whitespace for trimming. */
static bool whitespace(uint32_t cp) {
  return (cp >= 9 && cp <= 13) || cp == 0x20 || cp == 0x85 || cp == 0xA0 ||
         cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 ||
         cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

rmm_str rmm_trim(rmm_str s) {
  size_t start = s.length, end = 0;
  bool blank = false, have_cluster = false;
  uint8_t previous = 0;
  for (size_t at = 0; at < s.length;) {
    uint32_t cp;
    size_t next = rmm_utf8_next(s, at, &cp);
    if (!next) return s; /* Validated by the public entry point. */
    uint8_t kind = property(cp, rmm_grapheme, rmm_grapheme_count);
    /* Only boundaries adjacent to whitespace matter here. Controls
     * break even before an Extend; a Prepend owns the following scalar.
     * A space followed by an accent/ZWJ is one whitespace Character. */
    bool joined = have_cluster && previous != 3 && kind != 3 &&
                  (kind == 1 || previous == 2);
    if (!joined) blank = whitespace(cp);
    if (!blank) {
      if (start == s.length) start = at;
      end = next;
    }
    previous = kind;
    have_cluster = true;
    at = next;
  }
  if (start == s.length) return (rmm_str){s.bytes, 0};
  return (rmm_str){s.bytes + start, end - start};
}

typedef struct {
  uint32_t cp;
  uint8_t ccc;
  size_t order;
} scalar;

/* qsort comparator for canonical ordering: by combining class, with ties
 * broken by original order to keep the sort stable. */
static int by_combining(const void* a, const void* b) {
  const scalar *x = a, *y = b;
  if (x->ccc != y->ccc) return (x->ccc > y->ccc) - (x->ccc < y->ccc);
  return (x->order > y->order) - (x->order < y->order);
}

/* The canonical decomposition entry for cp, or nullptr if cp has none. */
static const rmm_decomposition* decomposition(uint32_t cp) {
  size_t lo = 0, hi = rmm_decompositions_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (rmm_decompositions[mid].scalar < cp)
      lo = mid + 1;
    else if (rmm_decompositions[mid].scalar > cp)
      hi = mid;
    else
      return &rmm_decompositions[mid];
  }
  return nullptr;
}

/* Expands s into its canonical (NFD) decomposition, in canonical order,
 * computing Hangul syllable decompositions rather than looking them up.
 * Writes the scalar array to *out and its length to *count. */
static rmm_status nfd(rmm_arena* arena, rmm_str s, scalar** out,
                      size_t* count) {
  if (s.length > SIZE_MAX / 4) return RMM_ERR_OUT_OF_MEMORY;
  scalar* values = rmm_arena_calloc(arena, s.length * 4, sizeof *values);
  if (!values) return RMM_ERR_OUT_OF_MEMORY;
  size_t used = 0;
  for (size_t at = 0; at < s.length;) {
    uint32_t cp, parts[4];
    at = rmm_utf8_next(s, at, &cp);
    if (!at) return RMM_ERR_INVALID_UTF8;
    size_t n = 1;
    parts[0] = cp;
    if (cp >= 0xAC00 && cp <= 0xD7A3) {
      uint32_t index = cp - 0xAC00;
      parts[0] = 0x1100 + index / 588;
      parts[1] = 0x1161 + (index % 588) / 28;
      parts[2] = 0x11A7 + index % 28;
      n = index % 28 ? 3 : 2;
    } else {
      const rmm_decomposition* d = decomposition(cp);
      if (d) {
        memcpy(parts, d->parts, sizeof parts);
        n = d->count;
      }
    }
    for (size_t i = 0; i < n; i += 1) {
      values[used] = (scalar){
          parts[i], property(parts[i], rmm_combining, rmm_combining_count),
          used};
      used += 1;
    }
  }
  /* Canonical order with stable ties, without quadratic insertion over a
   * hostile string consisting of thousands of combining marks. */
  for (size_t at = 0; at < used;) {
    if (values[at].ccc == 0) {
      at += 1;
      continue;
    }
    size_t end = at + 1;
    while (end < used && values[end].ccc != 0) end += 1;
    qsort(values + at, end - at, sizeof *values, by_combining);
    at = end;
  }
  *out = values;
  *count = used;
  return RMM_OK;
}

rmm_status rmm_text_equal(rmm_arena* arena, rmm_str a, rmm_str b, bool* equal) {
  if (a.length == b.length &&
      (!a.length || memcmp(a.bytes, b.bytes, a.length) == 0)) {
    *equal = true;
    return RMM_OK;
  }
  scalar *x, *y;
  size_t nx, ny;
  rmm_status status = nfd(arena, a, &x, &nx);
  if (status != RMM_OK) return status;
  status = nfd(arena, b, &y, &ny);
  if (status != RMM_OK) return status;
  *equal = nx == ny;
  for (size_t i = 0; *equal && i < nx; i += 1) *equal = x[i].cp == y[i].cp;
  return RMM_OK;
}
