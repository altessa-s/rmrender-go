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
} page_file;

/* qsort comparator ordering page_file entries by name bytes. */
static int compare(const void* a, const void* b) {
  const page_file *x = a, *y = b;
  size_t n = x->name.length < y->name.length ? x->name.length : y->name.length;
  int c = memcmp(x->name.bytes, y->name.bytes, n);
  return c                                 ? c
         : x->name.length < y->name.length ? -1
         : x->name.length > y->name.length ? 1
                                           : 0;
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
                           sizeof(page_file))
    return fail(e, RMR_ERR_LIMIT, "archive index memory limit");
  size_t base_bytes = sizeof(rmr_archive_document) + id_length + 1 +
                      archive->count * sizeof(page_file);
  if (archive->count > l.maximum_elements ||
      archive->count > l.maximum_plan_bytes / sizeof(page_file) ||
      id_length >= l.maximum_plan_bytes)
    return fail(e, RMR_ERR_LIMIT, "archive index limit");

  rmr_archive_document* d = calloc(1, sizeof *d);
  page_file* files =
      archive->count ? calloc(archive->count, sizeof *files) : nullptr;
  if (!d || (archive->count && !files)) {
    free(d);
    free(files);
    return fail(e, RMR_ERR_OUT_OF_MEMORY, "archive index allocation failed");
  }

  d->archive = archive;
  d->limits = l;
  d->id = malloc(id_length + 1);
  if (!d->id) {
    free(files);
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
      status = RMR_ERR_INVALID_ARGUMENT;
      goto done;
    }
    if (f->name.length > l.maximum_elements - work) {
      status = RMR_ERR_LIMIT;
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
        status = RMR_ERR_LIMIT;
        goto done;
      }
      strings += name.length; /* Includes room for NUL after removing .rm. */
      files[found++] = (page_file){i, f->name, {name.bytes, name.length - 3}};
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
    status = RMR_ERR_LIMIT;
    goto done;
  }

  size_t capacity = listed + found;
  size_t available =
      l.maximum_plan_bytes - base_bytes - rmr_document_layout_bytes(d->layout);
  if (capacity > available / (sizeof *d->pages + sizeof *d->files) ||
      strings > available - capacity * (sizeof *d->pages + sizeof *d->files)) {
    status = RMR_ERR_LIMIT;
    goto done;
  }

  d->pages = capacity ? calloc(capacity, sizeof *d->pages) : nullptr;
  d->files = capacity ? calloc(capacity, sizeof *d->files) : nullptr;
  d->strings = strings ? malloc(strings) : nullptr;
  if ((capacity && (!d->pages || !d->files)) || (strings && !d->strings)) {
    status = RMR_ERR_OUT_OF_MEMORY;
    goto done;
  }

  for (size_t i = 0; i < listed; i++) {
    d->pages[d->count] = *rmr_document_layout_page(d->layout, i);
    size_t match = SIZE_MAX;
    for (size_t j = 0; j < found; j++) {
      if (work >= l.maximum_elements) {
        status = RMR_ERR_LIMIT;
        goto done;
      }
      work++;
      if (same(files[j].base, d->pages[d->count].id)) match = files[j].index;
    }
    d->files[d->count++] = match;
  }

  if (found > 1) qsort(files, found, sizeof *files, compare);
  size_t at = 0;
  if (recover || (!listed && !deleted))
    for (size_t i = 0; i < found; i++) {
      bool taken = false;
      for (size_t j = 0; j < d->count; j++) {
        if (work >= l.maximum_elements) {
          status = RMR_ERR_LIMIT;
          goto done;
        }
        work++;
        if (same(files[i].base, d->pages[j].id)) {
          taken = true;
          break;
        }
      }
      for (size_t j = 0; !taken && j < deleted; j++) {
        if (work >= l.maximum_elements) {
          status = RMR_ERR_LIMIT;
          goto done;
        }
        work++;
        if (same(files[i].base, rmr_document_layout_deleted(d->layout, j)))
          taken = true;
      }
      if (taken) continue;
      char* name = d->strings + at;
      memcpy(name, files[i].base.bytes, files[i].base.length);
      name[files[i].base.length] = 0;
      at += files[i].base.length + 1;
      d->pages[d->count] = (rmr_document_page){.id = name};
      d->files[d->count++] = files[i].index;
    }

done:
  free(files);
  if (status != RMR_OK) {
    rmr_archive_document_free(d);
    return fail(e, status, "archive document assembly failed");
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

rmr_status rmr_archive_document_parse_page(const rmr_archive_document* d,
                                           size_t i, rm_page** out,
                                           rmr_error* e) {
  if (!d || i >= d->count || !out)
    return fail(e, RMR_ERR_INVALID_ARGUMENT, "archive page index out of range");

  const uint8_t* bytes = (const uint8_t*)RM_HEADER;
  size_t size = RM_HEADER_LENGTH;
  if (d->files[i] != SIZE_MAX) {
    rmd_bytes data = d->archive->files[d->files[i]].data;
    bytes = data.bytes;
    size = data.size;
  }
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
