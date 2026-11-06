/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <limits.h>
#include <pango/pangocairo.h>
#include <rmrender/pdf.h>

#include "../../src/rmr_pdf_edit.h"
#include "../common/rmr_pdf_options.h"
#include "rmr_linux_internal.h"

typedef struct {
  cairo_t* cr;
  cairo_surface_t* surface;
  rmr_sink sink;
  rmr_limits limits;
  rmr_sheet sheet;
  uint64_t group;
  double alpha;
  rmr_blend blend;
  FPDF_DOCUMENT pdf;
  FPDF_PAGE page;
  rmr_pdf_grain grain;
  bool dirty, clip_viewport;
} drawing;

/* Maps a blend mode to the matching Cairo compositing operator. */
static cairo_operator_t blend(rmr_blend b) {
  return b == RMR_BLEND_DARKEN     ? CAIRO_OPERATOR_DARKEN
         : b == RMR_BLEND_MULTIPLY ? CAIRO_OPERATOR_MULTIPLY
                                   : CAIRO_OPERATOR_OVER;
}

/* Sets the context's source color from an RMR color. */
static void color(cairo_t* cr, rmr_color c) {
  cairo_set_source_rgba(cr, c.red, c.green, c.blue, c.alpha);
}

/* Replays a path view's elements into the context as a new path. */
static void path(cairo_t* cr, rmr_path_view p) {
  cairo_new_path(cr);
  for (size_t i = 0; i < p.count; i++) {
    const rmr_element* v = &p.elements[i];
    switch (v->kind) {
      case RMR_MOVE:
        cairo_move_to(cr, v->end.x, v->end.y);
        break;
      case RMR_LINE:
        cairo_line_to(cr, v->end.x, v->end.y);
        break;
      case RMR_CURVE:
        cairo_curve_to(cr, v->control1.x, v->control1.y, v->control2.x,
                       v->control2.y, v->end.x, v->end.y);
        break;
      case RMR_CLOSE:
        cairo_close_path(cr);
        break;
    }
  }
}

/* Computes the ceiling pixel width and height for a surface of size w by
 * h, rejecting non-finite or non-positive sizes and sizes that exceed the
 * side or byte limits in l. */
static rmr_status dimensions(double w, double h, const rmr_limits* l,
                             size_t* width, size_t* height) {
  if (!isfinite(w) || !isfinite(h) || w <= 0 || h <= 0)
    return RMR_ERR_INVALID_ARGUMENT;
  if (w > l->maximum_bitmap_side || h > l->maximum_bitmap_side || w > INT_MAX ||
      h > INT_MAX)
    return RMR_ERR_LIMIT;

  *width = (size_t)ceil(w);
  *height = (size_t)ceil(h);
  if (*width > 64000000 / *height ||
      *width > l->maximum_bitmap_bytes / 4 / *height)
    return RMR_ERR_LIMIT;
  return RMR_OK;
}

/* Flattens the active transparency group onto the context using the
 * stored blend mode and alpha, if a group is currently open. */
static void group_end(drawing* c) {
  if (!c->group) return;
  cairo_pop_group_to_source(c->cr);
  cairo_set_operator(c->cr, blend(c->blend));
  cairo_paint_with_alpha(c->cr, c->alpha);
  c->group = 0;
}

/* Applies the sheet's transform to the context, inverting the vertical
 * axis against the sheet height so .rm coordinates land correctly in
 * Cairo's top-down space. */
static void setup(drawing* c) {
  cairo_matrix_t m = {
      c->sheet.transform.a,  -c->sheet.transform.b,
      c->sheet.transform.c,  -c->sheet.transform.d,
      c->sheet.transform.tx, c->sheet.height - c->sheet.transform.ty};
  cairo_transform(c->cr, &m);
}

/* Begins a new single-page Cairo PDF surface writing into c's sink, with
 * the sheet transform already applied. */
static void segment_start(drawing* c) {
  c->sink = (rmr_sink){.maximum = c->limits.maximum_pdf_bytes};
  c->surface = cairo_pdf_surface_create_for_stream(
      rmr_linux_write, &c->sink, c->sheet.width, c->sheet.height);
  c->cr = cairo_create(c->surface);
  setup(c);
  c->dirty = false;
}

/* Closes the current page's Cairo PDF surface and, if anything was
 * drawn, loads the resulting PDF bytes and forms them as a page object
 * into the destination page. Returns the first failure encountered. */
static rmr_status segment_end(drawing* c) {
  group_end(c);
  cairo_show_page(c->cr);
  cairo_status_t cs = cairo_status(c->cr);
  cairo_destroy(c->cr);
  c->cr = nullptr;
  cairo_surface_finish(c->surface);
  if (cs == CAIRO_STATUS_SUCCESS) cs = cairo_surface_status(c->surface);
  cairo_surface_destroy(c->surface);
  c->surface = nullptr;

  rmr_status s = c->sink.status;
  if (s == RMR_OK && cs != CAIRO_STATUS_SUCCESS) s = RMR_ERR_BACKEND;

  if (s == RMR_OK && c->dirty) {
    FPDF_DOCUMENT doc =
        FPDF_LoadMemDocument64(c->sink.data.bytes, c->sink.data.size, nullptr);
    s = doc ? rmr_pdfium_form(c->pdf, c->page, doc, 0,
                              (rmr_transform){1, 0, 0, 1, 0, 0})
            : RMR_ERR_BACKEND;
    if (doc) FPDF_CloseDocument(doc);
  }

  rmr_buffer_free(&c->sink.data);
  return s;
}

PangoLayout* rmr_linux_label(cairo_t* cr, const char* text, size_t size,
                             double font_size) {
  PangoLayout* p = pango_cairo_create_layout(cr);
  PangoFontDescription* f = pango_font_description_new();
  pango_font_description_set_family(
      f, "Helvetica,Arial,Liberation Sans,Noto Sans,sans-serif");
  pango_font_description_set_absolute_size(f, font_size * PANGO_SCALE);
  pango_layout_set_font_description(p, f);
  pango_font_description_free(f);
  pango_layout_set_text(p, text, (int)size);
  pango_layout_set_single_paragraph_mode(p, TRUE);
  return p;
}

/* Computes ink and logical text metrics for text via Pango, rejecting
 * invalid UTF-8, oversized text, and non-finite or out-of-range font
 * sizes. */
static rmr_status measure(void* ctx, const char* text, size_t size,
                          double font_size, rmr_text_metrics* out) {
  (void)ctx;
  if (!out || !text || size > 65536 || !isfinite(font_size) || font_size <= 0 ||
      font_size > 100000 || !g_utf8_validate(text, (gssize)size, nullptr))
    return RMR_ERR_INVALID_ARGUMENT;

  cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  cairo_t* cr = cairo_create(s);
  PangoLayout* p = rmr_linux_label(cr, text, size, font_size);
  PangoRectangle ink, logical;
  pango_layout_get_extents(p, &ink, &logical);
  double base = (double)pango_layout_get_baseline(p) / PANGO_SCALE;

  double x = fmin(ink.x, logical.x) / (double)PANGO_SCALE,
         y = fmin(ink.y, logical.y) / (double)PANGO_SCALE - base;
  double right =
      fmax(ink.x + ink.width, logical.x + logical.width) / (double)PANGO_SCALE;
  double bottom = fmax(ink.y + ink.height, logical.y + logical.height) /
                      (double)PANGO_SCALE -
                  base;
  *out = (rmr_text_metrics){(double)logical.width / PANGO_SCALE,
                            {x, y, right - x, bottom - y}};

  g_object_unref(p);
  cairo_destroy(cr);
  cairo_surface_destroy(s);
  return RMR_OK;
}

rmr_text_provider rmr_linux_text_provider(void) {
  return (rmr_text_provider){RMR_TEXT_PROVIDER_ABI, sizeof(rmr_text_provider),
                             nullptr, measure};
}

typedef struct {
  const rmr_limits* limits;
  rmr_status status;
} image_size;

/* GdkPixbufLoader size-prepared callback: checks w by h against the
 * bitmap limits and, if they are exceeded, shrinks the decode target to
 * 1x1 to abort the load early. */
static void prepared(GdkPixbufLoader* loader, int w, int h, void* data) {
  image_size* s = data;
  size_t x, y;
  s->status = dimensions(w, h, s->limits, &x, &y);
  if (s->status != RMR_OK) gdk_pixbuf_loader_set_size(loader, 1, 1);
}

/* Decodes a PNG or JPEG image into a premultiplied ARGB32 Cairo surface,
 * enforcing l's size limits. Returns RMR_ERR_BACKEND for an unrecognized
 * or undecodable format. */
static rmr_status image_surface(const rmr_image_view* im, const rmr_limits* l,
                                cairo_surface_t** out) {
  const char* type =
      im->size >= 8 && !memcmp(im->bytes, "\x89PNG\r\n\x1a\n", 8)   ? "png"
      : im->size >= 2 && im->bytes[0] == 255 && im->bytes[1] == 216 ? "jpeg"
                                                                    : nullptr;
  if (!type) return RMR_ERR_BACKEND;

  GdkPixbufLoader* loader = gdk_pixbuf_loader_new_with_type(type, nullptr);
  if (!loader) return RMR_ERR_BACKEND;

  image_size check = {l, RMR_OK};
  g_signal_connect(loader, "size-prepared", G_CALLBACK(prepared), &check);
  bool ok = gdk_pixbuf_loader_write(loader, im->bytes, im->size, nullptr) != 0;
  bool closed = gdk_pixbuf_loader_close(loader, nullptr) != 0;
  GdkPixbuf* pix = ok && closed && check.status == RMR_OK
                       ? gdk_pixbuf_loader_get_pixbuf(loader)
                       : nullptr;
  rmr_status status = check.status;
  if (status == RMR_OK && !pix) status = RMR_ERR_BACKEND;

  if (status == RMR_OK) {
    int w = gdk_pixbuf_get_width(pix), h = gdk_pixbuf_get_height(pix),
        channels = gdk_pixbuf_get_n_channels(pix),
        stride = gdk_pixbuf_get_rowstride(pix);
    cairo_surface_t* surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
      status = RMR_ERR_OUT_OF_MEMORY;
    else {
      unsigned char *src = gdk_pixbuf_get_pixels(pix),
                    *dst = cairo_image_surface_get_data(surface);
      int pitch = cairo_image_surface_get_stride(surface);
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
          const uint8_t* v =
              src + (size_t)y * (size_t)stride + (size_t)x * (size_t)channels;
          uint32_t a = channels == 4 ? v[3] : 255;
          uint32_t pixel = (a << 24) | (((v[0] * a + 127) / 255) << 16) |
                           (((v[1] * a + 127) / 255) << 8) |
                           ((v[2] * a + 127) / 255);
          memcpy(dst + (size_t)y * (size_t)pitch + (size_t)x * 4, &pixel, 4);
        }
      cairo_surface_mark_dirty(surface);
      *out = surface;
    }
    if (status != RMR_OK) cairo_surface_destroy(surface);
  }

  g_object_unref(loader);
  return status;
}

/* Paints an embedded PDF page. When c has a destination PDF, forms the
 * source page directly into it with blending and clipping applied via a
 * wrapper document; otherwise rasterizes the page to a bitmap and
 * composites it as an image. Must be called with the PDFium lock held. */
static rmr_status paint_pdf_locked(drawing* c, const rmr_draw* d) {
  if (d->image.size > c->limits.maximum_pdf_bytes || d->pdf_page > INT_MAX)
    return RMR_ERR_LIMIT;

  FPDF_DOCUMENT doc =
      FPDF_LoadMemDocument64(d->image.bytes, d->image.size, nullptr);
  if (!doc) return RMR_ERR_BACKEND;

  FPDF_PAGE page = FPDF_LoadPage(doc, (int)d->pdf_page);
  FS_RECTF page_box;
  rmr_status s = page && FPDF_GetPageBoundingBox(page, &page_box) &&
                         page_box.right > page_box.left &&
                         page_box.top > page_box.bottom
                     ? RMR_OK
                     : RMR_ERR_BACKEND;

  if (s == RMR_OK && c->pdf) {
    size_t count = 0;
    rmr_pdf_page_geometry* geometry = nullptr;
    if (s == RMR_OK)
      s = rmr_pdf_pages(d->image.bytes, d->image.size, &c->limits, nullptr, 0,
                        &count, nullptr);
    if (s == RMR_OK &&
        (d->pdf_page >= count ||
         count > c->limits.maximum_plan_bytes / sizeof *geometry))
      s = RMR_ERR_LIMIT;
    if (s == RMR_OK) {
      geometry = calloc(count, sizeof *geometry);
      if (!geometry) s = RMR_ERR_OUT_OF_MEMORY;
    }
    if (s == RMR_OK)
      s = rmr_pdf_pages(d->image.bytes, d->image.size, &c->limits, geometry,
                        count, &count, nullptr);
    if (s == RMR_OK && !geometry[d->pdf_page].has_media)
      s = RMR_ERR_UNSUPPORTED;
    if (s == RMR_OK) s = segment_end(c);

    if (s == RMR_OK) {
      rmr_pdf_page_geometry g = geometry[d->pdf_page];
      rmr_rect m = g.media;
      rmr_transform n;
      switch (g.rotate) {
        case 90:
          n = (rmr_transform){0, 1 / m.width,     1 / m.height,
                              0, -m.y / m.height, -m.x / m.width};
          break;
        case 180:
          n = (rmr_transform){
              -1 / m.width,   0, 0, 1 / m.height, 1 + m.x / m.width,
              -m.y / m.height};
          break;
        case 270:
          n = (rmr_transform){0, -1 / m.width,       -1 / m.height,
                              0, 1 + m.y / m.height, 1 + m.x / m.width};
          break;
        default:
          n = (rmr_transform){
              1 / m.width,       0, 0, -1 / m.height, -m.x / m.width,
              1 + m.y / m.height};
          break;
      }

      rmr_transform t = d->image.transform, q = c->sheet.transform;
      rmr_transform a = {
          t.a * n.a + t.c * n.b,          t.b * n.a + t.d * n.b,
          t.a * n.c + t.c * n.d,          t.b * n.c + t.d * n.d,
          t.a * n.tx + t.c * n.ty + t.tx, t.b * n.tx + t.d * n.ty + t.ty};
      rmr_transform final = {
          q.a * a.a + q.c * a.b,          q.b * a.a + q.d * a.b,
          q.a * a.c + q.c * a.d,          q.b * a.c + q.d * a.d,
          q.a * a.tx + q.c * a.ty + q.tx, q.b * a.tx + q.d * a.ty + q.ty};

      FPDF_DOCUMENT wrapper = FPDF_CreateNewDocument();
      FPDF_PAGE wrap =
          wrapper ? FPDFPage_New(wrapper, 0, c->sheet.width, c->sheet.height)
                  : nullptr;
      if (!wrap)
        s = RMR_ERR_OUT_OF_MEMORY;
      else {
        s = rmr_pdfium_form(wrapper, wrap, doc, d->pdf_page, final);
        if (s == RMR_OK) {
          FPDF_PAGEOBJECT object = FPDFPage_GetObject(wrap, 0);
          FPDFPageObj_SetBlendMode(object,
                                   d->blend == RMR_BLEND_DARKEN     ? "Darken"
                                   : d->blend == RMR_BLEND_MULTIPLY ? "Multiply"
                                                                    : "Normal");
          if (!FPDFPageObj_SetFillColor(
                  object, 0, 0, 0, (unsigned int)lrint(d->color.alpha * 255)) ||
              !FPDFPage_GenerateContent(wrap))
            s = RMR_ERR_BACKEND;

          rmr_buffer raw = {0}, clipped = {0};
          if (s == RMR_OK) s = rmr_pdfium_save(wrapper, &c->limits, &raw);
          if (s == RMR_OK)
            s = rmr_pdf_clip(raw.bytes, raw.size, d->path, q, &c->limits,
                             &clipped);

          if (s == RMR_OK) {
            FPDF_DOCUMENT clipped_doc =
                FPDF_LoadMemDocument64(clipped.bytes, clipped.size, nullptr);
            s = clipped_doc ? rmr_pdfium_form(c->pdf, c->page, clipped_doc, 0,
                                              (rmr_transform){1, 0, 0, 1, 0, 0})
                            : RMR_ERR_BACKEND;
            if (clipped_doc) FPDF_CloseDocument(clipped_doc);
          }

          rmr_buffer_free(&clipped);
          rmr_buffer_free(&raw);
        }
        FPDF_ClosePage(wrap);
      }

      if (wrapper) FPDF_CloseDocument(wrapper);
      segment_start(c);
    }

    free(geometry);
  } else if (s == RMR_OK) {
    rmr_transform t = d->image.transform;
    cairo_matrix_t cm;
    cairo_get_matrix(c->cr, &cm);
    double sx = hypot(cm.xx * t.a + cm.xy * t.b, cm.yx * t.a + cm.yy * t.b);
    double sy = hypot(cm.xx * t.c + cm.xy * t.d, cm.yx * t.c + cm.yy * t.d);
    size_t iw, ih;
    s = dimensions(fmax(1, sx), fmax(1, sy), &c->limits, &iw, &ih);

    if (s == RMR_OK) {
      cairo_surface_t* surf =
          cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)iw, (int)ih);
      if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS)
        s = RMR_ERR_OUT_OF_MEMORY;
      else {
        FPDF_BITMAP bm =
            FPDFBitmap_CreateEx((int)iw, (int)ih, FPDFBitmap_BGRA,
                                cairo_image_surface_get_data(surf),
                                cairo_image_surface_get_stride(surf));
        if (!bm)
          s = RMR_ERR_BACKEND;
        else {
          FPDFBitmap_FillRect(bm, 0, 0, (int)iw, (int)ih, 0);

          size_t count = 0;
          rmr_pdf_page_geometry* geometry = nullptr;
          s = rmr_pdf_pages(d->image.bytes, d->image.size, &c->limits, nullptr,
                            0, &count, nullptr);
          if (s == RMR_OK &&
              (d->pdf_page >= count ||
               count > c->limits.maximum_plan_bytes / sizeof *geometry))
            s = RMR_ERR_LIMIT;
          if (s == RMR_OK) {
            geometry = calloc(count, sizeof *geometry);
            if (!geometry) s = RMR_ERR_OUT_OF_MEMORY;
          }
          if (s == RMR_OK)
            s = rmr_pdf_pages(d->image.bytes, d->image.size, &c->limits,
                              geometry, count, &count, nullptr);
          if (s == RMR_OK && !geometry[d->pdf_page].has_media)
            s = RMR_ERR_UNSUPPORTED;

          if (s == RMR_OK) {
            rmr_pdf_page_geometry g = geometry[d->pdf_page];
            rmr_rect media = g.media;
            rmr_transform n;
            switch (g.rotate) {
              case 90:
                n = (rmr_transform){
                    0, 1 / media.width,         1 / media.height,
                    0, -media.y / media.height, -media.x / media.width};
                break;
              case 180:
                n = (rmr_transform){-1 / media.width,
                                    0,
                                    0,
                                    1 / media.height,
                                    1 + media.x / media.width,
                                    -media.y / media.height};
                break;
              case 270:
                n = (rmr_transform){
                    0, -1 / media.width,           -1 / media.height,
                    0, 1 + media.y / media.height, 1 + media.x / media.width};
                break;
              default:
                n = (rmr_transform){1 / media.width,
                                    0,
                                    0,
                                    -1 / media.height,
                                    -media.x / media.width,
                                    1 + media.y / media.height};
                break;
            }

            n = rmr_pdfium_raw_transform(page, n);
            double pw = FPDF_GetPageWidthF(page),
                   ph = FPDF_GetPageHeightF(page);
            FS_MATRIX matrix = {(float)(n.a * (double)iw),
                                (float)(n.b * (double)ih),
                                (float)(-n.c * (double)iw),
                                (float)(-n.d * (double)ih),
                                (float)((n.tx + n.c * ph) * (double)iw),
                                (float)((n.ty + n.d * ph) * (double)ih)};
            double xx[] = {matrix.e, matrix.a * pw + matrix.e,
                           matrix.c * ph + matrix.e,
                           matrix.a * pw + matrix.c * ph + matrix.e};
            double yy[] = {matrix.f, matrix.b * pw + matrix.f,
                           matrix.d * ph + matrix.f,
                           matrix.b * pw + matrix.d * ph + matrix.f};
            double left = xx[0], right = xx[0], top = yy[0], bottom = yy[0];
            for (size_t k = 1; k < 4; k++) {
              left = fmin(left, xx[k]);
              right = fmax(right, xx[k]);
              top = fmin(top, yy[k]);
              bottom = fmax(bottom, yy[k]);
            }
            FS_RECTF clip = {(float)left, (float)top, (float)right,
                             (float)bottom};

            /* Use the same software glyph rasterizer on macOS and Linux. */
            FPDF_RenderPageBitmapWithMatrix(bm, page, &matrix, &clip,
                                            FPDF_NO_NATIVETEXT);
          }

          free(geometry);
          FPDFBitmap_Destroy(bm);

          /* PDFium BGRA is straight alpha; Cairo ARGB32 is premultiplied. */
          uint8_t* pixels = cairo_image_surface_get_data(surf);
          size_t stride = (size_t)cairo_image_surface_get_stride(surf);
          for (size_t yy = 0; yy < ih; yy++)
            for (size_t xx = 0; xx < iw; xx++) {
              uint8_t* v = pixels + yy * stride + xx * 4;
              for (size_t channel = 0; channel < 3; channel++)
                v[channel] =
                    (uint8_t)(((unsigned)v[channel] * v[3] + 127) / 255);
            }

          cairo_surface_mark_dirty(surf);
          cairo_matrix_t m = {t.a / (double)iw,
                              t.b / (double)iw,
                              t.c / (double)ih,
                              t.d / (double)ih,
                              t.tx,
                              t.ty};
          cairo_transform(c->cr, &m);
          cairo_set_source_surface(c->cr, surf, 0, 0);
          cairo_paint_with_alpha(c->cr, d->color.alpha);
        }
      }
      cairo_surface_destroy(surf);
    }
  }

  if (page) FPDF_ClosePage(page);
  FPDF_CloseDocument(doc);
  return s;
}

/* Paints an embedded PDF page while holding the process-wide PDFium
 * lock. */
static rmr_status paint_pdf(drawing* c, const rmr_draw* d) {
  rmr_pdfium_lock();
  rmr_status s = paint_pdf_locked(c, d);
  rmr_pdfium_unlock();
  return s;
}

/* Backend begin callback: clips the context to the viewport when the
 * drawing was configured to clip it. */
static rmr_status begin(void* ctx, rmr_rect viewport) {
  drawing* c = ctx;
  if (c->clip_viewport) {
    cairo_rectangle(c->cr, viewport.x, viewport.y, viewport.width,
                    viewport.height);
    cairo_clip(c->cr);
  }
  return RMR_OK;
}

/* Backend draw callback: manages the transparency group, applies the
 * draw's stroke/fill/color state, then paints an image, PDF page, mask,
 * text, or vector path, finally cutting out any eraser ink drawn earlier
 * in the same isolated group. */
static rmr_status draw(void* ctx, const rmr_draw* d) {
  drawing* c = ctx;
  if (c->group != d->group) {
    group_end(c);
    if (d->group) {
      cairo_push_group(c->cr);
      c->group = d->group;
      c->alpha = d->group_alpha;
      c->blend = d->group_blend;
    }
  }

  if (d->mode == RMR_PDF && c->pdf) return paint_pdf(c, d);

  cairo_t* cr = c->cr;
  cairo_save(cr);
  cairo_set_operator(cr, blend(d->blend));
  color(cr, d->color);

  bool isolate = d->eraser_count > 0;
  if (isolate) {
    cairo_push_group(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  }

  cairo_set_line_width(cr, d->line_width);
  cairo_set_line_cap(cr, d->cap == RMR_CAP_ROUND    ? CAIRO_LINE_CAP_ROUND
                         : d->cap == RMR_CAP_SQUARE ? CAIRO_LINE_CAP_SQUARE
                                                    : CAIRO_LINE_CAP_BUTT);
  cairo_set_line_join(cr, d->join == RMR_JOIN_ROUND   ? CAIRO_LINE_JOIN_ROUND
                          : d->join == RMR_JOIN_BEVEL ? CAIRO_LINE_JOIN_BEVEL
                                                      : CAIRO_LINE_JOIN_MITER);
  cairo_set_miter_limit(cr, d->miter_limit != 0 ? d->miter_limit : 10);

  rmr_status s = RMR_OK;
  path(cr, d->path);

  if (d->mode == RMR_IMAGE || d->mode == RMR_PDF) {
    cairo_clip(cr);
    if (d->mode == RMR_PDF)
      s = paint_pdf(c, d);
    else {
      cairo_surface_t* image = nullptr;
      s = image_surface(&d->image, &c->limits, &image);
      if (s == RMR_OK) {
        rmr_transform t = d->image.transform;
        double w = cairo_image_surface_get_width(image),
               h = cairo_image_surface_get_height(image);
        cairo_matrix_t m = {t.a / w, t.b / w, t.c / h, t.d / h, t.tx, t.ty};
        cairo_transform(cr, &m);
        cairo_set_source_surface(cr, image, 0, 0);
        cairo_paint_with_alpha(cr, d->color.alpha);
        cairo_surface_destroy(image);
      }
    }
  } else if (d->mode == RMR_TEXT) {
    PangoLayout* p =
        rmr_linux_label(cr, d->text.utf8, d->text.size, d->text.font_size);
    cairo_move_to(
        cr, d->text.origin.x,
        d->text.origin.y - (double)pango_layout_get_baseline(p) / PANGO_SCALE);
    pango_cairo_show_layout(cr, p);
    g_object_unref(p);
  } else if (d->mask.bytes) {
    cairo_clip(cr);
    size_t mw, mh;
    s = dimensions((double)d->mask.width, (double)d->mask.height, &c->limits,
                   &mw, &mh);
    if (s == RMR_OK) {
      cairo_surface_t* mask =
          cairo_image_surface_create(CAIRO_FORMAT_A8, (int)mw, (int)mh);
      if (cairo_surface_status(mask) != CAIRO_STATUS_SUCCESS)
        s = RMR_ERR_OUT_OF_MEMORY;
      else {
        uint8_t* data = cairo_image_surface_get_data(mask);
        size_t stride = (size_t)cairo_image_surface_get_stride(mask);
        for (size_t y = 0; y < mh; y++)
          for (size_t x = 0; x < mw; x++)
            data[y * stride + x] =
                (d->mask.bytes[y * d->mask.stride + x / 8] & (128u >> (x % 8)))
                    ? 0
                    : 255;
        cairo_surface_mark_dirty(mask);
        cairo_translate(cr, d->mask.box.x, d->mask.box.y);
        cairo_scale(cr, d->mask.box.width / (double)mw,
                    d->mask.box.height / (double)mh);
        cairo_pattern_t* pat = cairo_pattern_create_for_surface(mask);
        cairo_pattern_set_filter(pat, CAIRO_FILTER_NEAREST);
        /* Translucent ink is masked opaque in a group painted at its alpha.
         * Opaque ink is masked straight onto the page unless grain asks for
         * the group too: a group repeats the clip inside it, and viewers
         * antialias the edge twice, lighter than the Apple backend draws it.
         */
        bool layered =
            c->pdf && (d->color.alpha < 1 || c->grain == RMR_PDF_GRAIN_GROUP);
        if (layered) {
          cairo_push_group(cr);
          cairo_set_source_rgba(cr, d->color.red, d->color.green, d->color.blue,
                                1);
        }
        cairo_mask(cr, pat);
        cairo_pattern_destroy(pat);
        if (layered) {
          cairo_pop_group_to_source(cr);
          cairo_paint_with_alpha(cr, d->color.alpha);
        }
      }
      cairo_surface_destroy(mask);
    }
  } else if (d->mode == RMR_STROKE)
    cairo_stroke(cr);
  else if (d->mode == RMR_FILL_STROKE) {
    cairo_fill_preserve(cr);
    color(cr, d->stroke_color);
    cairo_stroke(cr);
  } else
    cairo_fill(cr);

  if (isolate) {
    /* Restore the page matrix after masks, then remove earlier ink only. */
    cairo_identity_matrix(cr);
    setup(c);
    cairo_set_operator(cr, CAIRO_OPERATOR_DEST_OUT);
    cairo_set_source_rgba(cr, 0, 0, 0, 1);
    for (size_t i = 0; i < d->eraser_count; i++) {
      path(cr, d->erasers[i]);
      cairo_fill(cr);
    }
    cairo_pop_group_to_source(cr);
    cairo_set_operator(cr, blend(d->blend));
    cairo_paint(cr);
  }

  cairo_restore(cr);
  c->dirty = true;
  if (s == RMR_OK && cairo_status(cr) != CAIRO_STATUS_SUCCESS)
    s = RMR_ERR_BACKEND;
  return s;
}

/* Backend end callback: flattens any open transparency group and
 * reports whether the context is left in an error state. */
static rmr_status end(void* ctx) {
  drawing* c = ctx;
  group_end(c);
  return cairo_status(c->cr) == CAIRO_STATUS_SUCCESS ? RMR_OK : RMR_ERR_BACKEND;
}

/* Backend abort callback: flattens any open transparency group without
 * reporting a status. */
static void abort_draw(void* ctx) { group_end(ctx); }

/* Builds the rmr_backend vtable bound to c, advertising the feature
 * bitmask this backend supports. */
static rmr_backend backend(drawing* c) {
  return (rmr_backend){RMR_BACKEND_ABI,
                       sizeof(rmr_backend),
                       (UINT64_C(1) << 12) - 1,
                       c,
                       begin,
                       draw,
                       end,
                       abort_draw};
}

/* Checks that a sheet has finite, positive, bounded dimensions and a
 * uniformly scaled transform with no rotation or shear and a flipped y
 * axis. */
static bool sheet_valid(const rmr_sheet* s) {
  if (!s || !isfinite(s->width) || !isfinite(s->height) || s->width <= 0 ||
      s->height <= 0 || s->width > 1000000 || s->height > 1000000)
    return false;
  rmr_transform t = s->transform;
  return isfinite(t.a) && t.a > 0 && t.a <= 1e6 && t.b == 0 && t.c == 0 &&
         t.d == -t.a && isfinite(t.tx) && isfinite(t.ty) &&
         fabs(t.tx) <= 1e12 && fabs(t.ty) <= 1e12;
}

/* Renders a plan onto an explicit sheet into a bitmap: validates the
 * sheet and background color, paints the background, executes the plan
 * through the Cairo backend, then converts the premultiplied ARGB32
 * surface to unpremultiplied RGBA bytes. */
static rmr_status bitmap_on_sheet(const rmr_plan* plan, const rmr_sheet* sheet,
                                  bool clip, rmr_color bg,
                                  const rmr_limits* given, rmr_cancel_fn cancel,
                                  void* ctx, rmr_bitmap* out, rmr_error* e) {
  if (!plan || !out || !sheet_valid(sheet) ||
      floor(sheet->width) != sheet->width ||
      floor(sheet->height) != sheet->height || !isfinite(bg.red) ||
      !isfinite(bg.green) || !isfinite(bg.blue) || !isfinite(bg.alpha) ||
      bg.red < 0 || bg.red > 1 || bg.green < 0 || bg.green > 1 || bg.blue < 0 ||
      bg.blue > 1 || bg.alpha < 0 || bg.alpha > 1)
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT, "invalid bitmap input");

  drawing c = {.limits = given ? *given : rmr_limits_default(),
               .sheet = *sheet,
               .clip_viewport = clip};
  size_t w, h;
  rmr_status s = dimensions(sheet->width, sheet->height, &c.limits, &w, &h);
  if (s != RMR_OK) return rmr_linux_error(e, s, "bitmap limit");

  c.surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)w, (int)h);
  cairo_status_t created = cairo_surface_status(c.surface);
  if (created != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(c.surface);
    return rmr_linux_error(e,
                           created == CAIRO_STATUS_NO_MEMORY
                               ? RMR_ERR_OUT_OF_MEMORY
                               : RMR_ERR_BACKEND,
                           "bitmap allocation failed");
  }

  c.cr = cairo_create(c.surface);
  color(c.cr, bg);
  cairo_paint(c.cr);
  setup(&c);
  rmr_backend b = backend(&c);
  s = rmr_plan_execute(plan, &b, cancel, ctx, e);
  cairo_destroy(c.cr);
  cairo_surface_flush(c.surface);

  if (s == RMR_OK) {
    uint8_t* pixels = malloc(w * h * 4);
    if (!pixels)
      s = RMR_ERR_OUT_OF_MEMORY;
    else {
      const uint8_t* src = cairo_image_surface_get_data(c.surface);
      size_t stride = (size_t)cairo_image_surface_get_stride(c.surface);
      for (size_t y = 0; y < h; y++)
        for (size_t x = 0; x < w; x++) {
          uint32_t v;
          memcpy(&v, src + y * stride + x * 4, 4);
          uint8_t* p = pixels + (y * w + x) * 4;
          p[0] = (uint8_t)(v >> 16);
          p[1] = (uint8_t)(v >> 8);
          p[2] = (uint8_t)v;
          p[3] = (uint8_t)(v >> 24);
        }
      *out = (rmr_bitmap){pixels, w, h, w * 4};
    }
  }

  cairo_surface_destroy(c.surface);
  return s == RMR_OK ? s
                     : rmr_linux_error(e, s, "Linux bitmap rendering failed");
}

rmr_status rmr_linux_bitmap_on_sheet(const rmr_plan* plan,
                                     const rmr_sheet* sheet, rmr_color bg,
                                     const rmr_limits* given,
                                     rmr_cancel_fn cancel, void* ctx,
                                     rmr_bitmap* out, rmr_error* e) {
  return bitmap_on_sheet(plan, sheet, false, bg, given, cancel, ctx, out, e);
}

rmr_status rmr_linux_bitmap(const rmr_plan* plan, double scale, rmr_color bg,
                            const rmr_limits* limits, rmr_cancel_fn cancel,
                            void* ctx, rmr_bitmap* out, rmr_error* e) {
  if (!plan || !isfinite(scale) || scale <= 0)
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT, "invalid bitmap scale");
  rmr_rect v = rmr_plan_viewport(plan);
  rmr_sheet sh = {.width = ceil(v.width * scale),
                  .height = ceil(v.height * scale),
                  .transform = {scale, 0, 0, -scale, -v.x * scale,
                                ceil(v.height * scale) + v.y * scale}};
  return bitmap_on_sheet(plan, &sh, true, bg, limits, cancel, ctx, out, e);
}

rmr_status rmr_linux_render_pdf(const rmr_plan* const* plans,
                                const rmr_sheet* sheets, size_t count,
                                const rmr_limits* limits, rmr_pdf_grain grain,
                                rmr_cancel_fn cancel, void* ctx,
                                rmr_buffer* out, rmr_error* e) {
  if (!plans || !sheets || !count || !out)
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT, "invalid PDF pages");
  if (count > limits->maximum_draws || count > INT_MAX)
    return rmr_linux_error(e, RMR_ERR_LIMIT, "PDF page count limit");
  for (size_t i = 0; i < count; i++)
    if (!plans[i] || !sheet_valid(&sheets[i]))
      return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT, "invalid PDF sheet");

  FPDF_DOCUMENT doc = FPDF_CreateNewDocument();
  if (!doc)
    return rmr_linux_error(e, RMR_ERR_OUT_OF_MEMORY, "allocation failed");

  rmr_status s = RMR_OK;
  for (size_t i = 0; i < count && s == RMR_OK; i++) {
    if (cancel && cancel(ctx)) {
      s = RMR_ERR_CANCELLED;
      break;
    }

    /* Pages after the first are composed in a document of their own, then
     * imported: content generation walks every object of the document
     * holding the page, which would otherwise grow with each page. */
    FPDF_DOCUMENT single = i ? FPDF_CreateNewDocument() : doc;
    drawing c = {
        .limits = *limits, .sheet = sheets[i], .pdf = single, .grain = grain};
    c.page = single ? FPDFPage_New(single, 0, c.sheet.width, c.sheet.height)
                    : nullptr;
    if (!c.page) {
      if (single && single != doc) FPDF_CloseDocument(single);
      s = RMR_ERR_BACKEND;
      break;
    }

    segment_start(&c);
    rmr_backend b = backend(&c);
    s = rmr_plan_execute(plans[i], &b, cancel, ctx, e);
    if (c.cr) {
      rmr_status finish = segment_end(&c);
      if (s == RMR_OK) s = finish;
    }

    if (s == RMR_OK && !FPDFPage_GenerateContent(c.page)) s = RMR_ERR_BACKEND;
    FPDF_ClosePage(c.page);
    if (single != doc) {
      int first = 0;
      if (s == RMR_OK &&
          !FPDF_ImportPagesByIndex(doc, single, &first, 1, (int)i))
        s = RMR_ERR_BACKEND;
      FPDF_CloseDocument(single);
    }
  }

  if (s == RMR_OK) s = rmr_pdfium_save(doc, limits, out);

  FPDF_CloseDocument(doc);
  return s == RMR_OK ? s : rmr_linux_error(e, s, "Linux PDF rendering failed");
}

/* Serializes count plans/sheets into a single PDF, holding the
 * process-wide PDFium lock for the call. */
rmr_status rmr_linux_pdf_sheets_with_options(
    const rmr_plan* const* plans, const rmr_sheet* sheets, size_t count,
    const rmr_limits* given, const rmr_pdf_options* given_options,
    rmr_cancel_fn cancel, void* ctx, rmr_buffer* out, rmr_error* e) {
  rmr_limits limits = given ? *given : rmr_limits_default();
  rmr_pdf_options options;
  if (!rmr_pdf_options_read(given_options, &options))
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT, "invalid PDF options");
  rmr_pdfium_lock();
  rmr_status s = rmr_linux_render_pdf(plans, sheets, count, &limits,
                                      options.grain, cancel, ctx, out, e);
  rmr_pdfium_unlock();
  return s;
}

rmr_status rmr_linux_pdf_sheets(const rmr_plan* const* plans,
                                const rmr_sheet* sheets, size_t count,
                                const rmr_limits* limits, rmr_cancel_fn cancel,
                                void* ctx, rmr_buffer* out, rmr_error* e) {
  return rmr_linux_pdf_sheets_with_options(plans, sheets, count, limits,
                                           nullptr, cancel, ctx, out, e);
}

rmr_status rmr_linux_pdf_with_options(const rmr_plan* const* plans,
                                      size_t count, double scale,
                                      const rmr_limits* given,
                                      const rmr_pdf_options* options,
                                      rmr_cancel_fn cancel, void* ctx,
                                      rmr_buffer* out, rmr_error* e) {
  if (!plans || !out || !count || !isfinite(scale) || scale <= 0)
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT, "invalid PDF input");

  rmr_limits limits = given ? *given : rmr_limits_default();
  if (count > limits.maximum_draws ||
      count > limits.maximum_plan_bytes / sizeof(rmr_sheet))
    return rmr_linux_error(e, RMR_ERR_LIMIT, "PDF sheets limit");

  rmr_sheet* sheets = calloc(count, sizeof *sheets);
  if (!sheets)
    return rmr_linux_error(e, RMR_ERR_OUT_OF_MEMORY, "allocation failed");

  for (size_t i = 0; i < count; i++) {
    rmr_rect v = rmr_plan_viewport(plans[i]);
    sheets[i] = (rmr_sheet){.width = v.width * scale,
                            .height = v.height * scale,
                            .transform = {scale, 0, 0, -scale, -v.x * scale,
                                          (v.height + v.y) * scale}};
  }

  rmr_status s = rmr_linux_pdf_sheets_with_options(
      plans, sheets, count, &limits, options, cancel, ctx, out, e);
  free(sheets);
  return s;
}

rmr_status rmr_linux_pdf(const rmr_plan* const* plans, size_t count,
                         double scale, const rmr_limits* limits,
                         rmr_cancel_fn cancel, void* ctx, rmr_buffer* out,
                         rmr_error* e) {
  return rmr_linux_pdf_with_options(plans, count, scale, limits, nullptr,
                                    cancel, ctx, out, e);
}
