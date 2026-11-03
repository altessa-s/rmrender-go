/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#define _GNU_SOURCE
#include <locale.h>

#include "rmr_internal.h"
#ifdef __APPLE__
#include <xlocale.h>
#endif
#include <rmrender/template.h>

typedef struct block {
  struct block* next;
  max_align_t alignment;
} block;

typedef struct program {
  struct program* next;
  rmr_expression* expression;
} program;

typedef enum { OBJECT, ARRAY, STRING, NUMBER, NIL, BOOLEAN } kind;

typedef struct node {
  struct node *next, *child;
  char *key, *text;
  size_t offset, count;
  kind type;
  double number;
} node;

struct rmr_template_document {
  block* blocks;
  program* programs;
  rmr_template model;
  const char *name, *orientation;
  const char** categories;
  size_t category_count;
};

typedef struct {
  const char* source;
  size_t length, pos, work, bytes;
  rmr_limits limits;
  rmr_template_document* doc;
  rmr_error* error;
  rmr_status status;
  locale_t locale;
} parser;

/* Record the first failure in p and its error; later calls are no-ops. */
static void fail(parser* p, rmr_status status, size_t offset,
                 const char* message) {
  if (p->status == RMR_OK)
    p->status = rmr_fail(p->error, status, offset, message);
}

/* Charge n units against the decoding work budget, failing with LIMIT if
 * it would be exceeded. */
static bool work(parser* p, size_t n) {
  if (n > p->limits.maximum_elements - p->work) {
    fail(p, RMR_ERR_LIMIT, p->pos, "JSON work limit");
    return false;
  }
  p->work += n;
  return true;
}

/* Bump-allocate count*size zeroed bytes from the arena, tracked in
 * p->doc->blocks and bounded by maximum_plan_bytes. */
static void* allocate(parser* p, size_t count, size_t size) {
  if (!count || p->status != RMR_OK) return nullptr;

  size_t left = p->limits.maximum_plan_bytes - p->bytes;
  if (left < sizeof(block) || count > (left - sizeof(block)) / size) {
    fail(p, RMR_ERR_LIMIT, p->pos, "JSON memory limit");
    return nullptr;
  }

  size_t bytes = sizeof(block) + count * size;
  block* b = rmr_calloc(1, bytes);
  if (!b) {
    fail(p, RMR_ERR_OUT_OF_MEMORY, p->pos, nullptr);
    return nullptr;
  }

  b->next = p->doc->blocks;
  p->doc->blocks = b;
  p->bytes += bytes;
  return b + 1;
}

/* Skip JSON whitespace. */
static void space(parser* p) {
  while (p->pos < p->length && strchr(" \t\r\n", p->source[p->pos]) &&
         p->source[p->pos])
    p->pos++;
}

/* Skip whitespace, then consume c if the parser is positioned on it. */
static bool take(parser* p, char c) {
  space(p);
  if (p->pos < p->length && p->source[p->pos] == c) {
    p->pos++;
    return true;
  }
  return false;
}

/* Hex digit value of c, or -1 if c is not a hex digit. */
static int hex(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Parse a \uXXXX escape's four hex digits into a UTF-16 code unit. */
static unsigned unicode(parser* p) {
  unsigned v = 0;
  for (unsigned i = 0; i < 4; i++) {
    if (p->pos == p->length || hex((unsigned char)p->source[p->pos]) < 0) {
      fail(p, RMR_ERR_INVALID_ARGUMENT, p->pos, "invalid Unicode escape");
      return 0;
    }
    v = v * 16 + (unsigned)hex((unsigned char)p->source[p->pos++]);
  }
  return v;
}

/* Parse a JSON string literal, decoding escapes into a freshly allocated,
 * NUL-terminated UTF-8 buffer. */
static char* string(parser* p) {
  if (!take(p, '"')) {
    fail(p, RMR_ERR_INVALID_ARGUMENT, p->pos, "expected JSON string");
    return nullptr;
  }
  size_t start = p->pos, end = start;
  /* Find the raw span before allocating; decoding never grows it. */
  while (end < p->length && p->source[end] != '"') {
    if (p->source[end++] == '\\' && end < p->length) end++;
  }
  if (end == p->length) {
    fail(p, RMR_ERR_INVALID_ARGUMENT, start, "unterminated JSON string");
    return nullptr;
  }

  char* out = allocate(p, end - start + 1, 1);
  if (!out) return nullptr;

  size_t n = 0;
  while (p->pos < end && p->status == RMR_OK) {
    unsigned char c = (unsigned char)p->source[p->pos++];
    if (c < 32) {
      fail(p, RMR_ERR_INVALID_ARGUMENT, p->pos - 1, "control byte in string");
      break;
    }
    if (c != '\\') {
      out[n++] = (char)c;
      continue;
    }
    c = (unsigned char)p->source[p->pos++];
    switch (c) {
      case '"':
      case '\\':
      case '/':
        out[n++] = (char)c;
        break;
      case 'b':
        out[n++] = '\b';
        break;
      case 'f':
        out[n++] = '\f';
        break;
      case 'n':
        out[n++] = '\n';
        break;
      case 'r':
        out[n++] = '\r';
        break;
      case 't':
        out[n++] = '\t';
        break;
      case 'u': {
        unsigned v = unicode(p);
        if (v >= 0xd800 && v <= 0xdbff) {
          if (p->length - p->pos < 2 || p->source[p->pos] != '\\' ||
              p->source[p->pos + 1] != 'u') {
            fail(p, RMR_ERR_INVALID_ARGUMENT, p->pos, "missing low surrogate");
            break;
          }
          p->pos += 2;
          unsigned low = unicode(p);
          if (low < 0xdc00 || low > 0xdfff) {
            fail(p, RMR_ERR_INVALID_ARGUMENT, p->pos, "invalid low surrogate");
            break;
          }
          v = 0x10000 + (v - 0xd800) * 1024 + low - 0xdc00;
        } else if (v >= 0xdc00 && v <= 0xdfff) {
          fail(p, RMR_ERR_INVALID_ARGUMENT, p->pos, "unpaired surrogate");
          break;
        }

        if (!v) {
          fail(p, RMR_ERR_INVALID_ARGUMENT, p->pos, "NUL string unsupported");
          break;
        }

        if (v < 0x80)
          out[n++] = (char)v;
        else if (v < 0x800) {
          out[n++] = (char)(0xc0 | (v >> 6));
          out[n++] = (char)(0x80 | (v & 63));
        } else if (v < 0x10000) {
          out[n++] = (char)(0xe0 | (v >> 12));
          out[n++] = (char)(0x80 | ((v >> 6) & 63));
          out[n++] = (char)(0x80 | (v & 63));
        } else {
          out[n++] = (char)(0xf0 | (v >> 18));
          out[n++] = (char)(0x80 | ((v >> 12) & 63));
          out[n++] = (char)(0x80 | ((v >> 6) & 63));
          out[n++] = (char)(0x80 | (v & 63));
        }
        break;
      }
      default:
        fail(p, RMR_ERR_INVALID_ARGUMENT, p->pos - 1, "invalid string escape");
    }
  }

  out[n] = 0;
  if (p->status == RMR_OK) p->pos = end + 1;
  return out;
}

/* True if the parser is positioned on an ASCII decimal digit. */
static bool digit(parser* p) {
  return p->pos < p->length && p->source[p->pos] >= '0' &&
         p->source[p->pos] <= '9';
}

/* Recursive-descent JSON value parser, returning an arena-allocated node
 * tree; depth bounds object/array nesting. */
static node* parse(parser* p, unsigned depth) {
  space(p);
  if (depth > 128) {
    fail(p, RMR_ERR_LIMIT, p->pos, "JSON nesting limit");
    return nullptr;
  }
  if (!work(p, 1)) return nullptr;

  node* v = allocate(p, 1, sizeof *v);
  if (!v) return nullptr;
  v->offset = p->pos;
  if (p->pos == p->length) {
    fail(p, RMR_ERR_INVALID_ARGUMENT, p->pos, "missing JSON value");
    return v;
  }
  char c = p->source[p->pos];
  if (c == '{' || c == '[') {
    v->type = c == '{' ? OBJECT : ARRAY;
    p->pos++;
    char close = c == '{' ? '}' : ']';
    if (take(p, close)) return v;

    node** tail = &v->child;
    do {
      char* key = nullptr;
      if (v->type == OBJECT) {
        key = string(p);
        if (!key || p->status != RMR_OK) return v;

        size_t key_work = strlen(key) + 1;
        for (node* n = v->child; n; n = n->next) {
          if (!work(p, key_work)) return v;
          if (!strcmp(n->key, key)) {
            fail(p, RMR_ERR_INVALID_ARGUMENT, p->pos, "duplicate JSON key");
            return v;
          }
        }

        if (!take(p, ':')) {
          fail(p, RMR_ERR_INVALID_ARGUMENT, p->pos, "missing colon");
          return v;
        }
      }

      node* child = parse(p, depth + 1);
      if (!child || p->status != RMR_OK) return v;

      child->key = key;
      *tail = child;
      tail = &child->next;
      v->count++;

      if (take(p, close)) return v;
    } while (take(p, ','));

    fail(p, RMR_ERR_INVALID_ARGUMENT, p->pos, "missing JSON delimiter");
  } else if (c == '"') {
    v->type = STRING;
    v->text = string(p);
  } else if (c == '-' || (c >= '0' && c <= '9')) {
    v->type = NUMBER;
    size_t start = p->pos;
    if (c == '-') p->pos++;
    if (!digit(p)) goto number_error;
    if (p->source[p->pos] == '0')
      p->pos++;
    else
      while (digit(p)) p->pos++;

    if (p->pos < p->length && p->source[p->pos] == '.') {
      p->pos++;
      if (!digit(p)) goto number_error;
      while (digit(p)) p->pos++;
    }

    if (p->pos < p->length &&
        (p->source[p->pos] == 'e' || p->source[p->pos] == 'E')) {
      p->pos++;
      if (p->pos < p->length &&
          (p->source[p->pos] == '+' || p->source[p->pos] == '-'))
        p->pos++;
      if (!digit(p)) goto number_error;
      while (digit(p)) p->pos++;
    }

    size_t len = p->pos - start;
    char* buffer = allocate(p, len + 1, 1);
    if (!buffer) return v;
    memcpy(buffer, p->source + start, len);
    v->text = buffer;
    v->number = strtod_l(buffer, nullptr, p->locale);
    if (!isfinite(v->number)) goto number_error;
    return v;

  number_error:
    fail(p, RMR_ERR_INVALID_ARGUMENT, p->pos, "invalid JSON number");
  } else {
    const char* literal = c == 'n'   ? "null"
                          : c == 't' ? "true"
                          : c == 'f' ? "false"
                                     : "";
    size_t n = strlen(literal);
    if (!n || n > p->length - p->pos || memcmp(p->source + p->pos, literal, n))
      fail(p, RMR_ERR_INVALID_ARGUMENT, p->pos, "invalid JSON value");
    else {
      p->pos += n;
      v->type = c == 'n' ? NIL : BOOLEAN;
    }
  }

  return v;
}

/* Require v to be of kind wanted, failing with the first mismatch. */
static bool type(parser* p, node* v, kind wanted) {
  if (p->status != RMR_OK) return false;
  if (!v || v->type != wanted) {
    fail(p, RMR_ERR_INVALID_ARGUMENT, v ? v->offset : p->pos,
         "invalid template field type");
    return false;
  }
  return true;
}

/* Named child of object v, or null if key is absent; v must be an object. */
static node* field(parser* p, node* v, const char* key) {
  if (!type(p, v, OBJECT)) return nullptr;
  for (node* n = v->child; n; n = n->next) {
    if (!work(p, strlen(key) + 1)) return nullptr;
    if (!strcmp(n->key, key)) return n;
  }
  return nullptr;
}

/* Named field, treating a present JSON null the same as an absent key. */
static node* optional(parser* p, node* v, const char* key) {
  node* n = field(p, v, key);
  return n && n->type != NIL ? n : nullptr;
}

/* Decode a template value: a number literal verbatim, or a string compiled
 * as an expression. */
static void value(parser* p, node* n, rmr_template_value* out) {
  if (p->status != RMR_OK) return;
  if (n && n->type == NUMBER) {
    out->number = n->number;
    return;
  }
  if (!type(p, n, STRING)) return;

  program* pr = allocate(p, 1, sizeof *pr);
  if (!pr) return;

  rmr_limits l = p->limits;
  l.maximum_plan_bytes -= p->bytes;
  l.maximum_elements -= p->work;
  rmr_error e = {0};
  rmr_status status =
      rmr_expression_compile(n->text, strlen(n->text), &l, &pr->expression, &e);
  if (status != RMR_OK) {
    fail(p, status, n->offset, e.message);
    return;
  }

  pr->next = p->doc->programs;
  p->doc->programs = pr;
  p->bytes += rmr_expression_bytes(pr->expression);
  if (!work(p, rmr_expression_cost(pr->expression))) return;

  out->expression = pr->expression;
}

/* Decode an optional box's x/y/width/height value fields in place. */
static void box(parser* p, node* n, rmr_template_box* out) {
  if (!n) return;
  if (!type(p, n, OBJECT)) return;

  const char* keys[] = {"x", "y", "width", "height"};
  rmr_template_value* values[] = {&out->x, &out->y, &out->width, &out->height};
  for (size_t i = 0; i < 4; i++) {
    node* v = optional(p, n, keys[i]);
    if (v) value(p, v, values[i]);
  }
}

/* Decode a repeat spec: a direction keyword, or otherwise a count value. */
static void repeat(parser* p, node* n, rmr_template_repeat* out) {
  if (!n) return;

  if (n->type == STRING) {
    if (!strcmp(n->text, "down") || !strcmp(n->text, "right") ||
        !strcmp(n->text, "infinite")) {
      out->kind = RMR_REPEAT_FORWARD;
      return;
    }
    if (!strcmp(n->text, "up") || !strcmp(n->text, "left")) {
      out->kind = RMR_REPEAT_BACKWARD;
      return;
    }
  }

  out->kind = RMR_REPEAT_COUNT;
  value(p, n, &out->count);
}

/* Decode a template color: a named gray shade, or a #RGB/#RGBA hex string. */
static void color(parser* p, node* n, rmr_color* out) {
  if (!type(p, n, STRING)) return;

  char lower[16] = {0};
  size_t len = strlen(n->text);
  if (len < sizeof lower)
    for (size_t i = 0; i < len; i++) {
      char c = n->text[i];
      lower[i] = c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
    }

  double gray = -1;
  if (!strcmp(lower, "black"))
    gray = 0;
  else if (!strcmp(lower, "white"))
    gray = 1;
  else if (!strcmp(lower, "gray") || !strcmp(lower, "grey"))
    gray = .5;
  else if (!strcmp(lower, "lightgray") || !strcmp(lower, "lightgrey"))
    gray = .8;
  if (gray >= 0) {
    *out = (rmr_color){gray, gray, gray, 1};
    return;
  }

  const char* s = n->text;
  if (*s == '#') {
    s++;
    len--;
  }
  if (len != 6 && len != 8) {
    fail(p, RMR_ERR_INVALID_ARGUMENT, n->offset, "invalid template color");
    return;
  }

  uint32_t v = 0;
  for (size_t i = 0; i < len; i++) {
    int h = hex((unsigned char)s[i]);
    if (h < 0) {
      fail(p, RMR_ERR_INVALID_ARGUMENT, n->offset, "invalid template color");
      return;
    }
    v = (v << 4) | (unsigned)h;
  }

  if (len == 6) v = (v << 8) | 255;
  *out = (rmr_color){(double)(v >> 24) / 255, (double)((v >> 16) & 255) / 255,
                     (double)((v >> 8) & 255) / 255, (double)(v & 255) / 255};
}

/* True if n is a one-letter SVG-style path command, including letters this
 * parser does not implement (path() restricts those further). */
static bool command(node* n) {
  return n && n->type == STRING && strlen(n->text) == 1 &&
         strchr("MLHVCSQTAZmlhvcsqtaz", n->text[0]);
}

/* Decode explicit M/L/C/Z path elements plus optional fill/stroke paint. */
static void path(parser* p, node* n, rmr_template_path* out) {
  node* data = field(p, n, "data");
  if (!type(p, data, ARRAY)) return;

  size_t count = 0;
  for (node* v = data->child; v && p->status == RMR_OK;) {
    if (!work(p, 1)) return;
    if (!command(v)) {
      fail(p, RMR_ERR_INVALID_ARGUMENT, v->offset,
           "expected explicit path command");
      return;
    }
    char c = v->text[0];
    size_t args = c == 'M' || c == 'L' ? 2 : c == 'C' ? 6 : 0;
    if (c != 'M' && c != 'L' && c != 'C' && c != 'Z') {
      fail(p, RMR_ERR_UNSUPPORTED, v->offset, "unsupported path command");
      return;
    }

    v = v->next;
    count++;

    for (size_t i = 0; i < args; i++) {
      if (!v || command(v)) {
        fail(p, RMR_ERR_INVALID_ARGUMENT, v ? v->offset : data->offset,
             "missing path coordinate");
        return;
      }
      v = v->next;
    }
  }

  rmr_template_element* elements = allocate(p, count, sizeof *elements);
  out->elements = elements;
  out->count = count;
  if (count && !elements) return;

  node* v = data->child;
  bool open = false;
  for (size_t i = 0; i < count && p->status == RMR_OK; i++) {
    char c = v->text[0];
    if (c != 'M' && !open) {
      fail(p, RMR_ERR_INVALID_ARGUMENT, v->offset, "path must start with move");
      return;
    }

    open = c != 'Z';
    v = v->next;
    rmr_template_element* e = &elements[i];
    e->kind = c == 'M'   ? RMR_MOVE
              : c == 'L' ? RMR_LINE
              : c == 'C' ? RMR_CURVE
                         : RMR_CLOSE;
    rmr_template_value* coords[] = {&e->control1_x, &e->control1_y,
                                    &e->control2_x, &e->control2_y,
                                    &e->x,          &e->y};
    size_t start = c == 'C' ? 0 : c == 'Z' ? 6 : 4;
    for (size_t k = start; k < 6; k++) {
      value(p, v, coords[k]);
      v = v->next;
    }
  }

  node *vwidth = optional(p, n, "strokeWidth"),
       *stroke = optional(p, n, "strokeColor"),
       *fill = optional(p, n, "fillColor");
  if (vwidth) {
    out->has_stroke_width = true;
    value(p, vwidth, &out->stroke_width);
  }
  if (stroke) {
    out->has_stroke_color = true;
    color(p, stroke, &out->stroke);
  }
  if (fill) {
    out->has_fill = true;
    color(p, fill, &out->fill);
  }
}

/* Decode a template item array (path/group/text), recursing into group
 * children up to the nesting limit tracked by depth. */
static void items(parser* p, node* n, const rmr_template_item** out,
                  size_t* count, unsigned depth) {
  if (!type(p, n, ARRAY)) return;

  rmr_template_item* list = allocate(p, n->count, sizeof *list);
  if (n->count && !list) return;

  *out = list;
  *count = n->count;
  size_t i = 0;

  for (node* v = n->child; v && p->status == RMR_OK; v = v->next, i++) {
    node* t = field(p, v, "type");
    if (!type(p, t, STRING)) return;

    if (!strcmp(t->text, "path")) {
      list[i].kind = RMR_TEMPLATE_PATH;
      path(p, v, &list[i].path);
    } else if (!strcmp(t->text, "group")) {
      if (depth == 32) {
        fail(p, RMR_ERR_LIMIT, v->offset, "template group nesting limit");
        return;
      }

      list[i].kind = RMR_TEMPLATE_GROUP;
      rmr_template_group* g = &list[i].group;
      box(p, optional(p, v, "boundingBox"), &g->box);

      node* r = optional(p, v, "repeat");
      if (r) {
        repeat(p, optional(p, r, "rows"), &g->rows);
        repeat(p, optional(p, r, "columns"), &g->columns);
      }

      items(p, field(p, v, "children"), &g->children, &g->count, depth + 1);
    } else if (!strcmp(t->text, "text")) {
      list[i].kind = RMR_TEMPLATE_TEXT;
      rmr_template_text* label = &list[i].text;
      node* text = field(p, v, "text");
      if (!type(p, text, STRING)) return;

      label->utf8 = text->text;
      label->size = strlen(text->text);
      if (label->size > 65536) {
        fail(p, RMR_ERR_LIMIT, text->offset, "text byte limit");
        return;
      }

      node* size = optional(p, v, "fontSize");
      if (size) {
        label->has_font_size = true;
        value(p, size, &label->font_size);
      }
      box(p, optional(p, v, "position"), &label->position);
    } else {
      fail(p, RMR_ERR_UNSUPPORTED, t->offset, "unsupported template item");
      return;
    }
  }
}

void rmr_template_document_free(rmr_template_document* d) {
  if (!d) return;
  for (program* pr = d->programs; pr; pr = pr->next)
    rmr_expression_free(pr->expression);
  block* b = d->blocks;
  while (b) {
    block* next = b->next;
    free(b);
    b = next;
  }
  free(d);
}

rmr_status rmr_template_document_decode(const char* json, size_t length,
                                        const rmr_limits* given,
                                        rmr_template_document** out,
                                        rmr_error* error) {
  if (!json || !out)
    return rmr_fail(error, RMR_ERR_INVALID_ARGUMENT, 0,
                    "missing JSON input/output");

  rmr_limits l = given ? *given : rmr_limits_default();
  /* Bound the scan as well as allocation, even for whitespace-only inputs. */
  if (length > l.maximum_plan_bytes ||
      sizeof(rmr_template_document) > l.maximum_plan_bytes)
    return rmr_fail(error, RMR_ERR_LIMIT, 0, "JSON byte limit");

  if (!rmr_utf8_valid(json, length))
    return rmr_fail(error, RMR_ERR_INVALID_ARGUMENT, 0, "invalid JSON UTF-8");

  rmr_template_document* d = rmr_calloc(1, sizeof *d);
  if (!d) return rmr_fail(error, RMR_ERR_OUT_OF_MEMORY, 0, nullptr);

  parser p = {.source = json,
              .length = length,
              .limits = l,
              .doc = d,
              .error = error,
              .bytes = sizeof *d};

  p.locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  if (!p.locale) {
    free(d);
    return rmr_fail(error, RMR_ERR_OUT_OF_MEMORY, 0,
                    "numeric locale unavailable");
  }

  node* root = parse(&p, 0);
  space(&p);
  if (p.pos != length)
    fail(&p, RMR_ERR_INVALID_ARGUMENT, p.pos, "trailing JSON data");

  if (p.status == RMR_OK && type(&p, root, OBJECT)) {
    node* name = field(&p, root, "name");
    if (type(&p, name, STRING)) d->name = name->text;

    node* orientation = optional(&p, root, "orientation");
    if (orientation && type(&p, orientation, STRING))
      d->orientation = orientation->text;

    node* categories = optional(&p, root, "categories");
    if (categories && type(&p, categories, ARRAY)) {
      d->categories = allocate(&p, categories->count, sizeof *d->categories);
      if (d->categories) {
        d->category_count = categories->count;
        size_t i = 0;
        for (node* v = categories->child; v; v = v->next) {
          if (!type(&p, v, STRING)) break;
          d->categories[i++] = v->text;
        }
      }
    }

    node* constants = optional(&p, root, "constants");
    if (constants && type(&p, constants, ARRAY)) {
      if (constants->count > 250)
        fail(&p, RMR_ERR_LIMIT, constants->offset, "template constant limit");
      rmr_template_constant* cs = allocate(&p, constants->count, sizeof *cs);
      if (cs) {
        d->model.constants = cs;
        d->model.constant_count = constants->count;
        size_t i = 0;
        for (node* v = constants->child; v && p.status == RMR_OK;
             v = v->next, i++) {
          if (!type(&p, v, OBJECT)) break;
          if (v->count != 1) {
            fail(&p, RMR_ERR_INVALID_ARGUMENT, v->offset,
                 "constant must be a single-pair object");
            break;
          }
          cs[i].name = v->child->key;
          cs[i].length = strlen(cs[i].name);
          for (size_t j = 0; j < cs[i].length; j++) {
            unsigned char c = (unsigned char)cs[i].name[j];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  c == '_' || (j && c >= '0' && c <= '9'))) {
              fail(&p, RMR_ERR_INVALID_ARGUMENT, v->offset,
                   "invalid constant name");
              break;
            }
          }

          if (cs[i].length > 65536)
            fail(&p, RMR_ERR_LIMIT, v->offset, "constant name limit");
          if (!cs[i].length)
            fail(&p, RMR_ERR_INVALID_ARGUMENT, v->offset,
                 "empty constant name");
          value(&p, v->child, &cs[i].value);
        }
      }
    }

    items(&p, field(&p, root, "items"), &d->model.items, &d->model.count, 0);
  }

  freelocale(p.locale);

  if (p.status != RMR_OK) {
    rmr_template_document_free(d);
    return p.status;
  }

  *out = d;
  return RMR_OK;
}

const rmr_template* rmr_template_document_model(
    const rmr_template_document* d) {
  return d ? &d->model : nullptr;
}

const char* rmr_template_document_name(const rmr_template_document* d) {
  return d ? d->name : nullptr;
}

const char* rmr_template_document_orientation(const rmr_template_document* d) {
  return d ? d->orientation : nullptr;
}

size_t rmr_template_document_category_count(const rmr_template_document* d) {
  return d ? d->category_count : 0;
}

const char* rmr_template_document_category(const rmr_template_document* d,
                                           size_t i) {
  return d && i < d->category_count ? d->categories[i] : nullptr;
}

/* Document metadata shares the same bounded JSON parser/arena as templates. */
#include <rmrender/document.h>

struct rmr_document_layout {
  rmr_template_document* storage;
  rmr_document_page* pages;
  const char** deleted;
  size_t count, deleted_count, bytes;
  rmr_point canvas;
  bool landscape;
};

typedef struct {
  rmr_document_page page;
  const char* key;
  size_t position;
} ordered_page;

/* Order entries by page id, then by original document position. */
static int page_order(const void* a, const void* b) {
  const ordered_page *x = a, *y = b;
  int order = strcmp(x->key, y->key);
  return order                       ? order
         : x->position < y->position ? -1
         : x->position > y->position ? 1
                                     : 0;
}

/* Named field of entry (.content decoding), treating null as absent. */
static node* content_optional(parser* p, node* entry, const char* key) {
  node* value = optional(p, entry, key);
  return value && value->type != NIL ? value : nullptr;
}

/* CRDT-stamped field: unwrap the {value:...} wrapper object, treating a
 * missing field, non-object wrapper or null value as absent. */
static node* stamped(parser* p, node* entry, const char* key) {
  node* n = content_optional(p, entry, key);
  return n && type(p, n, OBJECT) ? content_optional(p, n, "value") : nullptr;
}

/* Parse a nonnegative JSON integer exactly, including decimal/exponent syntax.
 * A double would silently round page indexes above 2^53 or tiny fractions. */
static bool source_index(const char* text, size_t* out) {
  const char* p = text;
  bool negative = *p == '-';
  if (negative) p++;

  const char* digits = p;
  size_t count = 0, fraction = 0;
  bool decimal = false;
  while (*p && *p != 'e' && *p != 'E') {
    if (*p == '.')
      decimal = true;
    else {
      count++;
      if (decimal) fraction++;
    }
    p++;
  }

  int exponent = 0;
  if (*p) {
    p++;
    bool minus = *p == '-';
    if (*p == '-' || *p == '+') p++;
    while (*p) {
      if (exponent < 1000000) exponent = exponent * 10 + (*p - '0');
      p++;
    }
    if (minus) exponent = -exponent;
  }

  /* Fraction/count are bounded by the JSON allocation limit. Work stays
   * linear in the token length; huge exponents never cause long loops. */
  int64_t end = (int64_t)count - (int64_t)fraction + exponent;

  uint64_t value = 0;
  size_t index = 0;
  for (p = digits; *p && *p != 'e' && *p != 'E'; p++) {
    if (*p == '.') continue;
    unsigned digit_value = (unsigned)(*p - '0');
    if ((int64_t)index >= end) {
      if (digit_value) return false;
    } else {
      if (value > ((uint64_t)INT64_MAX - digit_value) / 10) return false;
      value = value * 10 + digit_value;
    }
    index++;
  }

  if (value) {
    if (negative || end - (int64_t)count > 18) return false;
    for (int64_t i = (int64_t)count; i < end; i++) {
      if (value > (uint64_t)INT64_MAX / 10) return false;
      value *= 10;
    }
  }

  if (value > SIZE_MAX) return false;
  *out = (size_t)value;
  return true;
}

/* Decode a page's source-page redirect (optionally CRDT-stamped) into
 * page's has_source/source_page/refused_source fields. */
static void redirection(parser* p, node* n, bool stamp,
                        rmr_document_page* page) {
  if (!n || n->type == NIL) return;

  if (stamp) {
    if (n->type != OBJECT) {
      page->refused_source = true;
      return;
    }
    n = content_optional(p, n, "value");
    if (!n) return;
  }

  if (n->type == NUMBER && source_index(n->text, &page->source_page)) {
    page->has_source = true;
  } else
    page->refused_source = true;
}

rmr_status rmr_document_layout_decode(const char* json, size_t length,
                                      const rmr_limits* given,
                                      rmr_document_layout** out,
                                      rmr_error* error) {
  if (!json || !out)
    return rmr_fail(error, RMR_ERR_INVALID_ARGUMENT, 0, "missing content JSON");

  rmr_limits l = given ? *given : rmr_limits_default();
  if (length > l.maximum_plan_bytes ||
      sizeof(rmr_template_document) > l.maximum_plan_bytes)
    return rmr_fail(error, RMR_ERR_LIMIT, 0, "content byte limit");

  if (!rmr_utf8_valid(json, length))
    return rmr_fail(error, RMR_ERR_INVALID_ARGUMENT, 0,
                    "invalid content UTF-8");

  rmr_template_document* storage = rmr_calloc(1, sizeof *storage);
  if (!storage) return rmr_fail(error, RMR_ERR_OUT_OF_MEMORY, 0, nullptr);

  parser p = {.source = json,
              .length = length,
              .limits = l,
              .doc = storage,
              .error = error,
              .bytes = sizeof *storage};

  p.locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  if (!p.locale) {
    free(storage);
    return rmr_fail(error, RMR_ERR_OUT_OF_MEMORY, 0,
                    "numeric locale unavailable");
  }

  node* root = parse(&p, 0);
  space(&p);
  if (p.pos != length)
    fail(&p, RMR_ERR_INVALID_ARGUMENT, p.pos, "trailing content JSON");

  rmr_document_layout* d = allocate(&p, 1, sizeof *d);
  if (d && type(&p, root, OBJECT)) {
    d->storage = storage;

    node *w = content_optional(&p, root, "customZoomPageWidth"),
         *h = content_optional(&p, root, "customZoomPageHeight");
    bool valid_w = !w || type(&p, w, NUMBER),
         valid_h = !h || type(&p, h, NUMBER);
    if (w && h && valid_w && valid_h && w->number > 0 && h->number > 0 &&
        w->number <= 100000 && h->number <= 100000)
      d->canvas = (rmr_point){w->number, h->number};

    node* orientation = content_optional(&p, root, "orientation");
    if (orientation && orientation->type == OBJECT)
      orientation = content_optional(&p, orientation, "value");
    d->landscape = orientation && orientation->type == STRING &&
                   !strcmp(orientation->text, "landscape");

    node *cp = content_optional(&p, root, "cPages"), *pages = nullptr;
    if (cp && type(&p, cp, OBJECT)) pages = content_optional(&p, cp, "pages");
    if (pages && !type(&p, pages, ARRAY)) pages = nullptr;
    bool crdt = pages && pages->count;
    if (!crdt) pages = content_optional(&p, root, "pages");

    node* redirects =
        crdt ? nullptr : content_optional(&p, root, "redirectionPageMap");
    if (redirects && !type(&p, redirects, ARRAY)) redirects = nullptr;

    if (pages && type(&p, pages, ARRAY)) {
      ordered_page* ordered = allocate(&p, pages->count, sizeof *ordered);
      d->pages = allocate(&p, pages->count, sizeof *d->pages);
      d->deleted = allocate(&p, pages->count, sizeof *d->deleted);
      size_t alive = 0, position = 0;
      node* redir = redirects ? redirects->child : nullptr;

      for (node* entry = pages->child; p.status == RMR_OK && entry;
           entry = entry->next, position++) {
        node* id = crdt ? field(&p, entry, "id") : entry;
        if (!type(&p, id, STRING)) break;
        ordered_page item = {
            .page = {.id = id->text}, .key = "", .position = position};

        if (crdt) {
          node* deleted = stamped(&p, entry, "deleted");
          if (deleted && type(&p, deleted, NUMBER) && deleted->number == 1) {
            d->deleted[d->deleted_count++] = id->text;
            continue;
          }

          node *name = stamped(&p, entry, "template"),
               *key = stamped(&p, entry, "idx");
          if (name && type(&p, name, STRING))
            item.page.template_name = name->text;
          if (key && type(&p, key, STRING)) item.key = key->text;
          redirection(&p, content_optional(&p, entry, "redir"), true,
                      &item.page);
        } else {
          redirection(&p, redir, false, &item.page);
          if (redir) redir = redir->next;
        }

        ordered[alive++] = item;
      }

      if (p.status == RMR_OK) {
        if (crdt && alive > 1)
          qsort(ordered, alive, sizeof *ordered, page_order);

        for (size_t i = 0; i < alive; i++) {
          bool duplicate = false;
          for (size_t j = 0; j < d->count; j++) {
            if (!work(&p, strlen(ordered[i].page.id) + 1)) break;
            if (!strcmp(d->pages[j].id, ordered[i].page.id)) {
              duplicate = true;
              break;
            }
          }
          if (p.status != RMR_OK) break;
          if (!duplicate) d->pages[d->count++] = ordered[i].page;
        }
      }
    }
  }

  freelocale(p.locale);

  if (p.status != RMR_OK) {
    rmr_template_document_free(storage);
    return p.status;
  }

  d->bytes = p.bytes;
  *out = d;
  return RMR_OK;
}

size_t rmr_document_layout_bytes(const rmr_document_layout* d) {
  return d ? d->bytes : 0;
}

void rmr_document_layout_free(rmr_document_layout* d) {
  if (d) rmr_template_document_free(d->storage);
}

size_t rmr_document_layout_count(const rmr_document_layout* d) {
  return d ? d->count : 0;
}

const rmr_document_page* rmr_document_layout_page(const rmr_document_layout* d,
                                                  size_t i) {
  return d && i < d->count ? &d->pages[i] : nullptr;
}

size_t rmr_document_layout_deleted_count(const rmr_document_layout* d) {
  return d ? d->deleted_count : 0;
}

const char* rmr_document_layout_deleted(const rmr_document_layout* d,
                                        size_t i) {
  return d && i < d->deleted_count ? d->deleted[i] : nullptr;
}

rmr_point rmr_document_layout_canvas(const rmr_document_layout* d) {
  return d ? d->canvas : (rmr_point){0};
}

bool rmr_document_layout_landscape(const rmr_document_layout* d) {
  return d && d->landscape;
}

rmr_document_page rmr_document_page_checked(rmr_document_page page,
                                            size_t count) {
  if (page.has_source && page.source_page >= count) {
    page.has_source = false;
    page.source_page = 0;
    page.refused_source = true;
  }
  return page;
}
