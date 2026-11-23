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
#include "rmlines/writer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A local editing session owns its source bytes and its undo history. A byte
 * view is borrowed until the next successful edit or the session's release.
 * Undo and redo navigate unpublished revisions of this page.
 * The library applies its implemented CRDT rules to partially merged concurrent
 * edits from several devices. These page shapes are verified with synthetic
 * files only. */
typedef struct rm_edit_session rm_edit_session;

[[nodiscard]] RM_API rm_status rm_edit_open(const uint8_t* data, size_t size,
                                            rm_edit_session** out,
                                            rm_error* error);
RM_API void rm_edit_close(rm_edit_session* session);
RM_API const uint8_t* rm_edit_bytes(const rm_edit_session* session,
                                    size_t* size);
RM_API bool rm_edit_undo(rm_edit_session* session);
RM_API bool rm_edit_redo(rm_edit_session* session);

[[nodiscard]] RM_API rm_status rm_edit_erase(rm_edit_session* session,
                                             const rm_item_id* ids,
                                             size_t count, rm_error* error);

/* Update a tree node's LWW label or visibility. The caller supplies a fresh
 * stamp from its registered page author. A stale stamp refuses the edit.
 * Node IDs are the IDs in SceneTreeNode blocks (layers and anchored groups). */
[[nodiscard]] RM_API rm_status rm_edit_set_node_name(rm_edit_session* session,
                                                     rm_item_id node,
                                                     rm_item_id stamp,
                                                     const char* name,
                                                     rm_error* error);
[[nodiscard]] RM_API rm_status
rm_edit_set_node_visible(rm_edit_session* session, rm_item_id node,
                         rm_item_id stamp, bool visible, rm_error* error);

/* Insert a stroke between scene items in a node. {0,0} denotes the
 * beginning or end. The caller reserves a fresh page-wide counter under
 * its registered author for id, including across concurrent sessions. */
[[nodiscard]] RM_API rm_status rm_edit_insert_stroke(
    rm_edit_session* session, rm_item_id parent, rm_item_id id, rm_item_id left,
    rm_item_id right, const rm_draft_stroke* stroke, rm_error* error);

/* Replace a range of Unicode scalars in the root text. Paragraph breaks
 * occupy one scalar. Reserve one consecutive ID per inserted scalar,
 * starting at first_new, under the caller's registered author. When both
 * removing and inserting scalars, reserve one additional ID at the end for
 * the operation's anchor stamp. It must be newer than every anchor stamp
 * being replaced. Pure deletion consumes no IDs. A text anchor
 * follows the replacement scalar at the same index within the range. If
 * that scalar is absent, the edit returns RM_ERR_UNVERIFIED atomically. */
[[nodiscard]] RM_API rm_status rm_edit_replace_text(
    rm_edit_session* session, size_t start, size_t remove_count,
    const char* utf8, rm_item_id first_new, rm_error* error);

typedef struct rm_edit_layer {
  rm_item_id node, link;
  bool visible;
} rm_edit_layer;

/* The live layers in drawing order. Layers without a live ordering link
 * follow linked layers in placement-record order and have link {0,0}.
 * Release the array with rm_edit_layers_free.
 */
[[nodiscard]] RM_API rm_status rm_edit_layers(const rm_edit_session* session,
                                              rm_edit_layer** out,
                                              size_t* count, rm_error* error);
RM_API void rm_edit_layers_free(rm_edit_layer* layers);

/* Positions are zero-based in the resulting drawing order. Reserve four
 * fresh consecutive IDs from first for add (the first becomes the node),
 * one for move and two for delete. A page retains at least one layer.
 * Add returns RM_ERR_INVALID_ARGUMENT if five live layers already exist
 * (the device limit documented in docs/FORMAT.md, section 2.9). */
[[nodiscard]] RM_API rm_status rm_edit_add_layer(rm_edit_session* session,
                                                 size_t position,
                                                 const char* name,
                                                 rm_item_id first,
                                                 rm_error* error);
[[nodiscard]] RM_API rm_status rm_edit_move_layer(rm_edit_session* session,
                                                  rm_item_id node,
                                                  size_t position,
                                                  rm_item_id first,
                                                  rm_error* error);
[[nodiscard]] RM_API rm_status rm_edit_delete_layer(rm_edit_session* session,
                                                    rm_item_id node,
                                                    rm_item_id first,
                                                    rm_error* error);

typedef struct rm_edit_transform {
  /* x' = a*x + c*y + tx, y' = b*x + d*y + ty, in the item's node space. */
  double a, b, c, d, tx, ty;
} rm_edit_transform;

/* Transform a stroke or cut a half-open range of its recorded samples.
 * Each edit tombstones the original. Reserve one fresh ID for a transform
 * and up to two consecutive IDs for the surviving pieces of a cut.
 * Both keep the parent node and its text anchor. */
[[nodiscard]] RM_API rm_status rm_edit_transform_stroke(
    rm_edit_session* session, rm_item_id stroke, rm_item_id fresh,
    rm_edit_transform transform, rm_error* error);
/* Apply one affine transform to a selection in a single undo step. Caller
 * reserves count consecutive IDs. A missing item leaves the revision intact. */
[[nodiscard]] RM_API rm_status rm_edit_transform_strokes(
    rm_edit_session* session, const rm_item_id* strokes, size_t count,
    rm_item_id first_new, rm_edit_transform transform, rm_error* error);

[[nodiscard]] RM_API rm_status rm_edit_cut_stroke(
    rm_edit_session* session, rm_item_id stroke, rm_item_id fresh,
    size_t first_point, size_t point_count, rm_error* error);

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
