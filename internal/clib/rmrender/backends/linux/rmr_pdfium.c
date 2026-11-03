#define _POSIX_C_SOURCE 200809L

/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <limits.h>
#include <pthread.h>

#include "rmr_linux_internal.h"

/* PDFium is process-global and not thread safe. All entry points are
 * serialized. Never destroy the engine while another library may retain a
 * PDFium handle. */
static pthread_mutex_t mutex;
static pthread_once_t once = PTHREAD_ONCE_INIT;

/* Initializes the recursive mutex and the PDFium library; runs once per
 * process. */
static void initialize(void) {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex, &attr);
  pthread_mutexattr_destroy(&attr);
  FPDF_InitLibrary();
}

void rmr_pdfium_lock(void) {
  pthread_once(&once, initialize);
  pthread_mutex_lock(&mutex);
}

void rmr_pdfium_unlock(void) { pthread_mutex_unlock(&mutex); }

rmr_status rmr_linux_error(rmr_error* e, rmr_status s, const char* message) {
  if (e) {
    *e = (rmr_error){.code = s};
    snprintf(e->message, sizeof e->message, "%s", message);
  }
  return s;
}

cairo_status_t rmr_linux_write(void* ctx, const unsigned char* data,
                               unsigned int size) {
  rmr_sink* s = ctx;
  if (s->status != RMR_OK) return CAIRO_STATUS_WRITE_ERROR;
  if ((size_t)size > s->maximum - s->data.size) {
    s->status = RMR_ERR_LIMIT;
    return CAIRO_STATUS_WRITE_ERROR;
  }
  if (!size) return CAIRO_STATUS_SUCCESS;

  size_t needed = s->data.size + size;
  if (needed > s->capacity) {
    /* Cairo writes in small pieces; grow geometrically, never past the limit.
     */
    size_t grown = s->capacity < 4096 ? 4096 : s->capacity;
    while (grown < needed)
      grown = grown > s->maximum / 2 ? s->maximum : grown * 2;
    if (grown > s->maximum) grown = s->maximum;
    uint8_t* p = realloc(s->data.bytes, grown);
    if (!p) {
      s->status = RMR_ERR_OUT_OF_MEMORY;
      return CAIRO_STATUS_NO_MEMORY;
    }
    s->data.bytes = p;
    s->capacity = grown;
  }

  memcpy(s->data.bytes + s->data.size, data, size);
  s->data.size = needed;
  return CAIRO_STATUS_SUCCESS;
}

typedef struct {
  FPDF_FILEWRITE base;
  rmr_sink sink;
} writer;

/* PDFium write callback: forwards to rmr_linux_write(), rejecting sizes
 * larger than UINT_MAX. */
static int write_block(FPDF_FILEWRITE* base, const void* data,
                       unsigned long size) {
  writer* w = (writer*)base;
  if (size > UINT_MAX) {
    w->sink.status = RMR_ERR_LIMIT;
    return 0;
  }
  return rmr_linux_write(&w->sink, data, (unsigned int)size) ==
         CAIRO_STATUS_SUCCESS;
}

rmr_status rmr_pdfium_save(FPDF_DOCUMENT doc, const rmr_limits* limits,
                           rmr_buffer* out) {
  writer w = {.base = {1, write_block},
              .sink = {.maximum = limits->maximum_pdf_bytes}};
  bool ok = FPDF_SaveAsCopy(doc, &w.base, FPDF_NO_INCREMENTAL) != 0;
  if (!ok || w.sink.status != RMR_OK) {
    free(w.sink.data.bytes);
    return w.sink.status ? w.sink.status : RMR_ERR_BACKEND;
  }

  *out = w.sink.data;
  return RMR_OK;
}

rmr_status rmr_pdfium_form(FPDF_DOCUMENT dest, FPDF_PAGE page,
                           FPDF_DOCUMENT source, size_t index,
                           rmr_transform t) {
  if (index > INT_MAX) return RMR_ERR_LIMIT;

  FPDF_XOBJECT x = FPDF_NewXObjectFromPage(dest, source, (int)index);
  if (!x) return RMR_ERR_BACKEND;

  FPDF_PAGEOBJECT object = FPDF_NewFormObjectFromXObject(x);
  FPDF_CloseXObject(x);
  if (!object) return RMR_ERR_BACKEND;

  FPDF_PAGE original = FPDF_LoadPage(source, (int)index);
  if (!original) {
    FPDFPageObj_Destroy(object);
    return RMR_ERR_BACKEND;
  }

  FS_RECTF bounds;
  if (!FPDF_GetPageBoundingBox(original, &bounds) ||
      bounds.right <= bounds.left || bounds.top <= bounds.bottom) {
    FPDF_ClosePage(original);
    FPDFPageObj_Destroy(object);
    return RMR_ERR_BACKEND;
  }

  t = rmr_pdfium_raw_transform(original, t);
  FPDF_ClosePage(original);
  FPDFPageObj_Transform(object, t.a, t.b, t.c, t.d, t.tx, t.ty);
  FPDFPage_InsertObject(page, object);
  return RMR_OK;
}

rmr_transform rmr_pdfium_raw_transform(FPDF_PAGE page, rmr_transform t) {
  FS_RECTF b = {0};
  FPDF_GetPageBoundingBox(page, &b);
  rmr_transform inverse;

  switch (FPDFPage_GetRotation(page)) {
    case 1:
      inverse = (rmr_transform){0, 1, -1, 0, b.right, b.bottom};
      break;
    case 2:
      inverse = (rmr_transform){-1, 0, 0, -1, b.right, b.top};
      break;
    case 3:
      inverse = (rmr_transform){0, -1, 1, 0, b.left, b.top};
      break;
    default:
      inverse = (rmr_transform){1, 0, 0, 1, b.left, b.bottom};
      break;
  }

  return (rmr_transform){t.a * inverse.a + t.c * inverse.b,
                         t.b * inverse.a + t.d * inverse.b,
                         t.a * inverse.c + t.c * inverse.d,
                         t.b * inverse.c + t.d * inverse.d,
                         t.a * inverse.tx + t.c * inverse.ty + t.tx,
                         t.b * inverse.tx + t.d * inverse.ty + t.ty};
}
