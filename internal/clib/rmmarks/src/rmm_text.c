/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "rmm_internal.h"

typedef struct {
  size_t line, piece;
} identity;

typedef struct {
  identity* pieces;
  size_t count;
  rmm_str text;
  rmm_page_box place;
  uint64_t hash;
} selection;

typedef struct repeat {
  size_t band;
  struct repeat* next;
} repeat;

typedef struct {
  selection selected;
  repeat *first, *last;
  size_t repeats;
} passage_work;

typedef struct {
  size_t key, index;
} key_index;

/* Mixes the bits of x into a well-spread 64-bit value, for hashing. */
static uint64_t hash_number(uint64_t x) {
  x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
  return x ^ (x >> 31);
}

/* Smallest power of two at least twice count, for an open-addressed hash
 * table. Returns 0 if that would overflow size_t. */
static size_t table_capacity(size_t count) {
  if (count > SIZE_MAX / 2) return 0;
  size_t capacity = 8;
  while (capacity < count * 2) {
    if (capacity > SIZE_MAX / 2) return 0;
    capacity *= 2;
  }
  return capacity;
}

/* qsort comparator ordering key_index entries by key. */
static int key_order(const void* a, const void* b) {
  const key_index *x = a, *y = b;
  return (x->key > y->key) - (x->key < y->key);
}

/* Binary search for key among keys, sorted by key_order. Returns the
 * matching index, or SIZE_MAX if key is not present. */
static size_t find_key(const key_index* keys, size_t count, size_t key) {
  size_t lo = 0, hi = count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (keys[mid].key < key)
      lo = mid + 1;
    else if (keys[mid].key > key)
      hi = mid;
    else
      return keys[mid].index;
  }
  return SIZE_MAX;
}

/* Bounding box of every piece in line, which must have at least one. */
static rmm_page_box span(rmm_line line) {
  rmm_page_box box = line.pieces[0].box;
  for (size_t i = 1; i < line.count; i += 1)
    box = rmm_box_union(box, line.pieces[i].box);
  return box;
}

/* Length of the overlap between intervals [a, b] and [c, d], or zero if
 * they do not overlap. */
static double overlap(double a, double b, double c, double d) {
  return fmax(0, fmin(b, d) - fmax(a, c));
}

/* True if band's vertical extent, with a small slack, reaches line's
 * vertical middle. */
static bool covers(rmm_page_box band, rmm_page_box line) {
  double middle = (line.y0 + line.y1) / 2;
  double slack = (line.y1 - line.y0) * RMM_PAST_THE_MIDDLE;
  return band.y0 - slack <= middle && middle <= band.y1 + slack;
}

/* True if b horizontally overlaps at least RMM_LEAST_WIDTH of p's width,
 * or, when p has no positive width, if p falls entirely within b. */
static bool half_covered(rmm_page_box b, rmm_page_box p) {
  double width = p.x1 - p.x0;
  if (!(width > 0)) return b.x0 <= p.x0 && p.x1 <= b.x1;
  return overlap(b.x0, b.x1, p.x0, p.x1) / width >= RMM_LEAST_WIDTH;
}

/* True if b and p horizontally share at least RMM_LEAST_TOUCH of the
 * narrower box's width, or any overlap at all when one has no width. */
static bool touched(rmm_page_box b, rmm_page_box p) {
  double width = fmin(p.x1 - p.x0, b.x1 - b.x0);
  double shared = overlap(b.x0, b.x1, p.x0, p.x1);
  return width > 0 ? shared / width >= RMM_LEAST_TOUCH : shared > 0;
}

/* The selection identity is (line index, piece index), never the text.
 * Two identical words in different places are two different passages. */
static rmm_status select_text(rmm_arena* arena, rmm_page_box band,
                              const rmm_line* lines, size_t count, size_t total,
                              selection* out) {
  *out = (selection){0};
  if (!total) return RMM_OK;
  identity* selected = rmm_arena_calloc(arena, total, sizeof *selected);
  if (!selected) return RMM_ERR_OUT_OF_MEMORY;
  size_t used = 0, bytes = 0;
  rmm_page_box place = {0};
  uint64_t hash = UINT64_C(0xcbf29ce484222325);
  for (size_t i = 0; i < count; i += 1) {
    rmm_line line = lines[i];
    if (!line.count) continue;
    rmm_page_box bounds = span(line);
    if (!covers(band, bounds)) continue;
    double width = bounds.x1 - bounds.x0;
    bool whole =
        width > 0 && overlap(band.x0, band.x1, bounds.x0, bounds.x1) / width >=
                         RMM_WHOLE_LINE;
    bool half = false;
    if (!whole)
      for (size_t j = 0; j < line.count; j += 1)
        half |= half_covered(band, line.pieces[j].box);
    for (size_t j = 0; j < line.count; j += 1) {
      rmm_placed_text p = line.pieces[j];
      if (!whole && !(half ? half_covered(band, p.box) : touched(band, p.box)))
        continue;
      if (p.text.length > SIZE_MAX - bytes ||
          (used && bytes + p.text.length == SIZE_MAX))
        return RMM_ERR_OUT_OF_MEMORY;
      bytes += p.text.length + (used ? 1u : 0u);
      selected[used] = (identity){i, j};
      place = used ? rmm_box_union(place, p.box) : p.box;
      used += 1;
      hash = (hash ^ hash_number(i)) * UINT64_C(0x100000001b3);
      hash = (hash ^ hash_number(j)) * UINT64_C(0x100000001b3);
    }
  }
  if (!used) return RMM_OK;
  if (bytes == SIZE_MAX) return RMM_ERR_OUT_OF_MEMORY;
  char* text = rmm_arena_alloc(arena, bytes + 1);
  if (!text) return RMM_ERR_OUT_OF_MEMORY;
  size_t at = 0;
  for (size_t i = 0; i < used; i += 1) {
    rmm_str piece = lines[selected[i].line].pieces[selected[i].piece].text;
    if (i) text[at++] = ' ';
    if (piece.length) memcpy(text + at, piece.bytes, piece.length);
    at += piece.length;
  }
  text[at] = '\0';
  rmm_str trimmed = rmm_trim((rmm_str){text, at});
  if (!trimmed.length) return RMM_OK;
  *out = (selection){selected, used, trimmed, place, hash};
  return RMM_OK;
}

/* True if a and b selected the exact same set of (line, piece) pieces. */
static bool same_selection(selection a, selection b) {
  if (a.hash != b.hash || a.count != b.count) return false;
  for (size_t i = 0; i < a.count; i += 1)
    if (a.pieces[i].line != b.pieces[i].line ||
        a.pieces[i].piece != b.pieces[i].piece)
      return false;
  return true;
}

/* Copies s into a new arena-owned string at *out. Returns false, leaving
 * *out unchanged, on allocation failure. */
static bool copy_string(rmm_arena* arena, rmm_str s, rmm_str* out) {
  char* bytes = rmm_arena_strndup(arena, s.bytes, s.length);
  if (!bytes) return false;
  *out = (rmm_str){bytes, s.length};
  return true;
}

/* qsort comparator ordering passages by (place.y0, place.x0, band). */
static int passage_order(const void* a, const void* b) {
  const rmm_marked_passage *x = a, *y = b;
  if (x->place.y0 != y->place.y0)
    return (x->place.y0 > y->place.y0) - (x->place.y0 < y->place.y0);
  if (x->place.x0 != y->place.x0)
    return (x->place.x0 > y->place.x0) - (x->place.x0 < y->place.x0);
  return (x->band > y->band) - (x->band < y->band);
}

rmm_status rmm_text_passages(const rmm_page_box* bands, size_t band_count,
                             const rmm_line* lines, size_t line_count,
                             const rmm_band_reading* alone, size_t alone_count,
                             rmm_passages** out, rmm_error* error) {
  if (!out || !rmm_array_valid(bands, band_count, sizeof *bands) ||
      !rmm_array_valid(alone, alone_count, sizeof *alone))
    return rmm_finish(error, RMM_ERR_INVALID_ARGUMENT);
  size_t total;
  rmm_status status = rmm_validate_lines(lines, line_count, &total);
  if (status != RMM_OK) return rmm_finish(error, status);
  for (size_t i = 0; i < band_count; i += 1)
    if (!rmm_box_valid(bands[i]))
      return rmm_finish(error, RMM_ERR_INVALID_ARGUMENT);
  rmm_arena *arena = rmm_arena_create(), *work = rmm_arena_create(),
            *scratch = nullptr;
  if (!arena || !work) {
    status = RMM_ERR_OUT_OF_MEMORY;
    goto fail;
  }
  rmm_passages* result = rmm_arena_calloc(arena, 1, sizeof *result);
  rmm_marked_passage* items =
      rmm_arena_calloc(arena, band_count, sizeof *items);
  passage_work* meta = rmm_arena_calloc(work, band_count, sizeof *meta);
  key_index* keys = rmm_arena_calloc(work, alone_count, sizeof *keys);
  size_t* alone_totals =
      rmm_arena_calloc(work, alone_count, sizeof *alone_totals);
  size_t capacity = table_capacity(band_count);
  size_t* slots = rmm_arena_calloc(work, capacity, sizeof *slots);
  if (!result || !items || !meta || !keys || !alone_totals || !capacity ||
      !slots) {
    status = RMM_ERR_OUT_OF_MEMORY;
    goto fail;
  }
  *result = (rmm_passages){items, 0, arena};
  for (size_t i = 0; i < alone_count; i += 1) {
    keys[i] = (key_index){alone[i].band, i};
    status = rmm_validate_lines(alone[i].lines, alone[i].line_count,
                                &alone_totals[i]);
    if (status != RMM_OK) goto fail;
  }
  qsort(keys, alone_count, sizeof *keys, key_order);
  for (size_t i = 1; i < alone_count; i += 1)
    if (keys[i - 1].key == keys[i].key) {
      status = RMM_ERR_INVALID_ARGUMENT;
      goto fail;
    }
  for (size_t i = 0; i < band_count; i += 1) {
    scratch = rmm_arena_create();
    if (!scratch) {
      status = RMM_ERR_OUT_OF_MEMORY;
      goto fail;
    }
    selection page = {0}, cut = {0};
    status = select_text(scratch, bands[i], lines, line_count, total, &page);
    if (status != RMM_OK) goto fail;
    size_t slot = (size_t)page.hash & (capacity - 1);
    if (page.count) {
      while (slots[slot] &&
             !same_selection(page, meta[slots[slot] - 1].selected))
        slot = (slot + 1) & (capacity - 1);
      if (slots[slot]) {
        passage_work* first = &meta[slots[slot] - 1];
        repeat* again = rmm_arena_calloc(work, 1, sizeof *again);
        if (!again) {
          status = RMM_ERR_OUT_OF_MEMORY;
          goto fail;
        }
        again->band = i;
        if (first->last)
          first->last->next = again;
        else
          first->first = again;
        first->last = again;
        first->repeats += 1;
        rmm_arena_destroy(scratch);
        scratch = nullptr;
        continue;
      }
    }
    size_t own = find_key(keys, alone_count, i);
    if (own != SIZE_MAX) {
      status = select_text(scratch, bands[i], alone[own].lines,
                           alone[own].line_count, alone_totals[own], &cut);
      if (status != RMM_OK) goto fail;
    }
    selection quoted = page.count ? page : cut;
    if (quoted.count) {
      size_t at = result->count;
      rmm_marked_passage* p = &items[at];
      p->band = i;
      p->place = quoted.place;
      if (!copy_string(arena, quoted.text, &p->text)) {
        status = RMM_ERR_OUT_OF_MEMORY;
        goto fail;
      }
      if (page.count && cut.count) {
        bool equal;
        status = rmm_text_equal(scratch, page.text, cut.text, &equal);
        if (status != RMM_OK) goto fail;
        if (!equal && !copy_string(arena, cut.text, &p->second)) {
          status = RMM_ERR_OUT_OF_MEMORY;
          goto fail;
        }
      }
      if (page.count) {
        identity* identities =
            rmm_arena_calloc(work, page.count, sizeof *identities);
        if (!identities) {
          status = RMM_ERR_OUT_OF_MEMORY;
          goto fail;
        }
        memcpy(identities, page.pieces, page.count * sizeof *identities);
        meta[at].selected = (selection){
            .pieces = identities, .count = page.count, .hash = page.hash};
        slots[slot] = at + 1;
      }
      result->count += 1;
    }
    rmm_arena_destroy(scratch);
    scratch = nullptr;
  }
  for (size_t i = 0; i < result->count; i += 1)
    if (meta[i].repeats) {
      size_t* again = rmm_arena_calloc(arena, meta[i].repeats, sizeof *again);
      if (!again) {
        status = RMM_ERR_OUT_OF_MEMORY;
        goto fail;
      }
      size_t at = 0;
      for (repeat* r = meta[i].first; r; r = r->next) again[at++] = r->band;
      items[i].again = again;
      items[i].again_count = at;
    }
  qsort(items, result->count, sizeof *items, passage_order);
  rmm_arena_destroy(work);
  *out = result;
  return rmm_finish(error, RMM_OK);
fail:
  rmm_arena_destroy(scratch);
  rmm_arena_destroy(work);
  rmm_arena_destroy(arena);
  return rmm_finish(error, status);
}

rmm_status rmm_text_passages_from_reading(const rmm_page_box* bands,
                                          size_t band_count,
                                          const rmm_recognized_line* lines,
                                          size_t line_count, rmm_passages** out,
                                          rmm_error* error) {
  if (!out || !rmm_array_valid(lines, line_count, sizeof *lines))
    return rmm_finish(error, RMM_ERR_INVALID_ARGUMENT);
  rmm_arena* work = rmm_arena_create();
  if (!work) return rmm_finish(error, RMM_ERR_OUT_OF_MEMORY);
  rmm_line* rows = rmm_arena_calloc(work, line_count, sizeof *rows);
  rmm_placed_text* pieces = rmm_arena_calloc(work, line_count, sizeof *pieces);
  rmm_status status = RMM_ERR_OUT_OF_MEMORY;
  if (rows && pieces) {
    for (size_t i = 0; i < line_count; i += 1) {
      pieces[i] = rmm_place_line(lines[i]);
      rows[i] = (rmm_line){pieces + i, 1};
    }
    status = rmm_text_passages(bands, band_count, rows, line_count, nullptr, 0,
                               out, error);
  }
  rmm_arena_destroy(work);
  return rmm_finish(error, status);
}

typedef struct {
  rmm_page_box box;
  size_t position;
} ordered_box;

/* qsort comparator ordering boxes by y0, with ties broken by original
 * position to keep the sort stable. */
static int vertical_order(const void* a, const void* b) {
  const ordered_box *x = a, *y = b;
  if (x->box.y0 != y->box.y0)
    return (x->box.y0 > y->box.y0) - (x->box.y0 < y->box.y0);
  return (x->position > y->position) - (x->position < y->position);
}

/* qsort comparator ordering boxes by (y0, x0). */
static int box_order(const void* a, const void* b) {
  const rmm_page_box *x = a, *y = b;
  if (x->y0 != y->y0) return (x->y0 > y->y0) - (x->y0 < y->y0);
  return (x->x0 > y->x0) - (x->x0 < y->x0);
}

rmm_status rmm_text_blocks(const rmm_marked_passage* passages, size_t count,
                           rmm_blocks** out, rmm_error* error) {
  if (!out || !rmm_array_valid(passages, count, sizeof *passages))
    return rmm_finish(error, RMM_ERR_INVALID_ARGUMENT);
  for (size_t i = 0; i < count; i += 1)
    if (!rmm_box_valid(passages[i].place))
      return rmm_finish(error, RMM_ERR_INVALID_ARGUMENT);
  rmm_arena *arena = rmm_arena_create(), *work = rmm_arena_create();
  rmm_status status = RMM_ERR_OUT_OF_MEMORY;
  if (!arena || !work) goto fail;
  rmm_blocks* result = rmm_arena_calloc(arena, 1, sizeof *result);
  rmm_page_box* blocks = rmm_arena_calloc(arena, count, sizeof *blocks);
  ordered_box* ordered = rmm_arena_calloc(work, count, sizeof *ordered);
  if (!result || !blocks || !ordered) goto fail;
  *result = (rmm_blocks){blocks, 0, arena};
  for (size_t i = 0; i < count; i += 1)
    ordered[i] = (ordered_box){passages[i].place, i};
  qsort(ordered, count, sizeof *ordered, vertical_order);
  for (size_t i = 0; i < count; i += 1) {
    rmm_page_box grown = ordered[i].box;
    double gap = (grown.y1 - grown.y0) * RMM_WITHIN_A_LINE;
    bool joining = true;
    while (joining) {
      joining = false;
      size_t apart = 0;
      for (size_t j = 0; j < result->count; j += 1) {
        rmm_page_box b = blocks[j];
        double distance = fmax(grown.y0 - b.y1, b.y0 - grown.y1);
        if (distance < gap && grown.x0 < b.x1 && b.x0 < grown.x1) {
          grown = rmm_box_union(grown, b);
          joining = true;
        } else
          blocks[apart++] = b;
      }
      result->count = apart;
    }
    blocks[result->count++] = grown;
  }
  qsort(blocks, result->count, sizeof *blocks, box_order);
  rmm_arena_destroy(work);
  *out = result;
  return rmm_finish(error, RMM_OK);
fail:
  rmm_arena_destroy(work);
  rmm_arena_destroy(arena);
  return rmm_finish(error, status);
}

rmm_status rmm_text_brought(const rmm_line* lines, size_t line_count,
                            const rmm_mark_strip* strips, size_t strip_count,
                            const size_t* numbered, size_t number_count,
                            rmm_readings** out, rmm_error* error) {
  if (!out || !rmm_array_valid(strips, strip_count, sizeof *strips) ||
      !rmm_array_valid(numbered, number_count, sizeof *numbered))
    return rmm_finish(error, RMM_ERR_INVALID_ARGUMENT);
  rmm_status status = rmm_validate_lines(lines, line_count, nullptr);
  if (status != RMM_OK) return rmm_finish(error, status);
  size_t paired = strip_count < number_count ? strip_count : number_count;
  for (size_t i = 0; i < paired; i += 1)
    if (!rmm_box_valid(strips[i].on_page) || !rmm_box_valid(strips[i].in_sheet))
      return rmm_finish(error, RMM_ERR_INVALID_ARGUMENT);
  rmm_arena *arena = rmm_arena_create(), *work = rmm_arena_create();
  status = RMM_ERR_OUT_OF_MEMORY;
  if (!arena || !work) goto fail;
  rmm_readings* result = rmm_arena_calloc(arena, 1, sizeof *result);
  rmm_band_reading* groups =
      rmm_arena_calloc(arena, line_count, sizeof *groups);
  size_t* assignment = rmm_arena_calloc(work, line_count, sizeof *assignment);
  size_t* chosen = rmm_arena_calloc(work, line_count, sizeof *chosen);
  size_t* used = rmm_arena_calloc(work, line_count, sizeof *used);
  size_t capacity = table_capacity(line_count);
  size_t* slots = rmm_arena_calloc(work, capacity, sizeof *slots);
  rmm_line** group_lines =
      rmm_arena_calloc(work, line_count, sizeof *group_lines);
  if (!result || !groups || !assignment || !chosen || !used || !capacity ||
      !slots || !group_lines)
    goto fail;
  *result = (rmm_readings){groups, 0, arena};
  for (size_t i = 0; i < line_count; i += 1) {
    assignment[i] = SIZE_MAX;
    if (!lines[i].count) continue;
    rmm_page_box b = span(lines[i]);
    double middle = (b.y0 + b.y1) / 2;
    for (size_t j = 0; j < paired; j += 1)
      if (strips[j].in_sheet.y0 <= middle && middle <= strips[j].in_sheet.y1) {
        size_t slot = (size_t)hash_number(numbered[j]) & (capacity - 1);
        while (slots[slot] && groups[slots[slot] - 1].band != numbered[j])
          slot = (slot + 1) & (capacity - 1);
        if (!slots[slot]) {
          groups[result->count].band = numbered[j];
          slots[slot] = ++result->count;
        }
        size_t group = slots[slot] - 1;
        assignment[i] = group;
        chosen[i] = j;
        groups[group].line_count += 1;
        break;
      }
  }
  for (size_t i = 0; i < result->count; i += 1) {
    group_lines[i] =
        rmm_arena_calloc(arena, groups[i].line_count, sizeof **group_lines);
    if (!group_lines[i]) goto fail;
    groups[i].lines = group_lines[i];
  }
  for (size_t i = 0; i < line_count; i += 1)
    if (assignment[i] != SIZE_MAX) {
      size_t group = assignment[i];
      rmm_line line = lines[i];
      rmm_mark_strip strip = strips[chosen[i]];
      rmm_placed_text* pieces =
          rmm_arena_calloc(arena, line.count, sizeof *pieces);
      if (!pieces) goto fail;
      for (size_t j = 0; j < line.count; j += 1) {
        pieces[j].box = rmm_box_within(
            rmm_box_inside(line.pieces[j].box, strip.in_sheet), strip.on_page);
        if (!rmm_box_valid(pieces[j].box)) {
          status = RMM_ERR_INVALID_ARGUMENT;
          goto fail;
        }
        if (!copy_string(arena, line.pieces[j].text, &pieces[j].text))
          goto fail;
      }
      group_lines[group][used[group]++] = (rmm_line){pieces, line.count};
    }
  rmm_arena_destroy(work);
  *out = result;
  return rmm_finish(error, RMM_OK);
fail:
  rmm_arena_destroy(work);
  rmm_arena_destroy(arena);
  return rmm_finish(error, status);
}
