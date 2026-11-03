/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* The vocabulary of a notebook page: what the `.rm` v6 format holds once it
 * has been read, and what a page to be written is made of.
 *
 * Nothing here draws. A width in file units divided by four is a width on the
 * page (`RM_INK_UNITS_PER_RAW_WIDTH`); what an index of the palette *looks*
 * like is a matter of the tablet's own rendering and belongs to a renderer.
 */

#ifndef RMLINES_TYPES_H
#define RMLINES_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rmlines/export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

/* The two shapes the format needs. Spelled here rather than taken from a
 * platform: a platform's own rectangle and size come with a tie to that
 * platform, and a library of bytes should not need one. */
typedef struct rm_rect {
  double x, y, width, height;
} rm_rect;

typedef struct rm_size {
  double width, height;
} rm_size;

/* A stretch of bytes the library owns: UTF-8, and NUL-terminated as well as
 * counted, so that it reaches `printf` without a copy. `length` excludes the
 * terminator. */
typedef struct rm_str {
  const char* bytes;
  size_t length;
} rm_str;

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

typedef enum rm_status : int {
  RM_OK = 0,
  /* The stream is shorter than a header, or a block declares a length past
   * the end of the file. A field that fails inside a block costs that block
   * and is counted — save a broken list of text items or paragraph styles,
   * which is dropped uncounted — and fewer than eight bytes left over after
   * the last block are ignored. */
  RM_ERR_UNEXPECTED_EOF,
  /* The bytes do not begin the way a page does at all. The first of them
   * travel in `rm_error.message`, because a header nobody recognises is the
   * one thing worth looking at. */
  RM_ERR_UNSUPPORTED_FORMAT,
  /* A page of the format all right, written to a version this reader does
   * not parse; `rm_error.version` is the number in its header.
   *
   * Told apart from RM_ERR_UNSUPPORTED_FORMAT because the answers differ.
   * This one is a known shape of file, and the honest answer is to leave
   * the page out and say which format it is in; the other is bytes we
   * cannot name, and lumping the two together would have an application
   * tell a user about "an older format" it never read a version out of. */
  RM_ERR_UNSUPPORTED_VERSION,
  /* A block that would not read. Raised by the individual block parsers and
   * never returned to the caller of `rm_parse_page`: a page is not lost for
   * one stroke, so the block is stepped over and the loss is counted in
   * `rm_page.skipped_blocks`. */
  RM_ERR_MALFORMED_PAGE,
  RM_ERR_OUT_OF_MEMORY,
  /* Writing: a mark or a placed image is fastened to a paragraph the draft
   * does not have. Written anyway, the ink would hang on nothing and vanish
   * without a trace. */
  RM_ERR_ANCHOR_OUT_OF_RANGE,
  /* Writing: more items of one kind than a page's identifier ranges can
   * name. Past the limit two different items would be handed one counter,
   * and the file cannot tell two meanings of one counter apart. */
  RM_ERR_TOO_MANY_TO_NUMBER,
  RM_ERR_INVALID_ARGUMENT,
  /* Editing: an identifier asked for is not on the page as a living stroke
   * or highlight — the page is not the one it was read from, or the item is
   * gone already. The edit does not guess which. `rm_error.value` is how
   * many were missing and `rm_error.message` names them. */
  RM_ERR_ITEMS_NOT_FOUND,
  /* Editing: the edited page did not read back as the edit meant it to —
   * the last check before the bytes leave, and a refusal rather than a page
   * that might be damaged. `rm_error.message` says what disagreed. */
  RM_ERR_UNVERIFIED,
  /* Editing: this many blocks of the page would not read
   * (`rm_error.value`), so what the page holds cannot be told in full. The
   * block lost may be a mark, and "every mark" less one is not every mark. */
  RM_ERR_INCOMPLETE_PAGE,
} rm_status;

/* What went wrong, with the one number or phrase that makes it actionable.
 * Every field but `status` is only as meaningful as `status` says. */
typedef struct rm_error {
  rm_status status;
  /* RM_ERR_UNSUPPORTED_VERSION: the generation the header named. */
  int version;
  /* RM_ERR_ANCHOR_OUT_OF_RANGE: the paragraph that was not there.
   * RM_ERR_TOO_MANY_TO_NUMBER: the limit that was passed.
   * RM_ERR_INCOMPLETE_PAGE: how many blocks would not read. */
  long value;
  /* RM_ERR_UNSUPPORTED_FORMAT: the first 32 bytes of the header.
   * Otherwise: what the block parser had to say. Always NUL-terminated. */
  char message[160];
} rm_error;

/* The status as a bare English phrase, for a log line. Never null. */
RM_API const char* rm_status_string(rm_status status);

/* ------------------------------------------------------------------ */
/* Tools and colours                                                   */
/* ------------------------------------------------------------------ */

/* The tool a stroke was made with. The numbers come from the tablet's own
 * format; RM_TOOL_UNKNOWN is this library's, for a number it has not seen.
 *
 * How a tool draws — how it blends over what is already drawn, and whether it
 * is stroked instead of filled — belongs to a renderer. */
typedef enum rm_pen_tool : int32_t {
  RM_TOOL_UNKNOWN = -1,
  RM_TOOL_PAINTBRUSH_1 = 0,
  RM_TOOL_PENCIL_1 = 1,
  RM_TOOL_BALLPOINT_1 = 2,
  RM_TOOL_MARKER_1 = 3,
  RM_TOOL_FINELINER_1 = 4,
  RM_TOOL_HIGHLIGHTER_1 = 5,
  RM_TOOL_ERASER = 6,
  RM_TOOL_MECHANICAL_PENCIL_1 = 7,
  RM_TOOL_ERASER_AREA = 8,
  RM_TOOL_PAINTBRUSH_2 = 12,
  RM_TOOL_MECHANICAL_PENCIL_2 = 13,
  RM_TOOL_PENCIL_2 = 14,
  RM_TOOL_BALLPOINT_2 = 15,
  RM_TOOL_MARKER_2 = 16,
  RM_TOOL_FINELINER_2 = 17,
  RM_TOOL_HIGHLIGHTER_2 = 18,
  RM_TOOL_CALIGRAPHY = 21,
  RM_TOOL_SHADER = 23,
} rm_pen_tool;

/* The eraser must not be drawn. Its strokes look like ordinary lines, and
 * drawing them covers the page in lines exactly where the user was erasing. */
RM_API bool rm_pen_tool_is_eraser(rm_pen_tool tool);

/* The marker, whose band says something about what lies under it.
 *
 * Over the text of an imported PDF the tablet does not use this tool at all:
 * it writes a glyph range with the words spelled out (`rm_highlight`). Over
 * handwriting, and over a scan with no text layer to snap to, it writes an
 * ordinary stroke — so a band of this tool is the only record that anything
 * was marked there. */
RM_API bool rm_pen_tool_is_highlighter(rm_pen_tool tool);

/* A stroke's colour: an index into the tablet's palette.
 *
 * How an index is coloured belongs to a renderer: the tablet keeps three
 * palettes over one set of indexes (pen, highlighter, text highlight), and
 * each has to be taken from the tablet's own output rather than guessed. */
typedef enum rm_pen_color : int32_t {
  RM_COLOR_BLACK = 0,
  RM_COLOR_GRAY = 1,
  RM_COLOR_WHITE = 2,
  RM_COLOR_YELLOW = 3,
  RM_COLOR_GREEN = 4,
  RM_COLOR_PINK = 5,
  RM_COLOR_BLUE = 6,
  RM_COLOR_RED = 7,
  RM_COLOR_GRAY_OVERLAP = 8,
  RM_COLOR_HIGHLIGHT = 9,
  RM_COLOR_GREEN_2 = 10,
  RM_COLOR_CYAN = 11,
  RM_COLOR_MAGENTA = 12,
  RM_COLOR_YELLOW_2 = 13,
  /* The highlighter's blue and orange under indexes of their own. The
   * tablet's palette names them (`HighlighterBlue = 0xFFBEEAFE`,
   * `HighlighterOrange = 0xFFFFC38C`), and the tablet writes them in `tag 2`
   * with no `argb` beside it; some pages spell the same two colours as 9
   * plus `argb`. Index 16 (`LastColor`) closes the enumeration and is no
   * colour at all. */
  RM_COLOR_HIGHLIGHTER_BLUE = 14,
  RM_COLOR_HIGHLIGHTER_ORANGE = 15,
} rm_pen_color;

/* ------------------------------------------------------------------ */
/* The scales the format itself is written in                          */
/* ------------------------------------------------------------------ */

/* What the numbers in the file mean before anything is drawn. Apart from the
 * drawing on purpose: these four say how to *read* a stroke. */

/* What the width in the file is divided by to become a width on the page.
 * The ratio is exact: on the two tools the tablet strokes rather than fills,
 * the drawn line width is the file's width divided by it. */
constexpr double RM_INK_UNITS_PER_RAW_WIDTH = 4.0;
/* Thin strokes have to stay visible in print; the format's own smallest is
 * 1.5 units, so this never fires on a width the tablet wrote. */
constexpr double RM_INK_MINIMUM_STROKE_WIDTH = 0.7;
/* A guard against a number that is not a width at all. It sits far above
 * anything the tablet writes: its widest tool is 128 raw, which is 32 units. */
constexpr double RM_INK_MAXIMUM_STROKE_WIDTH = 200.0;
/* How opaque a translucent stroke is when the file does not say: byte 0x40 of
 * the shader's own `argb`, the opacity the tablet draws the shader with. */
constexpr double RM_INK_FAINT_ALPHA = 0.25098;

/* ------------------------------------------------------------------ */
/* Typed text                                                          */
/* ------------------------------------------------------------------ */

/* How a paragraph of typed text is presented. The numbers are the tablet's
 * own; RM_PARA_UNKNOWN is this library's. Anything
 * unknown is read as plain text rather than guessed at — a wrong marker is
 * worse than none. */
typedef enum rm_paragraph_style : int32_t {
  RM_PARA_UNKNOWN = -1,
  RM_PARA_BASIC = 0,
  RM_PARA_PLAIN = 1,
  RM_PARA_HEADING = 2,
  /* Headings of the second and the third level: the tablet has three, and
   * `tag 1` covers the lower two. Which one it is, `tag 2` and `tag 3` say
   * (`rm_paragraph_format_heading_level`). */
  RM_PARA_SUBHEADING = 3,
  RM_PARA_BULLET = 4,
  RM_PARA_BULLET_NESTED = 5,
  RM_PARA_CHECKBOX = 6,
  RM_PARA_CHECKBOX_CHECKED = 7,
  /* An item of a list nested under another. The value says the family, and
   * `tag 3` of the same record says how deep. The bullet's nested value is
   * 5; reading that as a *filled* bullet on the strength of its name alone
   * would be wrong — it is not a shape at all, the shape follows the
   * level. */
  RM_PARA_CHECKBOX_NESTED = 8,
  /* A ticked checkbox of a nested list — the ticked counterpart of
   * RM_PARA_CHECKBOX_NESTED (`CheckboxCheckedIndented`). */
  RM_PARA_CHECKBOX_CHECKED_NESTED = 9,
  /* A numbered list. The number itself is nowhere in the file — the tablet
   * counts the consecutive items and so must a renderer. */
  RM_PARA_NUMBERED = 10,
  RM_PARA_NUMBERED_NESTED = 11,
  /* The paragraph that holds an image in the flow of the text: one object
   * replacement character between two newlines. How it is drawn is not
   * known. */
  RM_PARA_IMAGE_BLOCK = 12,
} rm_paragraph_style;

/* Which family of list a style belongs to, whatever its level. */
typedef enum rm_list_family : int32_t {
  RM_LIST_NONE = -1,
  RM_LIST_BULLET = 0,
  RM_LIST_CHECKBOX = 1,
  RM_LIST_NUMBERED = 2,
} rm_list_family;

/* Which family of list the style belongs to, or RM_LIST_NONE when it is not
 * one of a list. */
RM_API rm_list_family rm_paragraph_style_list_family(rm_paragraph_style style);
/* Whether the paragraph is one of a list: those get a marker drawn in front
 * of them and their text moved out of its way. */
RM_API bool rm_paragraph_style_is_list_item(rm_paragraph_style style);

/* The whole style record of a paragraph, not merely its first field.
 *
 * Two paragraphs both carrying `tag 1 = 3` are drawn by the tablet in
 * different sizes, and the only thing telling them apart is whether the
 * record goes on:
 *
 *     {1: 2}              heading 1          size 64
 *     {1: 3, 2: 2, 3: 3}  heading 2          size 46
 *     {1: 3}              heading 3          size 36
 *
 * What `tag 2` and `tag 3` mean apart is unknown, and taking them apart is
 * not needed: it is enough to tell the sets of
 * fields from one another. */
typedef struct rm_paragraph_format {
  /* `tag 1` as it stands in the file, known to us or not. A record with a
   * value we do not know is kept rather than dropped: dropping it loses the
   * paragraph's own indent along with the marker. */
  uint8_t raw;
  bool has_second;
  uint8_t second;
  bool has_third;
  uint32_t third;
} rm_paragraph_format;

/* The kind of paragraph, or RM_PARA_UNKNOWN for a value we do not know. */
RM_API rm_paragraph_style
rm_paragraph_format_style(const rm_paragraph_format* format);
/* Whether the record carried anything past `tag 1`. */
RM_API bool rm_paragraph_format_has_further_fields(
    const rm_paragraph_format* format);
/* Which level of heading the paragraph is, or 0 when it is not one.
 *
 * The tablet has three. The first has its own `tag 1` value; the second and
 * the third share the value 3 and are told apart by the `tag 2`/`tag 3` pair,
 * present on the second and absent on the third. */
RM_API int rm_paragraph_format_heading_level(const rm_paragraph_format* format);
/* How deep an item of a list is nested, counting from one.
 *
 * The first level carries no `tag 3`, or — a checkbox item may do this —
 * one at depth nought with state bits above it; a nested one packs the family
 * into the high half of the byte and the depth into the low half:
 *
 *     bullet         0x01  second level     0x02  third
 *     checkbox       0x11  second level     0x12  third
 *     numbered       0x21  second level     0x22  third
 */
RM_API int rm_paragraph_format_level(const rm_paragraph_format* format);
/* Whether the item's checkbox is ticked.
 *
 * The style says so for an item with nothing nested under it (7, 9). The high
 * half of `tag 3` says so for every item: bit 8 is the tick — and it is the
 * only place an item with nested items keeps it, because such an item stays
 * style 6 whatever its state (0x310 is drawn ticked, 0x210 not). */
RM_API bool rm_paragraph_format_is_ticked(const rm_paragraph_format* format);
/* Whether any item nested under this one is ticked — bit 9 of `tag 3`. It
 * stands on a parent with all of its nested items ticked (0x310, beside
 * bit 8) and on one with some ticked (0x210), while a parent with none
 * ticked carries no `tag 3` at all. */
RM_API bool rm_paragraph_format_has_ticked_nested_items(
    const rm_paragraph_format* format);

/* A stretch of a line set in a face of its own.
 *
 * Bold and italic are not properties of a paragraph: they are marked up on
 * the characters, by a pair of empty text records fencing the stretch off.
 * The range counts Unicode scalars of the line it belongs to, from nought,
 * and is half-open: [start, end). */
typedef struct rm_face_run {
  int32_t start;
  int32_t end;
  bool bold;
  bool italic;
} rm_face_run;

/* One paragraph as it reads, with everything the file says about it.
 *
 * The text, the style and the faces are gathered onto the line itself rather
 * than kept in parallel tables keyed by line number, so they cannot drift
 * apart, and a caller walking the text has one thing to walk. */
typedef struct rm_paragraph {
  rm_str text;
  bool has_format;
  rm_paragraph_format format;
  /* The bold and italic stretches of this line. Plain text yields none:
   * most of a page is plain. */
  const rm_face_run* faces;
  size_t face_count;
} rm_paragraph;

/* Typed text on a page.
 *
 * The tablet lets one type as well as write, and such a page can carry no
 * strokes at all — only this. A renderer that ignores it shows a typed
 * notebook as blank.
 *
 * An image placed in the flow of the text is one character of
 * it, U+FFFC OBJECT REPLACEMENT CHARACTER, so that the offsets after it are the
 * tablet's. Which image it is the file says through the attachment map; it is
 * not drawn here. */
typedef struct rm_text_block {
  /* Where the text begins, in the page's own coordinates: x counted from
   * the middle of the sheet, y growing downwards. */
  double origin_x;
  double origin_y;
  const rm_paragraph* paragraphs;
  size_t paragraph_count;
  /* How wide the column of text is, in page units, when the file declares
   * it. The tablet writes it twice — as this field and through the origin —
   * and this one is the better of the two: deriving the width from the
   * origin holds only while the column is centred on the sheet. */
  bool has_column_width;
  double column_width;
} rm_text_block;

/* How a checkbox of a list is shown.
 *
 * Not the paragraph style alone: an item with items nested under it keeps the
 * style of an unticked box (6) and is shown by the tablet from what is under it
 * — ticked and struck through when every nested item is ticked, a box with a
 * dash when only some are. The file says which in the high bits of `tag 3`
 * (`rm_paragraph_format_is_ticked`,
 * `rm_paragraph_format_has_ticked_nested_items`). RM_CHECKBOX_NONE is this
 * library's, for a line that is not an item of a checkbox list. */
typedef enum rm_checkbox_state : int32_t {
  RM_CHECKBOX_NONE = -1,
  RM_CHECKBOX_UNTICKED = 0,
  RM_CHECKBOX_TICKED = 1,
  /* Some of the nested items are ticked and some are not. */
  RM_CHECKBOX_PARTIAL = 2,
} rm_checkbox_state;

/* How the checkbox of a paragraph is shown, or RM_CHECKBOX_NONE when the
 * paragraph is not an item of a checkbox list (or is past the last one).
 *
 * Ticked when the file says so; partial when it says something under the item
 * is ticked. Without those bits — a parent with nothing ticked under it, or a
 * page that does not write them — the nested items themselves are read:
 * partial when anything under the item is ticked, unticked when nothing is. */
RM_API rm_checkbox_state rm_text_block_checkbox_state(const rm_text_block* text,
                                                      size_t paragraph);

/* Where in the typed text a stroke of handwriting is fastened.
 *
 * The file fastens it to a **character**, and the tablet counts the stroke's
 * coordinates from the pen: x where that character ends, y on the baseline of
 * the visual line it stands on. Which visual line a character falls on is a
 * matter of the font and the column width, so this stays a place in the text
 * until drawing time, where both are known. */
typedef struct rm_text_anchor {
  /* The paragraph, by its index in `rm_text_block.paragraphs`. */
  int32_t paragraph;
  /* How many of that paragraph's characters stand before the anchor,
   * counted in **Unicode scalars** — the way the file numbers them. Not
   * graphemes: on NFC Latin and Cyrillic the two agree, and on Arabic
   * diacritics they do not, where a grapheme count would shift every anchor
   * after the first mark. */
  int32_t offset;
  /* The horizontal the file itself declares — `anchor_origin_x`, tag 10 of
   * the tree node. Where the layout is computable it agrees with what the
   * layout gives; where it is not — a caret on the boundary of two writing
   * directions — it is the only right answer. The vertical stays computed. */
  bool has_origin_x;
  double origin_x;
} rm_text_anchor;

/* ------------------------------------------------------------------ */
/* What is on the page                                                 */
/* ------------------------------------------------------------------ */

/* The identifier of one item of a page's scene: a stroke, or a highlight over
 * the text of an imported PDF.
 *
 * What the page itself calls the item — the author of the edit and its
 * counter — and so the one handle that still names the same item after the
 * page has been read, measured and put away: an edit made later
 * (`rm_erase_items`) finds the item by it, and by nothing about its looks.
 *
 * Spelled `author:counter` as a string, for a consumer that has to carry it
 * without knowing this type. */
typedef struct rm_item_id {
  uint8_t author;
  uint64_t counter;
} rm_item_id;

/* Whether the two identifiers name the same item. */
RM_API bool rm_item_id_equal(rm_item_id a, rm_item_id b);
/* The `author:counter` spelling, NUL-terminated. Answers how many characters
 * the whole spelling takes, as `snprintf` does; 25 bytes always suffice — 24
 * characters for the widest identifier and its terminator. */
RM_API int rm_item_id_format(rm_item_id id, char* out, size_t size);
/* The identifier out of its `author:counter` spelling, and false for anything
 * else — an author past 255, a counter past 64 bits, a space, a third part.
 * Each number takes one leading sign: a plus on anything, a minus only on a
 * nought. `*out` is left alone on false. */
RM_API bool rm_item_id_parse(const char* text, rm_item_id* out);

/* A point of a stroke. The format also gives a direction, but drawing needs
 * the coordinates, the width, the pressure and — for the second ballpoint,
 * whose ink thins as the pen speeds up — the speed. */
typedef struct rm_stroke_point {
  float x, y, width, pressure;
  /* The pen's speed in version two's scale: the raw uint16. The tablet
   * reads it where a reader would expect the width — the ballpoint's ink
   * density falls from a quarter of it at 5 to 30. Version one's float is
   * scaled as its width is, by four: the scale of a version-one speed is not
   * known. */
  float speed;
} rm_stroke_point;

/* A single pen stroke. */
typedef struct rm_stroke {
  /* RM_TOOL_UNKNOWN when the file named a tool this library does not know. */
  rm_pen_tool tool;
  rm_pen_color color;
  double thickness_scale;
  const rm_stroke_point* points;
  size_t point_count;
  /* The explicit colour from the file, 0xAARRGGBB, when the page carries
   * one. Where it is set it wins over `color`: it is the only record of a
   * colour the palette does not name. */
  bool has_argb;
  uint32_t argb;
  /* The place in the typed text this stroke's coordinates are counted from,
   * when they are counted from the text rather than from the sheet. A place
   * and not a shift, and that is the point: turning one into the other
   * takes a font and a column width, and both belong to drawing. */
  bool has_anchor;
  rm_text_anchor anchor;
  /* What the page calls this stroke: the handle an edit finds it by. */
  rm_item_id id;
  /* The stroke this one is a piece of, when the eraser cut it (`tag 9`).
   *
   * Only the eraser sets it, and only on a shader: every piece left of a
   * shader stroke cut in two carries the original's id — or, when that was a
   * piece already, the original's own source — so the pieces of one stroke
   * name one source however often it was cut. The tablet draws neighbouring
   * pieces of one source as one stroke, so the translucent shader does not
   * deepen where they meet. Absent on every other stroke. */
  bool has_source_id;
  rm_item_id source_id;
} rm_stroke;

/* A highlight over the text of an imported PDF.
 *
 * Not a stroke, and that is the point of the type. Running the marker over
 * the text of a PDF writes a *glyph range*: the text, its place in the text
 * layer, and ready-made rectangles. The stroke recorded beside it is
 * degenerate — two points at one and the same x, for example, while the band
 * the tablet draws runs hundreds of points wide. Draw the stroke and nothing
 * appears; draw these rectangles and it does. */
typedef struct rm_highlight {
  /* The bands, in the page's own coordinates — the same space the strokes
   * live in. */
  const rm_rect* rectangles;
  size_t rectangle_count;
  rm_pen_color color;
  /* The explicit colour some pages carry, as on a stroke: where it is set
   * it wins over `color`. */
  bool has_argb;
  uint32_t argb;
  /* The words under the marker, as the tablet copied them out of the text
   * layer. Not needed to draw — the page underneath carries them — but the
   * only way to say what was highlighted without reading the PDF back. */
  bool has_text;
  rm_str text;
  /* What the page calls this highlight: the handle an edit finds it by. */
  rm_item_id id;
} rm_highlight;

/* One corner of an image's quad: where it stands on the page and which point
 * of the texture it pins there. The texture coordinates run 0…1, so the quad
 * can crop the image as well as move and scale it. */
typedef struct rm_image_vertex {
  double x, y, u, v;
} rm_image_vertex;

/* An image placed on the page — an insertion or an Image Capture.
 *
 * The pixels do not live in the `.rm`: the file names an attachment that lies
 * in the document's index one level deeper than a page —
 * `<document>/<page>/<uuid>.png` — and has to be fetched as a blob of its
 * own. */
typedef struct rm_page_image {
  /* The attachment's file name in the document's index, e.g. "<uuid>.png". */
  rm_str file;
  /* The four corners, in the order the file gives them — the indexes
   * 0 1 2 2 3 0 make a quad of them. */
  rm_image_vertex vertices[4];
  /* How many strokes of the page are drawn before this image. The scene is
   * one sequence and the z-order is the sequence: an image can lie over
   * some strokes and under others. */
  size_t strokes_before;
  /* Where in the typed text the image is fastened, when it is. An image
   * inserted into the text flow hangs on an anchored group exactly the way
   * handwriting does, and its vertices are then counted from the pen at
   * that anchor rather than from the sheet. */
  bool has_anchor;
  rm_text_anchor anchor;
} rm_page_image;

/* A parsed notebook page.
 *
 * Every pointer in here — and in everything it points at — belongs to the
 * page and lives exactly as long as it does. One `rm_page_free` releases the
 * lot; nothing inside is freed on its own. */
typedef struct rm_page {
  const rm_stroke* strokes;
  size_t stroke_count;
  /* Marker over the text of an imported PDF. Empty on a notebook, which has
   * no text layer to highlight. */
  const rm_highlight* highlights;
  size_t highlight_count;
  /* The images placed on the page, in the scene's order. A page of captures
   * carries nothing else — unread, it would come out blank. */
  const rm_page_image* images;
  size_t image_count;
  /* The typed text, if the page has any. */
  bool has_text;
  rm_text_block text;
  /* How many strokes were positioned from the text rather than from the
   * sheet. Diagnostic, like `skipped_blocks`: anchored handwriting that
   * fails to find its anchor stays at its raw coordinates and lands on top
   * of whatever is there, which looks like a drawing bug rather than like a
   * parsing one. A number makes the difference visible. */
  size_t anchored_strokes;
  /* The blocks we did not understand: unknown types, and known ones whose
   * body failed to parse. Counted so that a silent loss of content is
   * visible. The three service blocks every page opens with are not counted
   * — nothing in them is drawn, so stepping over them is no loss. */
  size_t skipped_blocks;
  /* The page's template. The name comes from the document's `.content`
   * rather than from the page itself: a `.rm` file knows nothing about its
   * own background, so the parser never sets this — `rm_page_set_template`
   * is for the layer that does hold documents. */
  bool has_template_name;
  rm_str template_name;
  /* The sheet size declared by the page itself in the scene info block.
   * More accurate than what `.content` writes; absent on files without such
   * a block. */
  bool has_paper_size;
  rm_size paper_size;
  /* Whether the page's template is shown. The user can hide the background
   * on the tablet, and a renderer that ignores the flag shows the template
   * anyway on a page that hid it — content the tablet does not show. */
  bool background_visible;
  /* Whether the imported document behind the page is shown. Same block,
   * same reason: hiding the source PDF is the user's own edit. */
  bool root_document_visible;

  /* Private: the arena every allocation above came out of. */
  struct rm_arena* arena;
} rm_page;

/* ------------------------------------------------------------------ */
/* The typed-text column                                               */
/* ------------------------------------------------------------------ */

/* The width of the typed-text column — the setting the tablet offers under
 * the text tool, and one of exactly three values.
 *
 * It reaches the file three times over: as the origin of the text box, as the
 * declared width of that box, and as `tag 9` of the scene block, which stores
 * the setting rather than the width. The three have to agree. */
typedef enum rm_text_column : int32_t {
  RM_COLUMN_NARROW = 0,
  RM_COLUMN_MEDIUM = 1,
  RM_COLUMN_WIDE = 2,
} rm_text_column;

/* The width of the box in the file's own units. The column is centred on the
 * sheet, so the origin is half of it to the left. */
RM_API float rm_text_column_width(rm_text_column column);
/* `tag 9` of SceneInfo: which of the three the user chose. */
RM_API uint8_t rm_text_column_setting(rm_text_column column);
/* The widest of the three that stands on a sheet **with a margin**: on a Move
 * only the narrow one does.
 *
 * The margin is the whole of the rule, and without it the answer is wrong on
 * exactly one device. A Move's sheet is 954 units and the medium column is
 * 936: it "fits" with nine units to spare on each side, which is no margin at
 * all. The tablet's own widest choice shows what it considers one: on a
 * reMarkable 2, `wide` is 1152 of 1404, leaving 9% of the sheet on each
 * side. */
RM_API rm_text_column rm_text_column_widest_fitting(double sheet_width);

#ifdef __cplusplus
}
#endif

#endif /* RMLINES_TYPES_H */
