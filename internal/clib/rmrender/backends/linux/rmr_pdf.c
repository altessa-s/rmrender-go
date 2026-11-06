/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <fpdf_annot.h>
#include <fpdf_doc.h>
#include <limits.h>
#include <rmrender/linux_pdf.h>

#include "../../src/rmr_pdf_edit.h"
#include "../common/rmr_pdf_options.h"
#include "rmr_linux_internal.h"
#include "rmr_pdf_platform.h"

struct rmr_linux_pdf_source {
  rmr_buffer data;
  size_t count;
  rmr_pdf_page_geometry* geometry;
};

/* Frees a parsed PDF source and its page geometry table; accepts null. */
void rmr_linux_pdf_source_free(rmr_linux_pdf_source* s) {
  if (s) {
    rmr_buffer_free(&s->data);
    free(s->geometry);
    free(s);
  }
}

/* Returns the source's page count, or 0 for a null source. */
size_t rmr_linux_pdf_source_count(const rmr_linux_pdf_source* s) {
  return s ? s->count : 0;
}

rmr_status rmr_linux_pdf_source_create(const uint8_t* bytes, size_t size,
                                       const rmr_limits* given,
                                       rmr_linux_pdf_source** out,
                                       rmr_error* e) {
  if (!bytes || !size || !out)
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT, "missing PDF input");

  rmr_limits l = given ? *given : rmr_limits_default();
  size_t count = 0;
  rmr_status status = rmr_pdf_pages(bytes, size, &l, nullptr, 0, &count, e);
  if (status != RMR_OK) return status;
  if (!count || count > INT_MAX ||
      count > l.maximum_plan_bytes / sizeof(rmr_pdf_page_geometry))
    return rmr_linux_error(e, RMR_ERR_LIMIT, "PDF page limit");

  rmr_linux_pdf_source* s = calloc(1, sizeof *s);
  if (!s) return rmr_linux_error(e, RMR_ERR_OUT_OF_MEMORY, "allocation failed");
  s->geometry = calloc(count, sizeof *s->geometry);
  s->data.bytes = malloc(size);
  if (!s->geometry || !s->data.bytes) {
    rmr_linux_pdf_source_free(s);
    return rmr_linux_error(e, RMR_ERR_OUT_OF_MEMORY, "allocation failed");
  }

  memcpy(s->data.bytes, bytes, size);
  s->data.size = size;
  s->count = count;
  status = rmr_pdf_pages(bytes, size, &l, s->geometry, count, &count, e);

  rmr_pdfium_lock();
  FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(bytes, size, nullptr);
  if (status == RMR_OK && (!doc || FPDF_GetPageCount(doc) != (int)count))
    status = RMR_ERR_UNSUPPORTED;

  for (size_t i = 0; i < count && status == RMR_OK; i++) {
    FPDF_PAGE page = FPDF_LoadPage(doc, (int)i);
    float a, b, c, d;
    if (!page || !s->geometry[i].has_media)
      status = RMR_ERR_UNSUPPORTED;
    else {
      rmr_rect r = s->geometry[i].media;
      if ((FPDFPage_GetMediaBox(page, &a, &b, &c, &d) &&
           (fabs(r.x - a) > 1e-3 || fabs(r.y - b) > 1e-3 ||
            fabs(r.width - (c - a)) > 1e-3 ||
            fabs(r.height - (d - b)) > 1e-3)) ||
          FPDFPage_GetRotation(page) * 90 != s->geometry[i].rotate)
        status = RMR_ERR_UNSUPPORTED;
    }
    if (page) FPDF_ClosePage(page);
  }
  if (doc) FPDF_CloseDocument(doc);
  rmr_pdfium_unlock();

  if (status != RMR_OK) {
    rmr_linux_pdf_source_free(s);
    return rmr_linux_error(e, status,
                           "PDF readers disagree or source unsupported");
  }

  *out = s;
  return RMR_OK;
}

rmr_status rmr_linux_pdf_source_size(const rmr_linux_pdf_source* s, size_t page,
                                     rmr_point* out, rmr_error* e) {
  if (!s || !out || page >= s->count)
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT,
                           "source page out of range");

  rmr_pdf_page_geometry g = s->geometry[page];
  *out = g.rotate % 180 ? (rmr_point){g.media.height, g.media.width}
                        : (rmr_point){g.media.width, g.media.height};
  return RMR_OK;
}

/* Composes two affine transforms, applying a then b. */
static rmr_transform concat(rmr_transform a, rmr_transform b) {
  return (rmr_transform){
      b.a * a.a + b.c * a.b,          b.b * a.a + b.d * a.b,
      b.a * a.c + b.c * a.d,          b.b * a.c + b.d * a.d,
      b.a * a.tx + b.c * a.ty + b.tx, b.b * a.tx + b.d * a.ty + b.ty};
}

/* Maps a source page's media box, through its rotation, into page p's
 * placement rectangle and sheet transform. */
static rmr_transform placement(rmr_pdf_page_geometry g, const rmr_pdf_page* p) {
  rmr_rect m = g.media, r = p->source_rect;
  rmr_transform t;

  switch (g.rotate) {
    case 90:
      t = (rmr_transform){0, 1 / m.width,     1 / m.height,
                          0, -m.y / m.height, -m.x / m.width};
      break;
    case 180:
      t = (rmr_transform){-1 / m.width,   0, 0, 1 / m.height, 1 + m.x / m.width,
                          -m.y / m.height};
      break;
    case 270:
      t = (rmr_transform){0, -1 / m.width,       -1 / m.height,
                          0, 1 + m.y / m.height, 1 + m.x / m.width};
      break;
    default:
      t = (rmr_transform){
          1 / m.width, 0, 0, -1 / m.height, -m.x / m.width, 1 + m.y / m.height};
      break;
  }

  return concat(concat(t, (rmr_transform){r.width, 0, 0, r.height, r.x, r.y}),
                p->sheet.transform);
}

/* Returns the inverse of an invertible affine transform. */
static rmr_transform inverse(rmr_transform t) {
  double det = t.a * t.d - t.b * t.c;
  return (rmr_transform){t.d / det,
                         -t.b / det,
                         -t.c / det,
                         t.a / det,
                         (t.c * t.ty - t.d * t.tx) / det,
                         (t.b * t.tx - t.a * t.ty) / det};
}

/* Returns the axis-aligned bounding box of a w by h rectangle under
 * transform t. */
static rmr_rect box(rmr_transform t, double w, double h) {
  double x[] = {t.tx, t.a * w + t.tx, t.c * h + t.tx, t.a * w + t.c * h + t.tx},
         y[] = {t.ty, t.b * w + t.ty, t.d * h + t.ty, t.b * w + t.d * h + t.ty};
  double left = x[0], right = x[0], bottom = y[0], top = y[0];

  for (size_t i = 1; i < 4; i++) {
    left = fmin(left, x[i]);
    right = fmax(right, x[i]);
    bottom = fmin(bottom, y[i]);
    top = fmax(top, y[i]);
  }

  return (rmr_rect){left, bottom, right - left, top - bottom};
}

rmr_status rmr_linux_pdf_incremental_marks(
    const rmr_linux_pdf_source* source, const rmr_pdf_page* pages, size_t count,
    const rmr_pdf_marks* marks, const rmr_limits* given,
    const rmr_pdf_options* given_options, rmr_cancel_fn cancel, void* ctx,
    rmr_buffer* out, rmr_pdf_incremental_report* report, rmr_error* e) {
  if (!source || !pages || !count || !out)
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT, "missing PDF pages");
  rmr_pdf_options options;
  if (!rmr_pdf_options_read(given_options, &options))
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT, "invalid PDF options");
  if (count != source->count)
    return rmr_linux_error(e, RMR_ERR_UNSUPPORTED,
                           "incremental output preserves page count/order");

  rmr_limits l = given ? *given : rmr_limits_default();
  if (count > l.maximum_draws ||
      count >
          l.maximum_plan_bytes / (sizeof(rmr_pdf_overlay) + sizeof(rmr_buffer)))
    return rmr_linux_error(e, RMR_ERR_LIMIT, "PDF allocation limit");

  rmr_pdf_overlay* overlays = calloc(count, sizeof *overlays);
  rmr_buffer* pdfs = calloc(count, sizeof *pdfs);
  if (!overlays || !pdfs) {
    free(overlays);
    free(pdfs);
    return rmr_linux_error(e, RMR_ERR_OUT_OF_MEMORY, "allocation failed");
  }
  l.maximum_plan_bytes -= count * (sizeof *overlays + sizeof *pdfs);

  size_t used = 0;
  rmr_status s = RMR_OK;
  bool reported = false; /* set once a marks or PDF call has filled e */
  for (size_t i = 0; i < count && s == RMR_OK; i++) {
    const rmr_pdf_page* p = &pages[i];
    if (cancel && cancel(ctx)) {
      s = RMR_ERR_CANCELLED;
      break;
    }

    if (!p->has_source || !p->source_visible || p->source_page != i) {
      s = RMR_ERR_UNSUPPORTED;
      break;
    }

    rmr_rect r = p->source_rect;
    if ((!marks && !p->marks) || !isfinite(r.x) || !isfinite(r.y) ||
        !isfinite(r.width) || !isfinite(r.height) || r.width <= 0 ||
        r.height <= 0) {
      s = RMR_ERR_INVALID_ARGUMENT;
      break;
    }

    const rmr_plan* plan = nullptr;
    s = rmr_pdf_marks_acquire(marks, pages, i, &plan, e);
    reported = s != RMR_OK;
    if (s != RMR_OK) break;
    if (!rmr_plan_count(plan)) {
      rmr_pdf_marks_release(marks, i);
      continue;
    }
    rmr_transform t = inverse(placement(source->geometry[i], p));
    s = rmr_linux_pdf_sheets_with_options(&plan, &p->sheet, 1, &l, &options,
                                          cancel, ctx, &pdfs[used], e);
    rmr_pdf_marks_release(marks, i);
    reported = s != RMR_OK;
    if (s != RMR_OK) break;
    if (pdfs[used].size > l.maximum_plan_bytes) {
      s = RMR_ERR_LIMIT;
      break;
    }
    l.maximum_plan_bytes -= pdfs[used].size;

    rmr_rect b = box(t, p->sheet.width, p->sheet.height),
             m = source->geometry[i].media;
    double left = fmin(b.x, m.x), bottom = fmin(b.y, m.y),
           right = fmax(b.x + b.width, m.x + m.width),
           top = fmax(b.y + b.height, m.y + m.height);
    overlays[used] =
        (rmr_pdf_overlay){.page = i,
                          .pdf = pdfs[used].bytes,
                          .size = pdfs[used].size,
                          .transform = t,
                          .replace_box = b.x < m.x - .01 || b.y < m.y - .01 ||
                                         b.x + b.width > m.x + m.width + .01 ||
                                         b.y + b.height > m.y + m.height + .01,
                          .box = {left, bottom, right - left, top - bottom}};
    used++;
  }

  if (s == RMR_OK) {
    s = rmr_pdf_incremental(source->data.bytes, source->data.size, overlays,
                            used, &l, cancel, ctx, out, report, e);
    reported = s != RMR_OK;
  }

  for (size_t i = 0; i < count; i++) rmr_buffer_free(&pdfs[i]);
  free(pdfs);
  free(overlays);
  return s == RMR_OK || reported
             ? s
             : rmr_linux_error(e, s, "incremental PDF export failed");
}

rmr_status rmr_linux_pdf_incremental(
    const rmr_linux_pdf_source* source, const rmr_pdf_page* pages, size_t count,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* ctx, rmr_buffer* out,
    rmr_pdf_incremental_report* report, rmr_error* e) {
  return rmr_linux_pdf_incremental_marks(source, pages, count, nullptr, limits,
                                         nullptr, cancel, ctx, out, report, e);
}

rmr_status rmr_linux_pdf_incremental_with_options(
    const rmr_linux_pdf_source* source, const rmr_pdf_page* pages, size_t count,
    const rmr_limits* limits, const rmr_pdf_options* options,
    rmr_cancel_fn cancel, void* ctx, rmr_buffer* out,
    rmr_pdf_incremental_report* report, rmr_error* e) {
  return rmr_linux_pdf_incremental_marks(source, pages, count, nullptr, limits,
                                         options, cancel, ctx, out, report, e);
}

typedef struct {
  rmr_pdf_local_link* links;
  rmr_pdf_outline* outline;
  FPDF_BOOKMARK* bookmarks;
  size_t* page_map;
  size_t link_count, outline_count, bytes, work;
} navigation;

/* Frees a navigation collection's outline titles and arrays. */
static void navigation_free(navigation* n) {
  for (size_t i = 0; i < n->outline_count; i++)
    free((void*)(uintptr_t)n->outline[i].title);
  free(n->links);
  free(n->outline);
  free(n->bookmarks);
  free(n->page_map);
}

/* Resolves dest to an output page and point through source geometry and the
 * target page's placement; returns false when dest is absent or its page is
 * not visible in the output. */
static bool local_destination(FPDF_DOCUMENT src, FPDF_DEST dest,
                              const rmr_linux_pdf_source* source,
                              const rmr_pdf_page* pages, size_t count,
                              size_t current, const size_t* page_map,
                              rmr_pdf_destination* out) {
  if (!dest) return false;
  int index = FPDFDest_GetDestPageIndex(src, dest);
  if (index < 0 || (size_t)index >= source->count) return false;

  size_t target = SIZE_MAX;
  if (current < count && pages[current].has_source &&
      pages[current].source_visible &&
      pages[current].source_page == (size_t)index)
    target = current;
  else
    target = page_map[index];
  if (target == SIZE_MAX) return false;

  rmr_rect m = source->geometry[index].media;
  FPDF_BOOL has_x = 0, has_y = 0, has_zoom = 0;
  float x = 0, y = 0, zoom = 0;
  FPDFDest_GetLocationInPage(dest, &has_x, &has_y, &has_zoom, &x, &y, &zoom);
  double px = has_x ? x : m.x, py = has_y ? y : m.y + m.height;
  if (!isfinite(px) || !isfinite(py)) return false;

  rmr_transform t = placement(source->geometry[index], &pages[target]);
  *out = (rmr_pdf_destination){
      target, {t.a * px + t.c * py + t.tx, t.b * px + t.d * py + t.ty}};
  return true;
}

/* Returns link's destination, following a GoTo action when the link has no
 * direct destination. */
static FPDF_DEST link_destination(FPDF_DOCUMENT doc, FPDF_LINK link) {
  FPDF_DEST dest = FPDFLink_GetDest(doc, link);
  FPDF_ACTION action = FPDFLink_GetAction(link);
  if (!dest && action && FPDFAction_GetType(action) == PDFACTION_GOTO)
    dest = FPDFAction_GetDest(doc, action);
  return dest;
}

/* Collects preserved local links and the bookmark outline from src, remapped
 * onto the output pages described by pages/count. */
static rmr_status navigation_collect(navigation* n, FPDF_DOCUMENT src,
                                     const rmr_linux_pdf_source* source,
                                     const rmr_pdf_page* pages, size_t count,
                                     const rmr_limits* l, rmr_cancel_fn cancel,
                                     void* ctx, rmr_pdf_report* stats) {
  if (source->count > l->maximum_plan_bytes / sizeof *n->page_map)
    return RMR_ERR_LIMIT;
  n->bytes = source->count * sizeof *n->page_map;
  n->page_map = malloc(n->bytes);
  if (!n->page_map) return RMR_ERR_OUT_OF_MEMORY;
  for (size_t i = 0; i < source->count; i++) n->page_map[i] = SIZE_MAX;
  for (size_t i = 0; i < count; i++)
    if (pages[i].has_source && pages[i].source_visible &&
        n->page_map[pages[i].source_page] == SIZE_MAX)
      n->page_map[pages[i].source_page] = i;

  for (size_t i = 0; i < count; i++) {
    if (cancel && cancel(ctx)) return RMR_ERR_CANCELLED;
    if (!pages[i].has_source || !pages[i].source_visible) continue;

    FPDF_PAGE page = FPDF_LoadPage(src, (int)pages[i].source_page);
    if (!page) return RMR_ERR_BACKEND;

    int pos = 0;
    FPDF_LINK link;
    rmr_status status = RMR_OK;
    while (FPDFLink_Enumerate(page, &pos, &link)) {
      if (++n->work > l->maximum_elements) {
        status = RMR_ERR_LIMIT;
        break;
      }

      FPDF_DEST dest = link_destination(src, link);
      if (!dest)
        continue; /* URI/unsupported links counted by the composition pass. */

      rmr_pdf_local_link entry = {.page = i};
      FS_RECTF rect;
      if (!local_destination(src, dest, source, pages, count, i, n->page_map,
                             &entry.destination) ||
          !FPDFLink_GetAnnotRect(link, &rect)) {
        stats->links_dropped++;
        continue;
      }
      rmr_transform t =
          placement(source->geometry[pages[i].source_page], &pages[i]);
      t.tx += t.a * rect.left + t.c * rect.bottom;
      t.ty += t.b * rect.left + t.d * rect.bottom;
      entry.rect = box(t, rect.right - rect.left, rect.top - rect.bottom);
      if (sizeof entry > l->maximum_plan_bytes - n->bytes) {
        status = RMR_ERR_LIMIT;
        break;
      }

      void* next = realloc(n->links, (n->link_count + 1) * sizeof entry);
      if (!next) {
        status = RMR_ERR_OUT_OF_MEMORY;
        break;
      }
      n->links = next;
      n->links[n->link_count++] = entry;
      n->bytes += sizeof entry;
      stats->links_preserved++;
    }
    FPDF_ClosePage(page);
    if (status != RMR_OK) return status;
  }

  /* Breadth first traversal keeps parents before children and bounds cyclic
   * trees. */
  size_t parent = SIZE_MAX;
  for (;;) {
    FPDF_BOOKMARK b = FPDFBookmark_GetFirstChild(
        src, parent == SIZE_MAX ? nullptr : n->bookmarks[parent]);
    while (b) {
      if (cancel && cancel(ctx)) return RMR_ERR_CANCELLED;
      if (++n->work > l->maximum_elements) return RMR_ERR_LIMIT;

      unsigned long length = FPDFBookmark_GetTitle(b, nullptr, 0);
      size_t fixed = sizeof(rmr_pdf_outline) + sizeof(FPDF_BOOKMARK);
      if (length < 2 || length % 2 ||
          length > l->maximum_plan_bytes - n->bytes ||
          fixed > l->maximum_plan_bytes - n->bytes - length)
        return RMR_ERR_LIMIT;

      uint8_t* title = malloc(length);
      if (!title) return RMR_ERR_OUT_OF_MEMORY;
      if (FPDFBookmark_GetTitle(b, title, length) != length ||
          title[length - 1] || title[length - 2]) {
        free(title);
        return RMR_ERR_INVALID_ARGUMENT;
      }
      void* o =
          realloc(n->outline, (n->outline_count + 1) * sizeof *n->outline);
      if (!o) {
        free(title);
        return RMR_ERR_OUT_OF_MEMORY;
      }
      n->outline = o;
      void* handles =
          realloc(n->bookmarks, (n->outline_count + 1) * sizeof *n->bookmarks);
      if (!handles) {
        free(title);
        return RMR_ERR_OUT_OF_MEMORY;
      }
      n->bookmarks = handles;

      rmr_pdf_outline entry = {.parent = parent,
                               .title = title,
                               .title_size = length - 2,
                               .destination = {.page = SIZE_MAX},
                               .open = FPDFBookmark_GetCount(b) >= 0};
      FPDF_DEST dest = FPDFBookmark_GetDest(src, b);
      FPDF_ACTION action = FPDFBookmark_GetAction(b);
      if (!dest && action && FPDFAction_GetType(action) == PDFACTION_GOTO)
        dest = FPDFAction_GetDest(src, action);
      local_destination(src, dest, source, pages, count, SIZE_MAX, n->page_map,
                        &entry.destination);

      n->outline[n->outline_count] = entry;
      n->bookmarks[n->outline_count++] = b;
      n->bytes += fixed + length;
      b = FPDFBookmark_GetNextSibling(src, b);
    }

    parent = parent == SIZE_MAX ? 0 : parent + 1;
    if (parent >= n->outline_count) break;
  }

  return RMR_OK;
}

rmr_status rmr_linux_pdf_overlay_marks(
    const rmr_linux_pdf_source* source, const rmr_pdf_page* pages, size_t count,
    const rmr_pdf_marks* provider, const rmr_limits* given,
    const rmr_pdf_options* given_options, rmr_cancel_fn cancel, void* ctx,
    rmr_buffer* out, rmr_pdf_report* report, rmr_error* e) {
  if (!pages || !count || !out)
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT, "missing PDF pages");
  rmr_pdf_options options;
  if (!rmr_pdf_options_read(given_options, &options))
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT, "invalid PDF options");

  rmr_limits l = given ? *given : rmr_limits_default();
  if (count > l.maximum_draws || count > INT_MAX)
    return rmr_linux_error(e, RMR_ERR_LIMIT, "PDF page limit");

  rmr_buffer result = {0};
  rmr_status s = RMR_OK;
  bool reported = false; /* set once a marks or drawing call has filled e */
  rmr_pdf_report stats = {0};

  rmr_pdfium_lock();
  FPDF_DOCUMENT dest = FPDF_CreateNewDocument(),
                src = source
                          ? FPDF_LoadMemDocument64(source->data.bytes,
                                                   source->data.size, nullptr)
                          : nullptr;
  if (!dest || (source && !src)) s = RMR_ERR_BACKEND;

  for (size_t i = 0; i < count && s == RMR_OK; i++) {
    if (cancel && cancel(ctx)) {
      s = RMR_ERR_CANCELLED;
      break;
    }

    const rmr_pdf_page* p = &pages[i];
    if ((!provider && !p->marks) ||
        (p->has_source && (!source || p->source_page >= source->count))) {
      s = RMR_ERR_INVALID_ARGUMENT;
      break;
    }

    const rmr_plan* plan = nullptr;
    s = rmr_pdf_marks_acquire(provider, pages, i, &plan, e);
    reported = s != RMR_OK;
    if (s != RMR_OK) break;
    rmr_buffer overlay = {0};
    s = rmr_linux_render_pdf(&plan, &p->sheet, 1, &l, options.grain, cancel,
                             ctx, &overlay, e);
    rmr_pdf_marks_release(provider, i);
    reported = s != RMR_OK;
    if (s != RMR_OK) break;

    /* The page is composed in a document of its own, then imported: content
     * generation walks every object of the document holding the page, so
     * composing it in dest would cost the whole output so far, page after
     * page. */
    FPDF_DOCUMENT marks =
        FPDF_LoadMemDocument64(overlay.bytes, overlay.size, nullptr);
    FPDF_DOCUMENT single = FPDF_CreateNewDocument();
    FPDF_PAGE page =
        single ? FPDFPage_New(single, 0, p->sheet.width, p->sheet.height)
               : nullptr;
    if (!marks || !page) s = RMR_ERR_BACKEND;

    if (s == RMR_OK && p->has_source && p->source_visible) {
      rmr_rect r = p->source_rect;
      if (!isfinite(r.width) || !isfinite(r.height) || !isfinite(r.x) ||
          !isfinite(r.y) || r.width <= 0 || r.height <= 0)
        s = RMR_ERR_INVALID_ARGUMENT;
      else {
        rmr_transform t = placement(source->geometry[p->source_page], p);
        s = rmr_pdfium_form(single, page, src, p->source_page, t);
        FPDF_PAGE original = FPDF_LoadPage(src, (int)p->source_page);
        if (!original)
          s = RMR_ERR_BACKEND;
        else {
          rmr_rect media = source->geometry[p->source_page].media;
          FS_RECTF crop;
          if (FPDF_GetPageBoundingBox(original, &crop) &&
              (fabs(crop.left - media.x) > .001 ||
               fabs(crop.bottom - media.y) > .001 ||
               fabs(crop.right - media.x - media.width) > .001 ||
               fabs(crop.top - media.y - media.height) > .001)) {
            rmr_transform shifted = t;
            shifted.tx += t.a * crop.left + t.c * crop.bottom;
            shifted.ty += t.b * crop.left + t.d * crop.bottom;
            rmr_rect b =
                box(shifted, crop.right - crop.left, crop.top - crop.bottom);
            FPDFPage_SetCropBox(page, (float)b.x, (float)b.y,
                                (float)(b.x + b.width),
                                (float)(b.y + b.height));
          }

          int pos = 0;
          FPDF_LINK link;
          size_t work = 0;
          while (s == RMR_OK && FPDFLink_Enumerate(original, &pos, &link)) {
            if (work++ >= l.maximum_elements) {
              s = RMR_ERR_LIMIT;
              break;
            }

            if (link_destination(src, link)) continue;
            FPDF_ACTION action = FPDFLink_GetAction(link);
            unsigned long length =
                action ? FPDFAction_GetURIPath(src, action, nullptr, 0) : 0;
            FS_RECTF rect;
            if (!action || FPDFAction_GetType(action) != PDFACTION_URI ||
                !length || length > l.maximum_plan_bytes || length > 65536 ||
                !FPDFLink_GetAnnotRect(link, &rect)) {
              stats.links_dropped++;
              continue;
            }

            char* uri = malloc(length);
            if (!uri) {
              s = RMR_ERR_OUT_OF_MEMORY;
              break;
            }
            if (FPDFAction_GetURIPath(src, action, uri, length) != length ||
                uri[length - 1] != 0) {
              free(uri);
              stats.links_dropped++;
              continue;
            }

            rmr_transform shifted = t;
            shifted.tx += t.a * rect.left + t.c * rect.bottom;
            shifted.ty += t.b * rect.left + t.d * rect.bottom;
            rmr_rect b =
                box(shifted, rect.right - rect.left, rect.top - rect.bottom);
            FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_LINK);
            FS_RECTF target = {(float)b.x, (float)(b.y + b.height),
                               (float)(b.x + b.width), (float)b.y};
            if (!annot || !FPDFAnnot_SetRect(annot, &target) ||
                !FPDFAnnot_SetURI(annot, uri))
              s = RMR_ERR_BACKEND;
            else
              stats.links_preserved++;
            if (annot) FPDFPage_CloseAnnot(annot);
            free(uri);
          }
          FPDF_ClosePage(original);
        }
      }
    }

    if (s == RMR_OK)
      s = rmr_pdfium_form(single, page, marks, 0,
                          (rmr_transform){1, 0, 0, 1, 0, 0});
    if (s == RMR_OK && !FPDFPage_GenerateContent(page)) s = RMR_ERR_BACKEND;
    if (page) FPDF_ClosePage(page);
    int first = 0;
    if (s == RMR_OK &&
        !FPDF_ImportPagesByIndex(dest, single, &first, 1, (int)i))
      s = RMR_ERR_BACKEND;
    if (single) FPDF_CloseDocument(single);
    if (marks) FPDF_CloseDocument(marks);
    rmr_buffer_free(&overlay);
  }

  if (s == RMR_OK) s = rmr_pdfium_save(dest, &l, &result);

  navigation nav = {0};
  if (s == RMR_OK && src)
    s = navigation_collect(&nav, src, source, pages, count, &l, cancel, ctx,
                           &stats);

  if (s == RMR_OK && (nav.link_count || nav.outline_count)) {
    rmr_buffer edited = {0};
    rmr_limits remaining = l;
    remaining.maximum_plan_bytes -= nav.bytes;
    s = rmr_pdf_navigation(result.bytes, result.size, nav.links, nav.link_count,
                           nav.outline, nav.outline_count, &remaining, cancel,
                           ctx, &edited);
    if (s == RMR_OK) {
      rmr_buffer_free(&result);
      result = edited;
    }
  }

  navigation_free(&nav);
  if (src) FPDF_CloseDocument(src);
  if (dest) FPDF_CloseDocument(dest);
  rmr_pdfium_unlock();

  if (s == RMR_OK) {
    *out = result;
    if (report) *report = stats;
  } else
    rmr_buffer_free(&result);
  return s == RMR_OK || reported
             ? s
             : rmr_linux_error(e, s, "PDF composition failed");
}

rmr_status rmr_linux_pdf_overlay(const rmr_linux_pdf_source* source,
                                 const rmr_pdf_page* pages, size_t count,
                                 const rmr_limits* limits, rmr_cancel_fn cancel,
                                 void* ctx, rmr_buffer* out,
                                 rmr_pdf_report* report, rmr_error* e) {
  return rmr_linux_pdf_overlay_marks(source, pages, count, nullptr, limits,
                                     nullptr, cancel, ctx, out, report, e);
}

rmr_status rmr_linux_pdf_overlay_with_options(
    const rmr_linux_pdf_source* source, const rmr_pdf_page* pages, size_t count,
    const rmr_limits* limits, const rmr_pdf_options* options,
    rmr_cancel_fn cancel, void* ctx, rmr_buffer* out, rmr_pdf_report* report,
    rmr_error* e) {
  return rmr_linux_pdf_overlay_marks(source, pages, count, nullptr, limits,
                                     options, cancel, ctx, out, report, e);
}

const uint8_t* rmr_linux_pdf_source_data(const rmr_linux_pdf_source* s) {
  return s->data.bytes;
}

size_t rmr_linux_pdf_source_length(const rmr_linux_pdf_source* s) {
  return s->data.size;
}
