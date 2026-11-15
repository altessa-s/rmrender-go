/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Page marks supplied on demand to the PDF writers, so document export need
 * not hold every page's plan at once: each page's plan is built as the page is
 * drawn and freed once it is. */

#ifndef RMR_PDF_MARKS_H
#define RMR_PDF_MARKS_H
#include <rmrender/pdf.h>

typedef struct {
  /* Borrow page i's marks into *marks until release(context, i); fills
   * error on failure. Pages are acquired in order, one at a time. */
  rmr_status (*acquire)(void* context, size_t page, const rmr_plan** marks,
                        rmr_error* error);
  void (*release)(void* context, size_t page);
  void* context;
  /* Document progress only; plan release also happens on drawing failure. */
  rmr_progress_fn progress;
  void* progress_context;
} rmr_pdf_marks;

/* Report a successfully completed page, after its pending output is settled.
 * Never used when draining earlier pages during failure cleanup. */
static inline void rmr_pdf_marks_progress(const rmr_pdf_marks* marks,
                                          size_t completed, size_t total) {
  if (marks && marks->progress)
    marks->progress(marks->progress_context, completed, total);
}

/* Report the settled prefix, including pages with no writer batches. */
static inline void rmr_pdf_marks_progress_until(const rmr_pdf_marks* marks,
                                                size_t* reported,
                                                size_t watermark,
                                                size_t total) {
  if (marks && marks->progress)
    while (*reported < watermark)
      rmr_pdf_marks_progress(marks, ++*reported, total);
}

/* Page i's marks: pages[i].marks, or acquired from marks when given. */
static inline rmr_status rmr_pdf_marks_acquire(const rmr_pdf_marks* marks,
                                               const rmr_pdf_page* pages,
                                               size_t i, const rmr_plan** out,
                                               rmr_error* error) {
  if (!marks) {
    *out = pages[i].marks;
    return RMR_OK;
  }
  *out = nullptr;
  return marks->acquire(marks->context, i, out, error);
}

/* Return page i's marks taken by rmr_pdf_marks_acquire. */
static inline void rmr_pdf_marks_release(const rmr_pdf_marks* marks, size_t i) {
  if (marks) marks->release(marks->context, i);
}

#endif
