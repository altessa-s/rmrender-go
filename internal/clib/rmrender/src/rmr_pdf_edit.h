/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Internal backend helpers; not part of the public SDK. */
#ifndef RMR_PDF_EDIT_H
#define RMR_PDF_EDIT_H
#include <rmrender/output.h>
#include <rmrender/pdf.h>

/* rmr_pdf_incremental and rmr_pdf_rewrite writing the file to output, which
 * the caller has checked, as it is written, out unused, when output is set;
 * out otherwise. The incremental update copies source to output first. */
RMR_API rmr_status rmr_pdf_incremental_output(
    const uint8_t* source, size_t size, const rmr_pdf_overlay* overlays,
    size_t count, const rmr_limits* limits, rmr_cancel_fn cancel, void* context,
    const rmr_output* output, rmr_buffer* out,
    rmr_pdf_incremental_report* report, rmr_error* error);
RMR_API rmr_status rmr_pdf_rewrite_output(const uint8_t* source, size_t size,
                                          const rmr_limits* limits,
                                          rmr_cancel_fn cancel, void* context,
                                          const rmr_output* output,
                                          rmr_buffer* out, rmr_error* error);

/* Appends an incremental update clipping a one-page PDF's content to path,
 * mapped through transform. */
RMR_API rmr_status rmr_pdf_clip(const uint8_t* pdf, size_t size,
                                rmr_path_view path, rmr_transform transform,
                                const rmr_limits* limits, rmr_buffer* out);

typedef struct {
  size_t page;
  rmr_point point;
} rmr_pdf_destination;

typedef struct {
  size_t page;
  rmr_rect rect;
  rmr_pdf_destination destination;
} rmr_pdf_local_link;

typedef struct {
  size_t parent; /* SIZE_MAX for top level; otherwise an earlier entry. */
  const uint8_t* title; /* UTF-16LE, without trailing NUL. */
  size_t title_size;
  rmr_pdf_destination destination; /* SIZE_MAX page for a grouping node. */
  bool open;
} rmr_pdf_outline;

/* Appends an incremental update adding local link annotations and,
 * optionally, a new document outline. */
RMR_API rmr_status rmr_pdf_navigation(
    const uint8_t* pdf, size_t size, const rmr_pdf_local_link* links,
    size_t link_count, const rmr_pdf_outline* outline, size_t outline_count,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* context,
    rmr_buffer* out);

/* rmr_plan_execute over draws first to end - 1 only: the same checks,
 * cancellation points and error reports, the failing draw's index counted
 * from the plan's start. */
RMR_API rmr_status rmr_plan_execute_range(const rmr_plan* plan, size_t first,
                                          size_t end,
                                          const rmr_backend* backend,
                                          rmr_cancel_fn cancel, void* context,
                                          rmr_error* error);

/* A fresh PDF written as it is produced: the caller appends serialized
 * objects numbered from rmr_pdf_builder_reserve, then a finishing call adds
 * the rest of the file and hands over the bytes. Output is bounded by
 * maximum_pdf_bytes, bookkeeping by maximum_plan_bytes and objects by
 * maximum_elements; cancel is polled as objects are written. */
typedef struct rmr_pdf_builder rmr_pdf_builder;

RMR_API rmr_status rmr_pdf_builder_create(const rmr_limits* limits,
                                          rmr_cancel_fn cancel, void* context,
                                          rmr_pdf_builder** out);
/* Releases the builder and any output it still holds; null is a no-op. */
RMR_API void rmr_pdf_builder_free(rmr_pdf_builder* builder);

/* Sends the file to output, which the caller has checked, as it is written
 * instead of collecting it: merge and finish then complete it there and leave
 * their out unused (it may be null). Set before anything is appended. Output
 * leaves in pieces of about 4 MiB, the header in the first, so a base whose
 * version merge would give the file must be given to rmr_pdf_builder_version
 * before then. */
RMR_API void rmr_pdf_builder_output(rmr_pdf_builder* builder,
                                    const rmr_output* output);

/* Gives the file the version of the PDF in base now, as merge would; merge
 * then fails if its base has another once the header has been written. */
RMR_API void rmr_pdf_builder_version(rmr_pdf_builder* builder,
                                     const uint8_t* base, size_t size);

/* The next unused object number; 0 once numbers are exhausted. */
RMR_API uint32_t rmr_pdf_builder_reserve(rmr_pdf_builder* builder);

/* Appends count complete objects ("N 0 obj ... endobj") held in bytes,
 * object numbers[i] starting at offsets[i]. On failure error is filled and
 * the builder can only be freed. */
RMR_API rmr_status rmr_pdf_builder_append(rmr_pdf_builder* builder,
                                          const uint8_t* bytes, size_t size,
                                          const uint32_t* numbers,
                                          const size_t* offsets, size_t count,
                                          rmr_error* error);

/* Objects appended for one page of the base document: a content stream
 * drawn after the page's own, and resources named RMRx<n> (XObjects),
 * RMRg<n> (graphics states) and RMRc<n> (color spaces) after their object
 * numbers n, each listed once. */
typedef struct {
  size_t page;
  uint32_t contents;
  const uint32_t *xobjects, *states, *spaces;
  size_t xobject_count, state_count, space_count;
} rmr_pdf_page_objects;

/* Completes the file with every object of base, renumbered after those
 * appended, its pages extended by pages (each page at most once), its
 * trailer's Root, Info and ID and a cross-reference table; the header takes
 * base's version. On success out owns the file; on failure error is filled.
 * Either way the builder is spent and must still be freed. */
RMR_API rmr_status rmr_pdf_builder_merge(rmr_pdf_builder* builder,
                                         const uint8_t* base, size_t size,
                                         const rmr_pdf_page_objects* pages,
                                         size_t count, rmr_buffer* out,
                                         rmr_error* error);

/* Reserves the page tree and count consecutive page numbers, returned from
 * the first (0 after failing the builder). Once per builder, before pages. */
RMR_API uint32_t rmr_pdf_builder_pages(rmr_pdf_builder* builder, size_t count);

/* Imports a one-page, unrotated PDF as a Form XObject whose BBox is the
 * page's box and whose content is the page's (its stream copied as is, or
 * several streams decoded and joined). As PDFium imports a page, the page's
 * /Group does not make the form a transparency group. *out receives its
 * number. */
RMR_API rmr_status rmr_pdf_builder_form(rmr_pdf_builder* builder,
                                        const uint8_t* pdf, size_t size,
                                        uint32_t* out, rmr_error* error);

/* A Form XObject over [0 0 width height] drawing forms in order, each
 * inside q/Q; *out receives its number. */
RMR_API rmr_status rmr_pdf_builder_wrap(rmr_pdf_builder* builder, double width,
                                        double height, const uint32_t* forms,
                                        size_t count, uint32_t* out,
                                        rmr_error* error);

/* Writes page index: the single page of pdf (its objects imported, its
 * inherited box and rotation made its own) or, when pdf is null, an empty
 * width by height page; then forms drawn after its content, from the
 * initial graphics state, and links added to its annotations. */
RMR_API rmr_status rmr_pdf_builder_page(rmr_pdf_builder* builder, size_t index,
                                        const uint8_t* pdf, size_t size,
                                        double width, double height,
                                        const uint32_t* forms, size_t count,
                                        const rmr_pdf_local_link* links,
                                        size_t link_count, rmr_error* error);

/* Completes a file of pages written by rmr_pdf_builder_page: the outline,
 * page tree, catalog and cross-reference table. Same contract as merge. */
RMR_API rmr_status rmr_pdf_builder_finish(rmr_pdf_builder* builder,
                                          const rmr_pdf_outline* outline,
                                          size_t outline_count, rmr_buffer* out,
                                          rmr_error* error);

#endif
