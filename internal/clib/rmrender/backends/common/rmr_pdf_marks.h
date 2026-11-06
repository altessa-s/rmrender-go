/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Page marks supplied on demand to the PDF writers, so document export need
 * not hold every page's plan at once: a long annotated document keeps only the
 * plans that fit its plan budget and rebuilds the others as they are drawn. */

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
} rmr_pdf_marks;

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
