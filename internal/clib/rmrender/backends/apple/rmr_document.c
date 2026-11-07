/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <rmlines/parser.h>
#include <rmrender/apple_document.h>

#include "../common/rmr_pdf_options.h"
#include "../common/rmr_pdf_output.h"
#include "rmr_pdf_platform.h"
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
    return rmr_apple_text_error(e, RMR_ERR_LIMIT, "SVG page scratch limit");

  rmr_draw* draws = calloc(marks + 1, sizeof *draws);
  if (!draws)
    return rmr_apple_text_error(e, RMR_ERR_OUT_OF_MEMORY,
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
  s = rmr_apple_svg(plans, &page->sheet, 1, raster_dpi, &remaining, cancel,
                    context, &pages, e);
  rmr_plan_free(plan);
  if (s != RMR_OK) return s;

  *out = pages.pages[0];
  pages.pages[0] = (rmr_buffer){0};
  rmr_svg_pages_free(&pages);
  return RMR_OK;
}

/* The size a page's marks are planned at and the viewport that plan gets,
 * measured before the plan is built so the page's sheet can be made early. */
typedef struct {
  rmr_point size;
  rmr_rect viewport;
} page_shape;

/* Build the marks plan of parsed (a blank sheet when null) at size or, with
 * out null, measure only the viewport that plan would get. Marks over a source
 * PDF paint no notebook template, as AnnotatedPDFRenderer, and keep a 5 mm
 * centreline margin; outlines draws typed text as glyph outlines. */
static rmr_status page_marks(const rm_page* parsed, bool source, bool outlines,
                             rmr_point size,
                             const rmr_page_resources* resources,
                             const rmr_apple_fonts* fonts,
                             const rmr_limits* limits, rmr_rect* viewport,
                             rmr_plan** out, rmr_error* e) {
  rm_page page = parsed ? *parsed
                        : (rm_page){.background_visible = true,
                                    .root_document_visible = true};
  if (source) page.has_template_name = false;
  rmr_ink_edge edge = source ? RMR_EDGE_CENTRELINE : RMR_EDGE_RIBBON;
  double margin = source ? 5 * 72 / 25.4 / RMR_POINTS_PER_UNIT : 0;
  if (!out)
    return rmr_apple_page_bounds(&page, size, edge, margin, fonts, limits,
                                 viewport, e);
  return (outlines ? rmr_apple_page_outline_plan : rmr_apple_page_plan)(
      &page, size, edge, margin, resources, fonts, limits, out, e);
}

/* Marks for the PDF writers: each page is parsed and planned as it is drawn,
 * at the shape measured for its sheet, and its plan is freed once drawn, so
 * the export holds one page's plan at a time however long the document. */
typedef struct {
  const rmr_archive_document* document;
  const page_shape* shapes;
  const rmr_page_resources* resources;
  const rmr_apple_fonts* fonts;
  const rmr_limits* limits;
  bool listed, source;
  rmr_plan* built;
} planner;

/* rmr_pdf_marks acquire: page i parsed and planned. */
static rmr_status plan_acquire(void* context, size_t i, const rmr_plan** out,
                               rmr_error* e) {
  planner* p = context;
  rm_page* parsed = nullptr;
  rmr_status status =
      p->listed ? rmr_archive_document_parse_page(p->document, i, &parsed, e)
                : RMR_OK;
  if (status == RMR_OK)
    status =
        page_marks(parsed, p->source, false, p->shapes[i].size, p->resources,
                   p->fonts, p->limits, nullptr, &p->built, e);
  rm_page_free(parsed);
  if (status != RMR_OK) return status;

  rmr_rect viewport = rmr_plan_viewport(p->built);
  if (memcmp(&viewport, &p->shapes[i].viewport, sizeof viewport)) {
    rmr_plan_free(p->built);
    p->built = nullptr;
    return rmr_apple_text_error(e, RMR_ERR_BACKEND,
                                "page plan differs from its measured sheet");
  }
  *out = p->built;
  return RMR_OK;
}

/* rmr_pdf_marks release: free the plan of the page just drawn. */
static void plan_release(void* context, size_t i) {
  (void)i;
  planner* p = context;
  rmr_plan_free(p->built);
  p->built = nullptr;
}

/* Shared implementation behind the three document PDF export modes and
 * SVG output, which renders each page as soon as it is assembled. */
static rmr_status document_pdf(
    const rmr_archive_document* document,
    const rmr_page_resources* given_resources, const rmr_apple_fonts* fonts,
    rmr_paper paper, size_t maximum_pages, const rmr_limits* given,
    const rmr_pdf_options* options, rmr_cancel_fn cancel, void* context,
    const rmr_output* output, rmr_buffer* out, rmr_document_report* report,
    rmr_error* e, pdf_output_mode mode, double raster_dpi, rmr_svg_pages* svg) {
  if (!document || (mode == SVG_PAGES ? !svg : !out && !output) ||
      paper > RMR_PAPER_TABLET)
    return rmr_apple_text_error(e, RMR_ERR_INVALID_ARGUMENT,
                                "invalid document render input");
  rmr_pdf_options checked;
  if (!rmr_pdf_options_read(options, &checked))
    return rmr_apple_text_error(e, RMR_ERR_INVALID_ARGUMENT,
                                "invalid PDF options");
  /* The export writes through guard, so that a failure of output's own is
   * reported as one however deep in the export it happens. */
  rmr_output_guard guard = {0};
  if (output && !rmr_output_valid(output))
    return rmr_apple_text_error(e, RMR_ERR_INVALID_ARGUMENT,
                                "invalid PDF output");
  if (output) rmr_output_guard_init(&guard, output);
  const rmr_output* target = output ? &guard.output : nullptr;

  rmr_limits limits = given ? *given : rmr_limits_default(), remaining = limits;
  rmr_asset_view pdf = rmr_archive_document_pdf(document);
  rmr_apple_pdf_source* source = nullptr;
  rmr_status status = RMR_OK;
  /* Failures detected here; every inner call has already filled e. */
  const char* failure = nullptr;
  if (pdf.bytes) {
    status =
        rmr_apple_pdf_source_create(pdf.bytes, pdf.size, &limits, &source, e);
    if (status != RMR_OK) return status;
  }

  size_t listed = rmr_archive_document_count(document),
         total = listed ? listed : rmr_apple_pdf_source_count(source);
  if (!total) total = 1; /* An empty notebook exports one blank sheet. */
  size_t count = maximum_pages && maximum_pages < total ? maximum_pages : total;
  if (!count || count > limits.maximum_draws ||
      count > limits.maximum_plan_bytes /
                  (sizeof(rm_page) + sizeof(rm_page*) + sizeof(rmr_pdf_page) +
                   sizeof(page_shape))) {
    rmr_apple_pdf_source_free(source);
    return rmr_apple_text_error(
        e, count ? RMR_ERR_LIMIT : RMR_ERR_INVALID_ARGUMENT,
        "document page count invalid");
  }

  /* papers keeps only what rmr_lines_document_canvas reads of each page, its
   * declared paper size, so that no parsed page is held across pages. */
  rm_page* papers = calloc(count, sizeof *papers);
  const rm_page** listing = calloc(count, sizeof *listing);
  rmr_pdf_page* pages = calloc(count, sizeof *pages);
  page_shape* shapes = calloc(count, sizeof *shapes);
  rmr_buffer* documents =
      mode == SVG_PAGES ? calloc(count, sizeof *documents) : nullptr;
  if (!papers || !listing || !pages || !shapes ||
      (mode == SVG_PAGES && !documents)) {
    free(papers);
    free(listing);
    free(pages);
    free(shapes);
    free(documents);
    rmr_apple_pdf_source_free(source);
    return rmr_apple_text_error(e, RMR_ERR_OUT_OF_MEMORY,
                                "document page allocation failed");
  }
  remaining.maximum_plan_bytes -= count * (sizeof *papers + sizeof *listing +
                                           sizeof *pages + sizeof *shapes);
  if (documents) remaining.maximum_plan_bytes -= count * sizeof *documents;
  /* PDF output measures each page's viewport here and builds its plan only
   * when the writer draws it (see planner), so memory stays bounded by one
   * page's plan however long the document. */
  size_t produced = 0;
  rm_page* parsed = nullptr;
  planner marks_planner = {};
  rmr_document_report result = {.document_pages = total,
                                .rendered_pages = count};

  for (size_t i = 0; i < count && listed; i++) {
    if (cancel && cancel(context)) {
      status = RMR_ERR_CANCELLED;
      failure = "document render cancelled";
      goto done;
    }
    status = rmr_archive_document_parse_page(document, i, &parsed, e);
    if (status != RMR_OK) goto done;
    if (parsed->skipped_blocks > SIZE_MAX - result.skipped_blocks) {
      status = RMR_ERR_LIMIT;
      failure = "skipped block count limit";
      goto done;
    }
    result.skipped_blocks += parsed->skipped_blocks;
    papers[i] = (rm_page){.has_paper_size = parsed->has_paper_size,
                          .paper_size = parsed->paper_size};
    listing[i] = &papers[i];
    rm_page_free(parsed);
    parsed = nullptr;
  }

  rmr_point canvas;
  status = rmr_lines_document_canvas(
      listing, count, rmr_archive_document_canvas(document),
      rmr_archive_document_landscape(document), &remaining, &canvas, e);
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
      failure = "document render cancelled";
      goto done;
    }
    if (listed) {
      status = rmr_archive_document_parse_page(document, i, &parsed, e);
      if (status != RMR_OK) goto done;
    }

    rm_page blank = {.background_visible = true, .root_document_visible = true};
    const rm_page* page = parsed ? parsed : &blank;
    rmr_document_page record =
        listed ? *rmr_archive_document_page(document, i)
               : (rmr_document_page){.has_source = true, .source_page = i};
    if (source)
      record =
          rmr_document_page_checked(record, rmr_apple_pdf_source_count(source));
    result.refused_sources += record.refused_source ? 1u : 0u;

    rmr_point size = canvas, points = {canvas.x * RMR_POINTS_PER_UNIT,
                                       canvas.y * RMR_POINTS_PER_UNIT};
    if (source && record.has_source) {
      status =
          rmr_apple_pdf_source_size(source, record.source_page, &points, e);
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

    /* SVG renders the page now from its plan; PDF only measures it here. */
    rmr_rect viewport;
    if (mode == SVG_PAGES) {
      rmr_plan* plan = nullptr;
      status = page_marks(parsed, source, true, size, &resources, fonts,
                          &remaining, nullptr, &plan, e);
      if (status != RMR_OK) goto done;
      pages[i].marks = plan;
      remaining.maximum_plan_bytes -= rmr_plan_bytes(plan);
      viewport = rmr_plan_viewport(plan);
    } else {
      status = page_marks(parsed, source, false, size, &resources, fonts,
                          &remaining, &viewport, nullptr, e);
      if (status != RMR_OK) goto done;
    }

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
    shapes[i] = (page_shape){size, viewport};
    rm_page_free(parsed);
    parsed = nullptr;

    if (mode == SVG_PAGES) {
      status = page_svg(&pages[i], pdf, raster_dpi, &remaining, produced,
                        cancel, context, &documents[i], e);
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

  /* The writers get the whole budget left, and so does each page's plan:
   * one plan is held at a time, freed once its page is drawn. */
  marks_planner = (planner){.document = document,
                            .shapes = shapes,
                            .resources = &resources,
                            .fonts = fonts,
                            .limits = &remaining,
                            .listed = listed,
                            .source = source};
  rmr_pdf_marks marks = {plan_acquire, plan_release, &marks_planner};
  if (mode == PDF_RECOMPOSED && source)
    status =
        rmr_apple_pdf_recompose_marks(source, pages, count, &marks, &remaining,
                                      options, cancel, context, target, out, e);
  else if (mode == PDF_INCREMENTAL && source)
    status = rmr_apple_pdf_incremental_marks(source, pages, count, &marks,
                                             &remaining, options, cancel,
                                             context, target, out, nullptr, e);
  else
    status = rmr_apple_pdf_overlay_marks(source, pages, count, &marks,
                                         &remaining, options, cancel, context,
                                         target, out, &result.pdf, e);
  if (status == RMR_OK && report) *report = result;

done:
  for (size_t i = 0; i < count; i++)
    rmr_plan_free((rmr_plan*)(uintptr_t)pages[i].marks);
  rmr_svg_pages unused = {documents, documents ? count : 0};
  rmr_svg_pages_free(&unused);
  rm_page_free(parsed);
  rmr_plan_free(marks_planner.built);
  free(papers);
  free(listing);
  free(pages);
  free(shapes);
  rmr_apple_pdf_source_free(source);
  if (output && guard.status != RMR_OK) {
    status = guard.status;
    failure = "PDF output write failed";
  }
  return failure ? rmr_apple_text_error(e, status, failure) : status;
}

rmr_status rmr_apple_document_pdf(
    const rmr_archive_document* document, const rmr_page_resources* resources,
    const rmr_apple_fonts* fonts, rmr_paper paper, size_t maximum_pages,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* context,
    rmr_buffer* out, rmr_document_report* report, rmr_error* error) {
  return document_pdf(document, resources, fonts, paper, maximum_pages, limits,
                      nullptr, cancel, context, nullptr, out, report, error,
                      PDF_FRESH, 0, nullptr);
}

rmr_status rmr_apple_document_pdf_preserving(
    const rmr_archive_document* document, const rmr_page_resources* resources,
    const rmr_apple_fonts* fonts, rmr_paper paper, size_t maximum_pages,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* context,
    rmr_buffer* out, rmr_document_report* report, rmr_error* error) {
  return document_pdf(document, resources, fonts, paper, maximum_pages, limits,
                      nullptr, cancel, context, nullptr, out, report, error,
                      PDF_INCREMENTAL, 0, nullptr);
}

rmr_status rmr_apple_document_pdf_recomposed(
    const rmr_archive_document* document, const rmr_page_resources* resources,
    const rmr_apple_fonts* fonts, rmr_paper paper, size_t maximum_pages,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* context,
    rmr_buffer* out, rmr_document_report* report, rmr_error* error) {
  return document_pdf(document, resources, fonts, paper, maximum_pages, limits,
                      nullptr, cancel, context, nullptr, out, report, error,
                      PDF_RECOMPOSED, 0, nullptr);
}

rmr_status rmr_apple_document_pdf_with_options(
    const rmr_archive_document* document, const rmr_page_resources* resources,
    const rmr_apple_fonts* fonts, rmr_paper paper, size_t maximum_pages,
    const rmr_limits* limits, const rmr_pdf_options* options,
    rmr_cancel_fn cancel, void* context, rmr_buffer* out,
    rmr_document_report* report, rmr_error* error) {
  return document_pdf(document, resources, fonts, paper, maximum_pages, limits,
                      options, cancel, context, nullptr, out, report, error,
                      PDF_FRESH, 0, nullptr);
}

rmr_status rmr_apple_document_pdf_preserving_with_options(
    const rmr_archive_document* document, const rmr_page_resources* resources,
    const rmr_apple_fonts* fonts, rmr_paper paper, size_t maximum_pages,
    const rmr_limits* limits, const rmr_pdf_options* options,
    rmr_cancel_fn cancel, void* context, rmr_buffer* out,
    rmr_document_report* report, rmr_error* error) {
  return document_pdf(document, resources, fonts, paper, maximum_pages, limits,
                      options, cancel, context, nullptr, out, report, error,
                      PDF_INCREMENTAL, 0, nullptr);
}

rmr_status rmr_apple_document_pdf_recomposed_with_options(
    const rmr_archive_document* document, const rmr_page_resources* resources,
    const rmr_apple_fonts* fonts, rmr_paper paper, size_t maximum_pages,
    const rmr_limits* limits, const rmr_pdf_options* options,
    rmr_cancel_fn cancel, void* context, rmr_buffer* out,
    rmr_document_report* report, rmr_error* error) {
  return document_pdf(document, resources, fonts, paper, maximum_pages, limits,
                      options, cancel, context, nullptr, out, report, error,
                      PDF_RECOMPOSED, 0, nullptr);
}

rmr_status rmr_apple_document_pdf_to_output(
    const rmr_archive_document* document, const rmr_page_resources* resources,
    const rmr_apple_fonts* fonts, rmr_paper paper, size_t maximum_pages,
    const rmr_limits* limits, const rmr_pdf_options* options,
    rmr_cancel_fn cancel, void* context, const rmr_output* output,
    rmr_document_report* report, rmr_error* error) {
  if (!output)
    return rmr_apple_text_error(error, RMR_ERR_INVALID_ARGUMENT,
                                "invalid PDF output");
  return document_pdf(document, resources, fonts, paper, maximum_pages, limits,
                      options, cancel, context, output, nullptr, report, error,
                      PDF_FRESH, 0, nullptr);
}

rmr_status rmr_apple_document_pdf_preserving_to_output(
    const rmr_archive_document* document, const rmr_page_resources* resources,
    const rmr_apple_fonts* fonts, rmr_paper paper, size_t maximum_pages,
    const rmr_limits* limits, const rmr_pdf_options* options,
    rmr_cancel_fn cancel, void* context, const rmr_output* output,
    rmr_document_report* report, rmr_error* error) {
  if (!output)
    return rmr_apple_text_error(error, RMR_ERR_INVALID_ARGUMENT,
                                "invalid PDF output");
  return document_pdf(document, resources, fonts, paper, maximum_pages, limits,
                      options, cancel, context, output, nullptr, report, error,
                      PDF_INCREMENTAL, 0, nullptr);
}

rmr_status rmr_apple_document_pdf_recomposed_to_output(
    const rmr_archive_document* document, const rmr_page_resources* resources,
    const rmr_apple_fonts* fonts, rmr_paper paper, size_t maximum_pages,
    const rmr_limits* limits, const rmr_pdf_options* options,
    rmr_cancel_fn cancel, void* context, const rmr_output* output,
    rmr_document_report* report, rmr_error* error) {
  if (!output)
    return rmr_apple_text_error(error, RMR_ERR_INVALID_ARGUMENT,
                                "invalid PDF output");
  return document_pdf(document, resources, fonts, paper, maximum_pages, limits,
                      options, cancel, context, output, nullptr, report, error,
                      PDF_RECOMPOSED, 0, nullptr);
}

rmr_status rmr_apple_document_svg(
    const rmr_archive_document* document, const rmr_page_resources* resources,
    const rmr_apple_fonts* fonts, rmr_paper paper, size_t maximum_pages,
    double raster_dpi, const rmr_limits* limits, rmr_cancel_fn cancel,
    void* context, rmr_svg_pages* out, rmr_document_report* report,
    rmr_error* error) {
  return document_pdf(document, resources, fonts, paper, maximum_pages, limits,
                      nullptr, cancel, context, nullptr, nullptr, report, error,
                      SVG_PAGES, raster_dpi, out);
}
