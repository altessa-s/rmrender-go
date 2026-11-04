/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <rmlines/parser.h>
#include <rmrender/linux_document.h>

#include "rmr_typography_internal.h"

typedef enum {
  PDF_FRESH,
  PDF_INCREMENTAL,
  PDF_RECOMPOSED,
  SVG_PAGES
} pdf_output_mode;

/* Renders one assembled page as SVG on its sheet: the visible source PDF page
 * becomes a PDF command under the marks, mapped like the PDF overlay maps it.
 * used counts output bytes already produced against maximum_pdf_bytes. */
static rmr_status page_svg(const rmr_pdf_page* page, rmr_asset_view pdf,
                           double raster_dpi, const rmr_limits* limits,
                           size_t used, rmr_cancel_fn cancel, void* context,
                           rmr_buffer* out, rmr_error* e) {
  size_t marks = rmr_plan_count(page->marks);
  bool source = page->has_source && page->source_visible;
  rmr_limits remaining = *limits;
  if (marks + 1 > remaining.maximum_plan_bytes / sizeof(rmr_draw))
    return rmr_linux_error(e, RMR_ERR_LIMIT, "SVG page scratch limit");

  rmr_draw* draws = calloc(marks + 1, sizeof *draws);
  if (!draws)
    return rmr_linux_error(e, RMR_ERR_OUT_OF_MEMORY,
                           "SVG page allocation failed");
  remaining.maximum_plan_bytes -= (marks + 1) * sizeof *draws;

  rmr_rect r = page->source_rect;
  rmr_element clip[] = {
      {.kind = RMR_MOVE, .end = {r.x, r.y}},
      {.kind = RMR_LINE, .end = {r.x + r.width, r.y}},
      {.kind = RMR_LINE, .end = {r.x + r.width, r.y + r.height}},
      {.kind = RMR_LINE, .end = {r.x, r.y + r.height}},
      {.kind = RMR_CLOSE}};
  if (source)
    draws[0] = (rmr_draw){
        .mode = RMR_PDF,
        .path = {clip, 5},
        .color = {0, 0, 0, 1},
        .image = {pdf.bytes, pdf.size, {r.width, 0, 0, r.height, r.x, r.y}},
        .pdf_page = page->source_page};
  for (size_t i = 0; i < marks; i++)
    draws[(source ? 1 : 0) + i] = *rmr_plan_draw(page->marks, i);

  rmr_plan* plan = nullptr;
  rmr_status s =
      rmr_plan_create(draws, marks + (source ? 1 : 0),
                      rmr_plan_viewport(page->marks), &remaining, &plan, e);
  free(draws);
  if (s != RMR_OK) return s;

  remaining = *limits;
  remaining.maximum_pdf_bytes -= used;
  rmr_svg_pages pages = {0};
  const rmr_plan* plans[] = {plan};
  s = rmr_linux_svg(plans, &page->sheet, 1, raster_dpi, &remaining, cancel,
                    context, &pages, e);
  rmr_plan_free(plan);
  if (s != RMR_OK) return s;

  *out = pages.pages[0];
  pages.pages[0] = (rmr_buffer){0};
  rmr_svg_pages_free(&pages);
  return RMR_OK;
}

/* Shared implementation behind the three public document-to-PDF entry
 * points and SVG output; lays out every page into a plan and sheet, then
 * dispatches to recompose, incremental, or overlay PDF output according to
 * mode, or renders each page to SVG as soon as it is assembled. */
static rmr_status document_pdf(const rmr_archive_document* document,
                               const rmr_page_resources* given_resources,
                               const rmr_linux_fonts* fonts, rmr_paper paper,
                               size_t maximum_pages, const rmr_limits* given,
                               rmr_cancel_fn cancel, void* context,
                               rmr_buffer* out, rmr_document_report* report,
                               rmr_error* e, pdf_output_mode mode,
                               double raster_dpi, rmr_svg_pages* svg) {
  if (!document || (mode == SVG_PAGES ? !svg : !out) ||
      paper > RMR_PAPER_TABLET)
    return rmr_linux_error(e, RMR_ERR_INVALID_ARGUMENT,
                           "invalid document render input");

  rmr_limits limits = given ? *given : rmr_limits_default(), remaining = limits;
  rmr_asset_view pdf = rmr_archive_document_pdf(document);
  rmr_linux_pdf_source* source = nullptr;
  rmr_status status = RMR_OK;
  bool reported = false; /* set once a PDF entry point has filled the error */

  if (pdf.bytes) {
    status =
        rmr_linux_pdf_source_create(pdf.bytes, pdf.size, &limits, &source, e);
    if (status != RMR_OK) return status;
  }

  size_t listed = rmr_archive_document_count(document),
         total = listed ? listed : rmr_linux_pdf_source_count(source);
  if (!total) total = 1; /* An empty notebook exports one blank sheet. */
  size_t count = maximum_pages && maximum_pages < total ? maximum_pages : total;

  if (!count || count > limits.maximum_draws ||
      count > limits.maximum_plan_bytes /
                  (sizeof(rm_page*) + sizeof(rmr_pdf_page))) {
    rmr_linux_pdf_source_free(source);
    return rmr_linux_error(e, count ? RMR_ERR_LIMIT : RMR_ERR_INVALID_ARGUMENT,
                           "document page count invalid");
  }

  rm_page** parsed = calloc(count, sizeof *parsed);
  rmr_pdf_page* pages = calloc(count, sizeof *pages);
  rmr_buffer* documents =
      mode == SVG_PAGES ? calloc(count, sizeof *documents) : nullptr;
  if (!parsed || !pages || (mode == SVG_PAGES && !documents)) {
    free(parsed);
    free(pages);
    free(documents);
    rmr_linux_pdf_source_free(source);
    return rmr_linux_error(e, RMR_ERR_OUT_OF_MEMORY,
                           "document page allocation failed");
  }
  remaining.maximum_plan_bytes -= count * (sizeof *parsed + sizeof *pages);
  if (documents) remaining.maximum_plan_bytes -= count * sizeof *documents;
  size_t produced = 0;
  rmr_document_report result = {.document_pages = total,
                                .rendered_pages = count};

  for (size_t i = 0; i < count && listed; i++) {
    if (cancel && cancel(context)) {
      status = RMR_ERR_CANCELLED;
      goto done;
    }
    status = rmr_archive_document_parse_page(document, i, &parsed[i], e);
    if (status != RMR_OK) goto done;
    if (parsed[i]->skipped_blocks > SIZE_MAX - result.skipped_blocks) {
      status = RMR_ERR_LIMIT;
      goto done;
    }
    result.skipped_blocks += parsed[i]->skipped_blocks;
  }

  rmr_point canvas;
  status = rmr_lines_document_canvas((const rm_page* const*)parsed, count,
                                     rmr_archive_document_canvas(document),
                                     rmr_archive_document_landscape(document),
                                     &remaining, &canvas, e);
  if (status != RMR_OK) goto done;

  rmr_page_resources resources =
      given_resources ? *given_resources : (rmr_page_resources){0};
  if (!resources.image) {
    resources.image = rmr_archive_document_image;
    resources.image_context = (void*)(uintptr_t)document;
  }

  for (size_t i = 0; i < count; i++) {
    if (cancel && cancel(context)) {
      status = RMR_ERR_CANCELLED;
      goto done;
    }

    rm_page blank = {.background_visible = true, .root_document_visible = true};
    const rm_page* page = parsed[i] ? parsed[i] : &blank;
    rmr_document_page record =
        listed ? *rmr_archive_document_page(document, i)
               : (rmr_document_page){.has_source = true, .source_page = i};
    if (source)
      record =
          rmr_document_page_checked(record, rmr_linux_pdf_source_count(source));
    result.refused_sources += record.refused_source ? 1u : 0u;
    rmr_point size = canvas, points = {canvas.x * RMR_POINTS_PER_UNIT,
                                       canvas.y * RMR_POINTS_PER_UNIT};

    if (source && record.has_source) {
      status =
          rmr_linux_pdf_source_size(source, record.source_page, &points, e);
      if (status != RMR_OK) goto done;
      size = (rmr_point){points.x / RMR_POINTS_PER_UNIT,
                         points.y / RMR_POINTS_PER_UNIT};
    } else if (source && page->has_paper_size) {
      const rm_page* one[] = {page};
      status = rmr_lines_document_canvas(one, 1, (rmr_point){0}, false,
                                         &remaining, &size, e);
      if (status != RMR_OK) goto done;
      points = (rmr_point){size.x * RMR_POINTS_PER_UNIT,
                           size.y * RMR_POINTS_PER_UNIT};
    }

    // Imported PDF annotations do not paint notebook template backgrounds.
    // This also applies to inserted sheets, matching AnnotatedPDFRenderer.
    rm_page annotation = *page;
    if (source) {
      annotation.has_template_name = false;
      page = &annotation;
    }

    rmr_plan* plan = nullptr;
    status =
        (mode == SVG_PAGES ? rmr_linux_page_outline_plan : rmr_linux_page_plan)(
            page, size, source ? RMR_EDGE_CENTRELINE : RMR_EDGE_RIBBON,
            source ? 5 * 72 / 25.4 / RMR_POINTS_PER_UNIT : 0, &resources, fonts,
            &remaining, &plan, e);
    if (status != RMR_OK) goto done;
    pages[i].marks = plan;
    remaining.maximum_plan_bytes -= rmr_plan_bytes(plan);

    rmr_rect viewport = rmr_plan_viewport(plan);
    status = source ? rmr_sheet_create(viewport, points.x, points.y,
                                       RMR_POINTS_PER_UNIT, &pages[i].sheet, e)
                    : rmr_paper_sheet(paper, size.x, size.y, viewport,
                                      &pages[i].sheet, e);
    if (status != RMR_OK) goto done;

    if (source) {
      // PDF dimensions remain fractional; bitmap sheet rounding is a
      // separate public contract and must not alter the page geometry.
      double scale = RMR_POINTS_PER_UNIT;
      pages[i].sheet.width = fmax(points.x, viewport.width * scale);
      pages[i].sheet.height = fmax(points.y, viewport.height * scale);
      pages[i].sheet.transform =
          (rmr_transform){scale,
                          0,
                          0,
                          -scale,
                          (pages[i].sheet.width - viewport.width * scale) / 2 -
                              viewport.x * scale,
                          pages[i].sheet.height + viewport.y * scale};
    }

    pages[i].source_rect = (rmr_rect){-size.x / 2, 0, size.x, size.y};
    pages[i].source_page = record.source_page;
    pages[i].has_source = source && record.has_source;
    pages[i].source_visible = page->root_document_visible;

    if (mode == SVG_PAGES) {
      status = page_svg(&pages[i], pdf, raster_dpi, &remaining, produced,
                        cancel, context, &documents[i], e);
      reported = status != RMR_OK;
      if (status != RMR_OK) goto done;

      produced += documents[i].size;
      remaining.maximum_plan_bytes += rmr_plan_bytes(pages[i].marks);
      rmr_plan_free((rmr_plan*)(uintptr_t)pages[i].marks);
      pages[i].marks = nullptr;
    }
  }

  if (mode == SVG_PAGES) {
    *svg = (rmr_svg_pages){documents, count};
    documents = nullptr;
    if (report) *report = result;
    goto done;
  }

  if (mode == PDF_RECOMPOSED && source)
    status = rmr_linux_pdf_recompose(source, pages, count, &remaining, cancel,
                                     context, out, e);
  else if (mode == PDF_INCREMENTAL && source)
    status = rmr_linux_pdf_incremental(source, pages, count, &remaining, cancel,
                                       context, out, nullptr, e);
  else
    status = rmr_linux_pdf_overlay(source, pages, count, &remaining, cancel,
                                   context, out, &result.pdf, e);
  reported = status != RMR_OK;
  if (status == RMR_OK && report) *report = result;

done:
  for (size_t i = 0; i < count; i++) {
    rm_page_free(parsed[i]);
    rmr_plan_free((rmr_plan*)(uintptr_t)pages[i].marks);
  }
  rmr_svg_pages unused = {documents, documents ? count : 0};
  rmr_svg_pages_free(&unused);
  free(parsed);
  free(pages);
  rmr_linux_pdf_source_free(source);
  return status == RMR_OK || reported
             ? status
             : rmr_linux_error(e, status, "document render failed");
}

rmr_status rmr_linux_document_pdf(
    const rmr_archive_document* document, const rmr_page_resources* resources,
    const rmr_linux_fonts* fonts, rmr_paper paper, size_t maximum_pages,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* context,
    rmr_buffer* out, rmr_document_report* report, rmr_error* error) {
  return document_pdf(document, resources, fonts, paper, maximum_pages, limits,
                      cancel, context, out, report, error, PDF_FRESH, 0,
                      nullptr);
}

rmr_status rmr_linux_document_pdf_preserving(
    const rmr_archive_document* document, const rmr_page_resources* resources,
    const rmr_linux_fonts* fonts, rmr_paper paper, size_t maximum_pages,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* context,
    rmr_buffer* out, rmr_document_report* report, rmr_error* error) {
  return document_pdf(document, resources, fonts, paper, maximum_pages, limits,
                      cancel, context, out, report, error, PDF_INCREMENTAL, 0,
                      nullptr);
}

rmr_status rmr_linux_document_pdf_recomposed(
    const rmr_archive_document* document, const rmr_page_resources* resources,
    const rmr_linux_fonts* fonts, rmr_paper paper, size_t maximum_pages,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* context,
    rmr_buffer* out, rmr_document_report* report, rmr_error* error) {
  return document_pdf(document, resources, fonts, paper, maximum_pages, limits,
                      cancel, context, out, report, error, PDF_RECOMPOSED, 0,
                      nullptr);
}

rmr_status rmr_linux_document_svg(
    const rmr_archive_document* document, const rmr_page_resources* resources,
    const rmr_linux_fonts* fonts, rmr_paper paper, size_t maximum_pages,
    double raster_dpi, const rmr_limits* limits, rmr_cancel_fn cancel,
    void* context, rmr_svg_pages* out, rmr_document_report* report,
    rmr_error* error) {
  return document_pdf(document, resources, fonts, paper, maximum_pages, limits,
                      cancel, context, nullptr, report, error, SVG_PAGES,
                      raster_dpi, out);
}
