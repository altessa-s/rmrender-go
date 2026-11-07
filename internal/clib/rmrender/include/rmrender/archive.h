/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_ARCHIVE_H
#define RMR_ARCHIVE_H
#include <rmdoc/rmdoc.h>

#include "document.h"
#include "rmlines.h"

typedef struct rmr_archive_document rmr_archive_document;

/* Borrow an unpacked RMDoc archive. Archive bytes/names must remain unchanged
 * until free. Only the named document is read. Missing metadata recovers .rm
 * files in name order; malformed metadata is an explicit error. Tombstones are
 * never recovered as unlisted pages. Parsing of individual .rm pages is lazy.
 */
[[nodiscard]] RMR_API rmr_status rmr_archive_document_create(
    const rmd_archive* archive, const char* document_id, bool include_unlisted,
    const rmr_limits* limits, rmr_archive_document** out, rmr_error* error);

/* Releases document and its owned index/string memory; a null pointer is a
 * no-op. */
RMR_API void rmr_archive_document_free(rmr_archive_document* document);

/* Number of pages in the document; null document returns 0. */
RMR_API size_t rmr_archive_document_count(const rmr_archive_document* document);

/* Borrowed page at index, valid until document is freed; out-of-range or
 * null document returns nullptr. */
RMR_API const rmr_document_page* rmr_archive_document_page(
    const rmr_archive_document* document, size_t index);

/* Declared canvas size, or {0,0} if absent or document is null. */
RMR_API rmr_point
rmr_archive_document_canvas(const rmr_archive_document* document);

/* Declared landscape orientation; null document returns false. */
RMR_API bool rmr_archive_document_landscape(
    const rmr_archive_document* document);

/* Borrowed source PDF bytes, or an empty view if the document has none. */
RMR_API rmr_asset_view
rmr_archive_document_pdf(const rmr_archive_document* document);

/* Own parsed result; release with rm_page_free. A listed page without a blob
 * becomes an empty page with its template. Parser failures propagate; skipped
 * blocks remain visible in rm_page.skipped_blocks. */
[[nodiscard]] RMR_API rmr_status
rmr_archive_document_parse_page(const rmr_archive_document* document,
                                size_t index, rm_page** out, rmr_error* error);

/* rmr_archive_document_parse_page for a page that may be unreadable (see
 * rmr_unreadable_policy in output.h): a page whose .rm file RMLines rejects
 * as malformed, truncated or of an unsupported version is answered as a page
 * without a blob is, an empty page with its template, and *unreadable
 * receives the status parsing it failed with (BACKEND or UNSUPPORTED); any
 * other page leaves RMR_OK there. Allocation failures and limits fail as they
 * do there. */
[[nodiscard]] RMR_API rmr_status rmr_archive_document_read_page(
    const rmr_archive_document* document, size_t index, rm_page** out,
    rmr_status* unreadable, rmr_error* error);

/* Resolver context for page resources; returns borrowed attachment bytes. */
RMR_API rmr_status rmr_archive_document_image(void* document, rm_str file,
                                              rmr_asset_view* out);

#endif
