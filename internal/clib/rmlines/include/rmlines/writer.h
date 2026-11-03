/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Writing what the tablet accepts — the other half of this library's charter,
 * and the narrower one. The writer builds **fresh pages only**: it never
 * edits an existing file, because inserting into somebody else's CRDT chains
 * is a different contract with a user's notebook at stake — that contract is
 * `editor.h`'s.
 *
 * The subset here is exactly what the tablet is known to accept: a typed-text
 * page with anchored and loose strokes, images, one layer, one author (two
 * when the text carries bold or italic). A new block type belongs here only
 * once its bytes are known and the tablet accepts them — the parser's
 * tolerance is no licence for the writer, which must produce what the tablet
 * takes, not what we can read. */

#ifndef RMLINES_WRITER_H
#define RMLINES_WRITER_H

#include "rmlines/export.h"
#include "rmlines/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The sheet the page declares, the three ways the tablet declares it. Which
 * sheet it is decides where absolute units land — a page claiming the legacy
 * 1404 × 1872 puts its text in the side area of a Paper Pro Move. */
typedef struct rm_draft_sheet {
  /* `tag 5` of SceneInfo — the size as whole units. */
  uint32_t screen_width;
  uint32_t screen_height;
  /* `tags 7` and `8` — the same size unrounded, and the one the format
   * calls authoritative. */
  double canvas_width;
  double canvas_height;
} rm_draft_sheet;

/* The typed-text column: its width in the file's own units, and which of the
 * tablet's three settings that is — `tag 9` stores the setting, not the
 * width. The two have to agree; `rm_draft_column_of` is how a caller is kept
 * from inventing a fourth or pairing a width with the wrong setting. */
typedef struct rm_draft_column {
  float width;
  uint8_t setting;
} rm_draft_column;

/* The width and the setting that go together for this choice of column. */
RM_API rm_draft_column rm_draft_column_of(rm_text_column column);

/* One paragraph of the typed text. */
typedef struct rm_draft_paragraph {
  /* NUL-terminated UTF-8. Must not contain a newline: a paragraph is a
   * paragraph, and the writer puts the newlines in. Ill-formed bytes are
   * written as U+FFFD, by the Unicode "maximal subpart" rule. */
  const char* text;
  /* The `tag 1` style value; absent writes no style record (plain body). */
  bool has_style;
  uint8_t style;
  /* `tag 2` / `tag 3` of the style record (emphasised paragraphs, nested
   * lists). */
  bool has_second;
  uint8_t second;
  bool has_third;
  uint32_t third;
  bool bold;
  bool italic;
} rm_draft_paragraph;

/* One sample of a stroke, with the raw numbers the file stores beside the
 * coordinates. The drawn half-width is `width / 4` in sheet units; the writer
 * stores, it does not interpret. */
typedef struct rm_draft_point {
  float x, y;
  uint16_t speed;
  uint16_t width;
  uint8_t direction;
  uint8_t pressure;
} rm_draft_point;

/* The defaults: a width of 12 and a pressure of 6. */
RM_API rm_draft_point rm_draft_point_make(float x, float y);

/* One stroke: the tool, the colour, and the points as written. */
typedef struct rm_draft_stroke {
  rm_pen_tool tool;
  /* The palette index, `tag 2` of the stroke. */
  rm_pen_color color;
  /* `tag 8`, 0xAARRGGBB. Set only where an index cannot name the colour:
   * the shader's colours. The highlighter's blue and orange have indexes of
   * their own (RM_COLOR_HIGHLIGHTER_BLUE, _ORANGE), which the tablet writes
   * with no `argb`; some pages spell them as 9 plus `argb`. */
  bool has_argb;
  uint32_t argb;
  /* `thicknessScale` — the setting on the tablet, 1, 2 or 3, and not a
   * factor: whatever it does to the line is already in each point's
   * `width`. */
  double thickness;
  const rm_draft_point* points;
  size_t point_count;
} rm_draft_stroke;

/* The defaults: no `argb` and a thickness of 2. The
 * points are borrowed, not copied. */
RM_API rm_draft_stroke rm_draft_stroke_make(rm_pen_tool tool,
                                            rm_pen_color color,
                                            const rm_draft_point* points,
                                            size_t point_count);

/* What an anchored stroke hangs on. */
typedef enum rm_anchor_kind : int32_t {
  /* The first character of the paragraph: `anchor_type 1`, threshold 0. */
  RM_ANCHOR_FIRST_CHARACTER,
  /* The sentinel "before the text": a line anchor, `anchor_type 2`,
   * threshold 35.75. */
  RM_ANCHOR_BEFORE_TEXT,
  /* The sentinel "after the text" — just past the last character typed,
   * in the paragraph the last Return closes. */
  RM_ANCHOR_AFTER_TEXT,
} rm_anchor_kind;

typedef struct rm_draft_anchor {
  rm_anchor_kind kind;
  /* Read only for RM_ANCHOR_FIRST_CHARACTER. */
  int32_t paragraph;
} rm_draft_anchor;

/* A stroke hung on the typed text rather than on the sheet: its coordinates
 * are counted from the pen, so the ink follows the text when it reflows. */
typedef struct rm_draft_mark {
  rm_draft_anchor anchor;
  /* `anchor_origin_x` — where the pen stands. The left edge of the text for
   * most anchors; the end of the last line for the sentinel after it. The
   * end of a line is the layout's to know, and the format has no layout —
   * so the number arrives from the caller. */
  float pen_origin;
  rm_draft_stroke stroke;
} rm_draft_mark;

/* A picture placed on the page. The pixels are not in the `.rm`: the file
 * lives beside the page (`<doc>/<page>/<file>`), and the page carries only
 * the 16-byte key and the placement. The key is opaque — minted at insert,
 * not a hash of the pixels — and the UV corners of an uncropped image are
 * plain noughts and ones. */
typedef struct rm_draft_image {
  /* The attachment's file name as the map spells it: `<uuid>.png`,
   * extension included. NUL-terminated UTF-8; ill-formed bytes are written
   * as U+FFFD. */
  const char* file;
  /* The 16 opaque bytes the item and the attachment map share. Sixteen
   * exactly — which is why this is an array and not a pointer and a count:
   * the item and the map have to name the attachment with the same bytes,
   * and a key of another length would be written out all the same, read
   * back as no image at all, and counted nowhere. */
  uint8_t key[16];
  /* The axis-aligned placement, in sheet units: x from the middle, y from
   * the top — or, for an anchored image, both counted from the pen at the
   * anchor, the way a mark's points are. */
  float x, y, width, height;
  /* Fastened to the typed text rather than to the sheet, when set. The
   * tablet hangs such an image on a group carrying the same four anchor
   * fields a mark's group does, and the item's parent is that group instead
   * of the layer. */
  bool has_anchor;
  rm_draft_anchor anchor;
  /* `anchor_origin_x` for the anchored placement, exactly as on a mark.
   * Read only when `has_anchor`. */
  float pen_origin;
} rm_draft_image;

/* A page to be written: everything `rm_write_page` needs, in the same
 * vocabulary the parser answers in.
 *
 * The draft speaks values, not settings: stroke points carry the raw numbers
 * the file stores (a width of 12, not a pen with a ramp), and a mark carries
 * the horizontal its pen stands at — where that number comes from is the
 * caller's business, because the format knows no layout.
 *
 * Every array is borrowed for the duration of the call and copied nowhere. */
typedef struct rm_page_draft {
  /* The page's author, author index 1 of its identifier table. A
   * document's `.content` spells the same UUID, in the same byte order. */
  uint8_t author[16];
  /* The author the formatting marks are written under, when the text
   * carries any.
   *
   * A page with a bold or an italic stretch names a second author: on a
   * tablet's page those marks are another session's edits, and their author
   * index has to resolve in the table. What the value *is* decides nothing
   * that is drawn — only that the index resolves. Left unset, one is
   * derived from `author`, and derived the same way every time: a fresh
   * UUID here would make the same draft write different bytes on every
   * call. */
  bool has_marks_author;
  uint8_t marks_author[16];
  rm_draft_sheet sheet;
  rm_draft_column column;
  const rm_draft_paragraph* paragraphs;
  size_t paragraph_count;
  /* Strokes hung on nothing, in the sheet's own coordinates. */
  const rm_draft_stroke* strokes;
  size_t stroke_count;
  /* Strokes hung on the text. */
  const rm_draft_mark* marks;
  size_t mark_count;
  /* Pictures placed on the page. They stand at the end of the drawing
   * chain, over the strokes — where a freshly inserted image stands on a
   * page the tablet wrote. */
  const rm_draft_image* images;
  size_t image_count;
} rm_page_draft;

/* Write the whole `.rm` file of one page.
 *
 * On success `*out_bytes` is yours until `rm_bytes_free` and `*out_size` says
 * how many. Nothing is written on failure — a half-page is worse than none.
 *
 * Fails with RM_ERR_INVALID_ARGUMENT when the draft, either out parameter, or
 * an array whose count is not nought is null, including a stroke's points
 * (`rm_error.message` names the array); a null pointer beside a count of
 * nought is how a draft says it has none of that kind, and is no error.
 *
 * Fails with RM_ERR_ANCHOR_OUT_OF_RANGE when a mark or an image is fastened
 * to a paragraph the draft has not got (`rm_error.value` names it), and with
 * RM_ERR_TOO_MANY_TO_NUMBER when there are more items of one kind than a
 * page's identifier ranges can name (`rm_error.value` is the limit and
 * `rm_error.message` says of what). */
[[nodiscard]] RM_API rm_status rm_write_page(const rm_page_draft* draft,
                                             uint8_t** out_bytes,
                                             size_t* out_size,
                                             rm_error* out_error);

/* Release what `rm_write_page` handed over. Null is accepted and ignored. */
RM_API void rm_bytes_free(uint8_t* bytes);

#ifdef __cplusplus
}
#endif

#endif /* RMLINES_WRITER_H */
