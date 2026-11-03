/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMMARKS_TEXT_H
#define RMMARKS_TEXT_H
#include "rmmarks/types.h"
#ifdef __cplusplus
extern "C" {
#endif

/* The thresholds of the matching rules, kept here so a consumer can name them.
 */
constexpr double RMM_PAST_THE_MIDDLE = 0.05;
constexpr double RMM_WHOLE_LINE = 0.85;
constexpr double RMM_LEAST_WIDTH = 0.5;
constexpr double RMM_LEAST_TOUCH = 0.2;
constexpr double RMM_WITHIN_A_LINE = 0.75;

/* Allocating operations are transactional: *out is unchanged on failure.
 * out_error is optional. A null array is valid only with count zero.
 * Counts must fit SIZE_MAX / sizeof(element); boxes must be finite with
 * finite extents. Strings must be valid UTF-8. Inputs are never mutated.
 * Results own copies of all text and arrays; one matching free releases
 * everything, and free(nullptr) is accepted. No result borrows input data.
 * Independent calls share no mutable state and may run concurrently. */

/* One passage per band, sorted by (y0, x0, band). Selection is judged by
 * the entire line's middle; words are joined in input order with spaces.
 * Identical selected page-piece identities are deduplicated, with later
 * bands recorded in again. Cut-only results are not deduplicated.
 * Out-of-range alone keys select no band and are ignored. */
[[nodiscard]] RMM_API rmm_status rmm_text_passages(
    const rmm_page_box* bands, size_t band_count, const rmm_line* lines,
    size_t line_count, const rmm_band_reading* alone, size_t alone_count,
    rmm_passages** out, rmm_error* out_error);
/* Convenience form of rmm_text_passages for lines that are already one
 * piece each, with no alternate (alone) reading. */
[[nodiscard]] RMM_API rmm_status rmm_text_passages_from_reading(
    const rmm_page_box* bands, size_t band_count,
    const rmm_recognized_line* lines, size_t line_count, rmm_passages** out,
    rmm_error* out_error);
/* Releases a result from rmm_text_passages or rmm_text_passages_from_reading.
 * Accepts nullptr. */
RMM_API void rmm_passages_free(rmm_passages* result);

/* Gather adjoining passages, repeatedly merging every reached block.
 * Ties in y0 preserve input order: the sort is stable. */
[[nodiscard]] RMM_API rmm_status
rmm_text_blocks(const rmm_marked_passage* passages, size_t count,
                rmm_blocks** out, rmm_error* out_error);
/* Releases a result from rmm_text_blocks. Accepts nullptr. */
RMM_API void rmm_blocks_free(rmm_blocks* result);

/* Zip strips with numbered (the shorter count wins). The first strip
 * containing a whole line's vertical middle owns that line. Groups are in
 * order of first encounter, lines remain in input order. Empty strips do
 * not produce groups. Coordinates may extend past the strip's bounds. */
[[nodiscard]] RMM_API rmm_status rmm_text_brought(
    const rmm_line* lines, size_t line_count, const rmm_mark_strip* strips,
    size_t strip_count, const size_t* numbered, size_t number_count,
    rmm_readings** out, rmm_error* out_error);
/* Releases a result from rmm_text_brought. Accepts nullptr. */
RMM_API void rmm_readings_free(rmm_readings* result);

#ifdef __cplusplus
}
#endif
#endif
