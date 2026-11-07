/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <rmrender/output.h>

#include "rmr_internal.h"
#ifdef RMRENDER_TESTING
static thread_local size_t remaining = SIZE_MAX;

void rmr_test_fail_after(size_t n) { remaining = n; }

/* Test hook: true once the countdown set by rmr_test_fail_after reaches
 * zero, simulating an allocation failure. */
static bool fail(void) {
  if (remaining == SIZE_MAX) return false;
  if (!remaining) return true;
  remaining--;
  return false;
}
#else
/* Allocation failures are never simulated outside RMRENDER_TESTING. */
static bool fail(void) { return false; }
#endif

void* rmr_alloc(size_t n) { return fail() ? nullptr : malloc(n ? n : 1); }

void* rmr_calloc(size_t n, size_t size) {
  if (size && n > SIZE_MAX / size) return nullptr;
  return fail() ? nullptr : calloc(n ? n : 1, size ? size : 1);
}

void* rmr_realloc(void* p, size_t n) {
  return fail() ? nullptr : realloc(p, n ? n : 1);
}

rmr_status rmr_fail(rmr_error* e, rmr_status s, size_t index,
                    const char* message) {
  if (e) {
    *e = (rmr_error){.code = s, .index = index};
    snprintf(e->message, sizeof e->message, "%s",
             message ? message : rmr_status_string(s));
  }
  return s;
}

#define RMR_STR_(x) #x
#define RMR_STR(x) RMR_STR_(x)
const char* rmr_version(void) {
  return RMR_STR(RMRENDER_VERSION_MAJOR) "." RMR_STR(
      RMRENDER_VERSION_MINOR) "." RMR_STR(RMRENDER_VERSION_PATCH);
}

const char* rmr_status_string(rmr_status s) {
  static const char* const names[] = {"ok",
                                      "invalid argument",
                                      "out of memory",
                                      "limit exceeded",
                                      "unsupported feature",
                                      "backend failed",
                                      "cancelled"};
  return (size_t)s < sizeof names / sizeof *names ? names[s] : "unknown status";
}

rmr_pdf_options rmr_pdf_options_default(void) {
  return (rmr_pdf_options){RMR_PDF_OPTIONS_ABI, sizeof(rmr_pdf_options),
                           RMR_PDF_GRAIN_MASK, 0};
}

rmr_limits rmr_limits_default(void) {
  return (rmr_limits){1000000,    4000000, 65535,    256u << 20, 256u << 20,
                      256u << 20, 32768,   10000000, 200};
}

void rmr_path_free(rmr_path* p) {
  if (p) {
    free(p->elements);
    *p = (rmr_path){0};
  }
}

void rmr_buffer_free(rmr_buffer* b) {
  if (b) {
    free(b->bytes);
    *b = (rmr_buffer){0};
  }
}

bool rmr_point_valid(rmr_point p, double bound) {
  return isfinite(p.x) && isfinite(p.y) && fabs(p.x) <= bound &&
         fabs(p.y) <= bound;
}

bool rmr_color_valid(rmr_color c) {
  return isfinite(c.red) && isfinite(c.green) && isfinite(c.blue) &&
         isfinite(c.alpha) && c.red >= 0 && c.red <= 1 && c.green >= 0 &&
         c.green <= 1 && c.blue >= 0 && c.blue <= 1 && c.alpha >= 0 &&
         c.alpha <= 1;
}

rmr_status rmr_page_to_pdf(rmr_rect v, double scale, rmr_transform* out,
                           rmr_error* e) {
  if (!out || !isfinite(scale) || scale <= 0 || !isfinite(v.x) ||
      !isfinite(v.y) || !isfinite(v.width) || !isfinite(v.height) ||
      v.width <= 0 || v.height <= 0)
    return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, 0,
                    "invalid viewport or scale");

  rmr_transform t = {
      scale, 0, 0, -scale, -v.x * scale, (v.y + v.height) * scale};
  if (!isfinite(t.tx) || !isfinite(t.ty) || !isfinite(v.width * scale) ||
      !isfinite(v.height * scale))
    return rmr_fail(e, RMR_ERR_LIMIT, 0, "transform overflow");

  *out = t;
  return RMR_OK;
}

rmr_point rmr_transform_point(rmr_transform t, rmr_point p) {
  return (rmr_point){t.a * p.x + t.c * p.y + t.tx,
                     t.b * p.x + t.d * p.y + t.ty};
}

rmr_status rmr_paper_points(rmr_paper paper, double w, double h, rmr_point* out,
                            rmr_error* e) {
  if (!out || paper > RMR_PAPER_TABLET || !isfinite(w) || !isfinite(h) ||
      w <= 0 || h <= 0)
    return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, 0, "invalid paper or canvas");

  rmr_point size =
      paper == RMR_PAPER_A4 ? (rmr_point){595.276, 841.89}
      : paper == RMR_PAPER_LETTER
          ? (rmr_point){612, 792}
          : (rmr_point){w * RMR_POINTS_PER_UNIT, h * RMR_POINTS_PER_UNIT};
  if (w > h && size.y > size.x) size = (rmr_point){size.y, size.x};

  if (!isfinite(size.x) || !isfinite(size.y) || size.x <= 0 || size.y <= 0)
    return rmr_fail(e, RMR_ERR_LIMIT, 0,
                    "paper dimensions overflow or underflow");

  *out = size;
  return RMR_OK;
}

rmr_status rmr_sheet_create(rmr_rect b, double fw, double fh, double scale,
                            rmr_sheet* out, rmr_error* e) {
  if (!out || !isfinite(b.x) || !isfinite(b.y) || !isfinite(b.width) ||
      !isfinite(b.height) || b.width <= 0 || b.height <= 0 || !isfinite(fw) ||
      !isfinite(fh) || fw <= 0 || fh <= 0 || !isfinite(scale) || scale <= 0)
    return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, 0, "invalid sheet input");

  double width = b.width * scale, height = b.height * scale;
  if (!isfinite(width) || !isfinite(height))
    return rmr_fail(e, RMR_ERR_LIMIT, 0, "sheet dimensions overflow");

  rmr_sheet s = {.width = round(fmax(fw, width)),
                 .height = round(fmax(fh, height))};
  s.transform = (rmr_transform){scale,
                                0,
                                0,
                                -scale,
                                (s.width - width) / 2 - b.x * scale,
                                s.height + b.y * scale};
  if (s.width < 1 || s.height < 1 || !isfinite(s.transform.tx) ||
      !isfinite(s.transform.ty))
    return rmr_fail(e, RMR_ERR_LIMIT, 0,
                    "sheet placement overflow or empty rounded sheet");

  *out = s;
  return RMR_OK;
}

rmr_status rmr_paper_sheet(rmr_paper paper, double w, double h, rmr_rect bounds,
                           rmr_sheet* out, rmr_error* e) {
  if (!out)
    return rmr_fail(e, RMR_ERR_INVALID_ARGUMENT, 0, "missing sheet output");

  rmr_point size;
  rmr_status s = rmr_paper_points(paper, w, h, &size, e);
  if (s != RMR_OK) return s;

  double scale = size.x / w;
  if (!isfinite(scale) || scale <= 0)
    return rmr_fail(e, RMR_ERR_LIMIT, 0, "paper scale overflow");

  return rmr_sheet_create(bounds, size.x, size.y, scale, out, e);
}

bool rmr_utf8_valid(const char* s, size_t n) {
  for (size_t i = 0; i < n;) {
    unsigned c = (unsigned char)s[i++];
    if (c < 128) continue;

    unsigned count = c >= 0xc2 && c <= 0xdf   ? 1
                     : c >= 0xe0 && c <= 0xef ? 2
                     : c >= 0xf0 && c <= 0xf4 ? 3
                                              : 0;
    if (!count || count > n - i) return false;
    unsigned v = c & (0x7fu >> count);
    for (unsigned j = 0; j < count; j++) {
      unsigned b = (unsigned char)s[i++];
      if ((b & 0xc0) != 0x80) return false;
      v = (v << 6) | (b & 63);
    }

    if ((count == 1 && v < 128) || (count == 2 && v < 2048) ||
        (count == 3 && v < 65536) || v > 0x10ffff ||
        (v >= 0xd800 && v <= 0xdfff))
      return false;
  }

  return true;
}

double rmr_clamp(double x, double low, double high) {
  return fmin(fmax(x, low), high);
}
