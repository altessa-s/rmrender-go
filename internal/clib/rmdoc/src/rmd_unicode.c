/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "rmd_internal.h"
#include "rmd_unicode_data.h"

/* ZIP entry names are always decoded as UTF-8, and invalid UTF-8 is refused
 * at the boundary. */
size_t rmd_utf8_next(rmd_str s, size_t at, uint32_t* cp) {
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

bool rmd_utf8_valid(rmd_str s) {
  if (s.length && !s.bytes) return false;
  for (size_t at = 0; at < s.length;) {
    uint32_t cp;
    at = rmd_utf8_next(s, at, &cp);
    if (!at) return false;
  }
  return true;
}

/* Binary-searches the sorted range table for cp; returns the matching
 * value, or 0 if cp is in no range. */
static uint8_t property(uint32_t cp, const rmd_unicode_range* table, size_t n) {
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

typedef struct {
  uint32_t cp;
  uint8_t ccc;
  size_t order;
} scalar;

/* qsort comparator for canonical ordering: by combining class, ties
 * broken by original position for stability. */
static int by_combining(const void* a, const void* b) {
  const scalar *x = a, *y = b;
  if (x->ccc != y->ccc) return (x->ccc > y->ccc) - (x->ccc < y->ccc);
  return (x->order > y->order) - (x->order < y->order);
}

/* Binary-searches the sorted decomposition table for cp; NULL if cp has
 * no canonical decomposition. */
static const rmd_decomposition* decomposition(uint32_t cp) {
  size_t lo = 0, hi = rmd_decompositions_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (rmd_decompositions[mid].scalar < cp)
      lo = mid + 1;
    else if (rmd_decompositions[mid].scalar > cp)
      hi = mid;
    else
      return &rmd_decompositions[mid];
  }
  return nullptr;
}

/* Canonically decomposes s (Hangul algorithmically, others via the
 * decomposition table) into out and count, then canonically reorders runs
 * of combining marks. */
static rmd_status nfd(rmd_str s, scalar** out, size_t* count) {
  if (s.length > SIZE_MAX / 4) return RMD_ERR_OUT_OF_MEMORY;
  scalar* values = rmd_calloc(s.length * 4, sizeof *values);
  if (!values) return RMD_ERR_OUT_OF_MEMORY;
  size_t used = 0;
  for (size_t at = 0; at < s.length;) {
    uint32_t cp, parts[4];
    at = rmd_utf8_next(s, at, &cp);
    if (!at) {
      free(values);
      return RMD_ERR_INVALID_NAME;
    }
    size_t n = 1;
    parts[0] = cp;
    if (cp >= 0xAC00 && cp <= 0xD7A3) {
      uint32_t index = cp - 0xAC00;
      parts[0] = 0x1100 + index / 588;
      parts[1] = 0x1161 + (index % 588) / 28;
      parts[2] = 0x11A7 + index % 28;
      n = index % 28 ? 3 : 2;
    } else {
      const rmd_decomposition* d = decomposition(cp);
      if (d) {
        memcpy(parts, d->parts, sizeof parts);
        n = d->count;
      }
    }
    for (size_t i = 0; i < n; i += 1) {
      values[used] = (scalar){
          parts[i], property(parts[i], rmd_combining, rmd_combining_count),
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
  return RMD_OK;
}

rmd_status rmd_validate_name(rmd_str s, rmd_layout layout, rmd_error* e) {
  if (layout != RMD_DEVICE && layout != RMD_PACKAGE)
    return RMD_FAIL(e, RMD_ERR_INVALID_ARGUMENT, "unknown layout");
  bool valid = s.bytes && s.length && s.length <= UINT16_MAX;
  size_t components = 0, start = 0;
  if (valid)
    for (size_t at = 0; at <= s.length; at++) {
      if (at != s.length && s.bytes[at] != '/') continue;
      size_t n = at - start;
      if (!n || (layout == RMD_DEVICE && s.bytes[start] == '.') ||
          (layout == RMD_PACKAGE && n == 2 && s.bytes[start] == '.' &&
           s.bytes[start + 1] == '.'))
        valid = false;
      start = at + 1;
      components++;
    }
  if (layout == RMD_DEVICE && components > 2) valid = false;
  if (valid)
    for (size_t at = 0; at < s.length;) {
      uint32_t cp;
      at = rmd_utf8_next(s, at, &cp);
      if (!at || cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) || cp == '\\' ||
          property(cp, rmd_format, rmd_format_count)) {
        valid = false;
        break;
      }
    }
  if (!valid)
    return rmd_fail(e, RMD_ERR_INVALID_NAME, s, 0, 0,
                    "unsafe or invalid UTF-8 name");
  return RMD_OK;
}

rmd_str rmd_document_id(rmd_str s) {
  if (!s.bytes) return (rmd_str){0};
  size_t dot = s.length;
  for (size_t i = 0; i < s.length; i++) {
    if (s.bytes[i] == '/') return (rmd_str){s.bytes, i};
    if (s.bytes[i] == '.' && dot == s.length) dot = i;
  }
  return (rmd_str){s.bytes, dot};
}

/* Node height, treating NULL as height 0. */
static int height(const rmd_name_node* p) { return p ? p->height : 0; }

/* Recomputes p's height from its children's heights. */
static void update(rmd_name_node* p) {
  int a = height(p->left), b = height(p->right);
  p->height = 1 + (a > b ? a : b);
}

/* AVL left rotation around p. */
static rmd_name_node* rotate_left(rmd_name_node* p) {
  rmd_name_node* q = p->right;
  p->right = q->left;
  q->left = p;
  update(p);
  update(q);
  return q;
}

/* AVL right rotation around p. */
static rmd_name_node* rotate_right(rmd_name_node* p) {
  rmd_name_node* q = p->left;
  p->left = q->right;
  q->right = p;
  update(p);
  update(q);
  return q;
}

/* Lexicographically compares a and b's code-point keys, shorter first on
 * a common prefix. */
static int compare(const rmd_name_node* a, const rmd_name_node* b) {
  size_t n = a->length < b->length ? a->length : b->length;
  for (size_t i = 0; i < n; i++)
    if (a->key[i] != b->key[i]) return a->key[i] < b->key[i] ? -1 : 1;
  return (a->length > b->length) - (a->length < b->length);
}

/* Inserts node into the AVL tree rooted at p, rebalancing as needed;
 * sets *duplicate without modifying the tree if an equal node is already
 * present. */
static rmd_name_node* insert(rmd_name_node* p, rmd_name_node* node,
                             bool* duplicate) {
  if (!p) return node;
  int c = compare(node, p);
  if (!c) {
    *duplicate = true;
    return p;
  }
  if (c < 0)
    p->left = insert(p->left, node, duplicate);
  else
    p->right = insert(p->right, node, duplicate);
  update(p);
  if (height(p->left) - height(p->right) > 1) {
    if (height(p->left->right) > height(p->left->left))
      p->left = rotate_left(p->left);
    return rotate_right(p);
  }
  if (height(p->right) - height(p->left) > 1) {
    if (height(p->right->left) > height(p->right->right))
      p->right = rotate_right(p->right);
    return rotate_left(p);
  }
  return p;
}

rmd_status rmd_names_insert(rmd_name_node** tree, rmd_str name) {
  scalar* values = nullptr;
  size_t n = 0;
  rmd_status status = nfd(name, &values, &n);
  if (status != RMD_OK) return status;
  rmd_name_node* node = rmd_calloc(1, sizeof *node);
  uint32_t* key = rmd_calloc(n, sizeof *key);
  if (!node || !key) {
    free(values);
    free(node);
    free(key);
    return RMD_ERR_OUT_OF_MEMORY;
  }
  for (size_t i = 0; i < n; i++) key[i] = values[i].cp;
  free(values);
  *node = (rmd_name_node){.key = key, .length = n, .height = 1};
  bool duplicate = false;
  *tree = insert(*tree, node, &duplicate);
  if (duplicate) {
    free(key);
    free(node);
    return RMD_ERR_DUPLICATE_NAME;
  }
  return RMD_OK;
}

void rmd_names_free(rmd_name_node* p) {
  if (!p) return;
  rmd_names_free(p->left);
  rmd_names_free(p->right);
  free(p->key);
  free(p);
}

/* Check without claiming a name: a pre-write failure remains recoverable. */
rmd_status rmd_names_check(const rmd_name_node* tree, rmd_str name) {
  if (!tree) return RMD_OK;
  scalar* values = nullptr;
  size_t n = 0;
  rmd_status status = nfd(name, &values, &n);
  if (status != RMD_OK) return status;
  while (tree) {
    size_t common = n < tree->length ? n : tree->length;
    int comparison = 0;
    for (size_t i = 0; i < common; i++) {
      if (values[i].cp == tree->key[i]) continue;
      comparison = values[i].cp < tree->key[i] ? -1 : 1;
      break;
    }
    if (!comparison) comparison = (n > tree->length) - (n < tree->length);
    if (!comparison) {
      free(values);
      return RMD_ERR_DUPLICATE_NAME;
    }
    tree = comparison < 0 ? tree->left : tree->right;
  }
  free(values);
  return RMD_OK;
}
