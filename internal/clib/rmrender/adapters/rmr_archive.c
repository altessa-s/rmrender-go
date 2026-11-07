/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <rmlines/parser.h>
#include <rmrender/archive.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rmr_archive_document {
  const rmd_archive* archive;
  rmr_document_layout* layout;
  rmr_document_page* pages;
  size_t *files, count;
  char *id, *strings;
  rmr_limits limits;
  rmr_asset_view pdf;
};

/* Fill *e (if given) with status s and message m, and return s. */
static rmr_status fail(rmr_error* e, rmr_status s, const char* m) {
  if (e) {
    *e = (rmr_error){.code = s};
    snprintf(e->message, sizeof e->message, "%s", m);
  }
  return s;
}

/* The portion of name after its last '/', or all of it if there is none. */
static rmd_str filename(rmd_str name) {
  size_t start = 0;
  for (size_t i = 0; i < name.length; i++)
    if (name.bytes[i] == '/') start = i + 1;
  return (rmd_str){name.bytes + start, name.length - start};
}

/* True if s ends with the NUL-terminated string tail. */
static bool suffix(rmd_str s, const char* tail) {
  size_t n = strlen(tail);
  return s.length >= n && !memcmp(s.bytes + s.length - n, tail, n);
}

/* True if s exactly matches the NUL-terminated string z. */
static bool same(rmd_str s, const char* z) {
  return strlen(z) == s.length && !memcmp(s.bytes, z, s.length);
}

/* True if the document ID embedded in name matches id. */
static bool belongs(rmd_str name, const char* id) {
  return same(rmd_document_id(name), id);
}

typedef struct {
  size_t index;
  rmd_str name, base;
  size_t run;  /* Position of the first file with this base, by base order. */
  size_t last; /* At a run's first file: the run's last archive index. */
  bool taken;  /* At a run's first file: a page already has this base. */
} page_file;

/* Order of byte strings x and y: by bytes, then the shorter first. */
static int order(rmd_str x, rmd_str y) {
  size_t n = x.length < y.length ? x.length : y.length;
  int c = n ? memcmp(x.bytes, y.bytes, n) : 0;
  return c ? c : x.length < y.length ? -1 : x.length > y.length ? 1 : 0;
}

/* qsort comparator ordering page_file entries by base name bytes, then by
 * archive position. */
static int by_base(const void* a, const void* b) {
  const page_file *x = a, *y = b;
  int c = order(x->base, y->base);
  return c ? c : (x->index > y->index) - (x->index < y->index);
}

/* qsort comparator ordering page_file pointers by name bytes, then by
 * archive position. */
static int by_name(const void* a, const void* b) {
  const page_file *x = *(page_file* const*)a, *y = *(page_file* const*)b;
  int c = order(x->name, y->name);
  return c ? c : (x->index > y->index) - (x->index < y->index);
}

/* Sets *out to the run of files (sorted by base) whose base is exactly page ID
 * id, or null if none. Each comparison reads at most the ID's bytes and is
 * charged that to *work; false, with *out unset, if limit would be passed. */
static bool run(page_file* files, size_t found, const char* id, size_t limit,
                size_t* work, page_file** out) {
  rmd_str key = {id, strlen(id)};
  size_t low = 0, high = found;
  while (low < high) {
    if (key.length + 1 > limit - *work) return false;
    *work += key.length + 1;
    size_t middle = low + (high - low) / 2;
    if (order(files[middle].base, key) < 0)
      low = middle + 1;
    else
      high = middle;
  }
  if (low < found && key.length + 1 > limit - *work) return false;
  if (low < found) *work += key.length + 1;
  *out = low < found && !order(files[low].base, key) ? &files[low] : nullptr;
  return true;
}

/* Units charged for sorting n entries: n*ceil(log2 n) comparisons. */
static size_t sorting(size_t n) {
  size_t depth = 0;
  while (depth < 64 && ((size_t)1 << depth) < n) depth++;
  return n * depth;
}

/* Adds units to *work; false, leaving it unchanged, past limit. */
static bool charge(size_t units, size_t limit, size_t* work) {
  if (units > limit - *work) return false;
  *work += units;
  return true;
}

/* Free a document and its owned index/string memory; d may be null. */
void rmr_archive_document_free(rmr_archive_document* d) {
  if (d) {
    rmr_document_layout_free(d->layout);
    free(d->pages);
    free(d->files);
    free(d->id);
    free(d->strings);
    free(d);
  }
}

rmr_status rmr_archive_document_create(const rmd_archive* archive,
                                       const char* id, bool recover,
                                       const rmr_limits* given,
                                       rmr_archive_document** out,
                                       rmr_error* e) {
  if (!archive || !id || !*id || !out || (!archive->files && archive->count))
    return fail(e, RMR_ERR_INVALID_ARGUMENT, "invalid archive document");

  rmr_limits l = given ? *given : rmr_limits_default();
  size_t id_length = strlen(id);
  if (sizeof(rmr_archive_document) > l.maximum_plan_bytes ||
      id_length >= l.maximum_plan_bytes - sizeof(rmr_archive_document) ||
      archive->count > (l.maximum_plan_bytes - sizeof(rmr_archive_document) -
                        id_length - 1) /
                           (sizeof(page_file) + sizeof(page_file*)))
    return fail(e, RMR_ERR_LIMIT, "archive index memory limit");
  size_t base_bytes = sizeof(rmr_archive_document) + id_length + 1 +
                      archive->count * (sizeof(page_file) + sizeof(page_file*));
  if (archive->count > l.maximum_elements ||
      archive->count > l.maximum_plan_bytes / sizeof(page_file) ||
      id_length >= l.maximum_plan_bytes)
    return fail(e, RMR_ERR_LIMIT, "archive index limit");

  rmr_archive_document* d = calloc(1, sizeof *d);
  page_file* files =
      archive->count ? calloc(archive->count, sizeof *files) : nullptr;
  page_file** named =
      archive->count ? calloc(archive->count, sizeof *named) : nullptr;
  if (!d || (archive->count && (!files || !named))) {
    free(d);
    free(files);
    free(named);
    return fail(e, RMR_ERR_OUT_OF_MEMORY, "archive index allocation failed");
  }

  d->archive = archive;
  d->limits = l;
  d->id = malloc(id_length + 1);
  if (!d->id) {
    free(files);
    free(named);
    rmr_archive_document_free(d);
    return fail(e, RMR_ERR_OUT_OF_MEMORY, "document ID allocation failed");
  }
  memcpy(d->id, id, id_length + 1);

  size_t found = 0, strings = 0, work = archive->count;
  const rmd_file* content = nullptr;
  rmr_status status = RMR_OK;
  for (size_t i = 0; i < archive->count; i++) {
    const rmd_file* f = &archive->files[i];
    if ((!f->name.bytes && f->name.length) ||
        (!f->data.bytes && f->data.size)) {
      status = fail(e, RMR_ERR_INVALID_ARGUMENT, "invalid archive file entry");
      goto done;
    }
    if (f->name.length > l.maximum_elements - work) {
      status = fail(e, RMR_ERR_LIMIT, "archive name work limit");
      goto done;
    }
    work += f->name.length;
    if (!f->name.bytes || !belongs(f->name, id)) continue;
    rmd_str name = filename(f->name);
    if (suffix(name, ".content") && !content)
      content = f;
    else if (suffix(name, ".pdf") && !d->pdf.bytes)
      d->pdf = (rmr_asset_view){f->data.bytes, f->data.size};
    else if (suffix(name, ".rm")) {
      if (name.length > l.maximum_plan_bytes - strings) {
        status = fail(e, RMR_ERR_LIMIT, "archive page name memory limit");
        goto done;
      }
      strings += name.length; /* Includes room for NUL after removing .rm. */
      files[found++] = (page_file){
          .index = i, .name = f->name, .base = {name.bytes, name.length - 3}};
    }
  }

  if (content) {
    rmr_limits metadata_limits = l;
    metadata_limits.maximum_plan_bytes -= base_bytes;
    status = rmr_document_layout_decode((const char*)content->data.bytes,
                                        content->data.size, &metadata_limits,
                                        &d->layout, e);
    if (status != RMR_OK) goto done;
  }

  size_t listed = rmr_document_layout_count(d->layout),
         deleted = rmr_document_layout_deleted_count(d->layout);
  if (found > l.maximum_draws || listed > l.maximum_draws - found) {
    status = fail(e, RMR_ERR_LIMIT, "page count limit");
    goto done;
  }

  size_t capacity = listed + found;
  size_t available =
      l.maximum_plan_bytes - base_bytes - rmr_document_layout_bytes(d->layout);
  if (capacity > available / (sizeof *d->pages + sizeof *d->files) ||
      strings > available - capacity * (sizeof *d->pages + sizeof *d->files)) {
    status = fail(e, RMR_ERR_LIMIT, "archive page memory limit");
    goto done;
  }

  d->pages = capacity ? calloc(capacity, sizeof *d->pages) : nullptr;
  d->files = capacity ? calloc(capacity, sizeof *d->files) : nullptr;
  d->strings = strings ? malloc(strings) : nullptr;
  if ((capacity && (!d->pages || !d->files)) || (strings && !d->strings)) {
    status = fail(e, RMR_ERR_OUT_OF_MEMORY, "archive page allocation failed");
    goto done;
  }

  /* Files sorted by base name, then archive position, form one run per base:
   * a page takes the last file of its run in archive order, and recovery
   * marks a run taken once a page has its base as ID. Sorting and binary
   * searches keep both linear-logarithmic in the page and file counts. A sort
   * is charged a unit per comparison, the names' bytes having been charged
   * once above; a search, the bytes of the ID it looks up per comparison. */
  if (!charge(sorting(found), l.maximum_elements, &work)) {
    status = fail(e, RMR_ERR_LIMIT, "page match work limit");
    goto done;
  }
  if (found > 1) qsort(files, found, sizeof *files, by_base);
  for (size_t i = 0; i < found; i++) {
    bool first = !i || order(files[i - 1].base, files[i].base);
    files[i].run = first ? i : files[i - 1].run;
    files[files[i].run].last = files[i].index;
  }

  for (size_t i = 0; i < listed; i++) {
    d->pages[d->count] = *rmr_document_layout_page(d->layout, i);
    page_file* match = nullptr;
    if (!run(files, found, d->pages[d->count].id, l.maximum_elements, &work,
             &match)) {
      status = fail(e, RMR_ERR_LIMIT, "page match work limit");
      goto done;
    }
    d->files[d->count++] = match ? match->last : SIZE_MAX;
  }

  size_t at = 0;
  if ((recover || (!listed && !deleted)) && found) {
    if (!charge(sorting(found), l.maximum_elements, &work)) {
      status = fail(e, RMR_ERR_LIMIT, "page recovery work limit");
      goto done;
    }
    for (size_t i = 0; i < listed + deleted; i++) {
      const char* page =
          i < listed ? d->pages[i].id
                     : rmr_document_layout_deleted(d->layout, i - listed);
      page_file* taken = nullptr;
      if (!run(files, found, page, l.maximum_elements, &work, &taken)) {
        status = fail(e, RMR_ERR_LIMIT, "page recovery work limit");
        goto done;
      }
      if (taken) taken->taken = true;
    }

    for (size_t i = 0; i < found; i++) named[i] = &files[i];
    if (found > 1) qsort(named, found, sizeof *named, by_name);
    for (size_t i = 0; i < found; i++) {
      const page_file* f = named[i];
      if (files[f->run].taken) continue;
      char* name = d->strings + at;
      memcpy(name, f->base.bytes, f->base.length);
      name[f->base.length] = 0;
      at += f->base.length + 1;
      d->pages[d->count] = (rmr_document_page){.id = name};
      d->files[d->count++] = f->index;
      /* Later files with this page ID as base are taken; a base holding a
       * NUL byte names the page only up to it. */
      page_file* taken = nullptr;
      if (!run(files, found, name, l.maximum_elements, &work, &taken)) {
        status = fail(e, RMR_ERR_LIMIT, "page recovery work limit");
        goto done;
      }
      if (taken) taken->taken = true;
    }
  }

done:
  /* Every failure above has already filled e: the local checks name their
   * own limit, and the layout decoder reports its own (e.g. JSON work). */
  free(files);
  free(named);
  if (status != RMR_OK) {
    rmr_archive_document_free(d);
    return status;
  }

  *out = d;
  return RMR_OK;
}

/* Number of pages in the document, or 0 if d is null. */
size_t rmr_archive_document_count(const rmr_archive_document* d) {
  return d ? d->count : 0;
}

/* Page at index i, or null if d is null or i is out of range. */
const rmr_document_page* rmr_archive_document_page(
    const rmr_archive_document* d, size_t i) {
  return d && i < d->count ? &d->pages[i] : nullptr;
}

/* Canvas size from the document's layout metadata, or {0} without one. */
rmr_point rmr_archive_document_canvas(const rmr_archive_document* d) {
  return d ? rmr_document_layout_canvas(d->layout) : (rmr_point){0};
}

/* True if the document's layout metadata marks it landscape. */
bool rmr_archive_document_landscape(const rmr_archive_document* d) {
  return d && rmr_document_layout_landscape(d->layout);
}

/* The document's PDF attachment, or an empty view if it has none. */
rmr_asset_view rmr_archive_document_pdf(const rmr_archive_document* d) {
  return d ? d->pdf : (rmr_asset_view){0};
}

/* Page i of d parsed from size bytes, with its template set. */
static rmr_status parse(const rmr_archive_document* d, size_t i,
                        const uint8_t* bytes, size_t size, rm_page** out,
                        rmr_error* e) {
  if (size > d->limits.maximum_plan_bytes)
    return fail(e, RMR_ERR_LIMIT, "page input byte limit");

  rm_page* page = nullptr;
  rm_error parse_error = {0};
  rm_status status = rm_parse_page(bytes, size, &page, &parse_error);
  if (status != RM_OK)
    return fail(e,
                status == RM_ERR_OUT_OF_MEMORY         ? RMR_ERR_OUT_OF_MEMORY
                : status == RM_ERR_UNSUPPORTED_VERSION ? RMR_ERR_UNSUPPORTED
                                                       : RMR_ERR_BACKEND,
                "RMLines could not parse archive page");

  status = rm_page_set_template(page, d->pages[i].template_name);
  if (status != RM_OK) {
    rm_page_free(page);
    return fail(e, RMR_ERR_OUT_OF_MEMORY, "template name allocation failed");
  }

  *out = page;
  return RMR_OK;
}

rmr_status rmr_archive_document_parse_page(const rmr_archive_document* d,
                                           size_t i, rm_page** out,
                                           rmr_error* e) {
  if (!d || i >= d->count || !out)
    return fail(e, RMR_ERR_INVALID_ARGUMENT, "archive page index out of range");
  if (d->files[i] == SIZE_MAX)
    return parse(d, i, (const uint8_t*)RM_HEADER, RM_HEADER_LENGTH, out, e);
  rmd_bytes data = d->archive->files[d->files[i]].data;
  return parse(d, i, data.bytes, data.size, out, e);
}

rmr_status rmr_archive_document_read_page(const rmr_archive_document* d,
                                          size_t i, rm_page** out,
                                          rmr_status* unreadable,
                                          rmr_error* e) {
  if (!d || i >= d->count || !out || !unreadable)
    return fail(e, RMR_ERR_INVALID_ARGUMENT, "archive page index out of range");
  rmr_error parsing = {0};
  rmr_status status = rmr_archive_document_parse_page(d, i, out, &parsing);
  /* Only the parser's own refusals are the page's; a template name that
   * cannot be stored fails with OUT_OF_MEMORY, never these. */
  if (status != RMR_ERR_BACKEND && status != RMR_ERR_UNSUPPORTED) {
    if (status != RMR_OK && e) *e = parsing;
    if (status == RMR_OK) *unreadable = RMR_OK;
    return status;
  }
  rmr_status blank =
      parse(d, i, (const uint8_t*)RM_HEADER, RM_HEADER_LENGTH, out, e);
  if (blank == RMR_OK) *unreadable = status;
  return blank;
}

rmr_status rmr_archive_document_image(void* context, rm_str file,
                                      rmr_asset_view* out) {
  const rmr_archive_document* d = context;
  if (!d || !out || (!file.bytes && file.length))
    return RMR_ERR_INVALID_ARGUMENT;

  size_t work = 0;
  for (size_t i = 0; i < d->archive->count; i++) {
    const rmd_file* f = &d->archive->files[i];
    if (f->name.length > d->limits.maximum_elements - work)
      return RMR_ERR_LIMIT;
    work += f->name.length;
    if (!belongs(f->name, d->id)) continue;
    rmd_str name = filename(f->name);
    if (name.length == file.length &&
        !memcmp(name.bytes, file.bytes, file.length)) {
      if (f->data.size > d->limits.maximum_plan_bytes) return RMR_ERR_LIMIT;
      *out = (rmr_asset_view){f->data.bytes, f->data.size};
      return RMR_OK;
    }
  }

  *out = (rmr_asset_view){0};
  return RMR_OK;
}
