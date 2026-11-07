/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Page marks written by rmrender instead of CoreGraphics. CoreGraphics prints
 * every coordinate through vfprintf and deflates every stream on the calling
 * thread, which dominates the export of ink-heavy documents. For pages whose
 * marks are fills, strokes, grain masks and erasers, this writer emits the
 * operators CoreGraphics would, with the same numbers computed by the same
 * floating-point steps and printed the way it prints them, and deflates them
 * on worker threads. Text, images, embedded PDFs and transparency groups stay
 * with CoreGraphics. */

#ifndef RMR_APPLE_PDF_WRITER_H
#define RMR_APPLE_PDF_WRITER_H
#include <rmrender/output.h>
#include <rmrender/pdf.h>

typedef struct rmr_apple_writer rmr_apple_writer;

/* A writer for one export. decimals (0, or 1 to 6) rounds path coordinates
 * as rmr_pdf_options.decimals does. merged makes a document completed by
 * rmr_apple_writer_merge; otherwise each page becomes an overlay PDF of its
 * own. */
rmr_status rmr_apple_writer_create(const rmr_limits* limits, unsigned decimals,
                                   bool merged, rmr_apple_writer** out);
void rmr_apple_writer_free(rmr_apple_writer* w);

/* Merged documents: writes the document to output as it is produced instead
 * of collecting it (see rmr_pdf_builder_output); merge then leaves its out
 * unused. base, CoreGraphics' output so far, gives the document its version
 * now, before any of it is written. Before the first page. */
void rmr_apple_writer_output(rmr_apple_writer* w, const rmr_output* output,
                             const uint8_t* base, size_t size);

/* Where the writable tail of plan starts: after its last draw only
 * CoreGraphics can serialize (text, images, embedded PDFs, transparency
 * groups), 0 when it has none. A page typically draws its text layer and
 * template labels first and its ink after them. */
size_t rmr_apple_writer_split(const rmr_plan* plan);

/* Merged documents: serializes draws first onwards of plan for base page
 * `page`, drawn under sheet without a clip as rmr_apple_draw_context draws
 * them, after whatever CoreGraphics drew on that page, and queues their
 * deflation. Fails as rmr_plan_execute does, filling error. */
rmr_status rmr_apple_writer_page(rmr_apple_writer* w, size_t page,
                                 const rmr_plan* plan, size_t first,
                                 const rmr_sheet* sheet, rmr_cancel_fn cancel,
                                 void* context, rmr_error* error);

/* Merged documents: the rmr_apple_mask_fn hook for the masks CoreGraphics
 * draws. It records each mask's bytes as the writer's do, in drawing order,
 * so that both give a mask the dimensions first seen with its bytes, as
 * CoreGraphics' image deduplication alone would. context is the writer. */
bool rmr_apple_writer_mask(void* context, const rmr_mask_view* mask,
                           rmr_mask_view* out);

/* Batches queued and not yet taken back. */
size_t rmr_apple_writer_pending(const rmr_apple_writer* w);

/* Merged documents: appends the deflated pages to the document, waiting for
 * all of them when all is set, otherwise only while too many are queued. A
 * failure (RMR_ERR_LIMIT past maximum_pdf_bytes) leaves error to the caller. */
rmr_status rmr_apple_writer_flush(rmr_apple_writer* w, bool all);

/* Pages written by rmr_apple_writer_page. */
size_t rmr_apple_writer_pages(const rmr_apple_writer* w);

/* Completes the document over CoreGraphics' base PDF, whose pages lack the
 * marks written here; failures as rmr_apple_writer_flush. */
rmr_status rmr_apple_writer_merge(rmr_apple_writer* w, const uint8_t* base,
                                  size_t size, rmr_buffer* out);

/* Overlays: serializes plan, which rmr_apple_writer_split must start at 0,
 * as a one-page PDF clipped to sheet, as rmr_apple_pdf_sheets draws it, and
 * queues it under tag. */
rmr_status rmr_apple_writer_overlay(rmr_apple_writer* w, size_t tag,
                                    const rmr_plan* plan,
                                    const rmr_sheet* sheet,
                                    rmr_cancel_fn cancel, void* context,
                                    rmr_error* error);

/* The oldest queued overlay once written, waiting for it when wait is set:
 * true with out and tag filled, false when none is ready or the overlay
 * failed (status says which; RMR_OK when none is ready). */
bool rmr_apple_writer_next_overlay(rmr_apple_writer* w, bool wait,
                                   rmr_buffer* out, size_t* tag,
                                   rmr_status* status);

#endif
