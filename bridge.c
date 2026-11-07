// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

// The definitions behind bridge.h. Kept apart from the Go files because a Go
// file that exports to C may only declare, not define, in its preamble.
//
// The page pipelines follow rmrender-lib's own document export
// (backends/*/rmr_document.c) for one page at a time, so that a page drawn
// here is the page the document PDF holds.

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "_cgo_export.h"
#include "bridge.h"

// The backend's page plan with typed text as glyph outlines, which its
// document export draws SVG pages with: exported by the vendored backend,
// though not part of its public headers.
#include "rmr_typography_internal.h"

#if defined(RMRGO_APPLE)
#define NATIVE(name) rmr_apple_##name
#else
#define NATIVE(name) rmr_linux_##name
#endif

typedef NATIVE(pdf_source) pdf_source;

// The margin rmrender-lib's document export grows a page over an imported
// PDF by: 5 mm, in .rm units.
static const double SOURCE_MARGIN = 5 * 72 / 25.4 / RMR_POINTS_PER_UNIT;

static rmr_status fail(rmr_error *error, rmr_status status, const char *message) {
    if (error) {
        *error = (rmr_error){.code = status};
        snprintf(error->message, sizeof error->message, "%s", message);
    }
    return status;
}

// Cancellation.

void *rmrgo_flag_new(void) {
    atomic_int *flag = malloc(sizeof *flag);
    if (flag)
        atomic_init(flag, 0);
    return flag;
}

void rmrgo_flag_raise(void *flag) { atomic_store((atomic_int *)flag, 1); }

void rmrgo_flag_free(void *flag) { free(flag); }

static bool cancelled(void *flag) { return flag && atomic_load((atomic_int *)flag); }

// Fonts.

static rmr_status font(void *context, const char *resource, rmr_asset_view *out) {
    return rmrgoFont((uintptr_t)context, (char *)resource, out);
}

rmr_status rmrgo_fonts_create(uintptr_t handle, const rmr_limits *limits, rmrgo_fonts **out,
                              rmr_error *error) {
    return NATIVE(fonts_create)(font, (void *)handle, limits, out, error);
}

void rmrgo_fonts_free(rmrgo_fonts *fonts) { NATIVE(fonts_free)(fonts); }

// Templates and images, resolved by the Go callbacks. A decoded template is
// kept until the next one is resolved or the render ends, which is as long
// as the library borrows it.

typedef struct {
    uintptr_t handle;
    rmr_template_document *document;
    rmr_limits limits;
} templates;

static rmr_status template_resolve(void *context, rm_str name, const rmr_template **out) {
    templates *t = context;
    rmr_asset_view view = {0};
    rmr_status status = rmrgoTemplate(t->handle, (char *)name.bytes, name.length, &view);
    if (status != RMR_OK)
        return status;
    if (!view.bytes) {
        *out = nullptr;
        return RMR_OK;
    }
    rmr_template_document_free(t->document);
    t->document = nullptr;
    status = rmr_template_document_decode((const char *)view.bytes, view.size, &t->limits,
                                          &t->document, nullptr);
    if (status == RMR_OK)
        *out = rmr_template_document_model(t->document);
    return status;
}

static rmr_status image_resolve(void *context, rm_str file, rmr_asset_view *out) {
    return rmrgoImage((uintptr_t)context, (char *)file.bytes, file.length, out);
}

// The resources of one render: templates through Go, images from the
// archive of a document or through Go for a lone page.
static rmr_page_resources resources(const rmrgo_request *r, templates *t,
                                    const rmr_archive_document *document) {
    *t = (templates){.handle = r->handle, .limits = r->limits};
    rmr_page_resources result = {.template = template_resolve, .template_context = t};
    if (document) {
        result.image = rmr_archive_document_image;
        result.image_context = (void *)(uintptr_t)document;
    } else {
        result.image = image_resolve;
        result.image_context = (void *)r->handle;
    }
    return result;
}

// Lone pages.

rmr_status rmrgo_page_parse(const uint8_t *bytes, size_t size, const char *template_name,
                            rm_page **out, rmr_error *error) {
    rm_page *page = nullptr;
    rm_error parse = {0};
    rm_status status = rm_parse_page(bytes, size, &page, &parse);
    if (status != RM_OK) {
        // Mapped as rmrender-lib's archive adapter maps a page it cannot
        // parse, with the parser's own words kept.
        rmr_status mapped = status == RM_ERR_OUT_OF_MEMORY         ? RMR_ERR_OUT_OF_MEMORY
                            : status == RM_ERR_UNSUPPORTED_VERSION ? RMR_ERR_UNSUPPORTED
                                                                   : RMR_ERR_BACKEND;
        if (error) {
            *error = (rmr_error){.code = mapped};
            snprintf(error->message, sizeof error->message, "page: %s%s%s",
                     rm_status_string(status), parse.message[0] ? ": " : "", parse.message);
        }
        return mapped;
    }
    if (template_name && rm_page_set_template(page, template_name) != RM_OK) {
        rm_page_free(page);
        return fail(error, RMR_ERR_OUT_OF_MEMORY, "template name allocation failed");
    }
    *out = page;
    return RMR_OK;
}

// One page laid out: its plan and the sheet geometry it is drawn on.
typedef struct {
    rmr_plan *plan;
    // The page in .rm units, and its paper floor in PDF points.
    rmr_point size, points;
    // Points per .rm unit.
    double unit;
    // Sized to its imported PDF rather than to paper.
    bool exact;
    // A source page drawn beneath, from the document's PDF.
    bool source;
    size_t source_page;
} layout;

static void layout_free(layout *l) { rmr_plan_free(l->plan); }

// The PDF sheet of a laid out page, as the document export makes it.
static rmr_status pdf_sheet(const layout *l, rmr_sheet *out, rmr_error *e) {
    rmr_rect viewport = rmr_plan_viewport(l->plan);
    if (!l->exact) {
        rmr_status s = rmr_sheet_create(viewport, l->points.x, l->points.y, l->unit, out, e);
        return s;
    }
    // Imported PDF pages keep fractional dimensions.
    double scale = RMR_POINTS_PER_UNIT;
    rmr_sheet sheet;
    sheet.width = fmax(l->points.x, viewport.width * scale);
    sheet.height = fmax(l->points.y, viewport.height * scale);
    sheet.transform =
        (rmr_transform){scale,
                        0,
                        0,
                        -scale,
                        (sheet.width - viewport.width * scale) / 2 - viewport.x * scale,
                        sheet.height + viewport.y * scale};
    *out = sheet;
    return RMR_OK;
}

// The bitmap sheet of a laid out page: its PDF sheet at scale pixels per
// .rm unit, rounded to whole pixels.
static rmr_status bitmap_sheet(const layout *l, double scale, rmr_sheet *out, rmr_error *e) {
    if (!isfinite(scale) || scale <= 0)
        return fail(e, RMR_ERR_INVALID_ARGUMENT, "scale must be finite and positive");
    return rmr_sheet_create(rmr_plan_viewport(l->plan), l->points.x / l->unit * scale,
                            l->points.y / l->unit * scale, scale, out, e);
}

// The part of .rm space a sheet shows, y down: the frame highlight boxes are
// fractions of.
static rmr_rect sheet_frame(rmr_sheet sheet) {
    double a = sheet.transform.a;
    return (rmr_rect){-sheet.transform.tx / a, (sheet.transform.ty - sheet.height) / a,
                      sheet.width / a, sheet.height / a};
}

// The canvas of a lone page: its own plausible size, else the tablet's.
static rmr_status lone_canvas(const rmrgo_request *r, const rm_page *page, rmr_point *out,
                              rmr_error *e) {
    const rm_page *pages[] = {page};
    return rmr_lines_document_canvas(pages, 1, (rmr_point){0}, false, &r->limits, out, e);
}

static rmr_status lone_layout(const rmrgo_request *r, const rm_page *page, layout *out,
                              rmr_error *e) {
    *out = (layout){0};
    rmr_point canvas;
    rmr_status s = lone_canvas(r, page, &canvas, e);
    if (s == RMR_OK)
        s = rmr_paper_points(r->paper, canvas.x, canvas.y, &out->points, e);
    if (s != RMR_OK)
        return s;
    out->size = canvas;
    out->unit = out->points.x / canvas.x;
    templates t;
    rmr_page_resources res = resources(r, &t, nullptr);
    s = NATIVE(page_plan)(page, canvas, r->edge, r->margin, &res, r->fonts, &r->limits,
                          &out->plan, e);
    rmr_template_document_free(t.document);
    return s;
}

rmr_status rmrgo_page_bitmap(const rmrgo_request *r, const rm_page *page, double scale,
                             rmr_color background, rmr_bitmap *out, rmr_error *e) {
    layout l;
    rmr_status s = lone_layout(r, page, &l, e);
    rmr_sheet sheet;
    if (s == RMR_OK)
        s = bitmap_sheet(&l, scale, &sheet, e);
    if (s == RMR_OK)
        s = NATIVE(bitmap_on_sheet)(l.plan, &sheet, background, &r->limits, cancelled, r->cancel,
                                    out, e);
    layout_free(&l);
    return s;
}

rmr_status rmrgo_page_pdf(const rmrgo_request *r, const rm_page *page, rmr_buffer *out,
                          rmr_error *e) {
    layout l;
    rmr_status s = lone_layout(r, page, &l, e);
    rmr_sheet sheet;
    if (s == RMR_OK)
        s = pdf_sheet(&l, &sheet, e);
    if (s == RMR_OK) {
        const rmr_plan *plans[] = {l.plan};
        s = NATIVE(pdf_sheets_with_options)(plans, &sheet, 1, &r->limits, &r->pdf, cancelled,
                                            r->cancel, out, e);
    }
    layout_free(&l);
    return s;
}

// The library's own page SVG lays the page out as its page plan does, with
// typed text as glyph outlines, on rmr_paper_sheet: the sheet pdf_sheet makes
// for a notebook page.
rmr_status rmrgo_page_svg(const rmrgo_request *r, const rm_page *page, rmr_buffer *out,
                          rmr_error *e) {
    rmr_point canvas;
    rmr_status s = lone_canvas(r, page, &canvas, e);
    if (s != RMR_OK)
        return s;
    templates t;
    rmr_page_resources res = resources(r, &t, nullptr);
    s = NATIVE(page_svg)(page, canvas, r->edge, r->margin, &res, r->fonts, r->paper,
                         r->raster_dpi, &r->limits, cancelled, r->cancel, out, e);
    rmr_template_document_free(t.document);
    return s;
}

static rmr_status marks(const rmrgo_request *r, const rm_page *page, rmr_point canvas,
                        const layout *l, rm_str id, rmr_page_marks **out, rmr_error *e) {
    rmr_sheet sheet;
    rmr_status s = pdf_sheet(l, &sheet, e);
    if (s == RMR_OK)
        s = NATIVE(page_marks)(page, canvas, sheet_frame(sheet), id, r->fonts, &r->limits, out,
                               e);
    return s;
}

rmr_status rmrgo_page_marks(const rmrgo_request *r, const rm_page *page, const char *id,
                            size_t id_length, rmr_page_marks **out, rmr_error *e) {
    layout l;
    rmr_status s = lone_layout(r, page, &l, e);
    if (s == RMR_OK)
        s = marks(r, page, l.size, &l, (rm_str){id, id_length}, out, e);
    layout_free(&l);
    return s;
}

// Documents.

struct rmrgo_document {
    rmd_archive archive;
    rmr_archive_document *document;
    pdf_source *source;
    char *id;
    // The document's canvas, found on first use.
    bool has_canvas;
    rmr_point canvas;
};

// An rmdoc-lib failure as a status of this library, with its words kept.
static rmr_status archive_failure(rmd_status status, const rmd_error *archive, rmr_error *e) {
    rmr_status mapped;
    switch (status) {
    case RMD_ERR_OUT_OF_MEMORY:
        mapped = RMR_ERR_OUT_OF_MEMORY;
        break;
    case RMD_ERR_FILE_TOO_LARGE:
    case RMD_ERR_ARCHIVE_TOO_LARGE:
    case RMD_ERR_TOO_MANY_ENTRIES:
    case RMD_ERR_ENTRY_TOO_LARGE:
    case RMD_ERR_TOTAL_SIZE_EXCEEDED:
    case RMD_ERR_COMPRESSION_RATIO_EXCEEDED:
    case RMD_ERR_COMPRESSED_ENTRY_TOO_LARGE:
    case RMD_ERR_COMPRESSED_TOTAL_SIZE_EXCEEDED:
    case RMD_ERR_DIRECTORY_TOO_LARGE:
    case RMD_ERR_NAME_TOO_LONG:
        mapped = RMR_ERR_LIMIT;
        break;
    case RMD_ERR_UNSUPPORTED_COMPRESSION:
    case RMD_ERR_ZIP64_UNSUPPORTED:
    case RMD_ERR_ENCRYPTED_ENTRY:
    case RMD_ERR_SPANNED_ARCHIVE:
        mapped = RMR_ERR_UNSUPPORTED;
        break;
    default:
        mapped = RMR_ERR_INVALID_ARGUMENT;
    }
    if (e) {
        *e = (rmr_error){.code = mapped};
        const char *words = rmd_status_string(status);
        if (archive->message[0] && strcmp(archive->message, words))
            snprintf(e->message, sizeof e->message, "archive: %s: %s", words, archive->message);
        else
            snprintf(e->message, sizeof e->message, "archive: %s", words);
    }
    return mapped;
}

static bool ends_with(rmd_str s, const char *tail) {
    size_t n = strlen(tail);
    return s.length >= n && !memcmp(s.bytes + s.length - n, tail, n);
}

void rmrgo_document_free(rmrgo_document *d) {
    if (d) {
        if (d->source)
            NATIVE(pdf_source_free)(d->source);
        rmr_archive_document_free(d->document);
        rmd_archive_free(&d->archive);
        free(d->id);
        free(d);
    }
}

rmr_status rmrgo_document_open(const uint8_t *bytes, size_t size, const char *id, size_t id_length,
                               bool include_unlisted, const rmr_limits *limits,
                               rmrgo_document **out, rmr_error *e) {
    rmrgo_document *d = calloc(1, sizeof *d);
    if (!d)
        return fail(e, RMR_ERR_OUT_OF_MEMORY, "document allocation failed");

    // The bytes are borrowed only for this call: the archive is unpacked
    // into memory the document owns.
    rmd_reader *reader = nullptr;
    rmd_error archive = {0};
    rmd_limits format = rmd_limits_format();
    rmd_status as = rmd_reader_open((rmd_bytes){bytes, size}, &format, &reader, &archive);
    if (as == RMD_OK)
        as = rmd_reader_unpack(reader, nullptr, nullptr, &d->archive, &archive);
    rmd_reader_close(reader);
    if (as != RMD_OK) {
        rmrgo_document_free(d);
        return archive_failure(as, &archive, e);
    }

    // Without an ID, the first document with a .content file, or else the
    // first document at all.
    rmd_str chosen = {id, id_length};
    if (!id) {
        for (size_t i = 0; i < d->archive.count && !chosen.bytes; i++)
            if (ends_with(d->archive.files[i].name, ".content"))
                chosen = rmd_document_id(d->archive.files[i].name);
        if (!chosen.bytes && d->archive.count)
            chosen = rmd_document_id(d->archive.files[0].name);
    }
    if (!chosen.bytes || !chosen.length) {
        rmrgo_document_free(d);
        return fail(e, RMR_ERR_INVALID_ARGUMENT, "archive holds no document");
    }
    d->id = malloc(chosen.length + 1);
    if (!d->id) {
        rmrgo_document_free(d);
        return fail(e, RMR_ERR_OUT_OF_MEMORY, "document allocation failed");
    }
    memcpy(d->id, chosen.bytes, chosen.length);
    d->id[chosen.length] = 0;

    rmr_status s = rmr_archive_document_create(&d->archive, d->id, include_unlisted, limits,
                                               &d->document, e);
    rmr_asset_view pdf = rmr_archive_document_pdf(d->document);
    if (s == RMR_OK && pdf.bytes)
        s = NATIVE(pdf_source_create)(pdf.bytes, pdf.size, limits, &d->source, e);
    if (s != RMR_OK) {
        rmrgo_document_free(d);
        return s;
    }
    *out = d;
    return RMR_OK;
}

const char *rmrgo_document_id(const rmrgo_document *d) { return d->id; }

size_t rmrgo_document_listed(const rmrgo_document *d) {
    return rmr_archive_document_count(d->document);
}

size_t rmrgo_document_source_pages(const rmrgo_document *d) {
    return d->source ? NATIVE(pdf_source_count)(d->source) : 0;
}

// Pages as the document export counts them: the listed pages, or else the
// pages of the imported PDF, or else one blank sheet.
size_t rmrgo_document_count(const rmrgo_document *d) {
    size_t listed = rmrgo_document_listed(d);
    size_t total = listed ? listed : rmrgo_document_source_pages(d);
    return total ? total : 1;
}

const rmr_document_page *rmrgo_document_page(const rmrgo_document *d, size_t index) {
    return rmr_archive_document_page(d->document, index);
}

// rmr_unreadable_fn: an unreadable page told to the Go callback of the
// handle in context.
static void unreadable_page(void *context, size_t page, rmr_status status) {
    rmrgoUnreadable((uintptr_t)context, page, status);
}

// The failure of an unreadable page under RMR_UNREADABLE_STRICT, worded and
// indexed as the document export words and indexes it.
static rmr_status unreadable_failure(rmr_error *e, rmr_status s, size_t index,
                                     const rmr_document_page *page) {
    if (e) {
        *e = (rmr_error){.code = s, .index = index};
        snprintf(e->message, sizeof e->message,
                 "unreadable page %zu (%s): RMLines could not parse it", index,
                 page && page->id ? page->id : "");
    }
    return s;
}

// *plan with the placeholder's label drawn over the middle of a page of
// size, as the document export draws it: *plan becomes the two together.
static rmr_status with_label(const rmrgo_request *r, rmr_point size, bool outlines,
                             rmr_plan **plan, rmr_error *e) {
    rmr_rect viewport = rmr_plan_viewport(*plan);
    rmr_plan *label = nullptr, *both = nullptr;
    rmr_status s = NATIVE(unreadable_plan)((rmr_rect){-size.x / 2, 0, size.x, size.y}, viewport,
                                           r->fonts, outlines, &r->limits, &label, e);
    if (s == RMR_OK) {
        const rmr_plan *parts[] = {*plan, label};
        s = rmr_plan_concat(parts, 2, viewport, &r->limits, &both, e);
    }
    rmr_plan_free(label);
    if (s == RMR_OK) {
        rmr_plan_free(*plan);
        *plan = both;
    }
    return s;
}

// What the request asks of an unreadable page, and the Go callback of its
// handle told of each one: the pdf options the document calls are given.
static rmr_pdf_options told(const rmrgo_request *r) {
    rmr_pdf_options options = r->pdf;
    options.on_unreadable = unreadable_page;
    options.unreadable_context = (void *)r->handle;
    return options;
}

// The document's canvas: the first plausible page size in page order, else
// the declared one. The first page that declares a plausible size decides,
// so pages are parsed only up to it; an unreadable page declares none, as
// in the document export.
static rmr_status document_canvas(rmrgo_document *d, const rmr_limits *limits, rmr_error *e) {
    if (d->has_canvas)
        return RMR_OK;
    rm_page *found = nullptr;
    rmr_status s = RMR_OK;
    size_t listed = rmrgo_document_listed(d);
    for (size_t i = 0; i < listed && !found && s == RMR_OK; i++) {
        rm_page *page = nullptr;
        rmr_status unreadable = RMR_OK;
        s = rmr_archive_document_read_page(d->document, i, &page, &unreadable, e);
        if (s != RMR_OK)
            break;
        double w = page->paper_size.width, h = page->paper_size.height;
        if (page->has_paper_size && isfinite(w) && isfinite(h) && w > 0 && h > 0 && w <= 100000 &&
            h <= 100000)
            found = page;
        else
            rm_page_free(page);
    }
    if (s == RMR_OK) {
        const rm_page *pages[] = {found};
        s = rmr_lines_document_canvas(pages, 1, rmr_archive_document_canvas(d->document),
                                      rmr_archive_document_landscape(d->document), limits,
                                      &d->canvas, e);
    }
    rm_page_free(found);
    if (s == RMR_OK)
        d->has_canvas = true;
    return s;
}

// One page of a document laid out as the document export lays it out. page
// receives the parsed page, null for a page with no notebook content.
// outlines plans typed text as glyph outlines, as the SVG export does. An
// unreadable page is told to Go and then handled as the request's policy
// says, as the document export handles it; drawn adds the placeholder's
// label to the plan, which reading the page's highlights does without.
static rmr_status document_layout(const rmrgo_request *r, rmrgo_document *d, size_t index,
                                  bool outlines, bool drawn, rm_page **parsed, rm_page *shown,
                                  layout *out, rmr_error *e) {
    *out = (layout){0};
    *parsed = nullptr;
    if (index >= rmrgo_document_count(d))
        return fail(e, RMR_ERR_INVALID_ARGUMENT, "page index out of range");
    rmr_status s = document_canvas(d, &r->limits, e);
    if (s != RMR_OK)
        return s;

    size_t listed = rmrgo_document_listed(d);
    rmr_status unreadable = RMR_OK;
    if (listed) {
        s = rmr_archive_document_read_page(d->document, index, parsed, &unreadable, e);
        if (s != RMR_OK)
            return s;
    }
    if (unreadable != RMR_OK) {
        unreadable_page((void *)r->handle, index, unreadable);
        if (r->pdf.unreadable == RMR_UNREADABLE_STRICT)
            return unreadable_failure(e, unreadable, index,
                                      rmr_archive_document_page(d->document, index));
    }
    bool label = unreadable != RMR_OK && r->pdf.unreadable == RMR_UNREADABLE_PLACEHOLDER;
    *shown = *parsed ? **parsed
                     : (rm_page){.background_visible = true, .root_document_visible = true};
    if (label)
        shown->has_template_name = false;

    rmr_document_page record = listed
                                   ? *rmr_archive_document_page(d->document, index)
                                   : (rmr_document_page){.has_source = true, .source_page = index};
    if (d->source)
        record = rmr_document_page_checked(record, NATIVE(pdf_source_count)(d->source));

    rmr_point size = d->canvas;
    rmr_point points = {size.x * RMR_POINTS_PER_UNIT, size.y * RMR_POINTS_PER_UNIT};
    if (d->source && record.has_source) {
        s = NATIVE(pdf_source_size)(d->source, record.source_page, &points, e);
        if (s != RMR_OK)
            return s;
        size = (rmr_point){points.x / RMR_POINTS_PER_UNIT, points.y / RMR_POINTS_PER_UNIT};
    } else if (d->source && shown->has_paper_size) {
        const rm_page *one[] = {shown};
        s = rmr_lines_document_canvas(one, 1, (rmr_point){0}, false, &r->limits, &size, e);
        if (s != RMR_OK)
            return s;
        points = (rmr_point){size.x * RMR_POINTS_PER_UNIT, size.y * RMR_POINTS_PER_UNIT};
    }

    // Marks over an imported PDF paint no notebook template.
    if (d->source)
        shown->has_template_name = false;

    out->size = size;
    if (d->source) {
        out->points = points;
        out->unit = RMR_POINTS_PER_UNIT;
        out->exact = true;
        out->source = record.has_source && shown->root_document_visible;
        out->source_page = record.source_page;
    } else {
        s = rmr_paper_points(r->paper, size.x, size.y, &out->points, e);
        if (s != RMR_OK)
            return s;
        out->unit = out->points.x / size.x;
    }

    templates t;
    rmr_page_resources res = resources(r, &t, d->document);
    s = (outlines ? NATIVE(page_outline_plan) : NATIVE(page_plan))(
        shown, size, d->source ? RMR_EDGE_CENTRELINE : RMR_EDGE_RIBBON,
        d->source ? SOURCE_MARGIN : 0, &res, r->fonts, &r->limits, &out->plan, e);
    rmr_template_document_free(t.document);
    if (s == RMR_OK && label && drawn)
        s = with_label(r, size, outlines, &out->plan, e);
    return s;
}

// A document page's plan with its imported PDF page drawn beneath the marks.
static rmr_status with_source(const rmrgo_request *r, const rmrgo_document *d, const layout *l,
                              rmr_plan **out, rmr_error *e) {
    rmr_asset_view pdf = rmr_archive_document_pdf(d->document);
    rmr_rect page = {-l->size.x / 2, 0, l->size.x, l->size.y};
    rmr_element clip[] = {
        {.kind = RMR_MOVE, .end = {page.x, page.y}},
        {.kind = RMR_LINE, .end = {page.x + page.width, page.y}},
        {.kind = RMR_LINE, .end = {page.x + page.width, page.y + page.height}},
        {.kind = RMR_LINE, .end = {page.x, page.y + page.height}},
        {.kind = RMR_CLOSE},
    };
    rmr_draw draw = {
        .mode = RMR_PDF,
        .path = {clip, sizeof clip / sizeof *clip},
        .color = {0, 0, 0, 1},
        .image = {pdf.bytes, pdf.size, {page.width, 0, 0, page.height, page.x, page.y}},
        .pdf_page = l->source_page,
    };
    rmr_rect viewport = rmr_plan_viewport(l->plan);
    rmr_plan *background = nullptr;
    rmr_status s = rmr_plan_create(&draw, 1, viewport, &r->limits, &background, e);
    if (s == RMR_OK) {
        const rmr_plan *plans[] = {background, l->plan};
        s = rmr_plan_concat(plans, 2, viewport, &r->limits, out, e);
    }
    rmr_plan_free(background);
    return s;
}

rmr_status rmrgo_document_bitmap(const rmrgo_request *r, rmrgo_document *d, size_t index,
                                 double scale, rmr_color background, rmr_bitmap *out,
                                 rmr_error *e) {
    rm_page *parsed = nullptr, shown;
    layout l;
    rmr_status s = document_layout(r, d, index, false, true, &parsed, &shown, &l, e);
    rmr_sheet sheet;
    if (s == RMR_OK)
        s = bitmap_sheet(&l, scale, &sheet, e);
    rmr_plan *full = nullptr;
    if (s == RMR_OK && l.source)
        s = with_source(r, d, &l, &full, e);
    if (s == RMR_OK)
        s = NATIVE(bitmap_on_sheet)(full ? full : l.plan, &sheet, background, &r->limits,
                                    cancelled, r->cancel, out, e);
    rmr_plan_free(full);
    layout_free(&l);
    rm_page_free(parsed);
    return s;
}

// A page of the document as the SVG export draws it: the outline plan on
// the PDF sheet, over its imported page, which the SVG writer rasterizes.
rmr_status rmrgo_document_page_svg(const rmrgo_request *r, rmrgo_document *d, size_t index,
                                   rmr_buffer *out, rmr_error *e) {
    rm_page *parsed = nullptr, shown;
    layout l;
    rmr_status s = document_layout(r, d, index, true, true, &parsed, &shown, &l, e);
    rmr_sheet sheet;
    if (s == RMR_OK)
        s = pdf_sheet(&l, &sheet, e);
    rmr_plan *full = nullptr;
    if (s == RMR_OK && l.source)
        s = with_source(r, d, &l, &full, e);
    rmr_svg_pages pages = {0};
    if (s == RMR_OK) {
        const rmr_plan *plans[] = {full ? full : l.plan};
        s = NATIVE(svg)(plans, &sheet, 1, r->raster_dpi, &r->limits, cancelled, r->cancel, &pages,
                        e);
    }
    if (s == RMR_OK) {
        *out = pages.pages[0];
        pages.pages[0] = (rmr_buffer){0};
    }
    rmr_svg_pages_free(&pages);
    rmr_plan_free(full);
    layout_free(&l);
    rm_page_free(parsed);
    return s;
}

rmr_status rmrgo_document_marks(const rmrgo_request *r, rmrgo_document *d, size_t index,
                                rmr_page_marks **out, rmr_error *e) {
    rm_page *parsed = nullptr, shown;
    layout l;
    rmr_status s = document_layout(r, d, index, false, false, &parsed, &shown, &l, e);
    if (s == RMR_OK) {
        const rmr_document_page *record = rmrgo_document_page(d, index);
        rm_str id = record && record->id ? (rm_str){record->id, strlen(record->id)} : (rm_str){0};
        s = marks(r, &shown, l.size, &l, id, out, e);
    }
    layout_free(&l);
    rm_page_free(parsed);
    return s;
}

rmr_status rmrgo_document_pdf(const rmrgo_request *r, rmrgo_document *d, uint32_t mode,
                              size_t maximum_pages, rmr_buffer *out, rmr_document_report *report,
                              rmr_error *e) {
    rmr_pdf_options pdf = told(r);
    templates t;
    rmr_page_resources res = resources(r, &t, d->document);
    rmr_status s;
    switch (mode) {
    case RMRGO_PDF_FRESH:
        s = NATIVE(document_pdf_with_options)(d->document, &res, r->fonts, r->paper, maximum_pages,
                                              &r->limits, &pdf, cancelled, r->cancel, out,
                                              report, e);
        break;
    case RMRGO_PDF_PRESERVING:
        s = NATIVE(document_pdf_preserving_with_options)(d->document, &res, r->fonts, r->paper,
                                                         maximum_pages, &r->limits, &pdf,
                                                         cancelled, r->cancel, out, report, e);
        break;
    case RMRGO_PDF_RECOMPOSED:
        s = NATIVE(document_pdf_recomposed_with_options)(d->document, &res, r->fonts, r->paper,
                                                         maximum_pages, &r->limits, &pdf,
                                                         cancelled, r->cancel, out, report, e);
        break;
    default:
        s = fail(e, RMR_ERR_INVALID_ARGUMENT, "unknown PDF mode");
    }
    rmr_template_document_free(t.document);
    return s;
}

// rmr_output write: the bytes go to the Go writer of the handle in context.
static rmr_status write_output(void *context, const uint8_t *bytes, size_t size) {
    return rmrgoWrite((uintptr_t)context, (uint8_t *)bytes, size);
}

rmr_status rmrgo_document_pdf_write(const rmrgo_request *r, rmrgo_document *d, uint32_t mode,
                                    size_t maximum_pages, rmr_document_report *report,
                                    rmr_error *e) {
    rmr_pdf_options pdf = told(r);
    templates t;
    rmr_page_resources res = resources(r, &t, d->document);
    rmr_output output = {RMR_OUTPUT_ABI, sizeof output, write_output, (void *)r->handle};
    rmr_status s;
    switch (mode) {
    case RMRGO_PDF_FRESH:
        s = NATIVE(document_pdf_to_output)(d->document, &res, r->fonts, r->paper, maximum_pages,
                                           &r->limits, &pdf, cancelled, r->cancel, &output,
                                           report, e);
        break;
    case RMRGO_PDF_PRESERVING:
        s = NATIVE(document_pdf_preserving_to_output)(d->document, &res, r->fonts, r->paper,
                                                      maximum_pages, &r->limits, &pdf,
                                                      cancelled, r->cancel, &output, report, e);
        break;
    case RMRGO_PDF_RECOMPOSED:
        s = NATIVE(document_pdf_recomposed_to_output)(d->document, &res, r->fonts, r->paper,
                                                      maximum_pages, &r->limits, &pdf,
                                                      cancelled, r->cancel, &output, report, e);
        break;
    default:
        s = fail(e, RMR_ERR_INVALID_ARGUMENT, "unknown PDF mode");
    }
    rmr_template_document_free(t.document);
    return s;
}

rmr_status rmrgo_document_svg(const rmrgo_request *r, rmrgo_document *d, size_t maximum_pages,
                              rmr_svg_pages *out, rmr_document_report *report, rmr_error *e) {
    templates t;
    rmr_page_resources res = resources(r, &t, d->document);
    rmr_pdf_options pdf = told(r);
    rmr_status s = NATIVE(document_svg_with_options)(d->document, &res, r->fonts, r->paper,
                                                     maximum_pages, r->raster_dpi, &r->limits,
                                                     &pdf, cancelled, r->cancel, out, report, e);
    rmr_template_document_free(t.document);
    return s;
}

rmr_status rmrgo_document_check(rmrgo_document *d, size_t index, rmr_status *unreadable,
                                rmr_error *e) {
    rm_page *page = nullptr;
    rmr_status s = rmr_archive_document_read_page(d->document, index, &page, unreadable, e);
    rm_page_free(page);
    if (s == RMR_OK && *unreadable != RMR_OK)
        unreadable_failure(e, *unreadable, index, rmr_archive_document_page(d->document, index));
    return s;
}
