/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_PROGRESS_H
#define RMR_PROGRESS_H
#include <rmrender/output.h>

/* Borrowed by the writers for one document export, on the calling thread. */
typedef struct {
  rmr_progress_fn fn;
  void* ctx;
  size_t completed, total;
} rmr_progress;

/* Report each newly settled page once, in order, including empty pages. */
static inline void rmr_progress_advance_to(rmr_progress* p, size_t watermark) {
  if (!p || watermark <= p->completed) return;
  if (p->fn)
    while (p->completed < watermark) p->fn(p->ctx, ++p->completed, p->total);
  else
    p->completed = watermark;
}

/* Failure cleanup may settle earlier output to find its error, but must
 * never report it. Silence the shared state before draining that work. */
static inline void rmr_progress_silence(rmr_progress* p) {
  if (p) p->fn = nullptr;
}

#endif
