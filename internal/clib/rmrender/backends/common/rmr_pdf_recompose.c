/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Page selection, overlays and a fresh rewrite in one call, shared by the Apple
 * and Cairo backends. Each one compiles this file with its own
 * rmr_pdf_platform.h on the include path. */

#include <rmrender/pdf.h>
#include <stdlib.h>

#include "../../src/rmr_pdf_edit.h"
#include "rmr_pdf_options.h"
#include "rmr_pdf_platform.h"

rmr_status RMR_PDF_NATIVE(pdf_recompose_marks)(
    const RMR_PDF_NATIVE(pdf_source) * source, const rmr_pdf_page* pages,
    size_t count, const rmr_pdf_marks* marks, const rmr_limits* given,
    const rmr_pdf_options* options, rmr_cancel_fn cancel, void* context,
    const rmr_output* output, rmr_buffer* out, rmr_error* error) {
  if (!pages || !count || (!out && !output))
    return RMR_PDF_FAIL(error, RMR_ERR_INVALID_ARGUMENT,
                        "missing PDF recomposition input");
  rmr_pdf_options checked;
  if (!rmr_pdf_options_read(options, &checked))
    return RMR_PDF_FAIL(error, RMR_ERR_INVALID_ARGUMENT, "invalid PDF options");
  if (!source)
    return RMR_PDF_NATIVE(pdf_overlay_marks)(nullptr, pages, count, marks,
                                             given, options, cancel, context,
                                             output, out, nullptr, error);

  rmr_limits limits = given ? *given : rmr_limits_default();
  if (count > limits.maximum_draws ||
      count >
          limits.maximum_plan_bytes / (sizeof(size_t) + sizeof(rmr_pdf_page)))
    return RMR_PDF_FAIL(error, RMR_ERR_LIMIT,
                        "PDF recomposition allocation limit");

  size_t* indices = calloc(count, sizeof *indices);
  rmr_pdf_page* selected_pages = calloc(count, sizeof *selected_pages);
  if (!indices || !selected_pages) {
    free(indices);
    free(selected_pages);
    return RMR_PDF_FAIL(error, RMR_ERR_OUT_OF_MEMORY,
                        "PDF recomposition allocation failed");
  }
  limits.maximum_plan_bytes -=
      count * (sizeof *indices + sizeof *selected_pages);

  rmr_status status = RMR_OK;
  for (size_t i = 0; i < count; i++) {
    if (cancel && cancel(context)) {
      status = RMR_ERR_CANCELLED;
      break;
    }
    if (!pages[i].has_source || !pages[i].source_visible) {
      status = RMR_ERR_UNSUPPORTED;
      break;
    }
    indices[i] = pages[i].source_page;
    selected_pages[i] = pages[i];
    selected_pages[i].source_page = i;
  }
  if (status != RMR_OK) {
    free(indices);
    free(selected_pages);
    return RMR_PDF_FAIL(
        error, status,
        status == RMR_ERR_CANCELLED
            ? "PDF recomposition cancelled"
            : "PDF recomposition needs a visible source page on every page");
  }

  rmr_buffer selected = {0}, painted = {0};
  RMR_PDF_NATIVE(pdf_source)* copy = nullptr;
  status =
      rmr_pdf_select_pages(RMR_PDF_BYTES(source), RMR_PDF_SIZE(source), indices,
                           count, &limits, cancel, context, &selected, error);
  if (status == RMR_OK)
    status = RMR_PDF_NATIVE(pdf_source_create)(selected.bytes, selected.size,
                                               &limits, &copy, error);
  rmr_buffer_free(&selected);

  if (status == RMR_OK)
    status = RMR_PDF_NATIVE(pdf_incremental_marks)(
        copy, selected_pages, count, marks, &limits, options, cancel, context,
        nullptr, &painted, nullptr, error);
  RMR_PDF_NATIVE(pdf_source_free)(copy);

  /* Only the rewrite goes to output as it is written: it reads the whole
   * painted file, which reads the whole selection. */
  if (status == RMR_OK)
    status = rmr_pdf_rewrite_output(painted.bytes, painted.size, &limits,
                                    cancel, context, output, out, error);
  rmr_buffer_free(&painted);

  free(indices);
  free(selected_pages);
  /* Each step has already reported its own failure. */
  return status;
}

rmr_status RMR_PDF_NATIVE(pdf_recompose)(const RMR_PDF_NATIVE(pdf_source) *
                                             source,
                                         const rmr_pdf_page* pages,
                                         size_t count, const rmr_limits* limits,
                                         rmr_cancel_fn cancel, void* context,
                                         rmr_buffer* out, rmr_error* error) {
  return RMR_PDF_NATIVE(pdf_recompose_marks)(source, pages, count, nullptr,
                                             limits, nullptr, cancel, context,
                                             nullptr, out, error);
}

rmr_status RMR_PDF_NATIVE(pdf_recompose_with_options)(
    const RMR_PDF_NATIVE(pdf_source) * source, const rmr_pdf_page* pages,
    size_t count, const rmr_limits* limits, const rmr_pdf_options* options,
    rmr_cancel_fn cancel, void* context, rmr_buffer* out, rmr_error* error) {
  return RMR_PDF_NATIVE(pdf_recompose_marks)(source, pages, count, nullptr,
                                             limits, options, cancel, context,
                                             nullptr, out, error);
}
