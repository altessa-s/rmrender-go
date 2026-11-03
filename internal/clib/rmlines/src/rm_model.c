/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* What the enumerations of the format mean: the questions a caller asks of a
 * tool, a paragraph style or a column, answered in one place so that no
 * consumer has to keep its own table of the tablet's numbers. */

#include <stdio.h>

#include "rm_internal.h"
#include "rmlines/parser.h"

const char* rm_status_string(rm_status status) {
  switch (status) {
    case RM_OK:
      return "ok";
    case RM_ERR_UNEXPECTED_EOF:
      return "the stream ended inside a field";
    case RM_ERR_UNSUPPORTED_FORMAT:
      return "these bytes do not begin as a page";
    case RM_ERR_UNSUPPORTED_VERSION:
      return "a page of a generation this reader does not parse";
    case RM_ERR_MALFORMED_PAGE:
      return "a block would not read";
    case RM_ERR_OUT_OF_MEMORY:
      return "out of memory";
    case RM_ERR_ANCHOR_OUT_OF_RANGE:
      return "an anchor names a paragraph the draft has not got";
    case RM_ERR_TOO_MANY_TO_NUMBER:
      return "more items than a page's counters can name";
    case RM_ERR_INVALID_ARGUMENT:
      return "an argument the call cannot be made with";
    case RM_ERR_ITEMS_NOT_FOUND:
      return "items the page does not hold as living ones";
    case RM_ERR_UNVERIFIED:
      return "the edited page did not read back as meant";
    case RM_ERR_INCOMPLETE_PAGE:
      return "blocks of the page would not read";
  }
  return "an error without a name";
}

bool rm_item_id_equal(rm_item_id a, rm_item_id b) {
  return a.author == b.author && a.counter == b.counter;
}

int rm_item_id_format(rm_item_id id, char* out, size_t size) {
  return snprintf(out, size, "%u:%llu", (unsigned)id.author,
                  (unsigned long long)id.counter);
}

/* Digits, into a number no wider than `limit` — read strictly as an unsigned
 * integer out of a string: one leading sign is allowed, a minus only
 * on a nought. Written out rather than left to `strtoull`, which takes leading
 * blanks, a minus on anything and a wrap past the top as a number all the
 * same. */
static bool parse_decimal(const char* text, size_t length, uint64_t limit,
                          uint64_t* out) {
  bool negative = false;
  if (length > 0 && (text[0] == '+' || text[0] == '-')) {
    negative = text[0] == '-';
    text += 1;
    length -= 1;
  }
  if (length == 0) return false;
  uint64_t value = 0;
  for (size_t i = 0; i < length; i += 1) {
    if (text[i] < '0' || text[i] > '9') return false;
    uint64_t digit = (uint64_t)(text[i] - '0');
    if (value > (limit - digit) / 10u) return false;
    value = value * 10u + digit;
  }
  if (negative && value != 0) return false;
  *out = value;
  return true;
}

bool rm_item_id_parse(const char* text, rm_item_id* out) {
  if (text == nullptr || out == nullptr) return false;
  const char* colon = strchr(text, ':');
  if (colon == nullptr) return false;
  const char* counter_text = colon + 1;
  uint64_t author, counter;
  if (!parse_decimal(text, (size_t)(colon - text), UINT8_MAX, &author) ||
      !parse_decimal(counter_text, strlen(counter_text), UINT64_MAX,
                     &counter)) {
    return false;
  }
  *out = (rm_item_id){.author = (uint8_t)author, .counter = counter};
  return true;
}

bool rm_pen_tool_is_eraser(rm_pen_tool tool) {
  return tool == RM_TOOL_ERASER || tool == RM_TOOL_ERASER_AREA;
}

bool rm_pen_tool_is_highlighter(rm_pen_tool tool) {
  return tool == RM_TOOL_HIGHLIGHTER_1 || tool == RM_TOOL_HIGHLIGHTER_2;
}

rm_list_family rm_paragraph_style_list_family(rm_paragraph_style style) {
  switch (style) {
    case RM_PARA_BULLET:
    case RM_PARA_BULLET_NESTED:
      return RM_LIST_BULLET;
    case RM_PARA_CHECKBOX:
    case RM_PARA_CHECKBOX_CHECKED:
    case RM_PARA_CHECKBOX_NESTED:
    case RM_PARA_CHECKBOX_CHECKED_NESTED:
      return RM_LIST_CHECKBOX;
    case RM_PARA_NUMBERED:
    case RM_PARA_NUMBERED_NESTED:
      return RM_LIST_NUMBERED;
    default:
      return RM_LIST_NONE;
  }
}

bool rm_paragraph_style_is_list_item(rm_paragraph_style style) {
  return rm_paragraph_style_list_family(style) != RM_LIST_NONE;
}

rm_paragraph_style rm_paragraph_format_style(
    const rm_paragraph_format* format) {
  switch (format->raw) {
    case 0:
      return RM_PARA_BASIC;
    case 1:
      return RM_PARA_PLAIN;
    case 2:
      return RM_PARA_HEADING;
    case 3:
      return RM_PARA_SUBHEADING;
    case 4:
      return RM_PARA_BULLET;
    case 5:
      return RM_PARA_BULLET_NESTED;
    case 6:
      return RM_PARA_CHECKBOX;
    case 7:
      return RM_PARA_CHECKBOX_CHECKED;
    case 8:
      return RM_PARA_CHECKBOX_NESTED;
    case 9:
      return RM_PARA_CHECKBOX_CHECKED_NESTED;
    case 10:
      return RM_PARA_NUMBERED;
    case 11:
      return RM_PARA_NUMBERED_NESTED;
    case 12:
      return RM_PARA_IMAGE_BLOCK;
    default:
      return RM_PARA_UNKNOWN;
  }
}

bool rm_paragraph_format_has_further_fields(const rm_paragraph_format* format) {
  return format->has_second || format->has_third;
}

int rm_paragraph_format_heading_level(const rm_paragraph_format* format) {
  switch (rm_paragraph_format_style(format)) {
    case RM_PARA_HEADING:
      return 1;
    case RM_PARA_SUBHEADING:
      return rm_paragraph_format_has_further_fields(format) ? 2 : 3;
    default:
      return 0;
  }
}

int rm_paragraph_format_level(const rm_paragraph_format* format) {
  if (!rm_paragraph_style_is_list_item(rm_paragraph_format_style(format)))
    return 1;
  if (!format->has_third) return 1;
  return (int)(format->third & 0x0Fu) + 1;
}

/* The state bits of a checkbox item's `tag 3`. */
constexpr uint32_t TICKED_BIT = 0x100u;
constexpr uint32_t TICKED_NESTED_BIT = 0x200u;

/* Whether the paragraph's style belongs to the checkbox family of lists. */
static bool is_checkbox(const rm_paragraph_format* format) {
  return rm_paragraph_style_list_family(rm_paragraph_format_style(format)) ==
         RM_LIST_CHECKBOX;
}

bool rm_paragraph_format_is_ticked(const rm_paragraph_format* format) {
  rm_paragraph_style style = rm_paragraph_format_style(format);
  if (style == RM_PARA_CHECKBOX_CHECKED ||
      style == RM_PARA_CHECKBOX_CHECKED_NESTED)
    return true;
  return is_checkbox(format) && format->has_third &&
         (format->third & TICKED_BIT) != 0;
}

bool rm_paragraph_format_has_ticked_nested_items(
    const rm_paragraph_format* format) {
  return is_checkbox(format) && format->has_third &&
         (format->third & TICKED_NESTED_BIT) != 0;
}

rm_checkbox_state rm_text_block_checkbox_state(const rm_text_block* text,
                                               size_t paragraph) {
  if (text == nullptr || paragraph >= text->paragraph_count)
    return RM_CHECKBOX_NONE;
  const rm_paragraph* line = &text->paragraphs[paragraph];
  if (!line->has_format || !is_checkbox(&line->format)) return RM_CHECKBOX_NONE;
  if (rm_paragraph_format_is_ticked(&line->format)) return RM_CHECKBOX_TICKED;
  if (rm_paragraph_format_has_ticked_nested_items(&line->format))
    return RM_CHECKBOX_PARTIAL;

  /* The nested items are the paragraphs after this one that stand deeper, up
   * to the first that does not — whatever their own families. */
  int level = rm_paragraph_format_level(&line->format);
  for (size_t next = paragraph + 1; next < text->paragraph_count; next += 1) {
    const rm_paragraph* nested = &text->paragraphs[next];
    bool listed =
        nested->has_format && rm_paragraph_style_is_list_item(
                                  rm_paragraph_format_style(&nested->format));
    if ((listed ? rm_paragraph_format_level(&nested->format) : 0) <= level)
      break;
    if (rm_paragraph_format_is_ticked(&nested->format))
      return RM_CHECKBOX_PARTIAL;
  }
  return RM_CHECKBOX_UNTICKED;
}

float rm_text_column_width(rm_text_column column) {
  switch (column) {
    case RM_COLUMN_NARROW:
      return 740.0f;
    case RM_COLUMN_MEDIUM:
      return 936.0f;
    case RM_COLUMN_WIDE:
      return 1152.0f;
  }
  return 740.0f;
}

uint8_t rm_text_column_setting(rm_text_column column) {
  switch (column) {
    case RM_COLUMN_NARROW:
      return 0;
    case RM_COLUMN_MEDIUM:
      return 1;
    case RM_COLUMN_WIDE:
      return 2;
  }
  return 0;
}

/* How much of the sheet's width typed text may claim. Set against the
 * tablet's own pairing of column to device: 1152 / 1404 = 0.82 is what it
 * does on a reMarkable 2, so 0.85 admits that and nothing wider. */
constexpr double RM_TEXT_SHARE_OF_SHEET = 0.85;

rm_text_column rm_text_column_widest_fitting(double sheet_width) {
  double usable = sheet_width * RM_TEXT_SHARE_OF_SHEET;
  rm_text_column widest = RM_COLUMN_NARROW;
  const rm_text_column all[] = {RM_COLUMN_NARROW, RM_COLUMN_MEDIUM,
                                RM_COLUMN_WIDE};
  for (size_t i = 0; i < sizeof all / sizeof *all; i += 1) {
    if ((double)rm_text_column_width(all[i]) <= usable) widest = all[i];
  }
  return widest;
}

bool rm_header_version(const char* header, size_t length, int* out_version) {
  constexpr size_t prefix = sizeof(RM_HEADER_PREFIX) - 1u;
  if (length < prefix) return false;
  if (memcmp(header, RM_HEADER_PREFIX, prefix) != 0) return false;

  /* A single leading digit is what the format writes; the padding spaces
   * behind it stop the scan. An empty run means the header only looks like
   * one. */
  size_t at = prefix;
  long value = 0;
  size_t digits = 0;
  while (at < length && header[at] >= '0' && header[at] <= '9') {
    if (value < 1000000L) value = value * 10 + (header[at] - '0');
    digits += 1;
    at += 1;
  }
  if (digits == 0) return false;
  if (out_version != nullptr) *out_version = (int)value;
  return true;
}
