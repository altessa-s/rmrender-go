/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file.
 *
 * Writing what the tablet accepts. The writer builds **fresh pages only**: it
 * never edits an existing file, because inserting into somebody else's CRDT
 * chains is a different contract with a user's notebook at stake.
 *
 * The per-block header versions are written down nowhere; the ones here are
 * the ones a page typed on the tablet carries. The tablet opens pages with
 * other versions too, so they are not a gate — but a generated page is worth
 * what it has in common with the tablet's own file, and the sheet it declares
 * is not cosmetic at all. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rm_internal.h"
#include "rmlines/parser.h"
#include "rmlines/writer.h"

/* ------------------------------------------------------------------ */
/* Every identifier a generated page hands out                         */
/* ------------------------------------------------------------------ */

/* In one place because the file cannot tell two meanings of one counter
 * apart: reuse a number and the page claims two edits are the same edit. */

constexpr uint8_t DRAFT_AUTHOR = 1;

/* The second author index a tablet page keeps for its formatting edits: its
 * marks all carry it, and its entry stands first in the authors table. */
constexpr uint8_t DRAFT_MARKS_AUTHOR = 2;

/* "Not set" — the start of the text, and the stamp of a field nobody has
 * touched. */
constexpr rm_wid ID_NONE = {.counter = 0, .author = 0};
constexpr rm_wid ID_ROOT = {.counter = 1, .author = 0};
constexpr rm_wid ID_TEXT = {.counter = 1, .author = DRAFT_AUTHOR};
constexpr rm_wid ID_LAYER = {.counter = 11, .author = DRAFT_AUTHOR};
constexpr rm_wid ID_GROUP_ITEM = {.counter = 12, .author = DRAFT_AUTHOR};
constexpr rm_wid ID_CANVAS_STAMP = {.counter = 13, .author = DRAFT_AUTHOR};
constexpr rm_wid ID_COLUMN_STAMP = {.counter = 14, .author = DRAFT_AUTHOR};
constexpr rm_wid ID_WIDTH_STAMP = {.counter = 15, .author = DRAFT_AUTHOR};

/* Where the characters of the text start counting. */
constexpr uint64_t FIRST_CHARACTER = 20;

/* Where the anchored groups and their strokes start counting. Far above the
 * text: the characters take one counter each, and a page can run to thousands
 * of them. */
constexpr uint64_t FIRST_MARK = 100000;

/* And where the strokes that hang on nothing start — above the marks, which
 * take eight counters each. */
constexpr uint64_t FIRST_LOOSE_STROKE = 200000;

/* Where the placed images start — above everything else. Each is given
 * sixteen counters' worth of room and spends eight of them. */
constexpr uint64_t FIRST_IMAGE = 300000;
constexpr uint64_t MARK_STRIDE = 8;
constexpr uint64_t IMAGE_STRIDE = 16;

/* How many of each kind fit before one range runs into the next. Derived from
 * the starts above rather than written out beside them, so that moving a start
 * moves its limit with it. Past these the page would name two edits with one
 * counter. */
constexpr size_t MARK_LIMIT =
    (size_t)((FIRST_LOOSE_STROKE - FIRST_MARK) / MARK_STRIDE);

constexpr size_t LOOSE_STROKE_LIMIT =
    (size_t)(FIRST_IMAGE - FIRST_LOOSE_STROKE);

/* The room the characters have before the marks begin — every scalar takes
 * one, and an empty paragraph still takes one for its newline. */
constexpr size_t CHARACTER_LIMIT = (size_t)(FIRST_MARK - FIRST_CHARACTER);

/* Each range ends where the next one begins: a limit that does not match
 * the next start is a collision waiting for the draft that reaches it, or room
 * given away for nothing. */
static_assert(FIRST_MARK + MARK_LIMIT * MARK_STRIDE == FIRST_LOOSE_STROKE,
              "the marks must end where the loose strokes begin");

static_assert(FIRST_LOOSE_STROKE + LOOSE_STROKE_LIMIT == FIRST_IMAGE,
              "the loose strokes must end where the images begin");

static_assert(FIRST_CHARACTER + CHARACTER_LIMIT == FIRST_MARK,
              "the characters must end where the marks begin");

/* The threshold a line anchor carries, as the tablet writes it: 35.75 is how
 * it reads, 35.748031 is what it writes. */
constexpr float LINE_ANCHOR_THRESHOLD = 35.748031f;

/* An identifier out of its two parts, for the counters this file hands out. */
static rm_wid wid(uint8_t author, uint64_t counter) {
  return (rm_wid){.counter = counter, .author = author};
}

/* A stroke of no anchor, by its number on the page. */
static rm_wid loose_stroke_id(size_t number) {
  return wid(DRAFT_AUTHOR, FIRST_LOOSE_STROKE + (uint64_t)number);
}

/* The identifiers of one mark: the group, its place in the drawing order, the
 * stroke, and a stamp for each of the four anchor fields.
 *
 * In one place because two places could count them differently: the chain is
 * built where the marks are looped over and the blocks are written a function
 * away, and a stride that agreed in one and not the other would break the
 * chain into ties — which the format resolves by identifier, so the marks
 * would come back in an order nobody wrote. */
typedef struct mark_ids {
  rm_wid node, item, stroke;
  /* The stamps of the anchor and of the three fields beside it. Each is an
   * identifier of its own, as they are on a page the tablet wrote. */
  rm_wid anchor, type, threshold, origin;
} mark_ids;

/* The identifiers of the mark numbered `number`, its range's base plus the
 * fixed offsets `mark_ids` lists. */
static mark_ids mark_ids_of(size_t number) {
  uint64_t base = FIRST_MARK + (uint64_t)number * MARK_STRIDE;
  return (mark_ids){
      .node = wid(DRAFT_AUTHOR, base),
      .item = wid(DRAFT_AUTHOR, base + 1),
      .stroke = wid(DRAFT_AUTHOR, base + 2),
      .anchor = wid(DRAFT_AUTHOR, base + 3),
      .type = wid(DRAFT_AUTHOR, base + 4),
      .threshold = wid(DRAFT_AUTHOR, base + 5),
      .origin = wid(DRAFT_AUTHOR, base + 6),
  };
}

/* The identifiers of one placed image: the item, and a stamp for each of the
 * two fields written with one — the key on the item, the name in the
 * attachment map. The group an anchored image hangs on and the stamps of three
 * of its anchor fields are spent only when the image is anchored; a loose one
 * takes the first three counters and no more. The fourth field,
 * `anchor_origin_x`, is stamped with the node's own id: that is what a tablet
 * page carries, not a fresh counter. */
typedef struct image_ids {
  rm_wid item, key_stamp, name_stamp;
  rm_wid node, group_item, anchor, type, threshold;
} image_ids;

/* The identifiers of the image numbered `number`, its range's base plus the
 * fixed offsets `image_ids` lists. */
static image_ids image_ids_of(size_t number) {
  uint64_t base = FIRST_IMAGE + (uint64_t)number * IMAGE_STRIDE;
  return (image_ids){
      .item = wid(DRAFT_AUTHOR, base),
      .key_stamp = wid(DRAFT_AUTHOR, base + 1),
      .name_stamp = wid(DRAFT_AUTHOR, base + 2),
      .node = wid(DRAFT_AUTHOR, base + 3),
      .group_item = wid(DRAFT_AUTHOR, base + 4),
      .anchor = wid(DRAFT_AUTHOR, base + 5),
      .type = wid(DRAFT_AUTHOR, base + 6),
      .threshold = wid(DRAFT_AUTHOR, base + 7),
  };
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* Text a C caller hands over is bytes, and bytes need not be UTF-8. Every
 * ill-formed run is replaced by U+FFFD before anything is counted, so that
 * the counters, the page info and the anchors agree with what a reader reads
 * back. A null string is an empty one. Malloc'd; null when out of memory. */
static char* well_formed(const char* text) {
  if (text == nullptr) text = "";
  size_t length = strlen(text);
  char* out = malloc(rm_utf8_repair_bound(length));
  if (out == nullptr) return nullptr;
  out[rm_utf8_repair(text, length, out)] = '\0';
  return out;
}

/* ------------------------------------------------------------------ */

rm_draft_column rm_draft_column_of(rm_text_column column) {
  return (rm_draft_column){
      .width = rm_text_column_width(column),
      .setting = rm_text_column_setting(column),
  };
}

rm_draft_point rm_draft_point_make(float x, float y) {
  return (rm_draft_point){
      .x = x, .y = y, .speed = 0, .width = 12, .direction = 0, .pressure = 6};
}

rm_draft_stroke rm_draft_stroke_make(rm_pen_tool tool, rm_pen_color color,
                                     const rm_draft_point* points,
                                     size_t point_count) {
  return (rm_draft_stroke){.tool = tool,
                           .color = color,
                           .has_argb = false,
                           .argb = 0,
                           .thickness = 2,
                           .points = points,
                           .point_count = point_count};
}

/* How many Unicode scalars `text` holds, nought for a null string. */
static size_t scalar_count(const char* text) {
  if (text == nullptr) return 0;
  return rm_utf8_scalar_count(text, strlen(text));
}

/* The author the formatting marks are written under, when the draft names
 * none.
 *
 * Derived from the page's own author, and derived the same way every time. A
 * fresh UUID here would make the writer answer with different bytes for one
 * and the same draft: a byte-level regression test would be impossible,
 * content-addressed storage would see a change on every rewrite, and two pages
 * of one notebook would name different authors for the same kind of edit.
 *
 * The value decides nothing that is drawn — only that the marks' author index
 * resolves in the page's table — so the transformation has only to be stable
 * and to differ from the author it came from. The last byte is flipped and
 * nothing else: bytes 6 and 8 carry the version and the variant, and leaving
 * them be is what keeps the result a well-formed UUID rather than sixteen
 * bytes that merely look like one. */
static void marks_author_derived_from(const uint8_t author[16],
                                      uint8_t out[16]) {
  memcpy(out, author, 16);
  out[15] ^= 0x01u;
}

/* ------------------------------------------------------------------ */
/* Blocks                                                              */
/* ------------------------------------------------------------------ */

/* `0x05` SceneLineItem: the preamble and the stroke itself.
 *
 * One writer for both kinds of stroke — the anchored ones differ only in which
 * node they name as their parent. */
static void line_item(rm_buf* out, const rm_draft_stroke* stroke, rm_wid parent,
                      rm_wid id, rm_wid previous) {
  rm_buf value;
  rm_buf_init(&value);
  rm_buf_append_byte(&value, 0x03); /* value type: line */
  rm_write_tag(&value, 1, RM_TAG_BYTE4);
  rm_write_u32(&value, (uint32_t)stroke->tool);
  rm_write_tag(&value, 2, RM_TAG_BYTE4);
  rm_write_u32(&value, (uint32_t)stroke->color);
  rm_write_tag(&value, 3, RM_TAG_BYTE8);
  rm_write_f64(&value, stroke->thickness); /* the setting, not a factor */
  rm_write_tag(&value, 4, RM_TAG_BYTE4);
  rm_write_f32(&value, 0); /* starting length */

  /* The four numbers beside the coordinates, in version 2's order — speed,
   * **width**, direction, pressure — and not version 1's, which is speed,
   * direction, width, pressure: swapped, the strokes come out the right
   * shape and the wrong thickness. */
  rm_buf points;
  rm_buf_init(&points);
  for (size_t i = 0; i < stroke->point_count; i += 1) {
    const rm_draft_point* point = &stroke->points[i];
    rm_write_f32(&points, point->x);
    rm_write_f32(&points, point->y);
    rm_write_u16(&points, point->speed);
    rm_write_u16(&points, point->width);
    rm_buf_append_byte(&points, point->direction);
    rm_buf_append_byte(&points, point->pressure);
  }
  rm_write_block(&value, 5, points.bytes, points.count);
  if (points.failed) value.failed = true;
  rm_buf_free(&points);

  /* The colour written out in full, for the strokes whose index cannot name
   * it: every shader colour, and the highlighter's blue and orange. The two
   * identifiers before it are the tail a tablet page carries — a timestamp
   * and a move — and they go in only here, because tag 8 stands behind them:
   * a field found before its predecessors is a field read as something
   * else. */
  if (stroke->has_argb) {
    rm_write_id(&value, 6, ID_NONE);
    rm_write_id(&value, 7, ID_NONE);
    rm_write_tag(&value, 8, RM_TAG_BYTE4);
    rm_write_u32(&value, stroke->argb);
  }

  rm_write_id(out, 1, parent);
  rm_write_id(out, 2, id);
  rm_write_id(out, 3, previous);
  rm_write_id(out, 4, ID_NONE);
  rm_write_tag(out, 5, RM_TAG_BYTE4);
  rm_write_u32(out, 0);
  rm_write_block(out, 6, value.bytes, value.count);
  if (value.failed) out->failed = true;
  rm_buf_free(&value);
}

/* The name of a node, as the format spells a string: how many bytes, the
 * encoding marker, the bytes. */
static void node_name(rm_buf* out, const char* label) {
  rm_buf value;
  rm_buf_init(&value);
  rm_write_string(&value, label);
  rm_write_id(out, 1, ID_NONE);
  rm_write_block(out, 2, value.bytes, value.count);
  if (value.failed) out->failed = true;
  rm_buf_free(&value);
}

/* `0x04` SceneGroupItem: a node's place in the drawing order. The layer sits
 * under the root; an anchored group sits under the layer. */
static void group_item_block(rm_buf* out, rm_wid node, rm_wid item,
                             rm_wid parent, rm_wid previous) {
  rm_buf value;
  rm_buf_init(&value);
  rm_buf_append_byte(&value, 0x02); /* value type: group */
  rm_write_id(&value, 2, node);

  rm_write_id(out, 1, parent);
  rm_write_id(out, 2, item);
  rm_write_id(out, 3, previous); /* left: the mark before */
  rm_write_id(out, 4, ID_NONE);
  rm_write_tag(out, 5, RM_TAG_BYTE4);
  rm_write_u32(out, 0);
  rm_write_block(out, 6, value.bytes, value.count);
  if (value.failed) out->failed = true;
  rm_buf_free(&value);
}

/* The identifier a mark or an anchored image hangs on, and how the tablet
 * describes that kind of anchor: a character, or a boundary between lines. */
typedef struct anchor_spec {
  rm_wid id;
  uint8_t type;
  float threshold;
} anchor_spec;

/* The identifier and the type/threshold pair that spell `anchor` the way the
 * tablet does. False when the anchor names a paragraph the text has not got. */
static bool anchor_spec_of(rm_draft_anchor anchor, const rm_wid* characters,
                           size_t count, anchor_spec* out) {
  switch (anchor.kind) {
    case RM_ANCHOR_FIRST_CHARACTER:
      if (anchor.paragraph < 0 || (size_t)anchor.paragraph >= count)
        return false;
      *out = (anchor_spec){
          .id = characters[anchor.paragraph], .type = 1, .threshold = 0};
      return true;
    case RM_ANCHOR_BEFORE_TEXT:
      /* The pen went nowhere near a character, so the anchor is a line
       * boundary — here the one before everything typed. Type and
       * threshold travel as a hard pair. */
      *out = (anchor_spec){.id = wid(0, 0xFFFF'FFFF'FFFEu),
                           .type = 2,
                           .threshold = LINE_ANCHOR_THRESHOLD};
      return true;
    case RM_ANCHOR_AFTER_TEXT:
      *out = (anchor_spec){.id = wid(0, 0xFFFF'FFFF'FFFFu),
                           .type = 2,
                           .threshold = LINE_ANCHOR_THRESHOLD};
      return true;
  }
  return false;
}

/* The stamps of the four anchor fields of one group — each an identifier of
 * its own, as they are on a page the tablet wrote. */
typedef struct anchor_stamps {
  rm_wid anchor, type, threshold, origin;
} anchor_stamps;

/* The group that carries an anchor: its tree record, its node with the four
 * anchor fields, and its place in the drawing order — everything of an
 * anchored mark or image except the item that hangs inside. */
static void anchored_group_blocks(rm_buf* file, rm_wid node, rm_wid item,
                                  anchor_stamps stamps, anchor_spec anchor,
                                  float pen_origin, rm_wid previous) {
  rm_buf tree, parent, anchor_field, node_block, group;
  rm_buf_init(&tree);
  rm_buf_init(&parent);
  rm_buf_init(&anchor_field);
  rm_buf_init(&node_block);
  rm_buf_init(&group);

  rm_write_id(&tree, 1, node);
  rm_write_id(&tree, 2, ID_NONE);
  rm_write_tag(&tree, 3, RM_TAG_BYTE1);
  rm_buf_append_byte(&tree, 1);
  rm_write_id(&parent, 1, ID_LAYER);
  rm_write_block(&tree, 4, parent.bytes, parent.count);

  /* Two identifiers, and the anchor is the second: the first belongs to the
   * node, the second names the character or the line boundary. */
  rm_write_id(&anchor_field, 1, stamps.anchor);
  rm_write_id(&anchor_field, 2, anchor.id);

  rm_write_id(&node_block, 1, node);
  /* An anchored group carries a name and it is empty — the tablet names its
   * layers and not these. */
  {
    rm_buf name;
    rm_buf_init(&name);
    node_name(&name, "");
    rm_write_block(&node_block, 2, name.bytes, name.count);
    if (name.failed) node_block.failed = true;
    rm_buf_free(&name);
  }
  rm_write_stamped_u8(&node_block, 3, ID_NONE, 1);
  rm_write_block(&node_block, 7, anchor_field.bytes, anchor_field.count);
  /* The type and the threshold go in hard pairs: a character anchor is 1 and
   * 0, a line anchor 2 and 35.75. All three fields are stamped values, like
   * the anchor above them: written bare, the page still parses and the
   * tablet refuses to draw it. */
  rm_write_stamped_u8(&node_block, 8, stamps.type, anchor.type);
  rm_write_stamped_f32(&node_block, 9, stamps.threshold, anchor.threshold);
  /* `anchor_origin_x`. Anchored to the first character, the pen stands on
   * the left edge of the text: the box origin plus the four units the text
   * sits in by. */
  rm_write_stamped_f32(&node_block, 10, stamps.origin, pen_origin);

  group_item_block(&group, node, item, ID_LAYER, previous);

  rm_write_top_block(file, 0x01, 1, 1, tree.bytes, tree.count);
  rm_write_top_block(file, 0x02, 1, 2, node_block.bytes, node_block.count);
  rm_write_top_block(file, 0x04, 1, 1, group.bytes, group.count);
  if (tree.failed || parent.failed || anchor_field.failed ||
      node_block.failed || group.failed) {
    file->failed = true;
  }

  rm_buf_free(&tree);
  rm_buf_free(&parent);
  rm_buf_free(&anchor_field);
  rm_buf_free(&node_block);
  rm_buf_free(&group);
}

/* One anchored stroke: the group that carries the anchor, its place in the
 * tree and in the drawing order, and the stroke itself.
 *
 * Four blocks rather than one because that is how the tablet says it: the
 * anchor lives on a **node**, and a stroke is anchored by having that node for
 * a parent. */
static void anchored_mark_blocks(rm_buf* file, const rm_draft_mark* mark,
                                 size_t number, anchor_spec anchor,
                                 rm_wid previous) {
  mark_ids ids = mark_ids_of(number);

  anchored_group_blocks(file, ids.node, ids.item,
                        (anchor_stamps){.anchor = ids.anchor,
                                        .type = ids.type,
                                        .threshold = ids.threshold,
                                        .origin = ids.origin},
                        anchor, mark->pen_origin, previous);

  /* The stroke hangs on the group, and the group on the text: that parent is
   * the whole of what "anchored" means in the file. The points are the v2
   * layout, and the block header is what says so — both halves of it: a
   * stroke block on a tablet page declares 2 as its minimum too. */
  rm_buf line;
  rm_buf_init(&line);
  line_item(&line, &mark->stroke, ids.node, ids.stroke, ID_NONE);
  rm_write_top_block(file, 0x05, 2, 2, line.bytes, line.count);
  if (line.failed) file->failed = true;
  rm_buf_free(&line);
}

/* `0x09` AuthorIds: our UUID against index 1.
 *
 * The bytes go in as the UUID reads, not byte-swapped: a page typed on the
 * tablet carries the same sixteen bytes its `.content` spells out. Swapped,
 * the page and the document it belongs to name two different authors. */
static void authors_block(rm_buf* out, const uint8_t author[16],
                          const uint8_t* marks) {
  rm_write_varuint(out, marks == nullptr ? 1u : 2u);
  /* The marks author's entry stands first, the way a tablet page keeps its
   * table. */
  if (marks != nullptr) {
    rm_buf entry;
    rm_buf_init(&entry);
    rm_write_varuint(&entry, 16);
    rm_buf_append(&entry, marks, 16);
    rm_write_u16(&entry, DRAFT_MARKS_AUTHOR);
    rm_write_block(out, 0, entry.bytes, entry.count);
    if (entry.failed) out->failed = true;
    rm_buf_free(&entry);
  }
  rm_buf entry;
  rm_buf_init(&entry);
  rm_write_varuint(&entry, 16);
  rm_buf_append(&entry, author, 16);
  rm_write_u16(&entry, DRAFT_AUTHOR);
  rm_write_block(out, 0, entry.bytes, entry.count);
  if (entry.failed) out->failed = true;
  rm_buf_free(&entry);
}

/* `0x00` MigrationInfo (also named `Conversion`):
 * the tablet wrote the page. Generated files are meant to be read as the
 * device's own, and this is what a page typed there says: `is_device` set,
 * `migratedLegacy` clear — a native v6 page, not one converted from `.lines`.
 * The flag only steers how two versions of a page merge; it decides nothing
 * about drawing. */
static void migration_block(rm_buf* out) {
  rm_write_id(out, 1, ID_TEXT);
  rm_write_tag(out, 2, RM_TAG_BYTE1);
  rm_buf_append_byte(out, 1); /* is_device = yes */
  rm_write_tag(out, 3, RM_TAG_BYTE1);
  rm_buf_append_byte(out, 0); /* migratedLegacy = no */
}

/* `0x0A` PageInfo: the counters the tablet keeps per page. */
static void page_info_block(rm_buf* out, size_t scalars, size_t paragraphs) {
  rm_write_tag(out, 1, RM_TAG_BYTE4);
  rm_write_u32(out, 1); /* loads */
  rm_write_tag(out, 2, RM_TAG_BYTE4);
  rm_write_u32(out, 0); /* merges */
  rm_write_tag(out, 3, RM_TAG_BYTE4);
  rm_write_u32(out, (uint32_t)scalars);
  rm_write_tag(out, 4, RM_TAG_BYTE4);
  rm_write_u32(out, (uint32_t)paragraphs);
  rm_write_tag(out, 5, RM_TAG_BYTE4);
  rm_write_u32(out, 0); /* keyboardCount */
}

/* `0x0D` SceneInfo: the current layer, what is shown under the ink, and the
 * sheet. */
static void scene_info_block(rm_buf* out, const rm_draft_sheet* sheet,
                             const rm_draft_column* column, bool has_images) {
  rm_buf current_layer, size, canvas;
  rm_buf_init(&current_layer);
  rm_buf_init(&size);
  rm_buf_init(&canvas);

  rm_write_id(&current_layer, 1, ID_NONE);
  rm_write_id(&current_layer, 2, ID_LAYER);

  /* The sheet, declared the three ways the tablet declares it: whole units
   * under tag 5, the same size unrounded under tag 7, and once more stamped
   * under tag 8 — the one the format calls authoritative. Which sheet it is
   * decides where the text lands. */
  rm_write_u32(&size, sheet->screen_width);
  rm_write_u32(&size, sheet->screen_height);
  rm_write_f64(&canvas, sheet->canvas_width);
  rm_write_f64(&canvas, sheet->canvas_height);

  rm_write_block(out, 1, current_layer.bytes, current_layer.count);
  rm_write_stamped_u8(out, 2, ID_NONE, 1); /* template shown */
  /* The imported document's visibility. A notebook has no PDF underneath and
   * the field decides nothing here; the tablet writes it on its typed pages,
   * so ours carry it too. */
  rm_write_stamped_u8(out, 3, ID_NONE, 1);
  rm_write_block(out, 5, size.bytes, size.count);
  /* Tag 6 is `extendedContentRect` — an LWW of four float64, all zeros. The
   * tablet may put it on every page; ours leaves it out of typed-only pages,
   * keeping them byte for byte what the tablet accepts. A page that carries
   * an image writes it, because a tablet page with an image does, and a
   * generated page is worth what it has in common with the tablet's own
   * file. */
  if (has_images) {
    rm_buf viewport;
    rm_buf_init(&viewport);
    rm_write_id(&viewport, 1, ID_NONE);
    uint8_t zeroes[32] = {0};
    rm_write_block(&viewport, 2, zeroes, sizeof zeroes);
    rm_write_block(out, 6, viewport.bytes, viewport.count);
    if (viewport.failed) out->failed = true;
    rm_buf_free(&viewport);
  }
  rm_write_block(out, 7, canvas.bytes, canvas.count);
  rm_write_stamped_block(out, 8, ID_CANVAS_STAMP, canvas.bytes, canvas.count);
  /* Tag 9 is the column setting itself — nought, one or two, not the width.
   * It has to name the same column the text box was built to. */
  rm_write_stamped_block(out, 9, ID_COLUMN_STAMP, &column->setting, 1);

  if (current_layer.failed || size.failed || canvas.failed) out->failed = true;
  rm_buf_free(&current_layer);
  rm_buf_free(&size);
  rm_buf_free(&canvas);
}

/* `0x01` SceneTree: the layer exists under the root. */
static void scene_tree_block(rm_buf* out) {
  rm_write_id(out, 1, ID_LAYER);
  rm_write_id(out, 2, ID_NONE);
  rm_write_tag(out, 3, RM_TAG_BYTE1);
  rm_buf_append_byte(out, 1);
  rm_buf parent;
  rm_buf_init(&parent);
  rm_write_id(&parent, 1, ID_ROOT);
  rm_write_block(out, 4, parent.bytes, parent.count);
  if (parent.failed) out->failed = true;
  rm_buf_free(&parent);
}

/* `0x02` SceneTreeNode: the layer's name and visibility. */
static void layer_node_block(rm_buf* out) {
  rm_write_id(out, 1, ID_LAYER);
  rm_buf name;
  rm_buf_init(&name);
  node_name(&name, "Layer 1");
  rm_write_block(out, 2, name.bytes, name.count);
  if (name.failed) out->failed = true;
  rm_buf_free(&name);
  rm_write_stamped_u8(out, 3, ID_NONE, 1);
}

/* `0x0E` AttachmentMap: every attachment of the page, keyed the way the image
 * items name them.
 *
 * As the tablet writes it: version 3/3, one map per page. An entry is the
 * 16-byte key, the stamped file name — extension included — and a second
 * stamped field whose two bytes read `11 00`. That tail is mirrored verbatim:
 * its meaning is unknown, and inventing one would be writing a format the
 * tablet does not write. */
static void attachment_map_block(rm_buf* out, const rm_draft_image* images,
                                 size_t count) {
  rm_buf entries;
  rm_buf_init(&entries);
  rm_write_varuint(&entries, (uint64_t)count);
  for (size_t number = 0; number < count; number += 1) {
    rm_buf entry, name, text, tail, tail_value;
    rm_buf_init(&entry);
    rm_buf_init(&name);
    rm_buf_init(&text);
    rm_buf_init(&tail);
    rm_buf_init(&tail_value);

    rm_buf_append(&entry, images[number].key, 16);
    rm_write_id(&name, 1, image_ids_of(number).name_stamp);
    char* file = well_formed(images[number].file);
    if (file == nullptr) {
      text.failed = true;
    } else {
      rm_write_string(&text, file);
      free(file);
    }
    rm_write_block(&name, 2, text.bytes, text.count);
    rm_write_block(&entry, 1, name.bytes, name.count);

    rm_buf_append_byte(&tail_value, 0x11);
    rm_buf_append_byte(&tail_value, 0x00);
    rm_write_id(&tail, 1, ID_NONE);
    rm_write_block(&tail, 2, tail_value.bytes, tail_value.count);
    rm_write_block(&entry, 2, tail.bytes, tail.count);

    rm_write_block(&entries, 0, entry.bytes, entry.count);
    if (entry.failed || name.failed || text.failed || tail.failed ||
        tail_value.failed) {
      entries.failed = true;
    }
    rm_buf_free(&entry);
    rm_buf_free(&name);
    rm_buf_free(&text);
    rm_buf_free(&tail);
    rm_buf_free(&tail_value);
  }
  rm_write_block(out, 1, entries.bytes, entries.count);
  if (entries.failed) out->failed = true;
  rm_buf_free(&entries);
}

/* `0x0F` SceneImageItem: the placement of one attachment, hung on the layer
 * like a stroke.
 *
 * As the tablet writes it: value type 7, the key stamped under tag 1, four
 * vertices under tag 3 — the quad's corners with their UV, plain noughts and
 * ones for an uncropped image — and under tag 4 the quad as two triangles, the
 * same six indices on every item. The two identifiers at tag 2 and tag
 * 5 are the insert flow's lineage — a moved or resized image points them at its
 * earlier incarnations — and a fresh page has none to point at. */
static void image_item(rm_buf* out, const rm_draft_image* image, image_ids ids,
                       rm_wid parent, rm_wid previous) {
  rm_buf value, keyed, vertices, indices;
  rm_buf_init(&value);
  rm_buf_init(&keyed);
  rm_buf_init(&vertices);
  rm_buf_init(&indices);

  rm_buf_append_byte(&value, 0x07); /* value type: image */
  rm_write_id(&keyed, 1, ids.key_stamp);
  rm_write_block(&keyed, 2, image->key, 16);
  rm_write_block(&value, 1, keyed.bytes, keyed.count);
  rm_write_id(&value, 2, ID_NONE);

  rm_write_varuint(&vertices, 16);
  const rm_image_vertex corners[4] = {
      {.x = image->x, .y = image->y, .u = 0, .v = 0},
      {.x = image->x + image->width, .y = image->y, .u = 1, .v = 0},
      {.x = image->x + image->width,
       .y = image->y + image->height,
       .u = 1,
       .v = 1},
      {.x = image->x, .y = image->y + image->height, .u = 0, .v = 1},
  };
  for (size_t i = 0; i < 4; i += 1) {
    rm_write_f32(&vertices, (float)corners[i].x);
    rm_write_f32(&vertices, (float)corners[i].y);
    rm_write_f32(&vertices, (float)corners[i].u);
    rm_write_f32(&vertices, (float)corners[i].v);
  }
  rm_write_block(&value, 3, vertices.bytes, vertices.count);

  rm_write_varuint(&indices, 6);
  const uint32_t quad[6] = {0, 1, 2, 2, 3, 0};
  for (size_t i = 0; i < 6; i += 1) rm_write_u32(&indices, quad[i]);
  rm_write_block(&value, 4, indices.bytes, indices.count);
  rm_write_id(&value, 5, ID_NONE);

  rm_write_id(out, 1, parent);
  rm_write_id(out, 2, ids.item);
  rm_write_id(out, 3, previous);
  rm_write_id(out, 4, ID_NONE);
  rm_write_tag(out, 5, RM_TAG_BYTE4);
  rm_write_u32(out, 0);
  rm_write_block(out, 6, value.bytes, value.count);

  if (value.failed || keyed.failed || vertices.failed || indices.failed)
    out->failed = true;
  rm_buf_free(&value);
  rm_buf_free(&keyed);
  rm_buf_free(&vertices);
  rm_buf_free(&indices);
}

/* ------------------------------------------------------------------ */
/* `0x07` RootText                                                     */
/* ------------------------------------------------------------------ */

/* The paragraphs, the styles keyed to them, and the box they are set in.
 *
 * Hands back two things only this walk knows: how many characters there are,
 * which `0x0A` has to declare, and the identifier of the first character of
 * each paragraph, which is what an anchored stroke names. */
typedef struct text_writer {
  rm_buf items;
  size_t records;
  uint64_t counter;
  /* Two different tails: `chain_tail` is the last text record of the CRDT
   * chain (what the next record's `left` names; face marks stand beside the
   * chain and do not move it); `last_newline` is the newline character that
   * keys the next paragraph's style record. */
  rm_wid chain_tail;
  rm_wid last_newline;
  size_t total_scalars;
  uint64_t mark_counter;
} text_writer;

/* One text record: `raw` repaired to well-formed UTF-8, chained onto the
 * record before it, and the chain tail and counters advanced past it. */
static void append_record(text_writer* state, const char* raw) {
  char* text = well_formed(raw);
  if (text == nullptr) {
    state->items.failed = true;
    return;
  }
  rm_buf body, value;
  rm_buf_init(&body);
  rm_buf_init(&value);

  rm_write_id(&body, 2, wid(DRAFT_AUTHOR, state->counter));
  rm_write_id(&body, 3, state->chain_tail);
  rm_write_id(&body, 4, ID_NONE);
  rm_write_tag(&body, 5, RM_TAG_BYTE4);
  rm_write_u32(&body, 0);
  rm_write_string(&value, text);
  rm_write_block(&body, 6, value.bytes, value.count);
  rm_write_block(&state->items, 0, body.bytes, body.count);
  if (body.failed || value.failed) state->items.failed = true;
  rm_buf_free(&body);
  rm_buf_free(&value);

  state->records += 1;
  /* The characters of a record continue its own identifier, and a record of
   * no characters still holds one place in the chain. */
  size_t scalars = scalar_count(text);
  uint64_t span = scalars < 1 ? 1u : (uint64_t)scalars;
  state->chain_tail = wid(DRAFT_AUTHOR, state->counter + span - 1u);
  state->counter += span;
  state->total_scalars += scalars;
  free(text);
}

/* A face mark, the way the tablet spells one: a second author's item standing
 * beside the text chain — its `left` names a text scalar, its `right` the
 * neighbour it precedes, and it consumes no scalar of the text counter. A mark
 * woven into the chain itself is a shape the tablet declines to draw. */
static rm_wid append_mark(text_writer* state, uint32_t mark, rm_wid left,
                          rm_wid right) {
  rm_wid id = wid(DRAFT_MARKS_AUTHOR, state->mark_counter);
  state->mark_counter += 1;

  rm_buf body, value;
  rm_buf_init(&body);
  rm_buf_init(&value);
  rm_write_id(&body, 2, id);
  rm_write_id(&body, 3, left);
  rm_write_id(&body, 4, right);
  rm_write_tag(&body, 5, RM_TAG_BYTE4);
  rm_write_u32(&body, 0);
  /* No text at all: what makes this a mark rather than a record is the field
   * behind the empty string. */
  rm_write_string(&value, "");
  rm_write_tag(&value, 2, RM_TAG_BYTE4);
  rm_write_u32(&value, mark);
  rm_write_block(&body, 6, value.bytes, value.count);
  rm_write_block(&state->items, 0, body.bytes, body.count);
  if (body.failed || value.failed) state->items.failed = true;
  rm_buf_free(&body);
  rm_buf_free(&value);

  state->records += 1;
  return id;
}

typedef struct text_block_result {
  rm_buf bytes;
  size_t scalars;
  /* The identifier of each paragraph's first character; one entry per
   * paragraph, malloc'd, the caller frees. */
  rm_wid* first_characters;
} text_block_result;

/* The `0x07` RootText block's payload, built from the draft's paragraphs:
 * the text and style records, and the box they are set in. False on
 * allocation failure, with `*out` left unset. */
static bool text_block(const rm_draft_paragraph* paragraphs, size_t count,
                       const rm_draft_column* column, text_block_result* out) {
  text_writer state = {
      .records = 0,
      .counter = FIRST_CHARACTER,
      .chain_tail = ID_NONE,
      .last_newline = ID_NONE,
      .total_scalars = 0,
      .mark_counter = 1000000,
  };
  rm_buf_init(&state.items);

  rm_wid* first_characters =
      calloc(count == 0 ? 1 : count, sizeof *first_characters);
  /* The style records, in the order the paragraphs carry them. */
  size_t* styled = calloc(count == 0 ? 1 : count, sizeof *styled);
  rm_wid* style_keys = calloc(count == 0 ? 1 : count, sizeof *style_keys);
  if (first_characters == nullptr || styled == nullptr ||
      style_keys == nullptr) {
    free(first_characters);
    free(styled);
    free(style_keys);
    rm_buf_free(&state.items);
    return false;
  }
  size_t style_count = 0;

  for (size_t index = 0; index < count; index += 1) {
    const rm_draft_paragraph* paragraph = &paragraphs[index];
    /* The style is keyed by the newline that ends the paragraph before;
     * the first paragraph is keyed by the start sentinel. */
    rm_wid key = index == 0 ? ID_NONE : state.last_newline;
    if (paragraph->has_style) {
      style_keys[style_count] = key;
      styled[style_count] = index;
      style_count += 1;
    }

    bool last = index + 1 == count;
    first_characters[index] = wid(DRAFT_AUTHOR, state.counter);

    if (paragraph->text == nullptr || paragraph->text[0] == '\0') {
      /* An empty paragraph is its newline and nothing else — one record,
       * so the scalar counters the anchors key on do not move. */
      append_record(&state, last ? "" : "\n");
      state.last_newline = state.chain_tail;
      continue;
    }

    /* Opening marks stand beside the chain, pointing right at the text
     * item — italic first, bold chained onto it, the tablet's order. */
    rm_wid text_id = wid(DRAFT_AUTHOR, state.counter);
    rm_wid opener = state.chain_tail;
    if (paragraph->italic)
      opener = append_mark(&state, 3, state.chain_tail, text_id);
    if (paragraph->bold) (void)append_mark(&state, 1, opener, text_id);

    append_record(&state, paragraph->text);
    rm_wid text_tail = state.chain_tail;
    rm_wid newline_id = last ? ID_NONE : wid(DRAFT_AUTHOR, state.counter);
    /* Closing marks hang off the last letter and point at the newline —
     * bold first, italic after, the nesting the tablet closes with. */
    if (paragraph->bold && paragraph->italic) {
      rm_wid italic_off = wid(DRAFT_MARKS_AUTHOR, state.mark_counter + 1u);
      (void)append_mark(&state, 2, text_tail, italic_off);
      (void)append_mark(&state, 4, text_tail, newline_id);
    } else if (paragraph->bold) {
      (void)append_mark(&state, 2, text_tail, newline_id);
    } else if (paragraph->italic) {
      (void)append_mark(&state, 4, text_tail, newline_id);
    }
    if (!last) {
      append_record(&state, "\n");
      state.last_newline = state.chain_tail;
    }
  }

  rm_buf item_rows, item_list, item_container, style_rows, style_list, both,
      position, stamped;
  rm_buf_init(&item_rows);
  rm_buf_init(&item_list);
  rm_buf_init(&item_container);
  rm_buf_init(&style_rows);
  rm_buf_init(&style_list);
  rm_buf_init(&both);
  rm_buf_init(&position);
  rm_buf_init(&stamped);
  rm_buf_init(&out->bytes);

  rm_write_varuint(&item_rows, (uint64_t)state.records);
  rm_buf_append(&item_rows, state.items.bytes, state.items.count);
  rm_write_block(&item_list, 1, item_rows.bytes, item_rows.count);
  rm_write_block(&item_container, 1, item_list.bytes, item_list.count);

  rm_write_varuint(&style_rows, (uint64_t)style_count);
  for (size_t i = 0; i < style_count; i += 1) {
    const rm_draft_paragraph* paragraph = &paragraphs[styled[i]];
    rm_write_bare_id(&style_rows, style_keys[i]);
    rm_write_id(&style_rows, 1, ID_NONE);
    rm_buf value;
    rm_buf_init(&value);
    rm_write_tag(&value, 1, RM_TAG_BYTE1);
    rm_buf_append_byte(&value, paragraph->style);
    if (paragraph->has_second) {
      rm_write_tag(&value, 2, RM_TAG_BYTE1);
      rm_buf_append_byte(&value, paragraph->second);
    }
    if (paragraph->has_third) {
      rm_write_tag(&value, 3, RM_TAG_BYTE4);
      rm_write_u32(&value, paragraph->third);
    }
    rm_write_block(&style_rows, 2, value.bytes, value.count);
    if (value.failed) style_rows.failed = true;
    rm_buf_free(&value);
  }
  rm_write_block(&style_list, 1, style_rows.bytes, style_rows.count);

  /* Both lists share the one subblock — the only place in the format where
   * that happens. */
  rm_buf_append(&both, item_container.bytes, item_container.count);
  rm_write_block(&both, 2, style_list.bytes, style_list.count);

  rm_write_id(&out->bytes, 1, ID_TEXT);
  rm_write_block(&out->bytes, 2, both.bytes, both.count);
  /* The box: the column is centred on the sheet, so it starts half its own
   * width to the left of the middle. */
  rm_write_f64(&position, (double)(-column->width / 2.0f));
  rm_write_f64(&position, 234);
  rm_write_block(&out->bytes, 3, position.bytes, position.count);
  rm_write_tag(&out->bytes, 4, RM_TAG_BYTE4);
  rm_write_f32(&out->bytes, column->width);
  /* Tag 5 — the text's settings, whose tag 1 is the same width once more,
   * stamped. It takes precedence over tag 4 (`textWidthLegacy`), and the
   * tablet writes it on every typed page. */
  rm_write_stamped_f32(&stamped, 1, ID_WIDTH_STAMP, column->width);
  rm_write_block(&out->bytes, 5, stamped.bytes, stamped.count);

  bool ok =
      !(state.items.failed || item_rows.failed || item_list.failed ||
        item_container.failed || style_rows.failed || style_list.failed ||
        both.failed || position.failed || stamped.failed || out->bytes.failed);

  rm_buf_free(&state.items);
  rm_buf_free(&item_rows);
  rm_buf_free(&item_list);
  rm_buf_free(&item_container);
  rm_buf_free(&style_rows);
  rm_buf_free(&style_list);
  rm_buf_free(&both);
  rm_buf_free(&position);
  rm_buf_free(&stamped);
  free(styled);
  free(style_keys);

  if (!ok) {
    rm_buf_free(&out->bytes);
    free(first_characters);
    return false;
  }
  out->scalars = state.total_scalars;
  out->first_characters = first_characters;
  return true;
}

/* ------------------------------------------------------------------ */
/* The door                                                            */
/* ------------------------------------------------------------------ */

/* Fill in `*out_error`, when the caller asked for one, with the status, the
 * numeric value and an optional message. */
static void write_fail(rm_error* out_error, rm_status status, long value,
                       const char* message) {
  if (out_error == nullptr) return;
  *out_error = (rm_error){.status = status, .value = value};
  if (message != nullptr) {
    snprintf(out_error->message, sizeof out_error->message, "%s", message);
  }
}

/* An array the draft declares a length for has to be there. A count without
 * its items is a caller's slip and not a page — and read on trust it is a
 * null dereference inside the writer rather than a status at its door.
 *
 * The pointer may be null when the count is nought: that is how a draft says
 * it has no strokes, and every loop over it is skipped. */
static rm_status check_arrays_are_there(const rm_page_draft* draft,
                                        rm_error* out_error) {
  struct {
    const void* items;
    size_t count;
    const char* what;
  } declared[] = {
      {draft->paragraphs, draft->paragraph_count, "paragraphs"},
      {draft->strokes, draft->stroke_count, "strokes"},
      {draft->marks, draft->mark_count, "marks"},
      {draft->images, draft->image_count, "images"},
  };
  for (size_t i = 0; i < sizeof declared / sizeof *declared; i += 1) {
    if (declared[i].count != 0 && declared[i].items == nullptr) {
      write_fail(out_error, RM_ERR_INVALID_ARGUMENT, (long)declared[i].count,
                 declared[i].what);
      return RM_ERR_INVALID_ARGUMENT;
    }
  }
  return RM_OK;
}

/* The same check as `check_arrays_are_there`, one level down: a stroke or a
 * mark's stroke whose point count is not nought has to carry the points. */
static rm_status check_point_arrays(const rm_page_draft* draft,
                                    rm_error* out_error) {
  for (size_t i = 0; i < draft->stroke_count; i += 1) {
    if (draft->strokes[i].point_count != 0 &&
        draft->strokes[i].points == nullptr) {
      write_fail(out_error, RM_ERR_INVALID_ARGUMENT, (long)i, "strokes.points");
      return RM_ERR_INVALID_ARGUMENT;
    }
  }
  for (size_t i = 0; i < draft->mark_count; i += 1) {
    if (draft->marks[i].stroke.point_count != 0 &&
        draft->marks[i].stroke.points == nullptr) {
      write_fail(out_error, RM_ERR_INVALID_ARGUMENT, (long)i,
                 "marks.stroke.points");
      return RM_ERR_INVALID_ARGUMENT;
    }
  }
  return RM_OK;
}

/* The two ranges that do not depend on the text: the marks and the loose
 * strokes, checked after the characters.
 *
 * Images are not checked: their range is the last one and has nothing above it
 * to run into, and their counters are the author's own — a page would have to
 * carry two hundred million of them to reach anywhere that matters. */
static rm_status check_item_counts_fit(const rm_page_draft* draft,
                                       rm_error* out_error) {
  if (draft->mark_count > MARK_LIMIT) {
    write_fail(out_error, RM_ERR_TOO_MANY_TO_NUMBER, (long)MARK_LIMIT, "marks");
    return RM_ERR_TOO_MANY_TO_NUMBER;
  }
  if (draft->stroke_count > LOOSE_STROKE_LIMIT) {
    write_fail(out_error, RM_ERR_TOO_MANY_TO_NUMBER, (long)LOOSE_STROKE_LIMIT,
               "strokes");
    return RM_ERR_TOO_MANY_TO_NUMBER;
  }
  return RM_OK;
}

/* The characters, which only the laid-out text can count: a paragraph is
 * numbered in Unicode scalars and the newlines the writer puts between the
 * paragraphs take a counter each. */
static rm_status check_characters_fit(const rm_page_draft* draft,
                                      size_t scalars, rm_error* out_error) {
  /* Every scalar takes a counter, and a paragraph with no text still takes
   * one for the newline that ends it. */
  size_t characters = scalars + draft->paragraph_count;
  if (characters > CHARACTER_LIMIT) {
    write_fail(out_error, RM_ERR_TOO_MANY_TO_NUMBER, (long)CHARACTER_LIMIT,
               "characters");
    return RM_ERR_TOO_MANY_TO_NUMBER;
  }
  return RM_OK;
}

/* Which paragraph an anchor names, for the error that says it could not be
 * found. The two sentinels name none, and never fail to resolve. */
static long paragraph_index_of(rm_draft_anchor anchor) {
  return anchor.kind == RM_ANCHOR_FIRST_CHARACTER ? (long)anchor.paragraph
                                                  : -1L;
}

/* The whole `.rm` file of one page: the service four, the text, and a
 * one-layer scene tree — the inventory of a page the tablet itself writes for
 * a typed-only notebook.
 *
 * The service four go out first and in the tablet's order; `0x07` stands
 * before the tree — a placement of our own. Block order past the four is the
 * writer's property, and the tablet accepts it as written. */
rm_status rm_write_page(const rm_page_draft* draft, uint8_t** out_bytes,
                        size_t* out_size, rm_error* out_error) {
  if (out_error != nullptr) *out_error = (rm_error){.status = RM_OK};
  if (draft == nullptr || out_bytes == nullptr || out_size == nullptr) {
    write_fail(out_error, RM_ERR_INVALID_ARGUMENT, 0, nullptr);
    return RM_ERR_INVALID_ARGUMENT;
  }

  /* Everything the draft can be refused for on its own terms, before a
   * single byte is written — in this order: the characters, then the marks
   * and the strokes. Arrays a count promises and does not
   * hand over come first of all; the text cannot be laid out without them. */
  rm_status status = check_arrays_are_there(draft, out_error);
  if (status != RM_OK) return status;

  text_block_result text = {0};
  if (!text_block(draft->paragraphs, draft->paragraph_count, &draft->column,
                  &text)) {
    write_fail(out_error, RM_ERR_OUT_OF_MEMORY, 0, nullptr);
    return RM_ERR_OUT_OF_MEMORY;
  }

  status = check_characters_fit(draft, text.scalars, out_error);
  if (status != RM_OK) goto fail;
  status = check_item_counts_fit(draft, out_error);
  if (status != RM_OK) goto fail;
  status = check_point_arrays(draft, out_error);
  if (status != RM_OK) goto fail;

  rm_buf file;
  rm_buf_init(&file);
  rm_buf_append(&file, RM_HEADER, RM_HEADER_LENGTH);

  /* A page whose text carries faces names a second author: on a tablet page
   * the formatting marks are another session's edits, and their author index
   * has to resolve in the table. */
  bool faces_present = false;
  for (size_t i = 0; i < draft->paragraph_count; i += 1) {
    if (draft->paragraphs[i].bold || draft->paragraphs[i].italic)
      faces_present = true;
  }
  uint8_t marks_author[16];
  const uint8_t* marks = nullptr;
  if (faces_present) {
    if (draft->has_marks_author)
      memcpy(marks_author, draft->marks_author, 16);
    else
      marks_author_derived_from(draft->author, marks_author);
    marks = marks_author;
  }

  rm_buf block;
  rm_buf_init(&block);

#define EMIT(type, min_version, current_version, build)                 \
  do {                                                                  \
    block.count = 0;                                                    \
    build;                                                              \
    rm_write_top_block(&file, (type), (min_version), (current_version), \
                       block.bytes, block.count);                       \
    if (block.failed) file.failed = true;                               \
  } while (0)

  EMIT(0x09, 1, 1, authors_block(&block, draft->author, marks));
  EMIT(0x00, 1, 1, migration_block(&block));
  EMIT(0x0A, 0, 1,
       page_info_block(&block, text.scalars, draft->paragraph_count));
  EMIT(0x0D, 0, 1,
       scene_info_block(&block, &draft->sheet, &draft->column,
                        draft->image_count != 0));

  /* The attachment map stands between SceneInfo and the scene tree — where a
   * tablet page with an image keeps it. One map per page, all attachments in
   * it. */
  if (draft->image_count != 0) {
    EMIT(0x0E, 3, 3,
         attachment_map_block(&block, draft->images, draft->image_count));
  }

  /* A page with no typed text carries no RootText block at all — a tablet
   * image-only page does not, and a generated page is worth what it has in
   * common with the tablet's own file. Marks and anchored images presuppose
   * text: they anchor to it. */
  bool anchored_image = false;
  for (size_t i = 0; i < draft->image_count; i += 1) {
    if (draft->images[i].has_anchor) anchored_image = true;
  }
  if (draft->paragraph_count != 0 || draft->mark_count != 0 || anchored_image) {
    rm_write_top_block(&file, 0x07, 1, 1, text.bytes.bytes, text.bytes.count);
  }

  EMIT(0x01, 1, 1, scene_tree_block(&block));
  EMIT(0x02, 1, 2, layer_node_block(&block));
  EMIT(0x04, 1, 1,
       group_item_block(&block, ID_LAYER, ID_GROUP_ITEM, ID_ROOT, ID_NONE));

  /* The marks and the loose strokes are one chain, not a heap: items sharing
   * a `left` are a tie, and the format breaks it in favour of the newer one —
   * so anything left unchained comes back in the reverse of the order it was
   * written in. They share the chain because they share a parent: both hang
   * on the layer. */
  rm_wid previous = ID_NONE;
  for (size_t number = 0; number < draft->stroke_count; number += 1) {
    block.count = 0;
    line_item(&block, &draft->strokes[number], ID_LAYER,
              loose_stroke_id(number), previous);
    rm_write_top_block(&file, 0x05, 2, 2, block.bytes, block.count);
    if (block.failed) file.failed = true;
    previous = loose_stroke_id(number);
  }

  for (size_t number = 0; number < draft->mark_count; number += 1) {
    const rm_draft_mark* mark = &draft->marks[number];
    anchor_spec anchor;
    /* A refusal, not a skip: a mark whose paragraph is not on the page
     * would vanish without a trace, and silently losing ink is the one
     * thing a writer must never do. */
    if (!anchor_spec_of(mark->anchor, text.first_characters,
                        draft->paragraph_count, &anchor)) {
      status = RM_ERR_ANCHOR_OUT_OF_RANGE;
      write_fail(out_error, status, paragraph_index_of(mark->anchor), "a mark");
      rm_buf_free(&block);
      rm_buf_free(&file);
      goto fail;
    }
    anchored_mark_blocks(&file, mark, number, anchor, previous);
    previous = mark_ids_of(number).item;
  }

  for (size_t number = 0; number < draft->image_count; number += 1) {
    const rm_draft_image* image = &draft->images[number];
    image_ids ids = image_ids_of(number);
    if (image->has_anchor) {
      anchor_spec anchor;
      /* The same refusal as a mark's, for the same reason. */
      if (!anchor_spec_of(image->anchor, text.first_characters,
                          draft->paragraph_count, &anchor)) {
        status = RM_ERR_ANCHOR_OUT_OF_RANGE;
        write_fail(out_error, status, paragraph_index_of(image->anchor),
                   "an image");
        rm_buf_free(&block);
        rm_buf_free(&file);
        goto fail;
      }
      /* The group carries the anchor and the image hangs inside it,
       * alone — as on a tablet page, where the item's `left` is unset
       * and the group's place in the layer's chain is what orders the
       * drawing. The tablet stamps `anchor_origin_x` with the id of
       * the group's own node, not a fresh counter — mirrored, like every
       * other byte of the group. */
      anchored_group_blocks(&file, ids.node, ids.group_item,
                            (anchor_stamps){.anchor = ids.anchor,
                                            .type = ids.type,
                                            .threshold = ids.threshold,
                                            .origin = ids.node},
                            anchor, image->pen_origin, previous);
      block.count = 0;
      image_item(&block, image, ids, ids.node, ID_NONE);
      rm_write_top_block(&file, 0x0F, 2, 2, block.bytes, block.count);
      if (block.failed) file.failed = true;
      previous = ids.group_item;
    } else {
      block.count = 0;
      image_item(&block, image, ids, ID_LAYER, previous);
      rm_write_top_block(&file, 0x0F, 2, 2, block.bytes, block.count);
      if (block.failed) file.failed = true;
      previous = ids.item;
    }
  }

#undef EMIT

  rm_buf_free(&block);
  if (file.failed) {
    rm_buf_free(&file);
    status = RM_ERR_OUT_OF_MEMORY;
    write_fail(out_error, status, 0, nullptr);
    goto fail;
  }

  *out_bytes = rm_buf_take(&file, out_size);
  if (*out_bytes == nullptr) {
    status = RM_ERR_OUT_OF_MEMORY;
    write_fail(out_error, status, 0, nullptr);
    goto fail;
  }
  rm_buf_free(&text.bytes);
  free(text.first_characters);
  return RM_OK;

fail:
  rm_buf_free(&text.bytes);
  free(text.first_characters);
  return status;
}

void rm_bytes_free(uint8_t* bytes) { free(bytes); }
