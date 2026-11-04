/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_LINUX_DOCUMENT_H
#define RMR_LINUX_DOCUMENT_H
#include "archive.h"
#include "linux_pdf.h"
#include "linux_text.h"

/* Render an assembled RMDoc archive document. Zero maximum_pages means all.
 * Archive attachments are used unless resources.image overrides them. Templates
 * and fonts stay explicit. Imported PDF pages use native point sizing; notebook
 * pages use paper. No source file is changed. Parsing errors fail the
 * operation; recoverable parser losses are counted in report. Dependency/native
 * allocations are outside portable plan accounting; aggregate retained plans
 * share limits. */
[[nodiscard]] RMR_API rmr_status rmr_linux_document_pdf(
    const rmr_archive_document* document, const rmr_page_resources* resources,
    const rmr_linux_fonts* fonts, rmr_paper paper, size_t maximum_pages,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* context,
    rmr_buffer* out, rmr_document_report* report, rmr_error* error);

/* Preserve imported PDF objects through a strict incremental update. Ordinary
 * notebooks still render fresh. Unsupported PDF structures or changed source
 * page membership/order/visibility fail, never redraw. Link counters are zero
 * in incremental mode: original annotations are retained, not enumerated.
 * maximum_pages truncating an imported PDF is unsupported. */
[[nodiscard]] RMR_API rmr_status rmr_linux_document_pdf_preserving(
    const rmr_archive_document* document, const rmr_page_resources* resources,
    const rmr_linux_fonts* fonts, rmr_paper paper, size_t maximum_pages,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* context,
    rmr_buffer* out, rmr_document_report* report, rmr_error* error);

/* Fresh preserving export allowing source-page reorder/removal/duplication.
 * Notebooks render fresh. Imported PDFs follow pdf_recompose's
 * native-coordinate and form/signature contract. No silent fallback for
 * unsupported page types. Link counters stay zero because original objects are
 * copied, not enumerated. */
[[nodiscard]] RMR_API rmr_status rmr_linux_document_pdf_recomposed(
    const rmr_archive_document* document, const rmr_page_resources* resources,
    const rmr_linux_fonts* fonts, rmr_paper paper, size_t maximum_pages,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* context,
    rmr_buffer* out, rmr_document_report* report, rmr_error* error);

/* One standalone SVG document per page, on the sheets rmr_linux_document_pdf
 * uses. Typed text becomes glyph outlines (see rmr_linux_page_svg). Visible
 * imported PDF pages cannot stay vector in SVG: each is rasterized at
 * raster_dpi (zero selects 150) by the platform renderer and embedded as PNG
 * under the vector marks; source crop boxes, links and outlines are not
 * represented. maximum_pdf_bytes bounds the total SVG output. */
[[nodiscard]] RMR_API rmr_status rmr_linux_document_svg(
    const rmr_archive_document* document, const rmr_page_resources* resources,
    const rmr_linux_fonts* fonts, rmr_paper paper, size_t maximum_pages,
    double raster_dpi, const rmr_limits* limits, rmr_cancel_fn cancel,
    void* context, rmr_svg_pages* out, rmr_document_report* report,
    rmr_error* error);

#endif
