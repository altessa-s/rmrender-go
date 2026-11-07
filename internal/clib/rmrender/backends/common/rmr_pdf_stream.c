/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "rmr_pdf_stream.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* Powers of ten up to 10^12, all exact in double. */
static const double powers[] = {1,   1e1, 1e2, 1e3,  1e4,  1e5, 1e6,
                                1e7, 1e8, 1e9, 1e10, 1e11, 1e12};

/* Writes the decimal digits of n; returns their count. */
static size_t digits(char* out, uint64_t n) {
  char reversed[20];
  size_t count = 0;
  do {
    reversed[count++] = (char)('0' + n % 10);
    n /= 10;
  } while (n);
  for (size_t i = 0; i < count; i++) out[i] = reversed[count - 1 - i];
  return count;
}

/* The integer nearest a * scale, ties to even, from the exact product; scale
 * must be a power of ten up to 10^12 and a * scale below 2^52. */
static uint64_t scaled(double a, double scale) {
  double high = a * scale, low = fma(a, scale, -high);
  double whole = floor(high);
  /* Both terms are multiples of ulp(high), which exceeds 2|low|: the sign of
   * the exact remainder minus a half is that of d unless d is zero. */
  double d = (high - whole) - 0.5;
  uint64_t n = (uint64_t)whole;
  if (d > 0 || (d == 0 && (low > 0 || (low == 0 && (n & 1))))) n++;
  return n;
}

/* %0.7g of v, whose magnitude a is in [1, 1e7]: seven significant digits,
 * trailing fraction zeros dropped. */
static size_t significant(char* out, double v, double a) {
  unsigned e = a < 1e1   ? 0
               : a < 1e2 ? 1
               : a < 1e3 ? 2
               : a < 1e4 ? 3
               : a < 1e5 ? 4
               : a < 1e6 ? 5
                         : 6;
  uint64_t n = scaled(a, powers[6 - e]);
  char* o = out;
  if (v < 0) *o++ = '-';

  if (n >= 10000000) {
    /* Rounded up to the next power of ten. */
    if (e == 6) {
      memcpy(o, "1e+07", 5);
      return (size_t)(o - out) + 5;
    }
    *o++ = '1';
    for (unsigned i = 0; i <= e; i++) *o++ = '0';
    return (size_t)(o - out);
  }

  /* Small fixed-size loops: cheaper here than memcpy calls. */
  uint32_t m = (uint32_t)n;
  char d[7];
  for (int i = 6; i >= 0; i--) {
    d[i] = (char)('0' + m % 10);
    m /= 10;
  }
  for (unsigned i = 0; i <= e; i++) *o++ = d[i];
  unsigned last = 6;
  while (last > e && d[last] == '0') last--;
  if (last > e) {
    *o++ = '.';
    for (unsigned i = e + 1; i <= last; i++) *o++ = d[i];
  }
  return (size_t)(o - out);
}

/* %0.*f of v, magnitude a below 1, with precision decimals (1 to 12). */
static size_t fixed(char* out, double v, double a, unsigned precision) {
  uint64_t n = scaled(a, powers[precision]), unit = (uint64_t)powers[precision];
  size_t used = 0;
  if (v < 0) out[used++] = '-';
  out[used++] = n >= unit ? '1' : '0';
  if (n >= unit) n -= unit;
  out[used++] = '.';
  for (unsigned i = precision; i-- > 0;) {
    out[used + i] = (char)('0' + n % 10);
    n /= 10;
  }
  return used + precision;
}

size_t rmr_pdf_number(char out[RMR_PDF_NUMBER_SIZE], double v) {
  if (isnan(v)) {
    out[0] = '0';
    return 1;
  }
  if (isinf(v)) {
    const char* end = v > 0 ? "2147483647" : "-2147483648";
    size_t length = strlen(end);
    memcpy(out, end, length);
    return length;
  }

  if (v >= -2147483648.0 && v <= 2147483647.0) {
    double r = v < 0 ? ceil(v - 0.5) : floor(v + 0.5);
    int32_t n = (int32_t)r;
    if (fabs(v - (double)n) < 1e-6) {
      size_t used = 0;
      if (n < 0) out[used++] = '-';
      return used +
             digits(out + used, n < 0 ? (uint64_t)(-(int64_t)n) : (uint64_t)n);
    }
  }

  double a = fabs(v);
  if (a >= 1) {
    if (a <= 1e7) return significant(out, v, a);
    /* %0.0f: the integer nearest v, ties to even. */
    size_t used = 0;
    if (v < 0) out[used++] = '-';
    if (a < 4503599627370496.0) /* 2^52 */
      return used + digits(out + used, scaled(a, 1));
    if (a < 18446744073709551616.0) /* 2^64, an integer already */
      return used + digits(out + used, (uint64_t)a);
    /* Digits only, so no locale radix can appear. */
    int length = snprintf(out + used, RMR_PDF_NUMBER_SIZE - used, "%.0f", a);
    return length < 0 ? 0 : used + (size_t)length;
  }
  if (a < 1e-6) {
    out[0] = '0';
    return 1;
  }

  /* CoreGraphics finds the first power of ten lifting a to 1 (at most 10^6),
   * then up to six more until the value is within 1e-5 of an integer. Its
   * multiplier is a 32-bit integer that wraps past 10^9; so does this one. */
  uint32_t multiplier = 1;
  unsigned leading = 0, extra = 0;
  while (!(a * (double)multiplier >= 1)) {
    multiplier *= 10;
    if (++leading == 6) break;
  }
  for (;;) {
    double x = a * (double)multiplier;
    if (fabs(x - round(x)) < 1e-5) break;
    multiplier *= 10;
    if (++extra == 6) break;
  }
  return fixed(out, v, a, leading + extra);
}

size_t rmr_pdf_decimal(char out[RMR_PDF_NUMBER_SIZE], double v,
                       unsigned decimals) {
  if (decimals < 1 || decimals > 6) return rmr_pdf_number(out, v);
  double x = v * powers[decimals];
  if (!(fabs(x) < 4503599627370496.0)) return rmr_pdf_number(out, v);
  double r = rint(x);
  if (r == 0) {
    out[0] = '0';
    return 1;
  }

  size_t used = 0;
  if (r < 0) out[used++] = '-';
  uint64_t n = (uint64_t)fabs(r), unit = (uint64_t)powers[decimals];
  used += digits(out + used, n / unit);
  uint64_t fraction = n % unit;
  if (fraction) {
    char d[6];
    for (unsigned i = decimals; i-- > 0;) {
      d[i] = (char)('0' + fraction % 10);
      fraction /= 10;
    }
    unsigned last = decimals;
    while (d[last - 1] == '0') last--;
    out[used++] = '.';
    memcpy(out + used, d, last);
    used += last;
  }
  return used;
}

bool rmr_pdf_text_reserve(rmr_pdf_text* t, size_t more) {
  if (t->failed) return false;
  if (more <= t->capacity - t->size) return true;
  if (more > SIZE_MAX / 2 - t->size) {
    t->failed = true;
    return false;
  }
  size_t capacity = t->capacity ? t->capacity : 256;
  while (capacity - t->size < more) capacity *= 2;
  uint8_t* bytes = realloc(t->bytes, capacity);
  if (!bytes) {
    t->failed = true;
    return false;
  }
  t->bytes = bytes;
  t->capacity = capacity;
  return true;
}

void rmr_pdf_text_append(rmr_pdf_text* t, const void* bytes, size_t size) {
  if (!size || !rmr_pdf_text_reserve(t, size)) return;
  memcpy(t->bytes + t->size, bytes, size);
  t->size += size;
}

void rmr_pdf_text_string(rmr_pdf_text* t, const char* s) {
  rmr_pdf_text_append(t, s, strlen(s));
}

void rmr_pdf_text_format(rmr_pdf_text* t, const char* format, ...) {
  char b[256];
  va_list a;
  va_start(a, format);
  int n = vsnprintf(b, sizeof b, format, a);
  va_end(a);
  if (n < 0 || (size_t)n >= sizeof b) {
    t->failed = true;
    return;
  }
  rmr_pdf_text_append(t, b, (size_t)n);
}

void rmr_pdf_text_number(rmr_pdf_text* t, double v, unsigned decimals) {
  if (RMR_PDF_NUMBER_SIZE > t->capacity - t->size &&
      !rmr_pdf_text_reserve(t, RMR_PDF_NUMBER_SIZE))
    return;
  char* at = (char*)t->bytes + t->size;
  t->size +=
      decimals ? rmr_pdf_decimal(at, v, decimals) : rmr_pdf_number(at, v);
}

void rmr_pdf_text_free(rmr_pdf_text* t) {
  free(t->bytes);
  *t = (rmr_pdf_text){0};
}

rmr_pdf_object* rmr_pdf_batch_add(rmr_pdf_batch* b, uint32_t number,
                                  bool stream) {
  if (b->count == b->capacity) {
    size_t capacity = b->capacity ? b->capacity * 2 : 8;
    void* objects = realloc(b->objects, capacity * sizeof *b->objects);
    if (!objects) return nullptr;
    b->objects = objects;
    b->capacity = capacity;
  }
  rmr_pdf_object* o = &b->objects[b->count++];
  *o = (rmr_pdf_object){.number = number, .stream = stream};
  return o;
}

void rmr_pdf_batch_free(rmr_pdf_batch* b) {
  if (!b) return;
  if (b->work && b->discard) b->discard(b);
  free(b->extra.bytes);
  for (size_t i = 0; i < b->count; i++) {
    rmr_pdf_text_free(&b->objects[i].dict);
    free(b->objects[i].data);
  }
  free(b->objects);
  rmr_pdf_text_free(&b->chunk);
  free(b->offsets);
  free(b);
}

/* Per-thread deflate state, reset between streams, and its output space. */
typedef struct {
  z_stream z;
  bool ready;
  uint8_t* out;
  size_t capacity;
} deflater;

static void deflater_free(deflater* d) {
  if (d->ready) deflateEnd(&d->z);
  free(d->out);
  *d = (deflater){0};
}

/* Deflates size bytes into d->out; returns the compressed length, or
 * SIZE_MAX on failure. */
static size_t compress_stream(deflater* d, const uint8_t* data, size_t size) {
  if (!d->ready) {
    if (deflateInit(&d->z, Z_DEFAULT_COMPRESSION) != Z_OK) return SIZE_MAX;
    d->ready = true;
  } else if (deflateReset(&d->z) != Z_OK)
    return SIZE_MAX;
  if (size > UINT32_MAX / 2) return SIZE_MAX;

  size_t bound = deflateBound(&d->z, (uLong)size);
  if (bound > d->capacity) {
    uint8_t* out = realloc(d->out, bound);
    if (!out) return SIZE_MAX;
    d->out = out;
    d->capacity = bound;
  }
  d->z.next_in = (Bytef*)(uintptr_t)data;
  d->z.avail_in = (uInt)size;
  d->z.next_out = d->out;
  d->z.avail_out = (uInt)bound;
  if (deflate(&d->z, Z_FINISH) != Z_STREAM_END) return SIZE_MAX;
  return bound - d->z.avail_out;
}

/* Writes every object of b into its chunk, recording each offset. */
static void serialize(rmr_pdf_batch* b, deflater* d) {
  if (b->status != RMR_OK) return;
  b->offsets = b->count ? calloc(b->count, sizeof *b->offsets) : nullptr;
  if (b->count && !b->offsets) {
    b->status = RMR_ERR_OUT_OF_MEMORY;
    return;
  }
  if (b->root) rmr_pdf_text_string(&b->chunk, "%PDF-1.3\n%\342\343\317\323\n");

  for (size_t i = 0; i < b->count; i++) {
    rmr_pdf_object* o = &b->objects[i];
    if (o->dict.failed) {
      b->status = RMR_ERR_OUT_OF_MEMORY;
      return;
    }
    b->offsets[i] = b->chunk.size;
    rmr_pdf_text_format(&b->chunk, "%u 0 obj\n", o->number);
    if (o->stream) {
      size_t length = compress_stream(d, o->data, o->size);
      if (length == SIZE_MAX) {
        b->status = RMR_ERR_OUT_OF_MEMORY;
        return;
      }
      rmr_pdf_text_string(&b->chunk, "<< ");
      rmr_pdf_text_append(&b->chunk, o->dict.bytes, o->dict.size);
      rmr_pdf_text_format(&b->chunk, " /Length %zu /Filter /FlateDecode >>",
                          length);
      rmr_pdf_text_string(&b->chunk, "\nstream\n");
      rmr_pdf_text_append(&b->chunk, d->out, length);
      rmr_pdf_text_string(&b->chunk, "\nendstream");
      free(o->data);
      o->data = nullptr;
    } else
      rmr_pdf_text_append(&b->chunk, o->dict.bytes, o->dict.size);
    rmr_pdf_text_string(&b->chunk, "\nendobj\n");
    rmr_pdf_text_free(&o->dict);
  }

  if (b->root) {
    /* Objects are numbered 1 to count, in any order. */
    size_t* at = calloc(b->count + 1, sizeof *at);
    if (!at) {
      b->status = RMR_ERR_OUT_OF_MEMORY;
      return;
    }
    for (size_t i = 0; i < b->count; i++) {
      uint32_t n = b->objects[i].number;
      if (!n || n > b->count || at[n] || b->offsets[i] > 9999999999u) {
        free(at);
        b->status = RMR_ERR_INVALID_ARGUMENT;
        return;
      }
      at[n] = b->offsets[i];
    }
    size_t xref = b->chunk.size;
    rmr_pdf_text_format(&b->chunk, "xref\n0 %zu\n0000000000 65535 f \n",
                        b->count + 1);
    for (size_t n = 1; n <= b->count; n++)
      rmr_pdf_text_format(&b->chunk, "%010zu 00000 n \n", at[n]);
    free(at);
    rmr_pdf_text_format(
        &b->chunk,
        "trailer\n<< /Size %zu /Root %u 0 R >>\nstartxref\n%zu\n"
        "%%%%EOF\n",
        b->count + 1, b->root, xref);
  }
  if (b->chunk.failed) b->status = RMR_ERR_OUT_OF_MEMORY;
}

struct rmr_pdf_pipeline {
  pthread_mutex_t lock;
  pthread_cond_t work, finished;
  pthread_t threads[64];
  size_t workers;
  rmr_pdf_batch *head, *tail; /* submitted and not yet returned, in order */
  size_t pending;
  bool stopping;
  deflater local; /* for a pipeline without workers */
};

/* The oldest batch no worker has claimed; called with the lock held. */
static rmr_pdf_batch* first_waiting(rmr_pdf_pipeline* p) {
  for (rmr_pdf_batch* b = p->head; b; b = b->next)
    if (!b->started) return b;
  return nullptr;
}

/* Worker thread: serializes the oldest unclaimed batch until stopped. */
static void* work(void* context) {
  rmr_pdf_pipeline* p = context;
  deflater d = {0};
  pthread_mutex_lock(&p->lock);
  for (;;) {
    rmr_pdf_batch* b = first_waiting(p);
    while (!b && !p->stopping) {
      pthread_cond_wait(&p->work, &p->lock);
      b = first_waiting(p);
    }
    if (!b) break;
    b->started = true;
    pthread_mutex_unlock(&p->lock);
    if (b->run)
      b->run(b);
    else
      serialize(b, &d);
    pthread_mutex_lock(&p->lock);
    b->done = true;
    pthread_cond_broadcast(&p->finished);
  }
  pthread_mutex_unlock(&p->lock);
  deflater_free(&d);
  return nullptr;
}

size_t rmr_pdf_pipeline_workers(void) {
#ifdef _WIN32
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  long online = (long)info.dwNumberOfProcessors;
#else
  long online = sysconf(_SC_NPROCESSORS_ONLN);
#endif
  if (online <= 1) return 0;
  return online - 1 > 8 ? 8 : (size_t)(online - 1);
}

rmr_pdf_pipeline* rmr_pdf_pipeline_create(size_t workers) {
  if (workers > 64) workers = 64;
  rmr_pdf_pipeline* p = calloc(1, sizeof *p);
  if (!p) return nullptr;
  if (pthread_mutex_init(&p->lock, nullptr)) {
    free(p);
    return nullptr;
  }
  if (pthread_cond_init(&p->work, nullptr)) {
    pthread_mutex_destroy(&p->lock);
    free(p);
    return nullptr;
  }
  if (pthread_cond_init(&p->finished, nullptr)) {
    pthread_cond_destroy(&p->work);
    pthread_mutex_destroy(&p->lock);
    free(p);
    return nullptr;
  }
  for (; p->workers < workers; p->workers++)
    if (pthread_create(&p->threads[p->workers], nullptr, work, p)) break;
  if (workers && !p->workers) {
    rmr_pdf_pipeline_free(p);
    return nullptr;
  }
  return p;
}

void rmr_pdf_pipeline_submit(rmr_pdf_pipeline* p, rmr_pdf_batch* b) {
  b->next = nullptr;
  b->started = b->done = false;
  if (!p->workers) {
    if (b->run)
      b->run(b);
    else
      serialize(b, &p->local);
    b->started = b->done = true;
  }
  pthread_mutex_lock(&p->lock);
  if (p->tail)
    p->tail->next = b;
  else
    p->head = b;
  p->tail = b;
  p->pending++;
  pthread_cond_signal(&p->work);
  pthread_mutex_unlock(&p->lock);
}

size_t rmr_pdf_pipeline_pending(const rmr_pdf_pipeline* p) {
  return p->pending;
}

rmr_pdf_batch* rmr_pdf_pipeline_next(rmr_pdf_pipeline* p, bool wait) {
  pthread_mutex_lock(&p->lock);
  rmr_pdf_batch* b = p->head;
  while (b && !b->done && wait) pthread_cond_wait(&p->finished, &p->lock);
  if (b && b->done) {
    p->head = b->next;
    if (!p->head) p->tail = nullptr;
    p->pending--;
    b->next = nullptr;
  } else
    b = nullptr;
  pthread_mutex_unlock(&p->lock);
  return b;
}

void rmr_pdf_pipeline_free(rmr_pdf_pipeline* p) {
  if (!p) return;
  pthread_mutex_lock(&p->lock);
  /* Batches no worker has claimed are dropped unserialized. */
  for (rmr_pdf_batch* b = p->head; b; b = b->next)
    if (!b->started) {
      b->status = RMR_ERR_CANCELLED;
      b->started = b->done = true;
    }
  p->stopping = true;
  pthread_cond_broadcast(&p->work);
  pthread_mutex_unlock(&p->lock);
  for (size_t i = 0; i < p->workers; i++) pthread_join(p->threads[i], nullptr);

  for (rmr_pdf_batch* b = p->head; b;) {
    rmr_pdf_batch* next = b->next;
    rmr_pdf_batch_free(b);
    b = next;
  }
  deflater_free(&p->local);
  pthread_cond_destroy(&p->finished);
  pthread_cond_destroy(&p->work);
  pthread_mutex_destroy(&p->lock);
  free(p);
}

rmr_pdf_digest rmr_pdf_hash(const uint8_t* bytes, size_t size) {
  return rmr_sha256_of(bytes, size);
}
