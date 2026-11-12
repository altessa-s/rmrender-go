/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* PDF objects written by the backends themselves rather than by a native PDF
 * library: numbers printed exactly as CoreGraphics prints them, growable text,
 * and batches of objects deflated on worker threads and handed back in the
 * order they were submitted, so the output never depends on scheduling. */

#ifndef RMR_PDF_STREAM_H
#define RMR_PDF_STREAM_H
#include <rmrender/rmrender.h>
#include <stddef.h>
#include <stdint.h>

#include "../../src/rmr_sha256.h"

/* Room for any number rmr_pdf_number or rmr_pdf_decimal writes. */
enum { RMR_PDF_NUMBER_SIZE = 400 };

/* v as CoreGraphics' PDF writer prints it (PDFWriteNumber): NaN as 0,
 * infinities as the int32 range ends, values within 1e-6 of an int32 as that
 * integer, magnitudes of at least 1 with %0.7g (%0.0f above 1e7), magnitudes
 * below 1e-6 as 0 and the rest with the fixed decimals CoreGraphics picks by
 * scaling until the value is within 1e-5 of an integer. Writes no terminator;
 * returns the length. */
size_t rmr_pdf_number(char out[RMR_PDF_NUMBER_SIZE], double v);

/* v rounded to decimals (1 to 6) places, trailing zeros and a bare point
 * dropped, -0 printed as 0. Magnitudes too large for that rounding print as
 * rmr_pdf_number does. Writes no terminator; returns the length. */
size_t rmr_pdf_decimal(char out[RMR_PDF_NUMBER_SIZE], double v,
                       unsigned decimals);

/* Growable bytes; a failed allocation sets failed and drops later appends. */
typedef struct {
  uint8_t* bytes;
  size_t size, capacity;
  bool failed;
} rmr_pdf_text;

/* Makes room for more bytes; false (and failed set) when it cannot. */
bool rmr_pdf_text_reserve(rmr_pdf_text* t, size_t more);
void rmr_pdf_text_append(rmr_pdf_text* t, const void* bytes, size_t size);
void rmr_pdf_text_string(rmr_pdf_text* t, const char* s);
/* printf-style; output longer than 255 bytes fails the text. */
[[gnu::format(printf, 2, 3)]] void rmr_pdf_text_format(rmr_pdf_text* t,
                                                       const char* format, ...);
/* v through rmr_pdf_number, or rmr_pdf_decimal when decimals is nonzero. */
void rmr_pdf_text_number(rmr_pdf_text* t, double v, unsigned decimals);
void rmr_pdf_text_free(rmr_pdf_text* t);

/* One indirect object of a batch. A stream object holds its dictionary's
 * entries in dict (without << >>, /Length or /Filter) and its unencoded data,
 * deflated when the batch runs; any other object holds its whole value in
 * dict. The batch owns both. */
typedef struct {
  uint32_t number;
  bool stream;
  rmr_pdf_text dict;
  uint8_t* data;
  size_t size;
} rmr_pdf_object;

/* Objects serialized together into chunk ("N 0 obj ... endobj" each), object
 * i starting at offsets[i]. With root set the chunk is a whole PDF instead:
 * a header, the objects (numbered 1 to count), a cross-reference table and a
 * trailer naming root as the catalog. status is the batch's first failure. */
typedef struct rmr_pdf_batch {
  struct rmr_pdf_batch* next; /* the pipeline's, while queued */
  bool started, done;         /* the pipeline's */
  rmr_pdf_object* objects;
  size_t count, capacity;
  uint32_t root;
  rmr_pdf_text chunk;
  size_t* offsets;
  rmr_status status;
  /* The caller's: run, when set, replaces serializing the objects (it
   * fills chunk or status from work); discard releases work that never ran
   * and is called by rmr_pdf_batch_free; extra is freed with the batch. */
  void (*run)(struct rmr_pdf_batch*);
  void (*discard)(struct rmr_pdf_batch*);
  void* work;
  size_t tag;
  bool last;
  rmr_buffer extra;
} rmr_pdf_batch;

/* Appends an empty object numbered number, or null when allocation fails. */
rmr_pdf_object* rmr_pdf_batch_add(rmr_pdf_batch* b, uint32_t number,
                                  bool stream);
/* Releases everything the batch holds, then the batch itself. */
void rmr_pdf_batch_free(rmr_pdf_batch* b);

/* Serializes batches on worker threads; submitted batches come back from
 * rmr_pdf_pipeline_next in submission order. With no workers, submit
 * serializes the batch before returning. */
typedef struct rmr_pdf_pipeline rmr_pdf_pipeline;

/* A pipeline with the given number of workers, at most 64; 0 serializes on
 * the calling thread. Null when threads or memory are unavailable. */
rmr_pdf_pipeline* rmr_pdf_pipeline_create(size_t workers);
/* Workers worth starting on this machine: the online processors less one,
 * at most 8. */
size_t rmr_pdf_pipeline_workers(void);
void rmr_pdf_pipeline_submit(rmr_pdf_pipeline* p, rmr_pdf_batch* b);
/* Batches submitted and not yet returned by rmr_pdf_pipeline_next. */
size_t rmr_pdf_pipeline_pending(const rmr_pdf_pipeline* p);
/* The oldest submitted batch once serialized, waiting for it when wait is
 * set; null when none is pending or (without wait) it is still running. */
rmr_pdf_batch* rmr_pdf_pipeline_next(rmr_pdf_pipeline* p, bool wait);
/* Waits for running batches, frees every batch not yet returned and stops
 * the workers. Null is a no-op. */
void rmr_pdf_pipeline_free(rmr_pdf_pipeline* p);

/* SHA-256 of size bytes, the key for deduplicating identical images. */
typedef rmr_sha256_digest rmr_pdf_digest;
rmr_pdf_digest rmr_pdf_hash(const uint8_t* bytes, size_t size);

#endif
