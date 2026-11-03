/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Parsing a notebook page (the `.rm` format, version 6).
 *
 * What goes unread is counted in `rm_page.skipped_blocks`: a block of a type
 * this parser does not know, and a block of a known type that would not read.
 * The service blocks every page opens with hold nothing to draw and are
 * stepped over uncounted — counting them would put three on every page ever
 * written and leave a real loss indistinguishable from the floor. */

#ifndef RMLINES_PARSER_H
#define RMLINES_PARSER_H

#include "rmlines/export.h"
#include "rmlines/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What every page begins with, whatever its generation: 32 characters, the
 * version number right behind them. */
#define RM_HEADER_PREFIX "reMarkable .lines file, version="
/* The file header, exactly 43 bytes including the padding spaces. */
#define RM_HEADER RM_HEADER_PREFIX "6          "
/* The one version this parser reads. Versions 3 and 5 carry the same 43-byte
 * header with another digit in it and nothing else in common: a flat
 * container of counters rather than the tagged CRDT blocks below. */
constexpr int RM_SUPPORTED_VERSION = 6;
constexpr size_t RM_HEADER_LENGTH = sizeof(RM_HEADER) - 1;

static_assert(RM_HEADER_LENGTH == 43,
              "the header is 43 bytes, padding included");

/* The version a header declares, or false when the bytes are not one.
 *
 * Read out of the header rather than compared against a list of known
 * spellings: the point is to recognise a generation nobody has written down
 * yet — a future 7 as much as the 3 and the 5 behind us — and say its number
 * instead of calling the file unrecognisable. */
RM_API bool rm_header_version(const char* header, size_t length,
                              int* out_version);

/* Read one page.
 *
 * On success the page is yours until `rm_page_free`; on failure nothing is
 * allocated and `*out_page` is left alone. `out_error` may be null.
 *
 * The statuses that reach this caller are: RM_ERR_INVALID_ARGUMENT when
 * `data` or `out_page` is null, RM_ERR_UNSUPPORTED_VERSION for a generation
 * this reader does not draw, RM_ERR_UNSUPPORTED_FORMAT for bytes that do not
 * begin as a page at all, RM_ERR_UNEXPECTED_EOF for a stream that ended
 * inside a field, and RM_ERR_OUT_OF_MEMORY when an allocation failed —
 * answered as a whole page lost rather than as a quietly shorter one, so a
 * caller that treats it as "no content here" is wrong about what it has.
 *
 * RM_ERR_MALFORMED_PAGE is what the individual block parsers raise and it
 * stops there: the block is stepped over, the loss is added to
 * `rm_page.skipped_blocks`, and a line goes to the diagnostics sink. A page is
 * not lost for one stroke it could not read. */
[[nodiscard]] RM_API rm_status rm_parse_page(const uint8_t* data, size_t size,
                                             rm_page** out_page,
                                             rm_error* out_error);

/* Release a page and everything it points at. Null is accepted and ignored. */
RM_API void rm_page_free(rm_page* page);

/* Name the page's template.
 *
 * The name is not in the `.rm` — it comes from the document's `.content` —
 * so the parser never sets it and the layer that holds documents does. The
 * string is copied into the page's own arena; pass null to clear it. */
[[nodiscard]] RM_API rm_status rm_page_set_template(rm_page* page,
                                                    const char* name);

#ifdef __cplusplus
}
#endif

#endif /* RMLINES_PARSER_H */
