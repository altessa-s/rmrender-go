/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Parsing a notebook page (the `.rm` format, version 6).
 *
 * The strokes, the text, the glyphs, the scene tree and the attachments each
 * have a parser of their own. What goes unread is counted in
 * `rm_page.skipped_blocks`: a block of a type this parser does not know, and a
 * block of a known type that would not read. The service blocks every page
 * opens with hold nothing to draw and are stepped over uncounted — counting
 * them would put three on every page ever written and leave a real loss
 * indistinguishable from the floor. */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rm_internal.h"
#include "rmlines/parser.h"

/* ------------------------------------------------------------------ */
/* The blocks and the fields                                           */
/* ------------------------------------------------------------------ */

/* The value type inside a stroke block (`RM_BLOCK_SCENE_LINE_ITEM`). */
constexpr uint8_t VALUE_LINE_ITEM = 0x03;

/* A scene tree node and the root text block. */
constexpr uint8_t BLOCK_TREE_NODE = 0x02;
constexpr uint8_t BLOCK_ROOT_TEXT = 0x07;

/* The tree itself, the order of a node's children, and the tombstones an undo
 * leaves. The three carry the layers: 0x01 says which nodes exist, 0x04 links
 * a child group into its parent's list, and 0x08 is a position-holding
 * tombstone like an erased stroke's. */
constexpr uint8_t BLOCK_SCENE_TREE = 0x01;
constexpr uint8_t BLOCK_SCENE_GROUP_ITEM = 0x04;
constexpr uint8_t BLOCK_UNDO_TOMBSTONE = 0x08;
constexpr uint8_t VALUE_GROUP_ITEM = 0x02;

/* The attachments: 0x0E maps a 16-byte key to a file name in the document's
 * index, 0x0F places the image on the page by four vertices. */
constexpr uint8_t BLOCK_ATTACHMENT_MAP = 0x0E;
constexpr uint8_t BLOCK_IMAGE_ITEM = 0x0F;
constexpr uint8_t VALUE_IMAGE_ITEM = 0x07;
constexpr uint8_t BLOCK_SCENE_INFO = 0x0D;

/* The service blocks every page opens with, in the order the tablet writes
 * them: the author table, the migration record, the page counters. All three
 * are known and hold nothing that is drawn, so stepping over them by length
 * loses no content and must not be counted as loss — counted, they would put
 * three on every page and a hundred-page notebook would report three hundred
 * phantom losses. */
constexpr uint8_t BLOCK_AUTHOR_IDS = 0x09;
constexpr uint8_t BLOCK_MIGRATION_INFO = 0x00;
constexpr uint8_t BLOCK_PAGE_INFO = 0x0A;

/* The scene info fields: whether the template and the imported document are
 * shown, and the sheet size. */
constexpr uint32_t TAG_BACKGROUND_VISIBLE = 2;
constexpr uint32_t TAG_ROOT_DOCUMENT_VISIBLE = 3;
constexpr uint32_t TAG_PAPER_SIZE = 5;

/* The same sheet as tag 5, unrounded — two float64. */
constexpr uint32_t TAG_CANVAS_SIZE = 7;

/* Field indexes of a tree node: the visibility, the anchor, and the
 * horizontal the anchored handwriting counts its x from. */
constexpr uint32_t TAG_VISIBILITY = 3;
constexpr uint32_t TAG_ANCHOR = 7;
constexpr uint32_t TAG_ANCHOR_ORIGIN = 10;

/* Fields of the root text block. */
constexpr uint32_t TAG_TEXT_ITEMS = 2;
constexpr uint32_t TAG_TEXT_POSITION = 3;

/* The width of the text column, declared outright. `byte4` by its tag and a
 * **float32** by its contents: 0x44900000 is 1152.0, and read as a uint32 the
 * same bytes are a billion and change. */
constexpr uint32_t TAG_COLUMN_WIDTH = 4;

/* The text's settings, whose `tag 1` is the same width once more, stamped —
 * the one that takes precedence. */
constexpr uint32_t TAG_TEXT_SETTINGS = 5;

/* The field a text record carries when it is not text at all but a marker: the
 * start or the end of a bold or an italic stretch or of a link, or an image in
 * the flow. */
constexpr uint32_t TAG_MARKER = 2;

/* The marker codes of a text record that are not faces. */
constexpr uint32_t MARK_LINK_START = 5;
constexpr uint32_t MARK_LINK_END = 6;
constexpr uint32_t MARK_INLINE_IMAGE = 0xFFFF'FFFFu;

/* What stands in the text for an image in its flow — the character the
 * tablet's own layout puts there, U+FFFC, in UTF-8. */
static const char OBJECT_REPLACEMENT[] = "\xEF\xBF\xBC";

/* The identifiers the format reserves for "before the text" and "after the
 * text". */
constexpr uint64_t ANCHOR_START_SENTINEL = 0xFFFF'FFFF'FFFEu;
constexpr uint64_t ANCHOR_END_SENTINEL = 0xFFFF'FFFF'FFFFu;

/* ------------------------------------------------------------------ */
/* What one pass gathers                                               */
/* ------------------------------------------------------------------ */

typedef enum : int {
  ITEM_TOMBSTONE,
  ITEM_STROKE,
  ITEM_GROUP,
  ITEM_IMAGE
} item_kind;

/* One item of a node's list of children — the unit the scene is ordered by. A
 * stroke, a reference to a child group, or a tombstone: the tombstones must
 * not be dropped while ordering, because a later record may hang its `left`
 * link on one. */
typedef struct scene_item {
  rm_crdt_id parent;
  rm_crdt_id id;
  rm_crdt_id left;
  /* How many records this one deletes — a counter, not a flag. A living
   * record carries nought and a value; a tombstone carries the count and no
   * value. */
  uint32_t deleted;
  item_kind kind;
  rm_stroke stroke;
  rm_crdt_id group;
  /* The opaque key of an image's attachment, pointing into the file's own
   * bytes, and the four vertices. */
  const uint8_t* image_key;
  rm_image_vertex vertices[4];
} scene_item;

/* One record of the scene tree (0x01): a node entering or leaving. Applied in
 * file order, these are the one authority on which layers exist — 0x02 and
 * 0x04 both survive a layer's deletion. */
typedef struct tree_event {
  rm_crdt_id node;
  bool in_tree;
  /* Where the record puts the node (tag 4) — read only for a node going in;
   * `has_parent` false when the record names no parent. */
  bool has_parent;
  rm_crdt_id parent;
} tree_event;

/* What a node hangs on in the typed text: the character the file names, and
 * the horizontal it carries alongside. */
typedef struct node_anchor {
  rm_crdt_id anchor;
  bool has_origin_x;
  double origin_x;
} node_anchor;

typedef struct attachment {
  uint8_t key[16];
  rm_str name;
} attachment;

/* One item of text: its own identifier, its neighbours in the CRDT list and
 * the characters themselves. The characters get identifiers running on from
 * the item's identifier. */
typedef struct text_item {
  rm_crdt_id id;
  rm_crdt_id left;
  rm_crdt_id right;
  rm_str text;
  /* How many characters this record deletes. Deleted characters keep their
   * identifiers, so the next record's `left` counts past them. */
  uint32_t deleted;
  /* The code a record carries when it is not text but a marker — any value
   * but nought; absent for text. 1 bold on, 2 bold off, 3 italic on, 4
   * italic off: odd switches on and even switches off — but not one pair for
   * every purpose, or italic would use the same two numbers as bold. 5 and 6
   * open and close a link, and the opening record's string is the address.
   * 0xFFFFFFFF is an image in the flow of the text, and its string is the
   * `{UUID}` key of the image's row in block 0x0E.
   *
   * Whatever its string, a marker takes exactly one identifier: the address
   * of a link adds no characters to the text. */
  bool has_marker;
  uint32_t marker;
} text_item;

/* How many identifiers a record covers: the characters it carries, the ones it
 * deleted, or the single one of a marker. Counted in Unicode scalars, the way
 * the identifiers run. */
static uint64_t text_item_span(const text_item* item) {
  if (item->has_marker) return 1;
  return item->text.length == 0 ? (uint64_t)item->deleted
                                : (uint64_t)rm_utf8_scalar_count(
                                      item->text.bytes, item->text.length);
}

typedef struct style_entry {
  rm_crdt_id key;
  rm_paragraph_format format;
} style_entry;

/* A line under construction: its bytes, and the face of each of its scalars.
 *
 * The faces are kept per scalar rather than as ranges while reading, because
 * bold and italic switch independently and a range would have to be broken
 * and begun again at each of the four marks. */
typedef struct line_build {
  rm_buf text;
  rm_vec faces; /* uint8_t: bit 0 bold, bit 1 italic */
} line_build;

constexpr uint8_t FACE_BOLD = 1u;
constexpr uint8_t FACE_ITALIC = 2u;

/* The layout of typed text: where it starts and which line each character
 * stands on. */
typedef struct text_layout {
  double origin_x, origin_y;
  /* Where each character stands: its paragraph and how many characters of
   * that paragraph come before the pen once it is typed. It is what locates
   * anchored handwriting. Packed as paragraph << 32 | offset. */
  rm_idmap place_of_character;
  rm_vec lines;  /* line_build */
  rm_vec styles; /* style_entry, in file order */
  bool has_column_width;
  double column_width;
} text_layout;

typedef struct parse_ctx {
  rm_arena* arena;
  rm_reader reader;

  rm_vec items;       /* scene_item */
  rm_vec tree_events; /* tree_event */
  rm_vec attachments; /* attachment */
  /* key -> index into `attachments`. A scan per entry would be quadratic in
   * the number of attachments, and the number is the file's to choose: a
   * page of nothing but map entries would take seconds to read. */
  rm_keymap attachment_index;
  rm_vec node_anchors;     /* node_anchor */
  rm_idmap anchor_of_node; /* node id -> index into node_anchors */
  /* The nodes the user hid. Their strokes stay in the file with every one
   * of their points; it is the layer that is hidden, not its contents, and
   * a hidden layer is not part of the page as drawn. Drawing them shows the
   * user what they put away — the worst kind of discrepancy, because nothing
   * says it happened. */
  rm_idmap hidden_nodes;
  rm_idmap visibility_of_node; /* node id -> index into visibility_stamps */
  rm_vec visibility_stamps; /* rm_crdt_id, including currently visible nodes */
  rm_vec highlights;        /* rm_highlight */

  bool has_text;
  text_layout text;

  /* Two reasons for skipping, not one. Mixed into a single counter they
   * hide the loss: nine hundred unknown blocks look ordinary, while two
   * thousand unread strokes in the same total do not. */
  size_t other_blocks;
  size_t unreadable_strokes;
  size_t deleted_items;
  char first_failure[160];
  /* What the block parser that failed last had to say, when it said more
   * than its status. Taken into `first_failure` with the status and cleared
   * for the next block. */
  char detail[120];

  bool has_paper_size;
  rm_size paper_size;
  /* Shown unless the file says otherwise: most pages carry no flag at all,
   * and a missing field means "never touched", not "hidden". */
  bool background_visible;
  bool root_document_visible;
} parse_ctx;

/* The block's failure, kept for the log when it is the first. The detail the
 * block parser left behind is taken along and cleared, so the next block
 * starts without one. */
static void ctx_note_failure(parse_ctx* ctx, rm_status status) {
  if (ctx->first_failure[0] == '\0') {
    if (ctx->detail[0] != '\0') {
      snprintf(ctx->first_failure, sizeof ctx->first_failure, "%s (%s)",
               rm_status_string(status), ctx->detail);
    } else {
      snprintf(ctx->first_failure, sizeof ctx->first_failure, "%s",
               rm_status_string(status));
    }
  }
  ctx->detail[0] = '\0';
}

/* A refusal with its reason: the reason is what the log line says, and "a
 * block would not read" alone sends whoever reads it to the bytes. */
static rm_status malformed(parse_ctx* ctx, const char* format, ...)
    RM_PRINTF(2, 3);

static rm_status malformed(parse_ctx* ctx, const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(ctx->detail, sizeof ctx->detail, format, arguments);
  va_end(arguments);
  return RM_ERR_MALFORMED_PAGE;
}

/* ------------------------------------------------------------------ */
/* The envelope every scene item starts with                           */
/* ------------------------------------------------------------------ */

rm_status rm_parse_item_preamble(rm_reader* reader, rm_item_preamble* out) {
  RM_TRY(rm_read_crdt(reader, 1, &out->parent));
  RM_TRY(rm_read_crdt(reader, 2, &out->id));
  RM_TRY(rm_read_crdt(reader, 3, &out->left));
  RM_TRY(rm_skip_crdt(reader, 4)); /* right */
  RM_TRY(rm_expect_tag(reader, 5, RM_TAG_BYTE4));
  return rm_read_u32(reader, &out->deleted);
}

/* The preamble alone, as a tombstone: what a value-less or already-deleted
 * record becomes. */
static scene_item item_tombstone(const rm_item_preamble* preamble) {
  return (scene_item){
      .parent = preamble->parent,
      .id = preamble->id,
      .left = preamble->left,
      .deleted = preamble->deleted,
      .kind = ITEM_TOMBSTONE,
  };
}

/* ------------------------------------------------------------------ */
/* Strokes                                                             */
/* ------------------------------------------------------------------ */

/* How many bytes one point takes at this version: 0x18 for version one's
 * five float32, 0x0E for version two's packed layout. */
static rm_status point_size(uint8_t version, size_t* out) {
  switch (version) {
    case 1:
      *out = 0x18;
      return RM_OK;
    case 2:
      *out = 0x0E;
      return RM_OK;
    default:
      return RM_ERR_UNSUPPORTED_FORMAT;
  }
}

/* After the coordinates, version one stores speed, direction, width and
 * pressure, each a float32; version two stores speed and width as uint16 and
 * direction and pressure as bytes, in the order speed, width, direction,
 * pressure. */
static rm_status parse_point(rm_reader* reader, uint8_t version,
                             rm_stroke_point* out) {
  float x, y;
  RM_TRY(rm_read_f32(reader, &x));
  RM_TRY(rm_read_f32(reader, &y));
  /* A coordinate that is not a number is not a coordinate. It travels
   * through every geometry type without a complaint of any kind and comes
   * out in the sheet's own size, where a NaN width is a PDF no reader
   * opens; worse, whether it is noticed at all depends on where in the
   * stroke it sits, because `min` and `max` keep the argument they are
   * given first. Refused here, at the one door every point comes through.
   * The stroke is lost whole rather than mended: joining the neighbours of
   * a dropped point draws a line the user never made, and the caller
   * already counts an unreadable stroke as the loss it is. */
  if (!isfinite(x) || !isfinite(y)) return RM_ERR_MALFORMED_PAGE;

  if (version == 1) {
    float speed, ignored, width, pressure;
    RM_TRY(rm_read_f32(reader, &speed));
    RM_TRY(rm_read_f32(reader, &ignored)); /* direction */
    RM_TRY(rm_read_f32(reader, &width));
    RM_TRY(rm_read_f32(reader, &pressure));
    /* The same door for the width and the pressure: a NaN here is a NaN
     * pen, and it travels as far as a NaN coordinate does. Checked after
     * the scaling, because a finite float near the top overflows by it. */
    float scaled_width = width * 4.0f;
    float scaled_pressure = pressure * 255.0f;
    if (!isfinite(scaled_width) || !isfinite(scaled_pressure))
      return RM_ERR_MALFORMED_PAGE;
    /* Speed is scaled as the width is, by analogy: the scale of a
     * version-one speed is not known. Not a door: nothing is drawn by a
     * speed, so the stroke is not lost for one that is not a number — it
     * reads as nought instead, because a NaN or an infinity here reaches
     * `rmdump` as a number no JSON reader takes. Checked after the
     * scaling, because a finite float near the top overflows by it. */
    float scaled_speed = speed * 4.0f;
    *out = (rm_stroke_point){
        .x = x,
        .y = y,
        .width = scaled_width,
        .pressure = scaled_pressure,
        .speed = isfinite(scaled_speed) ? scaled_speed : 0.0f};
    return RM_OK;
  }

  uint16_t speed, width;
  uint8_t direction, pressure;
  RM_TRY(rm_read_u16(reader, &speed));
  RM_TRY(rm_read_u16(reader, &width));
  RM_TRY(rm_read_u8(reader, &direction));
  RM_TRY(rm_read_u8(reader, &pressure));
  *out = (rm_stroke_point){.x = x,
                           .y = y,
                           .width = (float)width,
                           .pressure = (float)pressure,
                           .speed = (float)speed};
  return RM_OK;
}

/* The tool the file's raw number names, or RM_TOOL_UNKNOWN for a number
 * this library has not seen. */
static rm_pen_tool tool_of(uint32_t raw) {
  switch (raw) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 12:
    case 13:
    case 14:
    case 15:
    case 16:
    case 17:
    case 18:
    case 21:
    case 23:
      return (rm_pen_tool)raw;
    default:
      return RM_TOOL_UNKNOWN;
  }
}

/* The palette index the file's raw number names, or `fallback` when it is
 * past the end of the palette. */
static rm_pen_color color_of(uint32_t raw, rm_pen_color fallback) {
  return raw <= (uint32_t)RM_COLOR_HIGHLIGHTER_ORANGE ? (rm_pen_color)raw
                                                      : fallback;
}

/* `end` is the end of the block, and the value has to lie inside it. The
 * value declares its own length, and until that declaration is put against
 * the block it is only the file's word: bounding the points by it alone would
 * still leave a stroke free to claim gigabytes. */
static rm_status parse_line_item(parse_ctx* ctx, rm_reader* reader,
                                 uint8_t point_version, size_t end,
                                 scene_item* out) {
  /* The parent is a node of the tree. For handwriting anchored to text it
   * is what shows that its coordinates are counted from the text. */
  rm_item_preamble preamble;
  RM_TRY(rm_parse_item_preamble(reader, &preamble));

  /* No value means the item was deleted, and that is an everyday matter: on
   * a long-edited page there is more erased than left. It must not be
   * treated as an error, otherwise the erased looks lost. The tombstone is
   * kept all the same — it holds its place in the chain, and a later record
   * may hang on it. */
  if (!rm_match_tag(reader, 6, RM_TAG_LENGTH4)) {
    *out = item_tombstone(&preamble);
    return RM_OK;
  }
  uint32_t value_length;
  RM_TRY(rm_read_u32(reader, &value_length));
  size_t value_end;
  if (!rm_fits_within(reader, value_length, end, &value_end))
    return RM_ERR_MALFORMED_PAGE;
  reader->size = value_end;

  uint8_t item_type;
  RM_TRY(rm_read_u8(reader, &item_type));
  if (item_type != VALUE_LINE_ITEM) return RM_ERR_MALFORMED_PAGE;

  uint32_t tool_id, color_id;
  double thickness_scale;
  float ignored_f32;
  RM_TRY(rm_expect_tag(reader, 1, RM_TAG_BYTE4));
  RM_TRY(rm_read_u32(reader, &tool_id));
  RM_TRY(rm_expect_tag(reader, 2, RM_TAG_BYTE4));
  RM_TRY(rm_read_u32(reader, &color_id));
  RM_TRY(rm_expect_tag(reader, 3, RM_TAG_BYTE8));
  RM_TRY(rm_read_f64(reader, &thickness_scale));
  /* Refused with the stroke, as a coordinate is: the setting travels with
   * the stroke to whatever draws it, and a NaN here reaches `rmdump` as
   * `thickness: nan`, which no JSON reader takes. */
  if (!isfinite(thickness_scale)) return RM_ERR_MALFORMED_PAGE;
  RM_TRY(rm_expect_tag(reader, 4, RM_TAG_BYTE4));
  RM_TRY(rm_read_f32(reader, &ignored_f32)); /* starting length */

  if (!rm_match_tag(reader, 5, RM_TAG_LENGTH4)) return RM_ERR_MALFORMED_PAGE;
  uint32_t points_length;
  RM_TRY(rm_read_u32(reader, &points_length));
  size_t size;
  RM_TRY(point_size(point_version, &size));
  if (points_length % size != 0) return RM_ERR_MALFORMED_PAGE;
  /* The points have to lie inside the value they are declared in. The
   * declaration alone is the file's word, and a uint32 of them is three
   * hundred million points — five gigabytes reserved on the word of a
   * sixty-byte file. Refused rather than clamped: a stroke whose points do not
   * fit is a broken stroke, and the caller counts it as one. */
  if (points_length > rm_reader_remaining(reader)) return RM_ERR_MALFORMED_PAGE;

  size_t count = points_length / size;
  rm_stroke_point* points = nullptr;
  if (count != 0) {
    points = rm_arena_alloc(ctx->arena, count * sizeof *points);
    if (points == nullptr) return RM_ERR_OUT_OF_MEMORY;
    for (size_t i = 0; i < count; i += 1) {
      RM_TRY(parse_point(reader, point_version, &points[i]));
    }
  }

  /* The tail of the value: a timestamp, a move identifier and — on some
   * pages — an explicit colour. It must not be skipped: where a stroke's
   * colour is not one the palette names, this field is the only record of
   * it. */
  (void)rm_skip_crdt(reader, 6); /* timestamp */
  (void)rm_skip_crdt(reader, 7); /* move identifier */
  bool has_argb = false;
  uint32_t argb = 0;
  if (rm_match_tag(reader, 8, RM_TAG_BYTE4)) {
    has_argb = rm_read_u32(reader, &argb) == RM_OK;
  }
  /* The stroke an eraser cut this one from — on a shader only, and what
   * tells the pieces of one stroke from strokes that merely cross. The
   * reader is bounded by the value, so an identifier cut short is left
   * unread rather than taken from the next block. */
  bool has_source = false;
  rm_crdt_id source = RM_CRDT_NONE;
  size_t before_source = reader->offset;
  if (rm_match_tag(reader, 9, RM_TAG_ID)) {
    has_source = rm_read_bare_crdt(reader, &source) == RM_OK;
    if (!has_source) reader->offset = before_source;
  }

  /* The rest of the tail is of no use to us — we jump to the end of the
   * subblock. */
  RM_TRY(rm_read_seek(reader, value_end));

  *out = (scene_item){
      .parent = preamble.parent,
      .id = preamble.id,
      .left = preamble.left,
      .deleted = preamble.deleted,
      .kind = ITEM_STROKE,
      .stroke =
          {
              .tool = tool_of(tool_id),
              .color = color_of(color_id, RM_COLOR_BLACK),
              .thickness_scale = thickness_scale,
              .points = points,
              .point_count = count,
              .has_argb = has_argb,
              .argb = argb,
              .id = rm_item_id_of(preamble.id),
              .has_source_id = has_source,
              .source_id = rm_item_id_of(source),
          },
  };
  return RM_OK;
}

/* ------------------------------------------------------------------ */
/* The tree, the groups and the tombstones                             */
/* ------------------------------------------------------------------ */

/* A record of the scene tree. The record that creates a node names it in tag 1
 * with tag 2 at nought; every later record is a fresh one whose tag 2 names
 * the node it moves — out of the tree (`in_tree = 0`, a real removal: the
 * device's five-layer limit counts by these) or back in. The tablet does the
 * second when typed text is moved: the handwriting anchored to it is taken
 * out and put back, and a put-back read by its record's identifier would
 * leave the node removed. A record that puts a node in names its parent in
 * tag 4, and that parent can be another than the one before.
 *
 * Everything is read inside the block — `reader` ends where the block does:
 * a parent that does not fit is a block that does not read, not a parent
 * taken from the next block's bytes. */
static rm_status parse_scene_tree(rm_reader* reader, tree_event* out) {
  rm_crdt_id record, target;
  uint8_t flag;
  RM_TRY(rm_read_crdt(reader, 1, &record));
  RM_TRY(rm_read_crdt(reader, 2, &target));
  RM_TRY(rm_expect_tag(reader, 3, RM_TAG_BYTE1));
  RM_TRY(rm_read_u8(reader, &flag));
  bool in_tree = flag != 0;
  bool names = !rm_crdt_equal(target, RM_CRDT_NONE);
  *out = (tree_event){.node = names ? target : record, .in_tree = in_tree};
  if (in_tree && rm_match_tag(reader, 4, RM_TAG_LENGTH4)) {
    rm_reader place;
    RM_TRY(rm_read_subblock(reader, &place));
    RM_TRY(rm_read_crdt(&place, 1, &out->parent));
    out->has_parent = true;
  }
  return RM_OK;
}

/* A group's place in its parent's list (0x04): the same preamble as a stroke,
 * and a value naming the child node. A tombstone names nothing — the
 * reference is thrown out with the deletion.
 *
 * A value that is there but not of that shape is refused rather than answered
 * with a tombstone: a tombstone is what the scene walk reads as "the user
 * deleted this", and a link the file still carries was not deleted, it was
 * lost — along with the place of everything the group held. */
static rm_status parse_group_item(parse_ctx* ctx, rm_reader* reader,
                                  scene_item* out) {
  rm_item_preamble preamble;
  RM_TRY(rm_parse_item_preamble(reader, &preamble));
  *out = item_tombstone(&preamble);
  if (rm_match_tag(reader, 6, RM_TAG_LENGTH4)) {
    rm_reader value;
    uint8_t value_type;
    RM_TRY(rm_read_subblock(reader, &value));
    RM_TRY(rm_read_u8(&value, &value_type));
    if (value_type != VALUE_GROUP_ITEM) {
      return malformed(ctx, "a group item's value is of type %u, not a group",
                       (unsigned)value_type);
    }
    rm_crdt_id node;
    if (rm_read_crdt(&value, 2, &node) != RM_OK) {
      return malformed(ctx, "a group item names no node");
    }
    out->kind = ITEM_GROUP;
    out->group = node;
  }
  return RM_OK;
}

/* The tombstone an undo leaves (0x08): the preamble alone, no value. Read for
 * its position in the chain, nothing else. */
static rm_status parse_scene_tombstone(rm_reader* reader, scene_item* out) {
  rm_item_preamble preamble;
  RM_TRY(rm_parse_item_preamble(reader, &preamble));
  *out = item_tombstone(&preamble);
  return RM_OK;
}

/* A scene tree node: its own identifier, whether it is shown, the sign of an
 * anchor into the text and the anchor's own horizontal.
 *
 * The horizontal is the file's ready answer — `anchor_origin_x`, tag 10 — and
 * it cannot always be recomputed: on the boundary of two writing directions
 * the caret is ambiguous. The remaining fields (the layer's name, the anchor
 * type and threshold) are skipped by length. */
typedef struct tree_node {
  rm_crdt_id id;
  bool has_anchor;
  rm_crdt_id anchor;
  bool has_origin_x;
  double origin_x;
  bool has_visible;
  bool visible;
  rm_crdt_id visibility_stamp;
} tree_node;

/* One scene tree node's own identifier, anchor and visibility, read field by
 * field so that a field of an unknown shape ends the node rather than the
 * block. */
static rm_status parse_tree_node(rm_reader* reader, size_t end,
                                 tree_node* out) {
  *out = (tree_node){0};
  RM_TRY(rm_read_crdt(reader, 1, &out->id));

  while (reader->offset < end) {
    size_t saved = reader->offset;
    uint32_t index;
    uint8_t type;
    if (rm_read_tag(reader, &index, &type) != RM_OK) {
      reader->offset = saved;
      return RM_OK;
    }
    switch (type) {
      case RM_TAG_LENGTH4: {
        uint32_t length;
        RM_TRY(rm_read_u32(reader, &length));
        size_t sub_end;
        /* A field that runs past the block ends the node where it
         * stands: what was read before it is still the node's. */
        if (!rm_fits_within(reader, length, end, &sub_end)) return RM_OK;
        rm_reader field = *reader;
        field.size = sub_end;
        reader->offset = sub_end;
        rm_reader* outer = reader;
        reader = &field;
        if (index == TAG_ANCHOR) {
          /* There are two identifiers here. The first is unique to
           * the node, while the one pointing at a character of the
           * text is the second: nodes share characters, so the
           * second repeats where the first, one per node, never
           * does. */
          rm_crdt_id ignored;
          (void)rm_read_crdt(reader, 1, &ignored);
          rm_crdt_id anchor;
          if (rm_read_crdt(reader, 2, &anchor) == RM_OK) {
            out->has_anchor = true;
            out->anchor = anchor;
          }
        } else if (index == TAG_ANCHOR_ORIGIN) {
          /* The stamped spelling: a last-write-wins record — the
           * timestamp, then the value. This is what the tablet
           * writes, for this field and for the three beside it; the
           * format also allows the bare spelling read below. A
           * field lost to the difference would mean a silently
           * recomputed horizontal. */
          rm_crdt_id ignored;
          (void)rm_read_crdt(reader, 1, &ignored);
          float value;
          /* Left unset when it is not a place, the way the text
           * origin is: the horizontal is optional, and a renderer
           * without it recomputes — a renderer with a NaN draws
           * the handwriting nowhere. */
          if (rm_match_tag(reader, 2, RM_TAG_BYTE4) &&
              rm_read_f32(reader, &value) == RM_OK && isfinite(value)) {
            out->has_origin_x = true;
            out->origin_x = (double)value;
          }
        } else if (index == TAG_VISIBILITY) {
          /* The timestamp of the last write and then the flag. The
           * timestamp is kept: of two edits of one node's visibility,
           * the newer wins. A layer never hidden or shown carries
           * (0, 0); every hide and every show is an edit with a real
           * identifier. */
          if (rm_read_crdt(reader, 1, &out->visibility_stamp) != RM_OK) {
            out->visibility_stamp = rm_crdt_make(0, 0);
          }
          if (rm_match_tag(reader, 2, RM_TAG_BYTE1)) {
            /* A flag cut short is no "hidden": only a nought
             * read hides the layer. */
            uint8_t flag;
            out->has_visible = true;
            out->visible = rm_read_u8(reader, &flag) != RM_OK || flag != 0;
          }
        }
        reader = outer;
        break;
      }
      case RM_TAG_BYTE4: {
        if (index == TAG_ANCHOR_ORIGIN) {
          float value;
          RM_TRY(rm_read_f32(reader, &value));
          if (isfinite(value)) {
            out->has_origin_x = true;
            out->origin_x = (double)value;
          }
        } else {
          uint32_t ignored;
          RM_TRY(rm_read_u32(reader, &ignored));
        }
        break;
      }
      case RM_TAG_BYTE8: {
        double ignored;
        RM_TRY(rm_read_f64(reader, &ignored));
        break;
      }
      case RM_TAG_BYTE1: {
        uint8_t ignored;
        RM_TRY(rm_read_u8(reader, &ignored));
        break;
      }
      case RM_TAG_ID: {
        uint8_t author;
        uint64_t counter;
        RM_TRY(rm_read_u8(reader, &author));
        RM_TRY(rm_read_varuint(reader, &counter));
        break;
      }
      default:
        /* An unknown type — going further into the block blind is not
         * an option. */
        return RM_OK;
    }
  }
  return RM_OK;
}

/* ------------------------------------------------------------------ */
/* Scene info                                                          */
/* ------------------------------------------------------------------ */

/* The scene info block: the sheet size, and whether the template and the
 * imported document are shown.
 *
 * The size's only reliable source is here: `.content` declares
 * `customZoomPageWidth`/`Height`, but with `zoomMode: bestFit` that is the
 * state of a manual zoom left over from an earlier device rather than the page
 * size. A notebook written on a Paper Pro holds the legacy values 1404 × 1872
 * there, while the real sheet is 1620 × 2160.
 *
 * Inside the block every field comes as `length4`: a tag, a four-byte length,
 * a value. On meeting a field of another kind we stop parsing: the format is
 * unofficial, and guessing the length of an unfamiliar value is worse than
 * answering "unknown". This function does not fail — it reads the fields it
 * knows and leaves the rest at their defaults, so no outcome loses content. */
static void parse_scene_info(parse_ctx* ctx, rm_reader* reader, size_t end) {
  bool has_paper = false, has_canvas = false;
  rm_size paper = {0}, canvas = {0};

  while (reader->offset < end) {
    uint32_t index;
    uint8_t type;
    if (rm_read_tag(reader, &index, &type) != RM_OK) break;
    if (type != RM_TAG_LENGTH4) break;
    rm_reader field;
    if (rm_read_subblock(reader, &field) != RM_OK) break;
    size_t length = rm_reader_remaining(&field);
    rm_reader* outer = reader;
    reader = &field;

    if (index == TAG_PAPER_SIZE && length >= 8) {
      uint32_t width, height;
      if (rm_read_u32(reader, &width) == RM_OK &&
          rm_read_u32(reader, &height) == RM_OK && width > 0 && height > 0) {
        paper = (rm_size){.width = (double)width, .height = (double)height};
        has_paper = true;
      }
    } else if (index == TAG_CANVAS_SIZE && length >= 16) {
      /* The same sheet unrounded. Tag 7 is read and never tag 8, which
       * some readers take for the sheet: tag 7 is the sheet the page is
       * drawn on. A Move's canvas is 820.295 × 1458.303 where tag 5
       * says 820 × 1458, and the page comes out at 260 × 463 points —
       * the canvas times the unit, rounded to whole points, and not the
       * panel's 462.4. Read after the whole one so it wins when both
       * are there, and a page carrying only tag 5 still has a sheet.
       *
       * `isfinite` and not only `> 0`: a NaN fails the comparison on its
       * own, but +Inf passes it and becomes a sheet of infinite size —
       * every page of the book rendered into a MediaBox no reader will
       * open. */
      double width, height;
      if (rm_read_f64(reader, &width) == RM_OK &&
          rm_read_f64(reader, &height) == RM_OK && isfinite(width) &&
          isfinite(height) && width > 0 && height > 0) {
        canvas = (rm_size){.width = width, .height = height};
        has_canvas = true;
      }
    } else if (index == TAG_BACKGROUND_VISIBLE ||
               index == TAG_ROOT_DOCUMENT_VISIBLE) {
      /* The timestamp of the last write, then the flag — the same shape
       * as a layer's visibility in the tree node. */
      rm_crdt_id ignored;
      (void)rm_read_crdt(reader, 1, &ignored);
      uint8_t flag;
      if (rm_match_tag(reader, 2, RM_TAG_BYTE1) &&
          rm_read_u8(reader, &flag) == RM_OK) {
        if (index == TAG_BACKGROUND_VISIBLE)
          ctx->background_visible = flag != 0;
        else
          ctx->root_document_visible = flag != 0;
      }
    }

    reader = outer;
  }

  if (has_canvas) {
    ctx->paper_size = canvas;
    ctx->has_paper_size = true;
  } else if (has_paper) {
    ctx->paper_size = paper;
    ctx->has_paper_size = true;
  }
}

/* ------------------------------------------------------------------ */
/* Highlights                                                          */
/* ------------------------------------------------------------------ */

/* The marker over the text of an imported PDF.
 *
 * The same item preamble as a stroke — parent, identifier, neighbours, the
 * length of what was deleted — and then a value of its own: the place in the
 * text layer, the highlighted text, the colour, and the rectangles: tag 2
 * `start`, 3 `length`, 4 the colour, 5 the text, 6 the rectangles, 10 an
 * explicit colour.
 *
 * The rectangles and the colour are what drawing needs; the text is read as
 * well, because it is the only record of *what* was highlighted short of
 * reading the PDF back — and the page under the band may be a scan with no
 * text layer to read.
 *
 * `end` is the end of the block. It is not decoration: the rectangles come
 * with a count of their own, and a count is a declaration rather than a
 * promise — reserving on it hands a sixty-byte blob the keys to gigabytes. */
/* Step over an optional field of a value's tail when it is there and ends
 * inside the value; otherwise leave the reader where it stood. The reader is
 * bounded by the value, and the bound is the field's end, not its tag's: an
 * identifier is a byte and a varuint, and a tail cut short in the middle of
 * one must not carry the reader into the next block. */
static void skip_optional_tail(rm_reader* reader, uint32_t index,
                               uint8_t type) {
  size_t saved = reader->offset;
  if (!rm_match_tag(reader, index, type)) return;
  bool skipped = false;
  if (type == RM_TAG_ID) {
    rm_crdt_id ignored;
    skipped = rm_read_bare_crdt(reader, &ignored) == RM_OK;
  } else if (type == RM_TAG_BYTE1) {
    uint8_t ignored;
    skipped = rm_read_u8(reader, &ignored) == RM_OK;
  }
  if (!skipped) reader->offset = saved;
}

/* One highlight record (0x03): the preamble, then its value — the fuller
 * contract is documented above `skip_optional_tail`. */
static rm_status parse_glyph_item(parse_ctx* ctx, rm_reader* reader, size_t end,
                                  bool* out_found, rm_highlight* out) {
  *out_found = false;
  rm_item_preamble preamble;
  RM_TRY(rm_parse_item_preamble(reader, &preamble));

  /* No value means the highlight was removed — as ordinary here as an erased
   * stroke, and answered "not found" rather than refused for that reason.
   * Only a real tombstone is that: a deleted length, and the block ending
   * right behind it. Every other way of coming back without a highlight is
   * a refusal — a highlight that is there and will not read is a loss, and
   * must be counted as one. */
  if (!rm_match_tag(reader, 6, RM_TAG_LENGTH4)) {
    if (preamble.deleted == 0 || reader->offset != end) {
      return malformed(ctx, "a highlight has neither a value nor a deletion");
    }
    return RM_OK;
  }
  /* A deleted length removes the highlight whatever follows it, as it
   * removes a stroke from the scene: the editor and the scene walk read it
   * so, and a highlight the page shows has to be one the editor can
   * erase. */
  if (preamble.deleted != 0) return RM_OK;
  /* The value declares its length, and it is held to the block: what
   * follows is read inside the value and nowhere else. Its type is not
   * checked — the tablet writes 0x01, but a highlight refused on a type
   * byte spelt otherwise would vanish from the page for nothing the
   * drawing needs. */
  uint32_t ignored_u32;
  uint8_t ignored_u8;
  rm_reader value;
  if (rm_read_subblock(reader, &value) != RM_OK) {
    return malformed(ctx, "a highlight's value runs past its block");
  }
  reader = &value;
  end = reader->size;
  RM_TRY(rm_read_u8(reader, &ignored_u8)); /* value type */

  /* `start` and `length` are absent on some pages, and their absence is
   * not an error: they place the highlight in the text layer, which we do
   * not need — the rectangles say where to draw. */
  if (rm_match_tag(reader, 2, RM_TAG_BYTE4))
    RM_TRY(rm_read_u32(reader, &ignored_u32));
  if (rm_match_tag(reader, 3, RM_TAG_BYTE4))
    RM_TRY(rm_read_u32(reader, &ignored_u32));

  uint32_t color_id;
  RM_TRY(rm_expect_tag(reader, 4, RM_TAG_BYTE4));
  RM_TRY(rm_read_u32(reader, &color_id));

  /* The reader is put at the field's end whatever its length claimed, so a
   * short or padded field cannot shift the rectangles after it. */
  bool has_text = false;
  rm_str text = {.bytes = "", .length = 0};
  if (rm_match_tag(reader, 5, RM_TAG_LENGTH4)) {
    uint32_t text_length;
    RM_TRY(rm_read_u32(reader, &text_length));
    size_t text_end;
    if (!rm_fits_within(reader, text_length, end, &text_end)) text_end = end;
    rm_str decoded;
    RM_TRY(rm_read_string(reader, text_end, ctx->arena, &decoded));
    if (decoded.length != 0) {
      has_text = true;
      text = decoded;
    }
    RM_TRY(rm_read_seek(reader, text_end));
  }

  if (!rm_match_tag(reader, 6, RM_TAG_LENGTH4)) {
    return malformed(ctx, "a highlight has no rectangles");
  }
  rm_reader rectangle_data;
  if (rm_read_subblock(reader, &rectangle_data) != RM_OK) {
    return malformed(ctx, "a highlight's rectangles run past its value");
  }
  size_t count;
  RM_TRY(rm_read_count(&rectangle_data, &count));
  /* Four float64 per rectangle, all inside the rectangle list. Refuse the
   * count before allocating or reading any following field as coordinates. */
  size_t room = rm_reader_remaining(&rectangle_data) / 32u;
  if (count > room) {
    return malformed(
        ctx, "a highlight declares %zu rectangles, more than its field holds",
        count);
  }
  rm_vec gathered;
  rm_vec_init(&gathered, sizeof(rm_rect));
  if (!rm_vec_reserve(&gathered, count)) {
    rm_vec_free(&gathered);
    return RM_ERR_OUT_OF_MEMORY;
  }
  rm_status status = RM_OK;
  for (size_t i = 0; i < count; i += 1) {
    double x, y, width, height;
    if (rm_read_f64(&rectangle_data, &x) != RM_OK ||
        rm_read_f64(&rectangle_data, &y) != RM_OK ||
        rm_read_f64(&rectangle_data, &width) != RM_OK ||
        rm_read_f64(&rectangle_data, &height) != RM_OK) {
      status = RM_ERR_UNEXPECTED_EOF;
      break;
    }
    /* The whole highlight goes, not the one rectangle: the marker is drawn
     * as a run over a line of text, and a run with a hole in it is a
     * highlight of something the user did not highlight. Refused rather
     * than answered empty so that the caller counts it. */
    if (!isfinite(x) || !isfinite(y) || !isfinite(width) || !isfinite(height)) {
      status = RM_ERR_MALFORMED_PAGE;
      break;
    }
    rm_rect rectangle = {.x = x, .y = y, .width = width, .height = height};
    if (!rm_vec_push(&gathered, &rectangle)) {
      status = RM_ERR_OUT_OF_MEMORY;
      break;
    }
  }
  if (status != RM_OK) {
    rm_vec_free(&gathered);
    return status;
  }
  size_t written = gathered.count;
  if (written == 0) {
    rm_vec_free(&gathered);
    return malformed(ctx, "a highlight has no rectangles");
  }
  rm_rect* rectangles =
      rm_arena_alloc(ctx->arena, written * sizeof *rectangles);
  if (rectangles == nullptr) {
    rm_vec_free(&gathered);
    return RM_ERR_OUT_OF_MEMORY;
  }
  memcpy(rectangles, gathered.data, written * sizeof *rectangles);
  rm_vec_free(&gathered);

  /* The second form of a highlight: instead of `start` and `length`, the
   * first and the last character as identifiers, and whether the last one is
   * included (`first`/`last`/`includeLastId`). They stand
   * between the rectangles and the colour, and an unread one hides the
   * colour from the match below. Each is stepped over whole, and only while
   * it lies wholly inside the value. */
  skip_optional_tail(reader, 7, RM_TAG_ID);
  skip_optional_tail(reader, 8, RM_TAG_ID);
  skip_optional_tail(reader, 9, RM_TAG_BYTE1);

  bool has_argb = false;
  uint32_t argb = 0;
  if (rm_match_tag(reader, 10, RM_TAG_BYTE4)) {
    has_argb = rm_read_u32(reader, &argb) == RM_OK;
  }

  *out = (rm_highlight){
      .rectangles = rectangles,
      .rectangle_count = written,
      .color = color_of(color_id, RM_COLOR_YELLOW),
      .has_argb = has_argb,
      .argb = argb,
      .has_text = has_text,
      .text = text,
      .id = rm_item_id_of(preamble.id),
  };
  *out_found = true;
  return RM_OK;
}

/* ------------------------------------------------------------------ */
/* Attachments and images                                              */
/* ------------------------------------------------------------------ */

/* One entry of the attachment map: the key and, when it carries one, the
 * name. */
static rm_status parse_attachment_entry(parse_ctx* ctx, rm_reader* row,
                                        rm_vec* out) {
  const uint8_t* key;
  RM_TRY(rm_read_bytes(row, 16, &key));
  if (!rm_match_tag(row, 1, RM_TAG_LENGTH4)) return RM_OK;
  uint32_t ignored;
  RM_TRY(rm_read_u32(row, &ignored));
  if (rm_match_tag(row, 1, RM_TAG_ID)) {
    rm_crdt_id stamp;
    RM_TRY(rm_read_bare_crdt(row, &stamp));
  }
  if (!rm_match_tag(row, 2, RM_TAG_LENGTH4)) return RM_OK;
  rm_reader text;
  RM_TRY(rm_read_subblock(row, &text));
  rm_str name;
  RM_TRY(rm_read_string(&text, text.size, ctx->arena, &name));
  if (name.length == 0) return RM_OK;
  attachment entry = {.name = name};
  memcpy(entry.key, key, 16);
  return rm_vec_push(out, &entry) ? RM_OK : RM_ERR_OUT_OF_MEMORY;
}

/* The attachment map (0x0E): a 16-byte opaque key to a file name in the
 * document's index. One block declares every attachment of the page — it is a
 * list, not a record per asset.
 *
 * A list that runs out early — a count larger than its entries, an entry that
 * is not one, an entry past the block — keeps the names before it and is no
 * loss. An entry that fails inside costs the block's whole map: nothing of it
 * is merged, and the caller counts it. */
static rm_status parse_attachment_map(parse_ctx* ctx, rm_reader* reader) {
  if (!rm_match_tag(reader, 1, RM_TAG_LENGTH4)) return RM_OK;
  uint32_t ignored;
  RM_TRY(rm_read_u32(reader, &ignored));
  size_t count;
  RM_TRY(rm_read_count(reader, &count));

  rm_vec entries;
  rm_vec_init(&entries, sizeof(attachment));
  rm_status status = RM_OK;
  for (size_t i = 0; i < count; i += 1) {
    if (rm_reader_remaining(reader) == 0 ||
        !rm_match_tag(reader, 0, RM_TAG_LENGTH4))
      break;
    uint32_t entry_length;
    status = rm_read_u32(reader, &entry_length);
    if (status != RM_OK) goto done;
    size_t entry_end;
    if (!rm_fits_within(reader, entry_length, reader->size, &entry_end)) break;
    rm_reader row = *reader;
    row.size = entry_end;
    status = parse_attachment_entry(ctx, &row, &entries);
    if (status != RM_OK) goto done;
    reader->offset = entry_end;
  }

  /* Merged only once the block has read: later names win, within a block as
   * across blocks. */
  for (size_t i = 0; i < entries.count; i += 1) {
    const attachment* entry = rm_vec_at(&entries, i);
    uint64_t existing_at;
    if (rm_keymap_get(&ctx->attachment_index, entry->key, &existing_at)) {
      attachment* existing = rm_vec_at(&ctx->attachments, (size_t)existing_at);
      existing->name = entry->name;
    } else if (!rm_vec_push(&ctx->attachments, entry) ||
               !rm_keymap_set(&ctx->attachment_index, entry->key,
                              ctx->attachments.count - 1u)) {
      status = RM_ERR_OUT_OF_MEMORY;
      goto done;
    }
  }

done:
  rm_vec_free(&entries);
  return status;
}

/* An image on the page (0x0F): the same preamble as a stroke, and a value of
 * type 7 — the attachment's key and the four vertices (x, y, u, v) that place,
 * scale, crop or turn it. (The format has a second way, an image standing in
 * the text itself — a record of the root text keyed into block 0x0E with no
 * 0x0F item at all; that one is a character of the text here, not drawn.)
 *
 * Both the value and the vertex list have their own byte boundaries. A
 * placement needs exactly sixteen floats inside the declared list.
 *
 * Once the value is there, a tombstone is not an honest answer: it
 * reads as "the user erased it", and whatever goes wrong past that point was
 * not erased, it was lost. So every shape the value can be wrong in is a
 * refusal, and the caller counts it. */
static rm_status parse_image_item(parse_ctx* ctx, rm_reader* reader,
                                  scene_item* out) {
  rm_item_preamble preamble;
  RM_TRY(rm_parse_item_preamble(reader, &preamble));
  *out = item_tombstone(&preamble);

  if (!rm_match_tag(reader, 6, RM_TAG_LENGTH4)) return RM_OK;
  rm_reader value;
  RM_TRY(rm_read_subblock(reader, &value));
  reader = &value;
  uint8_t value_type;
  RM_TRY(rm_read_u8(reader, &value_type));
  if (value_type != VALUE_IMAGE_ITEM) {
    return malformed(ctx, "an image item's value is of type %u, not an image",
                     (unsigned)value_type);
  }

  const uint8_t* key = nullptr;
  size_t key_length = 0;
  if (rm_match_tag(reader, 1, RM_TAG_LENGTH4)) {
    rm_reader keyed;
    RM_TRY(rm_read_subblock(reader, &keyed));
    if (rm_match_tag(&keyed, 1, RM_TAG_ID)) {
      rm_crdt_id ignored;
      RM_TRY(rm_read_bare_crdt(&keyed, &ignored));
    }
    /* The key sits in a tag-2 wrapper of its own; the bare spelling,
     * which the format also allows, is read as a fallback. */
    rm_reader key_data = keyed;
    if (rm_match_tag(&keyed, 2, RM_TAG_LENGTH4)) {
      RM_TRY(rm_read_subblock(&keyed, &key_data));
    }
    key_length = rm_reader_remaining(&key_data);
    if (key_length >= 16) RM_TRY(rm_read_bytes(&key_data, 16, &key));
  }
  {
    rm_crdt_id ignored;
    (void)rm_read_crdt(reader, 2, &ignored);
  }

  rm_image_vertex vertices[4] = {0};
  size_t floats = 0;
  if (rm_match_tag(reader, 3, RM_TAG_LENGTH4)) {
    rm_reader vertex_data;
    RM_TRY(rm_read_subblock(reader, &vertex_data));
    RM_TRY(rm_read_count(&vertex_data, &floats));
    if (floats > rm_reader_remaining(&vertex_data) / 4u) {
      return malformed(
          ctx, "an image declares %zu vertex floats, more than its list holds",
          floats);
    }
    /* Four vertices place, scale and crop the image, and there is no
     * placing it on three. Whole vertices are counted: one to three
     * floats over the sixteen are no fifth vertex. The count is held to
     * that before a float is read; every float of the list must be a
     * number all the same. */
    if (floats / 4u == 4u) {
      for (size_t i = 0; i < floats; i += 1) {
        float number;
        RM_TRY(rm_read_f32(&vertex_data, &number));
        if (!isfinite(number)) {
          return malformed(ctx, "an image vertex is not a finite one");
        }
        if (i < 16) {
          rm_image_vertex* vertex = &vertices[i / 4u];
          switch (i % 4u) {
            case 0:
              vertex->x = number;
              break;
            case 1:
              vertex->y = number;
              break;
            case 2:
              vertex->u = number;
              break;
            default:
              vertex->v = number;
              break;
          }
        }
      }
    }
  }

  /* The same refusal as the vertex that is not a number, for the same
   * reason: a key of another length names no attachment, and there is no
   * placing a picture on three corners. */
  if (key == nullptr) {
    return malformed(ctx, "an image key is %zu bytes, not 16", key_length);
  }
  if (floats / 4u != 4u) {
    return malformed(ctx, "an image has %zu vertices, not 4", floats / 4u);
  }
  out->kind = ITEM_IMAGE;
  out->image_key = key;
  memcpy(out->vertices, vertices, sizeof vertices);
  return RM_OK;
}

/* ------------------------------------------------------------------ */
/* The typed text                                                      */
/* ------------------------------------------------------------------ */

/* One record of the text, held to its own bytes. */
static rm_status parse_text_row(parse_ctx* ctx, rm_reader* row,
                                text_item* item) {
  *item = (text_item){0};
  RM_TRY(rm_read_crdt(row, 2, &item->id));
  RM_TRY(rm_read_crdt(row, 3, &item->left));
  RM_TRY(rm_read_crdt(row, 4, &item->right));
  RM_TRY(rm_expect_tag(row, 5, RM_TAG_BYTE4));
  RM_TRY(rm_read_u32(row, &item->deleted));
  item->text = (rm_str){.bytes = "", .length = 0};

  if (rm_match_tag(row, 6, RM_TAG_LENGTH4)) {
    /* Held to the record: a value that claims more than its record
     * holds would otherwise read the next record's bytes as its own —
     * and a marker found there would quietly turn this record's text
     * into nothing. Clamped rather than refused, as the string inside
     * it is. */
    uint32_t value_length;
    RM_TRY(rm_read_u32(row, &value_length));
    size_t value_end;
    if (!rm_fits_within(row, value_length, row->size, &value_end))
      value_end = row->size;
    rm_reader value = *row;
    value.size = value_end;
    row->offset = value_end;
    RM_TRY(rm_read_string(&value, value.size, ctx->arena, &item->text));
    /* A record whose value goes on past the string is not text but
     * markup: the value carries a field an ordinary record has not got,
     * and that field says what the record marks. Bold and italic are
     * nowhere in the list of paragraph styles — a paragraph with a bold
     * word in it calls itself plain.
     *
     * Looked for whatever the string: a link's opening record spells
     * its address there, and an image in the flow its key, and read as
     * text either would land on the page as letters. The tablet writes
     * the field only when it is not nought. A code cut short by the
     * value's end is no code, and the record stays text. */
    uint32_t code;
    if (rm_match_tag(&value, TAG_MARKER, RM_TAG_BYTE4) &&
        rm_read_u32(&value, &code) == RM_OK) {
      if (code != 0) {
        item->has_marker = true;
        item->marker = code;
      }
    }
  }
  return RM_OK;
}

/* The text items lie in a nested list: a container, a list, a counter and
 * then one record per item.
 *
 * The reader is left where the list stopped, read through or not: the styles
 * are looked for from there. A list that runs out early — a count larger than
 * its records, a record that is not one, a record past the list — keeps the
 * records before it; a record that fails inside costs the whole list. */
static rm_status parse_text_items(parse_ctx* ctx, rm_reader* reader,
                                  rm_vec* out) {
  if (!rm_match_tag(reader, 1, RM_TAG_LENGTH4)) return RM_OK;
  uint32_t ignored;
  RM_TRY(rm_read_u32(reader, &ignored));
  if (!rm_match_tag(reader, 1, RM_TAG_LENGTH4)) return RM_OK;
  uint32_t list_length;
  RM_TRY(rm_read_u32(reader, &list_length));
  size_t list_end;
  if (!rm_fits_within(reader, list_length, reader->size, &list_end))
    list_end = reader->size;
  size_t count;
  RM_TRY(rm_read_count(reader, &count));

  for (size_t i = 0; i < count; i += 1) {
    if (reader->offset >= list_end || !rm_match_tag(reader, 0, RM_TAG_LENGTH4))
      break;
    uint32_t item_length;
    RM_TRY(rm_read_u32(reader, &item_length));
    size_t item_end;
    if (!rm_fits_within(reader, item_length, list_end, &item_end)) break;

    rm_reader row = *reader;
    row.size = item_end;
    text_item item;
    rm_status status = parse_text_row(ctx, &row, &item);
    if (status != RM_OK) {
      reader->offset = row.offset;
      return status;
    }
    if (!rm_vec_push(out, &item)) return RM_ERR_OUT_OF_MEMORY;
    reader->offset = item_end;
  }
  return RM_OK;
}

/* The value of one paragraph style, held to its own bytes. */
static rm_status parse_style_value(rm_reader* reader, rm_crdt_id key,
                                   rm_vec* out) {
  if (rm_match_tag(reader, 1, RM_TAG_BYTE1)) {
    style_entry entry = {.key = key};
    RM_TRY(rm_read_u8(reader, &entry.format.raw));
    /* The value goes on past the first field, and reading only the
     * first would run two different presentations together: an
     * emphasised paragraph and a subheading both carry 3, and apart they
     * are told only by these. */
    if (rm_match_tag(reader, 2, RM_TAG_BYTE1)) {
      RM_TRY(rm_read_u8(reader, &entry.format.second));
      entry.format.has_second = true;
    }
    if (rm_match_tag(reader, 3, RM_TAG_BYTE4)) {
      RM_TRY(rm_read_u32(reader, &entry.format.third));
      entry.format.has_third = true;
    }
    /* Stored as a pair, not interpreted field by field: telling the
     * known shapes apart is all the page needs. Two are known:
     * (2, 3) on the second heading, and
     * (3, state << 8 | family << 4 | depth) on a list item — families
     * 0…2, depths 0…2, and the state bits 8 (ticked) and 9 (something
     * nested is ticked) that a checkbox item may carry, the first level
     * included. Logged so that the unknown reports itself — a third
     * shape would otherwise arrive in silence. */
    bool further = rm_paragraph_format_has_further_fields(&entry.format);
    bool heading_pair = entry.format.has_second && entry.format.second == 2 &&
                        entry.format.has_third && entry.format.third == 3;
    bool nested_list_pair =
        entry.format.has_second && entry.format.second == 3 &&
        entry.format.has_third && (entry.format.third >> 10) == 0 &&
        ((entry.format.third >> 4) & 0x0Fu) <= 2 &&
        (entry.format.third & 0x0Fu) <= 2;
    if (further && !heading_pair && !nested_list_pair) {
      RM_LOG_DEBUGF("page: paragraph style %u carries an unseen pair (%d, %ld)",
                    (unsigned)entry.format.raw,
                    entry.format.has_second ? (int)entry.format.second : -1,
                    entry.format.has_third ? (long)entry.format.third : -1L);
    }
    if (!rm_vec_push(out, &entry)) {
      return RM_ERR_OUT_OF_MEMORY;
    }
    if (rm_paragraph_format_style(&entry.format) == RM_PARA_UNKNOWN) {
      /* A style we do not know is kept and drawn as plain text, and
       * said out loud, so that a new style value reports itself rather
       * than passing for plain text in silence. */
      RM_LOG_DEBUGF("render: unknown paragraph style %u",
                    (unsigned)entry.format.raw);
    }
  }
  return RM_OK;
}

/* The styles of the paragraphs, keyed by the character each one follows.
 *
 * The list stands right after the text items inside the same block. Its
 * records are short: the key written bare, then the timestamp of the last
 * write, then the style itself in a sub-block. A list that runs out early
 * keeps the styles before it; a style that fails inside costs the list. */
static rm_status parse_paragraph_styles(rm_reader* reader, rm_vec* out) {
  if (!rm_match_tag(reader, 2, RM_TAG_LENGTH4)) return RM_OK;
  uint32_t ignored;
  RM_TRY(rm_read_u32(reader, &ignored));
  if (!rm_match_tag(reader, 1, RM_TAG_LENGTH4)) return RM_OK;
  uint32_t list_length;
  RM_TRY(rm_read_u32(reader, &list_length));
  size_t list_end;
  if (!rm_fits_within(reader, list_length, reader->size, &list_end))
    list_end = reader->size;
  size_t count;
  RM_TRY(rm_read_count(reader, &count));

  for (size_t i = 0; i < count; i += 1) {
    if (reader->offset >= list_end) break;
    rm_crdt_id key;
    RM_TRY(rm_read_bare_crdt(reader, &key));
    /* The timestamp of the last write. Kept only to step over it: with one
     * writer there is nothing to resolve. */
    {
      rm_crdt_id stamp;
      (void)rm_read_crdt(reader, 1, &stamp);
    }
    if (!rm_match_tag(reader, 2, RM_TAG_LENGTH4)) break;
    uint32_t value_length;
    RM_TRY(rm_read_u32(reader, &value_length));
    size_t value_end;
    if (!rm_fits_within(reader, value_length, list_end, &value_end)) break;

    rm_reader value = *reader;
    value.size = value_end;
    rm_status status = parse_style_value(&value, key, out);
    if (status != RM_OK) {
      reader->offset = value.offset;
      return status;
    }
    reader->offset = value_end;
  }
  return RM_OK;
}

/* ------------------------------------------------------------------ */
/* The order the text reads in                                         */
/* ------------------------------------------------------------------ */

typedef struct text_key_ctx {
  const text_item* items;
} text_key_ctx;

/* The grouping key for a text record: the identifier its `left` names. */
static rm_crdt_id text_left_key(size_t index, void* ctx) {
  return ((text_key_ctx*)ctx)->items[index].left;
}

/* The grouping key for a position in a tie: the record's `right`. */
static rm_crdt_id text_right_key(size_t position, void* ctx) {
  /* Keyed over the positions of one tie, so the context carries the bucket
   * as well as the items. */
  struct {
    const text_item* items;
    const uint32_t* bucket;
  }* state = ctx;
  return state->items[state->bucket[position]].right;
}

/* The order in which the records of one tie leave it.
 *
 * Two records with the same left neighbour are an everyday matter in a CRDT.
 * The one that comes first is the one whose own right link points at another
 * record of the same tie — a record inserted between `left` and an existing
 * record carries that record's id as its `right` and stands before it. Where
 * none does, file order decides.
 *
 * That rule reads a record against **what is still left of the tie**, so it
 * looks as if it must be asked afresh at every step. It need not be: taking a
 * record can only shrink the set of identifiers the tie holds, so a record
 * whose right has already fallen outside can never fall back in. Both answers
 * the rule needs — the first record still in the tie, and the first one still
 * pointing inside it — therefore only ever move forward, and two marks
 * walking the tie once give the whole order in time proportional to its size
 * instead of to its square. */
static bool tie_order(const uint32_t* bucket, size_t count,
                      const text_item* items, uint32_t* out) {
  if (count <= 1) {
    if (count == 1) out[0] = bucket[0];
    return true;
  }

  bool ok = false;
  rm_idmap id_count;
  rm_idmap_init(&id_count);
  rm_grouping dependents = {0};
  bool* points_inside = calloc(count, sizeof *points_inside);
  bool* gone = calloc(count, sizeof *gone);
  if (points_inside == nullptr || gone == nullptr) goto done;

  /* How many records of the tie still carry each identifier. Counted and not
   * gathered into a set: two records may share an id, and the id leaves the
   * tie only with the last of them. */
  for (size_t i = 0; i < count; i += 1) {
    uint64_t carried = 0;
    (void)rm_idmap_get(&id_count, items[bucket[i]].id, &carried);
    if (!rm_idmap_set(&id_count, items[bucket[i]].id, carried + 1u)) goto done;
  }

  /* Which positions point at each identifier, so that the identifier leaving
   * the tie can put them all out at once. Every position leaves at most
   * once, which is what keeps the total linear. */
  {
    struct {
      const text_item* items;
      const uint32_t* bucket;
    } state = {.items = items, .bucket = bucket};
    if (!rm_grouping_build(&dependents, count, text_right_key, &state))
      goto done;
  }
  for (size_t position = 0; position < count; position += 1) {
    points_inside[position] =
        rm_idmap_contains(&id_count, items[bucket[position]].right);
  }

  /* The two marks. Neither ever goes back: a position one has passed is
   * either gone from the tie or has stopped pointing inside it, and neither
   * is undone. */
  size_t pointing = 0;
  size_t present = 0;
  size_t written = 0;
  while (written < count) {
    while (pointing < count && (gone[pointing] || !points_inside[pointing]))
      pointing += 1;
    size_t position;
    if (pointing < count) {
      position = pointing;
    } else {
      while (present < count && gone[present]) present += 1;
      if (present >= count) break;
      position = present;
    }

    gone[position] = true;
    out[written++] = bucket[position];

    rm_crdt_id id = items[bucket[position]].id;
    uint64_t carried;
    if (!rm_idmap_get(&id_count, id, &carried)) continue;
    if (carried > 1) {
      if (!rm_idmap_set(&id_count, id, carried - 1u)) goto done;
    } else {
      (void)rm_idmap_take(&id_count, id, nullptr);
      const uint32_t* pointers;
      size_t pointer_count;
      if (rm_grouping_find(&dependents, id, &pointers, &pointer_count)) {
        for (size_t k = 0; k < pointer_count; k += 1)
          points_inside[pointers[k]] = false;
      }
    }
  }
  /* A tie always empties: every turn marks one record gone, and the second
   * mark finds whichever is left. The fill below is belt and braces. */
  for (size_t i = 0; written < count && i < count; i += 1) {
    if (!gone[i]) {
      gone[i] = true;
      out[written++] = bucket[i];
    }
  }
  ok = true;

done:
  free(points_inside);
  free(gone);
  rm_grouping_free(&dependents);
  rm_idmap_free(&id_count);
  return ok;
}

/* The items in the order they read.
 *
 * The order is set by the `left` links: an item follows the character its link
 * points at. Sorting by the counter will not do — an item added later may
 * stand in the middle, and the marks of a face are added last of all.
 *
 * Indexed by the same grouping the scene walk uses, and for the same reason:
 * filtering the whole of what is left at every step and deleting from the
 * middle of it would be quadratic in the number of records — and a page
 * accumulates a record per edit, so the number is the file's to choose. On a
 * page big enough that is the parse itself hanging, in a task nothing
 * interrupts. */
static bool in_document_order(const text_item* items, size_t count,
                              uint32_t** out_order) {
  *out_order = nullptr;
  if (count == 0) return true;

  bool ok = false;
  rm_grouping by_left = {0};
  uint32_t* scratch = nullptr;
  uint32_t* cursor_of_group = nullptr;
  bool* taken = nullptr;
  uint32_t* ordered = nullptr;

  text_key_ctx key_ctx = {.items = items};
  if (!rm_grouping_build(&by_left, count, text_left_key, &key_ctx)) goto done;

  /* The order in which one bucket's records leave it depends on that bucket
   * and on nothing else — not on the cursor, not on what has been read
   * elsewhere — so it is worked out once per bucket rather than derived
   * again on every visit.
   *
   * Derived again, it would be quadratic: a page whose records all name one
   * `left` would re-scan the whole tie on every one of them, and the time
   * would grow with the square of a count the file chooses. */
  scratch = calloc(count, sizeof *scratch);
  cursor_of_group = calloc(by_left.group_count, sizeof *cursor_of_group);
  taken = calloc(count, sizeof *taken);
  ordered = calloc(count, sizeof *ordered);
  if (scratch == nullptr || cursor_of_group == nullptr || taken == nullptr ||
      ordered == nullptr) {
    goto done;
  }
  for (size_t group = 0; group < by_left.group_count; group += 1) {
    uint32_t start = by_left.starts[group];
    size_t size = by_left.ends[group] - start;
    if (!tie_order(by_left.items + start, size, items, scratch)) goto done;
    memcpy(by_left.items + start, scratch, size * sizeof *scratch);
  }

  size_t written = 0;
  rm_crdt_id cursor = RM_CRDT_NONE;
  while (written < count) {
    size_t group;
    if (!rm_grouping_number(&by_left, cursor, &group)) break;
    uint32_t start = by_left.starts[group];
    if (cursor_of_group[group] >= by_left.ends[group] - start) break;
    uint32_t index = by_left.items[start + cursor_of_group[group]];
    cursor_of_group[group] += 1;

    taken[index] = true;
    ordered[written++] = index;

    /* The characters this record covers: the ones it carries, the ones it
     * deleted — a deletion holds its place in the sequence — or the one of
     * a marker, whatever its string spells. */
    const text_item* item = &items[index];
    uint64_t span = text_item_span(item);
    /* An identifier at the top of the range would wrap. An overflow leaves
     * the cursor on the item itself — the chain will not add up and the
     * rest is appended in file order, which is what this walk already does
     * with broken links. */
    if (span == 0 || item->id.counter > UINT64_MAX - (span - 1u)) {
      cursor = item->id;
    } else {
      cursor = rm_crdt_make(item->id.author, item->id.counter + (span - 1u));
    }
  }
  /* The links did not add up — the rest is appended as it lies, in file
   * order, so as not to lose it. */
  for (size_t i = 0; i < count; i += 1) {
    if (!taken[i]) ordered[written++] = (uint32_t)i;
  }

  *out_order = ordered;
  ordered = nullptr;
  ok = true;

done:
  free(ordered);
  free(taken);
  free(cursor_of_group);
  free(scratch);
  rm_grouping_free(&by_left);
  return ok;
}

/* ------------------------------------------------------------------ */
/* Composing the lines                                                 */
/* ------------------------------------------------------------------ */

/* A paragraph and an offset into one `uint64_t`, the value `place_of_character`
 * maps an identifier to. */
static uint64_t anchor_pack(int32_t paragraph, int32_t offset) {
  return (uint64_t)(uint32_t)paragraph << 32 | (uint64_t)(uint32_t)offset;
}

/* The paragraph and offset `anchor_pack` packed into `packed`. */
static rm_text_anchor anchor_unpack(uint64_t packed) {
  return (rm_text_anchor){
      .paragraph = (int32_t)(uint32_t)(packed >> 32),
      .offset = (int32_t)(uint32_t)(packed & 0xFFFF'FFFFu),
  };
}

/* Start a fresh, empty paragraph at the end of the layout. False on
 * allocation failure. */
static bool layout_push_line(text_layout* layout) {
  line_build line;
  rm_buf_init(&line.text);
  rm_vec_init(&line.faces, sizeof(uint8_t));
  return rm_vec_push(&layout->lines, &line);
}

/* Release every paragraph's text and faces, and the layout's own tables. */
static void layout_free(text_layout* layout) {
  for (size_t i = 0; i < layout->lines.count; i += 1) {
    line_build* line = rm_vec_at(&layout->lines, i);
    rm_buf_free(&line->text);
    rm_vec_free(&line->faces);
  }
  rm_vec_free(&layout->lines);
  rm_vec_free(&layout->styles);
  rm_idmap_free(&layout->place_of_character);
}

/* The text, its line numbers and its faces — one walk of the items in
 * document order.
 *
 * One walk and not three, so that what gets drawn, what the handwriting is
 * positioned against and what is set in bold cannot drift apart. */
static rm_status compose(const text_item* items, size_t count,
                         text_layout* layout) {
  uint32_t* order = nullptr;
  if (!in_document_order(items, count, &order)) {
    return RM_ERR_OUT_OF_MEMORY;
  }

  rm_status status = RM_OK;
  if (!layout_push_line(layout)) {
    status = RM_ERR_OUT_OF_MEMORY;
    goto done;
  }

  uint8_t face = 0;
  /* Scalars in the current paragraph get their own counter: a count of
   * characters as a reader sees them is not what the file numbers, and a
   * combining mark does not grow it while the file does give it an
   * identifier. */
  int32_t scalars = 0;

  for (size_t step = 0; step < count; step += 1) {
    const text_item* item = &items[order[step]];
    if (item->has_marker) {
      switch (item->marker) {
        case 1:
          face |= FACE_BOLD;
          break;
        case 2:
          face &= (uint8_t)~FACE_BOLD;
          break;
        case 3:
          face |= FACE_ITALIC;
          break;
        case 4:
          face &= (uint8_t)~FACE_ITALIC;
          break;
        case MARK_LINK_START:
        case MARK_LINK_END:
          /* A link of typed text: kept in the file, not drawn as
           * anything of its own. Its characters are the ordinary
           * records between the two marks, and they stay text. */
          break;
        case MARK_INLINE_IMAGE: {
          /* An image in the flow is one character of the text — the
           * object replacement character — and takes its place in
           * the paragraph, so the offsets after it are the tablet's.
           * A deleted one is no character at all. */
          if (item->deleted != 0) break;
          int32_t paragraph = (int32_t)layout->lines.count - 1;
          line_build* line = rm_vec_at(&layout->lines, (size_t)paragraph);
          rm_buf_append(&line->text, OBJECT_REPLACEMENT,
                        sizeof OBJECT_REPLACEMENT - 1u);
          if (line->text.failed || !rm_vec_push(&line->faces, &face)) {
            status = RM_ERR_OUT_OF_MEMORY;
            goto done;
          }
          scalars += 1;
          if (!rm_idmap_set(&layout->place_of_character, item->id,
                            anchor_pack(paragraph, scalars))) {
            status = RM_ERR_OUT_OF_MEMORY;
            goto done;
          }
          break;
        }
        default:
          RM_LOG_DEBUGF("render: unknown text mark %lu",
                        (unsigned long)item->marker);
      }
      continue;
    }

    /* By Unicode scalars, not by graphemes: the file numbers characters as
     * scalars. On NFC Latin and Cyrillic the two agree; on Arabic
     * diacritics they do not, and a grapheme count shifts every style key
     * and anchor after the first such mark. */
    uint64_t offset = 0;
    size_t at = 0;
    while (at < item->text.length) {
      uint32_t scalar;
      size_t next =
          rm_utf8_next(item->text.bytes, item->text.length, at, &scalar);
      /* The characters run on from the item's own identifier. On a
       * corrupted file that identifier can sit at the top of the range,
       * and the sum would wrap into somebody else's character. The text
       * is still worth drawing; what is lost is the anchoring of
       * handwriting to these characters. */
      bool has_id = item->id.counter <= UINT64_MAX - offset;
      rm_crdt_id id = rm_crdt_make(item->id.author, item->id.counter + offset);
      offset += 1;

      int32_t paragraph = (int32_t)layout->lines.count - 1;
      if (scalar == '\n') {
        /* A newline ends its paragraph, and the pen that typed it
         * stood at the end of that paragraph rather than at the start
         * of the next one. It is the paragraph's character after the
         * last letter, and the pen stands before the character an
         * anchor names — so one past the letters, not on the last of
         * them. */
        if (has_id && !rm_idmap_set(&layout->place_of_character, id,
                                    anchor_pack(paragraph, scalars + 1))) {
          status = RM_ERR_OUT_OF_MEMORY;
          goto done;
        }
        if (!layout_push_line(layout)) {
          status = RM_ERR_OUT_OF_MEMORY;
          goto done;
        }
        scalars = 0;
      } else {
        line_build* line = rm_vec_at(&layout->lines, (size_t)paragraph);
        rm_buf_append(&line->text, item->text.bytes + at, next - at);
        if (line->text.failed || !rm_vec_push(&line->faces, &face)) {
          status = RM_ERR_OUT_OF_MEMORY;
          goto done;
        }
        scalars += 1;
        /* The character's own number, counted from one: the renderer
         * stands the pen before the character an anchor names. */
        if (has_id && !rm_idmap_set(&layout->place_of_character, id,
                                    anchor_pack(paragraph, scalars))) {
          status = RM_ERR_OUT_OF_MEMORY;
          goto done;
        }
      }
      at = next;
    }
  }

done:
  free(order);
  return status;
}

/* The place an anchor points at.
 *
 * Most anchors name a character, and the answer is the pen position just past
 * it. Two do not: the format has sentinel identifiers for the start and the
 * end of the text, and handwriting placed before or after everything typed is
 * anchored to those instead. They must not be left to the "nowhere" fallback:
 * treating a sentinel as line zero drags every stroke anchored to it to the
 * top of the text. */
static bool layout_anchor(const text_layout* layout, rm_crdt_id anchor,
                          rm_text_anchor* out) {
  uint64_t packed;
  if (rm_idmap_get(&layout->place_of_character, anchor, &packed)) {
    *out = anchor_unpack(packed);
    return true;
  }
  if (anchor.counter == ANCHOR_START_SENTINEL) {
    *out = (rm_text_anchor){.paragraph = 0, .offset = 0};
    return true;
  }
  if (anchor.counter == ANCHOR_END_SENTINEL) {
    /* Just past the last character, the way any anchor is — and a
     * newline, like every newline, ends its own paragraph: text ending in
     * a Return puts the pen at the end of the paragraph that Return
     * closes, not in the empty one after it. On a page of nineteen
     * Returns the block anchored here stands at paragraph 18, one line
     * under paragraph 17's. Skipping empty paragraphs would put it at the
     * top, across other blocks; the empty last paragraph would put it a
     * line too low.
     * The offset is one past the letters: an anchor names the character
     * the pen stands before, and after the text that is the one that
     * would come next.
     * No text at all — no lines, or the one empty line the parser starts
     * with — ends where it starts. */
    size_t count = layout->lines.count;
    if (count == 0) {
      *out = (rm_text_anchor){.paragraph = 0, .offset = 0};
      return true;
    }
    size_t last = count - 1u;
    const line_build* line = rm_vec_at(&layout->lines, last);
    if (count == 1 && line->faces.count == 0) {
      *out = (rm_text_anchor){.paragraph = 0, .offset = 0};
      return true;
    }
    if (line->faces.count == 0 && last > 0) {
      last -= 1u;
      line = rm_vec_at(&layout->lines, last);
    }
    *out = (rm_text_anchor){
        .paragraph = (int32_t)last,
        .offset = (int32_t)line->faces.count + 1,
    };
    return true;
  }
  return false;
}

/* The column width out of the text's settings (tag 5): a record whose tag 1 is
 * the width as a last-write-wins value — the stamp, then the float32. Pages
 * nest it so, one level deeper than a bare LWW (`5:{1:{1:stamp,
 * 2:f32}}`). Anything else, or a width that is not a width, answers false. */
static bool read_settings_width(rm_reader* reader, double* out) {
  if (!rm_match_tag(reader, 1, RM_TAG_LENGTH4)) return false;
  rm_reader width;
  if (rm_read_subblock(reader, &width) != RM_OK) return false;
  rm_crdt_id stamp;
  float value;
  if (rm_read_crdt(&width, 1, &stamp) != RM_OK ||
      !rm_match_tag(&width, 2, RM_TAG_BYTE4) ||
      rm_read_f32(&width, &value) != RM_OK) {
    return false;
  }
  double result = (double)value;
  if (!isfinite(result) || result <= 0) return false;
  *out = result;
  return true;
}

/* The root text block: the paragraphs, the styles keyed to them, and the box
 * they are set in. */
static rm_status parse_root_text(parse_ctx* ctx, rm_reader* reader, size_t end,
                                 bool* out_found) {
  *out_found = false;
  rm_crdt_id ignored;
  RM_TRY(rm_read_crdt(reader, 1, &ignored));

  rm_vec items;
  rm_vec_init(&items, sizeof(text_item));
  text_layout layout = {0};
  rm_vec_init(&layout.lines, sizeof(line_build));
  rm_vec_init(&layout.styles, sizeof(style_entry));
  rm_idmap_init(&layout.place_of_character);

  bool has_origin = false;
  bool has_settings_width = false;
  double settings_width = 0;
  rm_status status = RM_OK;

  while (reader->offset < end) {
    size_t saved = reader->offset;
    uint32_t index;
    uint8_t type;
    if (rm_read_tag(reader, &index, &type) != RM_OK) {
      reader->offset = saved;
      break;
    }
    /* The column width is the one field of this block that is not a
     * subblock, and stopping at it would leave it unread. */
    if (type == RM_TAG_BYTE4) {
      float raw;
      status = rm_read_f32(reader, &raw);
      if (status != RM_OK) goto done;
      double value = (double)raw;
      if (index == TAG_COLUMN_WIDTH && isfinite(value) && value > 0) {
        layout.has_column_width = true;
        layout.column_width = value;
      }
      continue;
    }
    if (type != RM_TAG_LENGTH4) break;
    uint32_t length;
    status = rm_read_u32(reader, &length);
    if (status != RM_OK) goto done;
    size_t sub_end;
    if (!rm_fits_within(reader, length, end, &sub_end)) break;

    if (index == TAG_TEXT_ITEMS) {
      /* Two lists live side by side in here: the text itself and the
       * styles of its paragraphs. The reader is handed the same window
       * twice — the items parser stops where its own list ends, and the
       * styles begin right after it. A list that runs out early keeps
       * what it read; a failure inside a record costs that list, and
       * neither costs the block. */
      rm_reader inner = *reader;
      inner.size = sub_end;
      /* A list that fails inside a record is lost whole and leaves an
       * empty one; the styles are looked for from wherever the items
       * stopped. An allocation failure always aborts the whole page. */
      rm_vec_free(&items);
      rm_vec_init(&items, sizeof(text_item));
      status = parse_text_items(ctx, &inner, &items);
      if (status != RM_OK) {
        if (status == RM_ERR_OUT_OF_MEMORY) goto done;
        rm_vec_free(&items);
        rm_vec_init(&items, sizeof(text_item));
      }
      rm_vec_free(&layout.styles);
      rm_vec_init(&layout.styles, sizeof(style_entry));
      status = parse_paragraph_styles(&inner, &layout.styles);
      if (status != RM_OK) {
        if (status == RM_ERR_OUT_OF_MEMORY) goto done;
        rm_vec_free(&layout.styles);
        rm_vec_init(&layout.styles, sizeof(style_entry));
      }
    } else if (index == TAG_TEXT_POSITION && length >= 16) {
      double x, y;
      status = rm_read_f64(reader, &x);
      if (status != RM_OK) goto done;
      status = rm_read_f64(reader, &y);
      if (status != RM_OK) goto done;
      /* Left unset when it is not a place: the block ends by refusing
       * text nobody can lay out, whole and counted, the same as text
       * that will not parse. */
      if (isfinite(x) && isfinite(y)) {
        layout.origin_x = x;
        layout.origin_y = y;
        has_origin = true;
      }
    } else if (index == TAG_TEXT_SETTINGS) {
      rm_reader settings = *reader;
      settings.size = sub_end;
      has_settings_width = read_settings_width(&settings, &settings_width);
    }
    status = rm_read_seek(reader, sub_end);
    if (status != RM_OK) goto done;
  }
  /* The stamped width wins over the bare one: tag 4 is the legacy copy,
   * taken only when the settings carry no width of their own. */
  if (has_settings_width) {
    layout.has_column_width = true;
    layout.column_width = settings_width;
  }

  if (!has_origin) {
    status = RM_OK;
    goto done;
  }

  status = compose(items.data, items.count, &layout);
  if (status != RM_OK) goto done;

  rm_vec_free(&items);
  /* Replaced only by a text that parsed: a later block that will not read
   * leaves the earlier one standing. A later block whose item or style list
   * broke still parses, and replaces it with what it holds. */
  if (ctx->has_text) layout_free(&ctx->text);
  ctx->text = layout;
  ctx->has_text = true;
  *out_found = true;
  return RM_OK;

done:
  rm_vec_free(&items);
  layout_free(&layout);
  return status;
}

/* ------------------------------------------------------------------ */
/* The order the scene draws in                                        */
/* ------------------------------------------------------------------ */

/* Which of two identifiers is the older — the chain's tie-break reads "newer
 * first". The counters grow monotonically as a journal of edits, so the
 * counter decides; two counters can only tie across authors, and there the
 * **lower** author goes first in the chain. On a page edited on two tablets at
 * once, where each added a layer as the same item after the same neighbour,
 * the second author's layer is drawn over the first's — the higher author
 * later in the chain, on top. The other way round would put the first
 * author's stroke over the second's where the two cross. */
static bool is_ordered_before(rm_crdt_id left, rm_crdt_id right) {
  if (left.counter != right.counter) return left.counter < right.counter;
  return left.author > right.author;
}

/* A stable bottom-up merge sort. Stable and not `qsort` because two records
 * may carry one identifier, and where the rule cannot separate them the file's
 * own order has to stand — twice over the same page must draw the same
 * picture. */
static void sort_by_chain_rule(uint32_t* values, size_t count,
                               const scene_item* items, uint32_t* scratch) {
  for (size_t width = 1; width < count; width *= 2) {
    for (size_t start = 0; start < count; start += 2 * width) {
      size_t middle = start + width < count ? start + width : count;
      size_t end = start + 2 * width < count ? start + 2 * width : count;
      size_t left = start, right = middle, out = start;
      while (left < middle && right < end) {
        if (is_ordered_before(items[values[right]].id,
                              items[values[left]].id)) {
          scratch[out++] = values[right++];
        } else {
          scratch[out++] = values[left++];
        }
      }
      while (left < middle) scratch[out++] = values[left++];
      while (right < end) scratch[out++] = values[right++];
    }
    memcpy(values, scratch, count * sizeof *values);
  }
}

typedef struct scene_key_ctx {
  const scene_item* items;
  const uint32_t* positions;
} scene_key_ctx;

/* The grouping key for a scene item: the node it hangs on. */
static rm_crdt_id scene_parent_key(size_t index, void* ctx) {
  return ((scene_key_ctx*)ctx)->items[index].parent;
}

/* The grouping key for a position among a node's children: its `left`. */
static rm_crdt_id scene_left_key(size_t position, void* ctx) {
  scene_key_ctx* state = ctx;
  return state->items[state->positions[position]].left;
}

/* A node's children in the order of their chain: from the head at (0, 0),
 * each record followed by whatever hangs on it, and a tie between records
 * sharing a `left` going to the newer — larger — identifier.
 *
 * Iterative, because a chain is as long as a page has strokes and the walk
 * must not be a stack overflow. The result is malloc'd and the caller frees
 * it. */
static uint32_t* chain_order(const uint32_t* indices, size_t count,
                             const scene_item* items, size_t* out_count) {
  *out_count = count;
  uint32_t* result = calloc(count == 0 ? 1 : count, sizeof *result);
  if (result == nullptr) return nullptr;
  if (count <= 1) {
    if (count == 1) result[0] = indices[0];
    return result;
  }

  rm_grouping by_left = {0};
  bool* spent = nullptr;
  bool* taken = nullptr;
  uint32_t* stack = nullptr;
  uint32_t* scratch = nullptr;

  scene_key_ctx key_ctx = {.items = items, .positions = indices};
  if (!rm_grouping_build(&by_left, count, scene_left_key, &key_ctx)) goto fail;
  spent =
      calloc(by_left.group_count == 0 ? 1 : by_left.group_count, sizeof *spent);
  taken = calloc(count, sizeof *taken);
  stack = calloc(count, sizeof *stack);
  scratch = calloc(count, sizeof *scratch);
  if (spent == nullptr || taken == nullptr || stack == nullptr ||
      scratch == nullptr)
    goto fail;

  /* The chain, walked from the head at (0, 0): each record, then whatever
   * hangs on it. A bucket leaves its group whole — every record sharing a
   * `left` is a tie, and the tie is ordered once, when it is first reached.
   *
   * The stack holds item indexes and never grows past `count`, because every
   * bucket is pushed at most once and the buckets partition the input. */
  size_t stack_count = 0;
  size_t written = 0;
  rm_crdt_id cursor = RM_CRDT_NONE;
  for (;;) {
    size_t group;
    if (rm_grouping_number(&by_left, cursor, &group) && !spent[group]) {
      spent[group] = true;
      uint32_t start = by_left.starts[group];
      size_t size = by_left.ends[group] - start;
      uint32_t* run = stack + stack_count;
      for (size_t i = 0; i < size; i += 1) {
        uint32_t position = by_left.items[start + i];
        taken[position] = true;
        run[i] = indices[position];
      }
      /* Ascending, because a stack pops from the end: the newest — the
       * one that goes first — has to be pushed last. */
      sort_by_chain_rule(run, size, items, scratch);
      stack_count += size;
    }
    if (stack_count == 0) break;
    uint32_t index = stack[--stack_count];
    result[written++] = index;
    cursor = items[index].id;
  }

  /* Links that did not add up — appended as they lie, not lost. */
  for (size_t position = 0; position < count && written < count;
       position += 1) {
    if (!taken[position]) result[written++] = indices[position];
  }

  free(spent);
  free(taken);
  free(stack);
  free(scratch);
  rm_grouping_free(&by_left);
  return result;

fail:
  free(spent);
  free(taken);
  free(stack);
  free(scratch);
  rm_grouping_free(&by_left);
  free(result);
  return nullptr;
}

typedef struct ordered_stroke {
  uint32_t item;
  rm_crdt_id parent;
} ordered_stroke;

/* An image item as the scene walk hands it over: the attachment it names, its
 * place in the drawing order, and the node it hangs on — the parent is what
 * says whether it is anchored. */
typedef struct placed_image {
  uint32_t item;
  size_t strokes_before;
  rm_crdt_id parent;
} placed_image;

/* The scene tree as its records leave it, applied in file order: the nodes
 * taken out, and the parent each node in the tree was last put under. */
typedef struct tree_state {
  /* Only an explicit removal counts: a node nobody added is not dropped, or
   * a file without tree records would draw nothing. */
  rm_idmap removed;
  /* node -> index into `events` of the record that last put it under a
   * parent. The record's place and not the parent itself: an identifier is
   * wider than the map's value. */
  rm_idmap placed_by;
  const tree_event* events;
} tree_state;

/* Apply the page's tree records in order. False on allocation failure; the
 * state is to be freed either way. */
static bool tree_state_build(const parse_ctx* ctx, tree_state* out) {
  rm_idmap_init(&out->removed);
  rm_idmap_init(&out->placed_by);
  out->events = ctx->tree_events.data;
  for (size_t i = 0; i < ctx->tree_events.count; i += 1) {
    const tree_event* event = &out->events[i];
    if (!event->in_tree) {
      if (!rm_idmap_set(&out->removed, event->node, 1)) return false;
      continue;
    }
    (void)rm_idmap_take(&out->removed, event->node, nullptr);
    if (event->has_parent && !rm_idmap_set(&out->placed_by, event->node, i))
      return false;
  }
  return true;
}

/* Release what `tree_state_build` built. */
static void tree_state_free(tree_state* tree) {
  rm_idmap_free(&tree->placed_by);
  rm_idmap_free(&tree->removed);
}

/* The parent the records last put `node` under, or false when no record
 * places it. */
static bool tree_parent_of(const tree_state* tree, rm_crdt_id node,
                           rm_crdt_id* out) {
  uint64_t index;
  if (!rm_idmap_get(&tree->placed_by, node, &index)) return false;
  *out = tree->events[index].parent;
  return true;
}

/* Whether a group item under `parent` that names `child` is where the child
 * is now. A node moved to another parent keeps its old group item until the
 * tablet tombstones it; the tree record is the newer word on where the node
 * lives. A node no record places is taken where its group item puts it. */
static bool tree_links(const tree_state* tree, rm_crdt_id parent,
                       rm_crdt_id child) {
  rm_crdt_id placed;
  return !tree_parent_of(tree, child, &placed) || rm_crdt_equal(placed, parent);
}

/* Whether `node` or a node above it, as the tree records place them, is in
 * `removed` or `hidden`; the answer in `*out_under`. The answer for every
 * node on the climb is kept in `known` (1 under, 0 not), so a page of many
 * items on one deep node climbs once and not once per item. The climb ends
 * at a node already known, at one no record places, or — a cycle in the
 * records — after one step more than there are placed nodes, which no climb
 * without a repeat can take. `path` is scratch. False only on allocation
 * failure. */
static bool tree_is_under(const tree_state* tree, const rm_idmap* removed,
                          const rm_idmap* hidden, rm_idmap* known, rm_vec* path,
                          rm_crdt_id node, bool* out_under) {
  path->count = 0;
  bool under = false;
  rm_crdt_id at = node;
  for (size_t steps = 0; steps <= tree->placed_by.count; steps += 1) {
    uint64_t answer;
    if (rm_idmap_get(known, at, &answer)) {
      under = answer != 0;
      break;
    }
    if (!rm_vec_push(path, &at)) return false;
    if (rm_idmap_contains(removed, at) || rm_idmap_contains(hidden, at)) {
      under = true;
      break;
    }
    if (!tree_parent_of(tree, at, &at)) break;
  }
  for (size_t i = 0; i < path->count; i += 1) {
    const rm_crdt_id* visited = rm_vec_at(path, i);
    if (!rm_idmap_set(known, *visited, under ? 1 : 0)) return false;
  }
  *out_under = under;
  return true;
}

typedef struct walk_frame {
  rm_crdt_id node;
  uint32_t* children;
  size_t child_count;
  size_t next;
} walk_frame;

typedef struct scene_walk {
  const scene_item* items;
  size_t item_count;
  const rm_grouping* children_of;
  const tree_state* tree;
  const rm_idmap* removed;
  const rm_idmap* hidden;
  bool* placed;
  rm_vec strokes; /* ordered_stroke */
  rm_vec images;  /* placed_image */
  bool failed;
} scene_walk;

/* A subtree that is not drawn, and whose items are nonetheless spoken for:
 * the fallback pass must not resurrect a hidden layer. A node moved out of
 * it is not part of it any more and is left to be reached from where it is
 * now. */
static void claim_subtree(scene_walk* walk, rm_crdt_id node) {
  rm_vec stack;
  rm_vec_init(&stack, sizeof(uint32_t));
  const uint32_t* children;
  size_t count;
  if (rm_grouping_find(walk->children_of, node, &children, &count)) {
    for (size_t i = 0; i < count; i += 1) {
      if (!rm_vec_push(&stack, &children[i])) {
        walk->failed = true;
        break;
      }
    }
  }
  while (stack.count > 0 && !walk->failed) {
    uint32_t index = *(uint32_t*)rm_vec_at(&stack, stack.count - 1);
    stack.count -= 1;
    if (walk->placed[index]) continue;
    walk->placed[index] = true;
    if (walk->items[index].kind != ITEM_GROUP) continue;
    if (!tree_links(walk->tree, walk->items[index].parent,
                    walk->items[index].group)) {
      continue;
    }
    if (!rm_grouping_find(walk->children_of, walk->items[index].group,
                          &children, &count)) {
      continue;
    }
    for (size_t i = 0; i < count; i += 1) {
      if (!rm_vec_push(&stack, &children[i])) {
        walk->failed = true;
        break;
      }
    }
  }
  rm_vec_free(&stack);
}

/* The node's children (there may be none), ordered the way `chain_order`
 * orders them. Malloc'd; the caller frees it. Null only on allocation
 * failure. */
static uint32_t* children_in_chain_order(const scene_walk* walk,
                                         rm_crdt_id node, size_t* out_count) {
  const uint32_t* children;
  size_t count;
  if (!rm_grouping_find(walk->children_of, node, &children, &count)) {
    *out_count = 0;
    return calloc(1, sizeof(uint32_t));
  }
  return chain_order(children, count, walk->items, out_count);
}

/* Depth first with a stack of its own rather than with the call stack: groups
 * nest as deeply as a page has of them, and the depth is the file's to choose.
 * A stack overflow is not an error one can catch — it is the end of the
 * process. Each frame is a node, its children in chain order, and how far down
 * them the walk has got. */
static void walk_from(scene_walk* walk, rm_crdt_id root) {
  if (rm_idmap_contains(walk->removed, root) ||
      rm_idmap_contains(walk->hidden, root)) {
    claim_subtree(walk, root);
    return;
  }

  rm_vec stack;
  rm_vec_init(&stack, sizeof(walk_frame));
  walk_frame first = {.node = root, .next = 0};
  first.children = children_in_chain_order(walk, root, &first.child_count);
  if (first.children == nullptr || !rm_vec_push(&stack, &first)) {
    free(first.children);
    walk->failed = true;
    rm_vec_free(&stack);
    return;
  }

  while (stack.count > 0) {
    walk_frame* frame = rm_vec_at(&stack, stack.count - 1);
    bool descended = false;
    while (frame->next < frame->child_count) {
      uint32_t index = frame->children[frame->next];
      frame->next += 1;
      if (walk->placed[index]) continue;
      walk->placed[index] = true;
      if (walk->items[index].deleted != 0) continue;

      switch (walk->items[index].kind) {
        case ITEM_STROKE: {
          ordered_stroke entry = {.item = index, .parent = frame->node};
          if (!rm_vec_push(&walk->strokes, &entry)) walk->failed = true;
          break;
        }
        case ITEM_GROUP: {
          rm_crdt_id child = walk->items[index].group;
          /* A stale link: the child is drawn, if at all, from its
           * present parent. */
          if (!tree_links(walk->tree, frame->node, child)) continue;
          if (rm_idmap_contains(walk->removed, child) ||
              rm_idmap_contains(walk->hidden, child)) {
            claim_subtree(walk, child);
            continue;
          }
          /* The parent stays on the stack with its place kept, and
           * the child goes on top of it: that is what "depth first"
           * is once the call stack is not doing the remembering. */
          walk_frame next = {.node = child, .next = 0};
          next.children =
              children_in_chain_order(walk, child, &next.child_count);
          if (next.children == nullptr || !rm_vec_push(&stack, &next)) {
            free(next.children);
            walk->failed = true;
          }
          descended = true;
          break;
        }
        case ITEM_IMAGE: {
          /* Its place in the sequence travels as the number of
           * strokes before it: the z-order is the sequence. */
          placed_image entry = {
              .item = index,
              .strokes_before = walk->strokes.count,
              .parent = frame->node,
          };
          if (!rm_vec_push(&walk->images, &entry)) walk->failed = true;
          break;
        }
        case ITEM_TOMBSTONE:
          break;
      }
      if (descended) break;
    }
    if (descended) continue;
    walk_frame* top = rm_vec_at(&stack, stack.count - 1);
    free(top->children);
    stack.count -= 1;
  }
  rm_vec_free(&stack);
}

/* The strokes in the order the tablet draws them: the scene tree walked
 * depth-first, each node's children in their chain order. File order is not
 * that order — a reordering of layers rewrites no stroke at all.
 *
 * A node is left out when the user hid it (0x02) or took it out of the tree
 * (0x01); everything under it goes too. Strokes whose parent the walk never
 * reaches — a file without group blocks, or links that do not add up — are
 * appended in file order rather than lost: order is worth less than the ink. */
static bool ordered_scene(parse_ctx* ctx, rm_vec* out_strokes,
                          rm_vec* out_images) {
  const scene_item* items = ctx->items.data;
  size_t count = ctx->items.count;

  bool ok = false;
  /* A group item that names a node the records have since put elsewhere is
   * a stale link: the node is reached from where it is now, or not at all. */
  tree_state tree;
  rm_idmap interior, known;
  rm_vec path;
  rm_idmap_init(&interior);
  rm_idmap_init(&known);
  rm_vec_init(&path, sizeof(rm_crdt_id));
  rm_grouping children_of = {0};
  bool* placed = nullptr;

  if (!tree_state_build(ctx, &tree)) goto done;

  scene_key_ctx key_ctx = {.items = items, .positions = nullptr};
  if (!rm_grouping_build(&children_of, count, scene_parent_key, &key_ctx))
    goto done;

  /* The roots: nodes that hold items but are named by no group item — the
   * page root, ordinarily one. */
  for (size_t i = 0; i < count; i += 1) {
    if (items[i].kind != ITEM_GROUP) continue;
    if (!rm_idmap_set(&interior, items[i].group, 1)) goto done;
  }

  placed = calloc(count == 0 ? 1 : count, sizeof *placed);
  if (placed == nullptr) goto done;

  scene_walk walk = {
      .items = items,
      .item_count = count,
      .children_of = &children_of,
      .tree = &tree,
      .removed = &tree.removed,
      .hidden = &ctx->hidden_nodes,
      .placed = placed,
  };
  walk.strokes = *out_strokes;
  walk.images = *out_images;

  for (size_t i = 0; i < count; i += 1) {
    if (rm_idmap_contains(&interior, items[i].parent)) continue;
    if (placed[i]) continue;
    walk_from(&walk, items[i].parent);
    if (walk.failed) goto done_walk;
  }

  /* Whatever the walk never reached — in file order. A node the records
   * moved under a removed or hidden one goes with it here too. */
  for (size_t i = 0; i < count; i += 1) {
    if (placed[i]) continue;
    if (items[i].deleted != 0) continue;
    rm_crdt_id parent = items[i].parent;
    bool gone;
    if (!tree_is_under(&tree, &tree.removed, &ctx->hidden_nodes, &known, &path,
                       parent, &gone)) {
      goto done_walk;
    }
    if (gone) continue;
    if (items[i].kind == ITEM_STROKE) {
      ordered_stroke entry = {.item = (uint32_t)i, .parent = parent};
      if (!rm_vec_push(&walk.strokes, &entry)) goto done_walk;
    } else if (items[i].kind == ITEM_IMAGE) {
      placed_image entry = {
          .item = (uint32_t)i,
          .strokes_before = walk.strokes.count,
          .parent = parent,
      };
      if (!rm_vec_push(&walk.images, &entry)) goto done_walk;
    }
  }
  ok = !walk.failed;

done_walk:
  *out_strokes = walk.strokes;
  *out_images = walk.images;

done:
  free(placed);
  rm_grouping_free(&children_of);
  rm_vec_free(&path);
  rm_idmap_free(&known);
  rm_idmap_free(&interior);
  tree_state_free(&tree);
  return ok;
}

/* ------------------------------------------------------------------ */
/* Assembly                                                            */
/* ------------------------------------------------------------------ */

/* The anchor an item takes from the node it hangs on, when that node carries
 * one the laid-out text can locate — the one translation both anchored
 * handwriting and anchored images go through. The place says which character;
 * the horizontal comes with the node, straight from the file, and travels
 * alongside. */
static bool anchor_under(const parse_ctx* ctx, rm_crdt_id parent,
                         rm_text_anchor* out) {
  if (!ctx->has_text) return false;
  uint64_t index;
  if (!rm_idmap_get(&ctx->anchor_of_node, parent, &index)) return false;
  const node_anchor* node = rm_vec_at(&ctx->node_anchors, (size_t)index);
  rm_text_anchor place;
  if (!layout_anchor(&ctx->text, node->anchor, &place)) return false;
  place.has_origin_x = node->has_origin_x;
  place.origin_x = node->origin_x;
  *out = place;
  return true;
}

/* The file name the attachment map gave `key`, or null when the map does
 * not know it. */
static const rm_str* attachment_name(const parse_ctx* ctx, const uint8_t* key) {
  uint64_t at;
  if (!rm_keymap_get(&ctx->attachment_index, key, &at)) return nullptr;
  const attachment* entry = rm_vec_at(&ctx->attachments, (size_t)at);
  return &entry->name;
}

/* Neighbouring characters of one face gathered into a stretch. Plain text
 * yields nothing: most of a page is plain, and an entry per line saying so
 * would be an entry for nothing. */
static rm_status build_faces(rm_arena* arena, const line_build* line,
                             const rm_face_run** out_runs, size_t* out_count) {
  *out_runs = nullptr;
  *out_count = 0;
  size_t scalars = line->faces.count;
  if (scalars == 0) return RM_OK;
  const uint8_t* marks = line->faces.data;

  size_t runs = 0;
  for (size_t start = 0; start < scalars;) {
    uint8_t face = marks[start];
    size_t end = start + 1;
    while (end < scalars && marks[end] == face) end += 1;
    if (face != 0) runs += 1;
    start = end;
  }
  if (runs == 0) return RM_OK;

  rm_face_run* out = rm_arena_alloc(arena, runs * sizeof *out);
  if (out == nullptr) return RM_ERR_OUT_OF_MEMORY;
  size_t written = 0;
  for (size_t start = 0; start < scalars;) {
    uint8_t face = marks[start];
    size_t end = start + 1;
    while (end < scalars && marks[end] == face) end += 1;
    if (face != 0) {
      out[written++] = (rm_face_run){
          .start = (int32_t)start,
          .end = (int32_t)end,
          .bold = (face & FACE_BOLD) != 0,
          .italic = (face & FACE_ITALIC) != 0,
      };
    }
    start = end;
  }
  *out_runs = out;
  *out_count = runs;
  return RM_OK;
}

/* Which line each paragraph style belongs to.
 *
 * A style is keyed by the character that **ends the paragraph before** it —
 * the newline — and so belongs to the line after that one. The first paragraph
 * has no newline before it and is keyed by the format's start-of-text
 * sentinel. */
static void apply_styles(const parse_ctx* ctx, rm_paragraph* paragraphs,
                         size_t count) {
  for (size_t i = 0; i < ctx->text.styles.count; i += 1) {
    const style_entry* entry = rm_vec_at(&ctx->text.styles, i);
    size_t line;
    if (rm_crdt_equal(entry->key, RM_CRDT_NONE)) {
      line = 0;
    } else {
      uint64_t packed;
      if (!rm_idmap_get(&ctx->text.place_of_character, entry->key, &packed))
        continue;
      rm_text_anchor place = anchor_unpack(packed);
      line = (size_t)place.paragraph + 1u;
    }
    if (line >= count) continue;
    paragraphs[line].has_format = true;
    paragraphs[line].format = entry->format;
  }
}

/* The page's `rm_text_block`: one `rm_paragraph` per laid-out line, its text
 * copied into the page's arena and its style and faces attached. */
static rm_status build_text_block(parse_ctx* ctx, rm_text_block* out) {
  size_t count = ctx->text.lines.count;
  rm_paragraph* paragraphs =
      rm_arena_calloc(ctx->arena, count == 0 ? 1 : count, sizeof *paragraphs);
  if (paragraphs == nullptr) return RM_ERR_OUT_OF_MEMORY;

  for (size_t i = 0; i < count; i += 1) {
    const line_build* line = rm_vec_at(&ctx->text.lines, i);
    char* text = rm_arena_strndup(ctx->arena, (const char*)line->text.bytes,
                                  line->text.count);
    if (text == nullptr) return RM_ERR_OUT_OF_MEMORY;
    paragraphs[i].text = (rm_str){.bytes = text, .length = line->text.count};
    RM_TRY(build_faces(ctx->arena, line, &paragraphs[i].faces,
                       &paragraphs[i].face_count));
  }
  apply_styles(ctx, paragraphs, count);

  *out = (rm_text_block){
      .origin_x = ctx->text.origin_x,
      .origin_y = ctx->text.origin_y,
      .paragraphs = paragraphs,
      .paragraph_count = count,
      .has_column_width = ctx->text.has_column_width,
      .column_width = ctx->text.column_width,
  };
  return RM_OK;
}

/* The gathered blocks as one page. */
static rm_status assemble(parse_ctx* ctx, rm_page* page) {
  rm_vec strokes, images;
  rm_vec_init(&strokes, sizeof(ordered_stroke));
  rm_vec_init(&images, sizeof(placed_image));
  rm_status status = RM_OK;

  /* The drawing order is the scene's, not the file's: layers in the chain
   * order of 0x04, strokes in their own chain within a layer. A hidden or
   * deleted layer is dropped here rather than at drawing time, so that it
   * widens no sheet and shifts no anchor either. */
  if (!ordered_scene(ctx, &strokes, &images)) {
    status = RM_ERR_OUT_OF_MEMORY;
    goto done;
  }

  const scene_item* items = ctx->items.data;
  size_t drawable = 0;
  for (size_t i = 0; i < ctx->items.count; i += 1) {
    if (items[i].kind == ITEM_STROKE) drawable += 1;
  }
  if (strokes.count != drawable) {
    RM_LOG_DEBUGF("page: %zu strokes on hidden or deleted layers are not drawn",
                  drawable - strokes.count);
  }

  /* Mark the handwriting that is counted from the text with the paragraph it
   * belongs to. Where that paragraph lands is not decided here: a paragraph
   * too long for one line takes more than one step, and working out how much
   * more takes a font and a column width. Both belong to drawing, so the
   * number travels to the renderer. */
  rm_stroke* out_strokes = nullptr;
  if (strokes.count != 0) {
    out_strokes =
        rm_arena_alloc(ctx->arena, strokes.count * sizeof *out_strokes);
    if (out_strokes == nullptr) {
      status = RM_ERR_OUT_OF_MEMORY;
      goto done;
    }
  }
  size_t anchored = 0;
  for (size_t i = 0; i < strokes.count; i += 1) {
    const ordered_stroke* entry = rm_vec_at(&strokes, i);
    out_strokes[i] = items[entry->item].stroke;
    rm_text_anchor place;
    if (anchor_under(ctx, entry->parent, &place)) {
      out_strokes[i].has_anchor = true;
      out_strokes[i].anchor = place;
      anchored += 1;
    }
  }

  /* The scene's images, their keys resolved to file names. A key the
   * attachment map does not know is content we cannot fetch — said out loud
   * rather than dropped in silence, because a page that quietly loses a
   * picture looks like a page that never had one. */
  rm_page_image* out_images = nullptr;
  if (images.count != 0) {
    out_images = rm_arena_calloc(ctx->arena, images.count, sizeof *out_images);
    if (out_images == nullptr) {
      status = RM_ERR_OUT_OF_MEMORY;
      goto done;
    }
  }
  size_t image_count = 0;
  for (size_t i = 0; i < images.count; i += 1) {
    const placed_image* entry = rm_vec_at(&images, i);
    const rm_str* name = attachment_name(ctx, items[entry->item].image_key);
    if (name == nullptr) {
      RM_LOG_ERRORF(
          "page: an image without a name in the attachment map is lost");
      continue;
    }
    rm_page_image* image = &out_images[image_count++];
    image->file = *name;
    memcpy(image->vertices, items[entry->item].vertices,
           sizeof image->vertices);
    image->strokes_before = entry->strokes_before;
    /* An image inserted into the text flow hangs on an anchored group the
     * way handwriting does, and takes the same anchor: its vertices are
     * counted from the pen, and where the pen stands is the layout's to
     * say. */
    image->has_anchor = anchor_under(ctx, entry->parent, &image->anchor);
  }

  rm_highlight* out_highlights = nullptr;
  if (ctx->highlights.count != 0) {
    out_highlights = rm_arena_alloc(
        ctx->arena, ctx->highlights.count * sizeof *out_highlights);
    if (out_highlights == nullptr) {
      status = RM_ERR_OUT_OF_MEMORY;
      goto done;
    }
    memcpy(out_highlights, ctx->highlights.data,
           ctx->highlights.count * sizeof *out_highlights);
  }

  *page = (rm_page){
      .strokes = out_strokes,
      .stroke_count = strokes.count,
      .highlights = out_highlights,
      .highlight_count = ctx->highlights.count,
      .images = out_images,
      .image_count = image_count,
      .anchored_strokes = anchored,
      /* Deleted items do not count as skipped: the user erased them, and a
       * "loss" here would be a false alarm. */
      .skipped_blocks = ctx->unreadable_strokes + ctx->other_blocks,
      .has_paper_size = ctx->has_paper_size,
      .paper_size = ctx->paper_size,
      .background_visible = ctx->background_visible,
      .root_document_visible = ctx->root_document_visible,
      .arena = ctx->arena,
  };
  if (ctx->has_text) {
    status = build_text_block(ctx, &page->text);
    page->has_text = status == RM_OK;
  }

done:
  rm_vec_free(&strokes);
  rm_vec_free(&images);
  return status;
}

/* ------------------------------------------------------------------ */
/* The door                                                            */
/* ------------------------------------------------------------------ */

/* Release everything gathered into the context except its arena, which the
 * page built from it still owns. */
static void ctx_free(parse_ctx* ctx) {
  rm_vec_free(&ctx->items);
  rm_vec_free(&ctx->tree_events);
  rm_vec_free(&ctx->attachments);
  rm_keymap_free(&ctx->attachment_index);
  rm_vec_free(&ctx->node_anchors);
  rm_vec_free(&ctx->highlights);
  rm_idmap_free(&ctx->anchor_of_node);
  rm_idmap_free(&ctx->hidden_nodes);
  rm_idmap_free(&ctx->visibility_of_node);
  rm_vec_free(&ctx->visibility_stamps);
  if (ctx->has_text) layout_free(&ctx->text);
}

/* Fill in `*out_error`, when the caller asked for one, with the status, the
 * version and an optional message. */
static void fail_with(rm_error* out_error, rm_status status, int version,
                      const char* message) {
  if (out_error == nullptr) return;
  *out_error = (rm_error){.status = status, .version = version};
  if (message != nullptr) {
    snprintf(out_error->message, sizeof out_error->message, "%s", message);
  }
}

/* Fold one node's visibility record into the running tables, newer stamp
 * winning over older. */
static rm_status apply_visibility(parse_ctx* ctx, const tree_node* node) {
  if (!node->has_visible) return RM_OK;
  uint64_t index;
  if (rm_idmap_get(&ctx->visibility_of_node, node->id, &index)) {
    rm_crdt_id* stamp = rm_vec_at(&ctx->visibility_stamps, (size_t)index);
    /* Counters are page-wide. Author breaks concurrent ties; an older
     * record arriving later must not undo a newer visibility edit. */
    if (node->visibility_stamp.counter < stamp->counter ||
        (node->visibility_stamp.counter == stamp->counter &&
         node->visibility_stamp.author < stamp->author))
      return RM_OK;
    *stamp = node->visibility_stamp;
  } else {
    index = ctx->visibility_stamps.count;
    if (!rm_vec_push(&ctx->visibility_stamps, &node->visibility_stamp) ||
        !rm_idmap_set(&ctx->visibility_of_node, node->id, index)) {
      return RM_ERR_OUT_OF_MEMORY;
    }
  }
  if (node->visible) {
    (void)rm_idmap_take(&ctx->hidden_nodes, node->id, nullptr);
  } else if (!rm_idmap_set(&ctx->hidden_nodes, node->id, 1)) {
    return RM_ERR_OUT_OF_MEMORY;
  }
  return RM_OK;
}

/* Every top-level block of the page, dispatched by type and gathered into
 * `ctx`. An out-of-memory failure stops the parse; any other block failure
 * is counted and the parse moves on to the next block. */
static rm_status parse_blocks(parse_ctx* ctx) {
  rm_reader* reader = &ctx->reader;

  while (rm_reader_remaining(reader) >= 8) {
    rm_block_header block;
    RM_TRY(rm_read_block_header(reader, &block));
    size_t block_end = block.end;
    uint8_t current_version = block.current_version;
    uint8_t block_type = block.type;

    rm_reader inner = *reader;
    inner.size = block_end;
    switch (block_type) {
      case RM_BLOCK_SCENE_LINE_ITEM: {
        /* A stroke we did not understand is a lost piece of the
         * drawing, but no reason to lose the whole page. */
        scene_item item;
        rm_status status =
            parse_line_item(ctx, &inner, current_version, block_end, &item);
        if (status == RM_OK) {
          if (item.kind == ITEM_TOMBSTONE) ctx->deleted_items += 1;
          if (!rm_vec_push(&ctx->items, &item)) return RM_ERR_OUT_OF_MEMORY;
        } else if (status == RM_ERR_OUT_OF_MEMORY) {
          return RM_ERR_OUT_OF_MEMORY;
        } else {
          ctx->unreadable_strokes += 1;
          ctx_note_failure(ctx, status);
        }
        break;
      }

      /* Every block below counts itself only when its own parse failed,
       * which is what keeps `skipped_blocks` meaning what the field says:
       * the pieces of this page that will not be drawn. A block of a type
       * we understand and read is not a loss and is not counted —
       * counted regardless, the number would say "blocks other than
       * strokes" and could not tell a loss from an ordinary page. */
      case BLOCK_SCENE_TREE: {
        /* Which nodes are in the tree. The one authority on the list of
         * layers: a deleted layer keeps its 0x02 and its 0x04, and only
         * this block says it is gone. */
        tree_event event;
        if (parse_scene_tree(&inner, &event) == RM_OK) {
          if (!rm_vec_push(&ctx->tree_events, &event))
            return RM_ERR_OUT_OF_MEMORY;
        } else {
          ctx->other_blocks += 1;
        }
        break;
      }

      case BLOCK_SCENE_GROUP_ITEM: {
        /* The link of a child group into its parent's list of items —
         * what the order of layers is made of. Unread, the layer it
         * named comes unhooked from the tree and its strokes fall back
         * to file order — kept, but not where they were. */
        scene_item item;
        rm_status status = parse_group_item(ctx, &inner, &item);
        if (status == RM_OK) {
          if (!rm_vec_push(&ctx->items, &item)) return RM_ERR_OUT_OF_MEMORY;
        } else {
          ctx->other_blocks += 1;
          ctx_note_failure(ctx, status);
        }
        break;
      }

      case BLOCK_UNDO_TOMBSTONE: {
        /* The tombstone an undo leaves: the same preamble as an erased
         * stroke's, no value. It holds a position in the chain, and
         * dropping it would unhook whatever hangs on it. */
        scene_item item;
        if (parse_scene_tombstone(&inner, &item) == RM_OK) {
          if (!rm_vec_push(&ctx->items, &item)) return RM_ERR_OUT_OF_MEMORY;
        } else {
          ctx->other_blocks += 1;
        }
        break;
      }

      case BLOCK_ATTACHMENT_MAP: {
        /* The names of the page's attachments, keyed the way the image
         * items name them. Unread, a page of captures comes out blank —
         * the tablet rasterises a capture whole, and nothing of it
         * lives anywhere else. */
        rm_status status = parse_attachment_map(ctx, &inner);
        if (status != RM_OK) {
          if (status == RM_ERR_OUT_OF_MEMORY) return status;
          ctx->other_blocks += 1;
        }
        break;
      }

      case BLOCK_IMAGE_ITEM: {
        /* A picture that will not parse is a lost picture, and a
         * picture is the whole of a captured page: counted, and its
         * failure kept for the log, the same as a stroke's. */
        scene_item item;
        rm_status status = parse_image_item(ctx, &inner, &item);
        if (status == RM_OK) {
          if (!rm_vec_push(&ctx->items, &item)) return RM_ERR_OUT_OF_MEMORY;
        } else {
          ctx->other_blocks += 1;
          ctx_note_failure(ctx, status);
        }
        break;
      }

      case BLOCK_TREE_NODE: {
        tree_node node;
        if (parse_tree_node(&inner, block_end, &node) == RM_OK) {
          if (node.has_anchor) {
            node_anchor anchor = {
                .anchor = node.anchor,
                .has_origin_x = node.has_origin_x,
                .origin_x = node.origin_x,
            };
            if (!rm_vec_push(&ctx->node_anchors, &anchor)) {
              return RM_ERR_OUT_OF_MEMORY;
            }
            if (!rm_idmap_set(&ctx->anchor_of_node, node.id,
                              ctx->node_anchors.count - 1u)) {
              return RM_ERR_OUT_OF_MEMORY;
            }
          }
          RM_TRY(apply_visibility(ctx, &node));
        } else {
          ctx->other_blocks += 1;
        }
        break;
      }

      case BLOCK_ROOT_TEXT: {
        bool found = false;
        rm_status status = parse_root_text(ctx, &inner, block_end, &found);
        if (status == RM_ERR_OUT_OF_MEMORY) return status;
        /* A text block that will not parse is counted, and the text of
         * an earlier block that did parse stays. A broken item or
         * style list inside a block that parses is not counted. */
        if (!found) ctx->other_blocks += 1;
        break;
      }

      case RM_BLOCK_SCENE_GLYPH_ITEM: {
        /* The marker over the text of an imported PDF. Not a stroke: it
         * carries the highlighted text and ready-made rectangles, so
         * skipping it drops the highlight silently. A highlight the
         * user took off is a tombstone, as ordinary as an erased
         * stroke, and is not counted as a loss — only a highlight that
         * is there and would not read is. */
        bool found = false;
        rm_highlight highlight;
        rm_status status =
            parse_glyph_item(ctx, &inner, block_end, &found, &highlight);
        if (status == RM_ERR_OUT_OF_MEMORY) return status;
        if (status != RM_OK) {
          ctx->other_blocks += 1;
          ctx_note_failure(ctx, status);
        } else if (found) {
          if (!rm_vec_push(&ctx->highlights, &highlight))
            return RM_ERR_OUT_OF_MEMORY;
        } else {
          ctx->deleted_items += 1;
        }
        break;
      }

      case BLOCK_SCENE_INFO:
        /* Never counted: this one does not fail. It reads the fields it
         * knows and leaves the rest at their defaults, so no outcome
         * here loses content. */
        parse_scene_info(ctx, &inner, block_end);
        break;

      case BLOCK_AUTHOR_IDS:
      case BLOCK_MIGRATION_INFO:
      case BLOCK_PAGE_INFO:
        /* The other three of the service quartet. Stepped over by
         * length on purpose and never counted: nothing in them is
         * drawn, so passing them over is not a loss. */
        break;

      default:
        /* A block of a type this parser does not know — the one case
         * where stepping over it by length is all there is to do. */
        ctx->other_blocks += 1;
        break;
    }

    RM_TRY(rm_read_seek(reader, block_end));
  }
  /* Fewer bytes than a block header are left behind: not a block, and not
   * a reason to lose the page that came before them. */
  return RM_OK;
}

/* The header, checked the way every door into a page checks it: bytes too
 * short or of another shape are a format nobody can name, and a header of
 * another generation is named by its version. */
static rm_status check_header(const uint8_t* data, size_t size,
                              rm_error* out_error) {
  if (size < RM_HEADER_LENGTH) {
    /* Too short to hold a header: the stream ends inside the first field
     * there is to read. */
    fail_with(out_error, RM_ERR_UNEXPECTED_EOF, 0, nullptr);
    return RM_ERR_UNEXPECTED_EOF;
  }
  if (memcmp(data, RM_HEADER, RM_HEADER_LENGTH) != 0) {
    /* A page of an older generation is not a broken file, and the two must
     * not arrive as one error: the caller leaves the page out and tells the
     * user which format it is in, and it can only say that about a version
     * it has actually read. */
    int version = 0;
    if (rm_header_version((const char*)data, RM_HEADER_LENGTH, &version)) {
      fail_with(out_error, RM_ERR_UNSUPPORTED_VERSION, version, nullptr);
      return RM_ERR_UNSUPPORTED_VERSION;
    }
    char seen[33] = {0};
    memcpy(seen, data, 32);
    fail_with(out_error, RM_ERR_UNSUPPORTED_FORMAT, 0, seen);
    return RM_ERR_UNSUPPORTED_FORMAT;
  }
  return RM_OK;
}

/* One pass over a page's blocks, everything gathered and nothing yet decided
 * about what is drawn: hidden layers are in here with the rest. On success
 * the context is the caller's to free, arena included; on failure nothing is
 * left behind. */
static rm_status read_blocks(const uint8_t* data, size_t size, parse_ctx* ctx,
                             rm_error* out_error) {
  RM_TRY(check_header(data, size, out_error));

  *ctx = (parse_ctx){
      .background_visible = true,
      .root_document_visible = true,
  };
  ctx->arena = rm_arena_create();
  if (ctx->arena == nullptr) {
    fail_with(out_error, RM_ERR_OUT_OF_MEMORY, 0, nullptr);
    return RM_ERR_OUT_OF_MEMORY;
  }
  rm_reader_init(&ctx->reader, data, size);
  ctx->reader.offset = RM_HEADER_LENGTH;
  rm_vec_init(&ctx->items, sizeof(scene_item));
  rm_vec_init(&ctx->tree_events, sizeof(tree_event));
  rm_vec_init(&ctx->attachments, sizeof(attachment));
  rm_keymap_init(&ctx->attachment_index);
  rm_vec_init(&ctx->node_anchors, sizeof(node_anchor));
  rm_vec_init(&ctx->highlights, sizeof(rm_highlight));
  rm_idmap_init(&ctx->anchor_of_node);
  rm_idmap_init(&ctx->hidden_nodes);
  rm_idmap_init(&ctx->visibility_of_node);
  rm_vec_init(&ctx->visibility_stamps, sizeof(rm_crdt_id));

  rm_status status = parse_blocks(ctx);
  if (status != RM_OK) {
    ctx_free(ctx);
    rm_arena_destroy(ctx->arena);
    fail_with(out_error, status, 0, nullptr);
  }
  return status;
}

rm_status rm_parse_page(const uint8_t* data, size_t size, rm_page** out_page,
                        rm_error* out_error) {
  if (out_error != nullptr) *out_error = (rm_error){.status = RM_OK};
  if (data == nullptr || out_page == nullptr) {
    fail_with(out_error, RM_ERR_INVALID_ARGUMENT, 0, nullptr);
    return RM_ERR_INVALID_ARGUMENT;
  }

  parse_ctx ctx;
  RM_TRY(read_blocks(data, size, &ctx, out_error));

  /* Only the unread must not be passed over in silence: that is a missing
   * piece of what was written. Deleted items alone do not call for a line —
   * there are many of them and they are normal — though their count rides
   * along when one is written. The other blocks are counted in
   * `skipped_blocks` on the same terms as a stroke, so they are said on the
   * same terms too: a lost image or an unhooked layer that reached the count
   * and not the log would be found months later, in somebody's page. */
  if (ctx.unreadable_strokes + ctx.other_blocks > 0) {
    RM_LOG_ERRORF(
        "page: %zu scene items, %zu unreadable, %zu deleted, %zu other blocks; "
        "first failure: %s",
        ctx.items.count, ctx.unreadable_strokes, ctx.deleted_items,
        ctx.other_blocks,
        ctx.first_failure[0] == '\0' ? "-" : ctx.first_failure);
  }

  rm_status status;
  rm_page* page = rm_arena_alloc(ctx.arena, sizeof *page);
  if (page == nullptr) {
    status = RM_ERR_OUT_OF_MEMORY;
    goto fail;
  }
  memset(page, 0, sizeof *page);
  status = assemble(&ctx, page);
  if (status != RM_OK) goto fail;

  ctx_free(&ctx);
  *out_page = page;
  return RM_OK;

fail:
  ctx_free(&ctx);
  rm_arena_destroy(ctx.arena);
  fail_with(out_error, status, 0, nullptr);
  return status;
}

/* A node under its parent, as a group item or a tree record puts it. */
typedef struct tree_link {
  rm_crdt_id parent;
  rm_crdt_id child;
} tree_link;

/* The grouping key for a link, by its position in the links: the node it
 * hangs on. */
static rm_crdt_id link_parent_key(size_t position, void* ctx) {
  const tree_link* links = ctx;
  return links[position].parent;
}

/* Every node taken out of the tree, and everything under one, added to
 * `tree->removed`: a group nested in a removed layer is as gone as the layer.
 * A node is under the parent the tree records last put it under: a group
 * item left behind by a move links nothing, and a node moved without a group
 * item of its own is still under its parent. Walked with a queue of its own,
 * and each node once, so a cycle in the links ends the walk. */
static bool removed_subtrees(const parse_ctx* ctx, tree_state* tree) {
  const scene_item* items = ctx->items.data;
  bool ok = false;
  rm_vec links, queue;
  rm_vec_init(&links, sizeof(tree_link));
  rm_vec_init(&queue, sizeof(rm_crdt_id));
  rm_grouping children_of = {0};

  for (size_t i = 0; i < ctx->items.count; i += 1) {
    if (items[i].deleted != 0 || items[i].kind != ITEM_GROUP) continue;
    if (!tree_links(tree, items[i].parent, items[i].group)) continue;
    tree_link link = {.parent = items[i].parent, .child = items[i].group};
    if (!rm_vec_push(&links, &link)) goto done;
  }
  size_t cursor = 0;
  rm_crdt_id node;
  uint64_t index;
  while (rm_idmap_next(&tree->placed_by, &cursor, &node, &index)) {
    tree_link link = {.parent = tree->events[index].parent, .child = node};
    if (!rm_vec_push(&links, &link)) goto done;
  }
  if (!rm_grouping_build(&children_of, links.count, link_parent_key,
                         links.data))
    goto done;

  cursor = 0;
  while (rm_idmap_next(&tree->removed, &cursor, &node, &index)) {
    if (!rm_vec_push(&queue, &node)) goto done;
  }
  const tree_link* all = links.data;
  while (queue.count > 0) {
    node = *(rm_crdt_id*)rm_vec_at(&queue, queue.count - 1);
    queue.count -= 1;
    const uint32_t* children;
    size_t count;
    if (!rm_grouping_find(&children_of, node, &children, &count)) continue;
    for (size_t k = 0; k < count; k += 1) {
      rm_crdt_id child = all[children[k]].child;
      if (rm_idmap_contains(&tree->removed, child)) continue;
      if (!rm_idmap_set(&tree->removed, child, 1) ||
          !rm_vec_push(&queue, &child))
        goto done;
    }
  }
  ok = true;

done:
  rm_grouping_free(&children_of);
  rm_vec_free(&queue);
  rm_vec_free(&links);
  return ok;
}

/* Every living mark of the highlighter: its strokes on every layer the tablet
 * still holds — a hidden one included, since showing it again brings them
 * back — and every highlight over a text layer. Strokes on a layer taken out
 * of the tree are left out: no switch brings them back. */
rm_status rm_scan_highlighter_items(const uint8_t* data, size_t size,
                                    rm_vec* out_ids, rm_error* out_error) {
  parse_ctx ctx;
  RM_TRY(read_blocks(data, size, &ctx, out_error));

  /* A page with a block that would not read is refused rather than answered
   * in part: the block lost may be a mark, and "every mark" less one is not
   * every mark. */
  size_t lost = ctx.unreadable_strokes + ctx.other_blocks;
  if (lost > 0) {
    ctx_free(&ctx);
    rm_arena_destroy(ctx.arena);
    char message[80];
    snprintf(message, sizeof message, "%zu blocks of the page would not read",
             lost);
    fail_with(out_error, RM_ERR_INCOMPLETE_PAGE, 0, message);
    if (out_error != nullptr) out_error->value = (long)lost;
    return RM_ERR_INCOMPLETE_PAGE;
  }

  rm_status status = RM_ERR_OUT_OF_MEMORY;
  tree_state tree;
  rm_idmap seen;
  rm_idmap_init(&seen);
  if (!tree_state_build(&ctx, &tree)) goto done;
  if (!removed_subtrees(&ctx, &tree)) goto done;
  const rm_idmap* removed = &tree.removed;

  /* In file order, strokes first, and each identifier once: an order that
   * does not change between two calls on the same bytes. */
  const scene_item* items = ctx.items.data;
  for (size_t i = 0; i < ctx.items.count; i += 1) {
    if (items[i].kind != ITEM_STROKE || items[i].deleted != 0) continue;
    if (!rm_pen_tool_is_highlighter(items[i].stroke.tool)) continue;
    if (rm_idmap_contains(removed, items[i].parent)) continue;
    if (rm_idmap_contains(&seen, items[i].id)) continue;
    rm_item_id id = rm_item_id_of(items[i].id);
    if (!rm_idmap_set(&seen, items[i].id, 1) || !rm_vec_push(out_ids, &id))
      goto done;
  }
  for (size_t i = 0; i < ctx.highlights.count; i += 1) {
    const rm_highlight* highlight = rm_vec_at(&ctx.highlights, i);
    rm_crdt_id key = rm_crdt_of(highlight->id);
    if (rm_idmap_contains(&seen, key)) continue;
    if (!rm_idmap_set(&seen, key, 1) || !rm_vec_push(out_ids, &highlight->id))
      goto done;
  }
  status = RM_OK;

done:
  rm_idmap_free(&seen);
  tree_state_free(&tree);
  ctx_free(&ctx);
  rm_arena_destroy(ctx.arena);
  if (status != RM_OK) fail_with(out_error, status, 0, nullptr);
  return status;
}

void rm_page_free(rm_page* page) {
  if (page == nullptr) return;
  /* The page is itself inside the arena, so this is the last read of it. */
  rm_arena_destroy(page->arena);
}

rm_status rm_page_set_template(rm_page* page, const char* name) {
  if (page == nullptr) return RM_ERR_INVALID_ARGUMENT;
  if (name == nullptr) {
    page->has_template_name = false;
    page->template_name = (rm_str){.bytes = "", .length = 0};
    return RM_OK;
  }
  size_t length = strlen(name);
  char* copy = rm_arena_strndup(page->arena, name, length);
  if (copy == nullptr) return RM_ERR_OUT_OF_MEMORY;
  page->has_template_name = true;
  page->template_name = (rm_str){.bytes = copy, .length = length};
  return RM_OK;
}
