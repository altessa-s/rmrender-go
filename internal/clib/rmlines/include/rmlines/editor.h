/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Edits made to a page the tablet wrote, in place.
 *
 * Not the writer. `rm_write_page` composes a new page from a draft, and a page
 * composed anew has lost what makes it the tablet's: the identifiers every
 * item is known by, the chain the items hang in, the counters the tablet
 * merges by. An edit here leaves all of that as it was and rewrites only the
 * blocks it was asked about; every other byte of the page goes out as it came
 * in. */

#ifndef RMLINES_EDITOR_H
#define RMLINES_EDITOR_H

#include "rmlines/export.h"
#include "rmlines/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The page with these strokes and highlights erased, the way the tablet's own
 * eraser does it.
 *
 * An erased item stays in the page as a tombstone: the same block, the same
 * identifier, parent and neighbours, a deleted length of one, and no value.
 * That is the shape the tablet writes — every stroke its own eraser takes is
 * exactly that — and it has to be kept rather than the block
 * dropped: a later item may hang its `left` on this one, and the tablet merges
 * pages by these identifiers.
 *
 * The block's header keeps its versions and its unknown byte; only the length
 * changes. Every other block, and whatever trails the last one, is copied byte
 * for byte.
 *
 * Every identifier asked for must name a living stroke (0x05) or highlight
 * (0x03) of this page, or nothing is erased: RM_ERR_ITEMS_NOT_FOUND, with
 * `rm_error.value` the number missing and `rm_error.message` naming them. The
 * result is read back before it is handed over: it must parse, lose nothing it
 * did not lose before, and hold every item it held except the erased ones —
 * RM_ERR_UNVERIFIED otherwise.
 *
 * No identifiers at all is no edit: the bytes come back as they went in,
 * unread. A page that does not parse is refused with the status
 * `rm_parse_page` gives it. On success `*out_bytes` is yours until
 * `rm_bytes_free`; on failure nothing is allocated. `out_error` may be null. */
[[nodiscard]] RM_API rm_status rm_erase_items(
    const uint8_t* data, size_t size, const rm_item_id* ids, size_t id_count,
    uint8_t** out_bytes, size_t* out_size, rm_error* out_error);

/* Every living mark of the highlighter on a page: its strokes, on every layer
 * — a layer hidden on the tablet included, since showing it again would bring
 * them back — and its highlights over a text layer.
 *
 * What an erase of "everything the marker did" takes. `rm_page` cannot answer
 * it: the page is what is drawn, and a hidden layer's strokes are left out of
 * it. Strokes on a layer taken out of the tree are left out here too — no
 * switch brings them back.
 *
 * A page with a block that would not read is refused rather than answered in
 * part: RM_ERR_INCOMPLETE_PAGE, with `rm_error.value` the number of blocks
 * lost. The block lost may be a mark, and "every mark" less one is not every
 * mark — so a caller needs no separate parse to be sure it has them all.
 *
 * Each identifier once, strokes in file order and then highlights. On success
 * `*out_ids` is yours until `rm_item_ids_free` — null when `*out_count` is
 * nought. Otherwise the statuses are those of `rm_parse_page`. */
[[nodiscard]] RM_API rm_status rm_highlighter_items(const uint8_t* data,
                                                    size_t size,
                                                    rm_item_id** out_ids,
                                                    size_t* out_count,
                                                    rm_error* out_error);

/* Release what `rm_highlighter_items` handed over. Null is accepted and
 * ignored. */
RM_API void rm_item_ids_free(rm_item_id* ids);

#ifdef __cplusplus
}
#endif

#endif /* RMLINES_EDITOR_H */
