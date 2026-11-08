/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* mmap's MAP_ANON under a strict C standard on glibc. */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#include <limits.h>
#include <rmrender/pdf.h>
#include <stdarg.h>
#include <stddef.h>
#include <zlib.h>

#include "rmr_internal.h"
#include "rmr_pdf_edit.h"
#include "rmr_sha256.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* Bounded arena; parsed scalar tokens retain their exact source spelling.
 * References are the only scalar rewritten when importing an overlay. */
typedef struct mem {
  struct mem* next;
} mem;

typedef struct {
  rmr_limits limits;
  rmr_status status;
  const char* reason;
  size_t allocated, work;
  mem* memory;
  rmr_cancel_fn cancel;
  void* context;
} job;

/* Records the first failure and its reason; later failures are ignored. */
static void stop(job* j, rmr_status s, const char* why) {
  if (j->status == RMR_OK) {
    j->status = s;
    j->reason = why;
  }
}

/* Charges one unit of work and polls cancellation. Returns false once the
 * job has failed. */
static bool tick(job* j) {
  if (j->status != RMR_OK) return false;

  if (j->cancel && j->cancel(j->context))
    stop(j, RMR_ERR_CANCELLED, "PDF update cancelled");
  if (j->work >= j->limits.maximum_elements)
    stop(j, RMR_ERR_LIMIT, "PDF work limit");
  else
    j->work++;
  return j->status == RMR_OK;
}

/* Zeroed arena allocation charged to the scratch budget; freed by
 * release(). Returns null, failing the job, when over budget or out of
 * memory. */
static void* take(job* j, size_t n) {
  if (j->status != RMR_OK) return nullptr;
  if (n > SIZE_MAX - sizeof(mem) ||
      n + sizeof(mem) > j->limits.maximum_plan_bytes - j->allocated) {
    stop(j, RMR_ERR_LIMIT, "PDF scratch limit");
    return nullptr;
  }

  mem* m = rmr_calloc(1, sizeof *m + n);
  if (!m) {
    stop(j, RMR_ERR_OUT_OF_MEMORY, "PDF allocation failed");
    return nullptr;
  }
  m->next = j->memory;
  j->memory = m;
  j->allocated += sizeof *m + n;
  return m + 1;
}

typedef enum { SCALAR, NAME, ARRAY, DICT, REF } kind;

typedef struct node {
  kind type;
  const uint8_t* raw;
  size_t length;
  struct node *child, *next;
  uint32_t number, generation;
} node;

typedef struct {
  job* j;
  const uint8_t* data;
  size_t size, pos;
} scan;

/* PDF whitespace, which includes NUL. */
static bool space(uint8_t c) {
  return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}

/* True for whitespace and the PDF delimiter characters. */
static bool delim(uint8_t c) {
  return space(c) || strchr("()<>[]{}/%", c) != nullptr;
}

/* Skips whitespace and % comments. */
static void ws(scan* s) {
  while (s->pos < s->size) {
    uint8_t c = s->data[s->pos];
    if (space(c))
      s->pos++;
    else if (c == '%') {
      while (s->pos < s->size && s->data[s->pos] != '\n' &&
             s->data[s->pos] != '\r')
        s->pos++;
    } else
      break;
  }
}

/* Consumes w if it is the next complete token. */
static bool word(scan* s, const char* w) {
  ws(s);
  size_t n = strlen(w);
  if (n > s->size - s->pos || memcmp(s->data + s->pos, w, n) ||
      (s->pos + n < s->size && !delim(s->data[s->pos + n])))
    return false;
  s->pos += n;
  return true;
}

/* Reads an unsigned decimal token without sign. On overflow or a
 * non-delimited token, only leading whitespace is consumed. */
static bool integer(scan* s, uint64_t* v) {
  ws(s);
  size_t p = s->pos;
  uint64_t n = 0;

  while (p < s->size && s->data[p] >= '0' && s->data[p] <= '9') {
    unsigned d = (unsigned)(s->data[p++] - '0');
    if (n > (UINT64_MAX - d) / 10) return false;
    n = n * 10 + d;
  }

  if (p == s->pos || (p < s->size && !delim(s->data[p]))) return false;
  s->pos = p;
  *v = n;
  return true;
}

/* Value of a hexadecimal digit, or -1. */
static int hex(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

/* True if n is a name token equal to name after decoding #xx escapes. */
static bool named(const node* n, const char* name) {
  if (!n || n->type != NAME) return false;
  size_t k = 0;

  for (size_t i = 1; i < n->length; i++) {
    uint8_t c = n->raw[i];
    if (c == '#') {
      if (i + 2 >= n->length) return false;
      int a = hex(n->raw[++i]), b = hex(n->raw[++i]);
      if (a < 0 || b < 0) return false;
      c = (uint8_t)(a * 16 + b);
    }
    if (!name[k] || c != (uint8_t)name[k++]) return false;
  }

  return name[k] == 0;
}

/* Orders two validated name tokens by their bytes after decoding #xx
 * escapes; 0 when they name the same thing. */
static int name_order(const node* a, const node* b) {
  size_t i = 1, k = 1;

  while (i < a->length && k < b->length) {
    unsigned x = a->raw[i++], y = b->raw[k++];
    if (x == '#') {
      x = (unsigned)(hex(a->raw[i]) * 16 + hex(a->raw[i + 1]));
      i += 2;
    }
    if (y == '#') {
      y = (unsigned)(hex(b->raw[k]) * 16 + hex(b->raw[k + 1]));
      k += 2;
    }
    if (x != y) return x < y ? -1 : 1;
  }

  return (i < a->length) - (k < b->length);
}

/* Compares two validated name tokens after decoding #xx escapes. */
static bool same_name(const node* a, const node* b) {
  return !name_order(a, b);
}

/* Unresolved value of key in dictionary d; null if absent or d is not a
 * dictionary. */
static node* get(node* d, const char* key) {
  if (!d || d->type != DICT) return nullptr;
  for (node* n = d->child; n && n->next; n = n->next->next)
    if (named(n, key)) return n->next;
  return nullptr;
}

/* Charges n units of work at once, as n ticks would. */
static bool charge(job* j, size_t n) {
  if (!tick(j)) return false;
  if (n > j->limits.maximum_elements - j->work) {
    stop(j, RMR_ERR_LIMIT, "PDF work limit");
    return false;
  }
  j->work += n;
  return true;
}

/* qsort order of dictionary keys by decoded name. */
static int key_order(const void* a, const void* b) {
  return name_order(*(const node* const*)a, *(const node* const*)b);
}

/* False when dictionary d, of count keys, names a key twice (the job still
 * fine) or the check fails the job. A small dictionary is checked pair by
 * pair; a larger one sorted, so that its work grows as n log n rather than
 * n², and a page's resources naming each of its thousands of images once
 * stay well inside the work budget. */
static bool unique_keys(job* j, node* d, size_t count) {
  if (count <= 16) {
    for (node* k = d->child; k; k = k->next->next)
      for (node* other = k->next->next; other; other = other->next->next) {
        if (!tick(j)) return false;
        if (same_name(k, other)) return false;
      }
    return true;
  }

  size_t bytes = count * sizeof(node*), depth = 0;
  for (size_t n = count; n; n >>= 1) depth++;
  if (count > SIZE_MAX / sizeof(node*) / 2 ||
      bytes > j->limits.maximum_plan_bytes - j->allocated) {
    stop(j, RMR_ERR_LIMIT, "PDF scratch limit");
    return false;
  }
  if (!charge(j, count * depth)) return false;
  node** keys = rmr_alloc(bytes);
  if (!keys) {
    stop(j, RMR_ERR_OUT_OF_MEMORY, "PDF allocation failed");
    return false;
  }
  size_t n = 0;
  for (node* k = d->child; k; k = k->next->next) keys[n++] = k;
  qsort(keys, n, sizeof *keys, key_order);
  bool unique = true;
  for (size_t i = 1; i < n && unique; i++)
    unique = name_order(keys[i - 1], keys[i]) != 0;
  free(keys);
  return unique;
}

/* Parses one direct object into arena nodes. Strings, numbers and keywords
 * stay raw scalars; "n g R" becomes a reference. Rejects duplicate
 * dictionary keys, malformed tokens and nesting of 64 levels. */
static node* parse(scan* s, unsigned depth) {
  if (!tick(s->j)) return nullptr;
  if (depth >= 64) {
    stop(s->j, RMR_ERR_LIMIT, "PDF object nesting limit");
    return nullptr;
  }
  ws(s);
  if (s->pos >= s->size) goto bad;

  node* n = take(s->j, sizeof *n);
  if (!n) return nullptr;
  size_t start = s->pos;
  uint8_t c = s->data[s->pos++];
  n->raw = s->data + start;

  if (c == '[' || (c == '<' && s->pos < s->size && s->data[s->pos] == '<')) {
    n->type = c == '[' ? ARRAY : DICT;
    if (n->type == DICT) s->pos++;
    node** tail = &n->child;
    size_t count = 0;

    while (s->j->status == RMR_OK) {
      ws(s);
      if (s->pos >= s->size) goto bad;
      if (n->type == ARRAY && s->data[s->pos] == ']') {
        s->pos++;
        break;
      }
      if (n->type == DICT && s->size - s->pos >= 2 && s->data[s->pos] == '>' &&
          s->data[s->pos + 1] == '>') {
        if (count % 2) goto bad;
        s->pos += 2;
        if (!unique_keys(s->j, n, count / 2)) {
          if (s->j->status != RMR_OK) return nullptr;
          goto bad;
        }
        break;
      }

      node* v = parse(s, depth + 1);
      if (!v) return nullptr;

      if (n->type == DICT && count % 2 == 0 && v->type != NAME) goto bad;

      *tail = v;
      tail = &v->next;
      count++;
    }
  } else if (c == '(') {
    unsigned nesting = 1;

    while (s->pos < s->size && nesting) {
      c = s->data[s->pos++];
      if (c == '\\') {
        if (s->pos == s->size) goto bad;
        s->pos++;
      } else if (c == '(') {
        if (nesting >= 64) goto bad;
        nesting++;
      } else if (c == ')')
        nesting--;
    }

    if (nesting) goto bad;
  } else if (c == '<') {
    while (s->pos < s->size && s->data[s->pos] != '>') {
      c = s->data[s->pos++];
      if (!space(c) && hex(c) < 0) goto bad;
    }
    if (s->pos == s->size) goto bad;
    s->pos++;
  } else {
    if (c == '/')
      n->type = NAME;
    else if (delim(c))
      goto bad;

    while (s->pos < s->size && !delim(s->data[s->pos])) s->pos++;

    if (n->type == NAME) {
      for (size_t i = start + 1; i < s->pos; i++)
        if (s->data[i] == '#') {
          if (s->pos - i < 3 || hex(s->data[i + 1]) < 0 ||
              hex(s->data[i + 2]) < 0)
            goto bad;
          i += 2;
        }
    } else {
      size_t end = s->pos;
      scan t = *s;
      t.pos = start;
      uint64_t a, b;
      if (integer(&t, &a) && integer(&t, &b) && word(&t, "R")) {
        if (a > UINT32_MAX || b > 65535) goto bad;
        n->type = REF;
        n->number = (uint32_t)a;
        n->generation = (uint32_t)b;
        s->pos = t.pos;
      } else {
        bool valid = (end - start == 4 && !memcmp(n->raw, "null", 4)) ||
                     (end - start == 4 && !memcmp(n->raw, "true", 4)) ||
                     (end - start == 5 && !memcmp(n->raw, "false", 5));
        size_t i = start;
        bool digits = false, dot = false;
        if (s->data[i] == '+' || s->data[i] == '-') i++;

        for (; i < end; i++) {
          uint8_t b0 = s->data[i];
          if (b0 >= '0' && b0 <= '9')
            digits = true;
          else if (b0 == '.' && !dot)
            dot = true;
          else {
            digits = false;
            break;
          }
        }
        if (!valid && !digits) goto bad;
      }
    }
  }

  n->length = s->pos - start;
  return n;

bad:
  stop(s->j, RMR_ERR_INVALID_ARGUMENT, "malformed PDF object");
  return nullptr;
}

/* PDF integers may carry an explicit plus sign. */
static bool uint_value(node* n, uint64_t* out) {
  if (!n || n->type != SCALAR) return false;
  scan s = {.data = n->raw,
            .size = n->length,
            .pos = n->length > 1 && n->raw[0] == '+'};
  return integer(&s, out) && s.pos == s.size;
}

/* zlib state is heap memory charged to the scratch budget and released by
 * inflateEnd; each block records its accounted size in front of the payload. */
typedef struct {
  size_t size;
  max_align_t align;
} zblock;

/* zlib allocator charged to the job's scratch budget. */
static voidpf pdf_zalloc(voidpf context, uInt count, uInt size) {
  job* j = context;
  if (j->status != RMR_OK) return nullptr;
  if (size && count > (SIZE_MAX - sizeof(zblock)) / size) {
    stop(j, RMR_ERR_LIMIT, "zlib allocation overflow");
    return nullptr;
  }
  size_t n = (size_t)count * size + sizeof(zblock);
  if (n > j->limits.maximum_plan_bytes - j->allocated) {
    stop(j, RMR_ERR_LIMIT, "PDF scratch limit");
    return nullptr;
  }
  zblock* b = rmr_calloc(1, n);
  if (!b) {
    stop(j, RMR_ERR_OUT_OF_MEMORY, "zlib allocation failed");
    return nullptr;
  }
  b->size = n;
  j->allocated += n;
  return b + 1;
}

/* zlib deallocator; returns the block's size to the scratch budget. */
static void pdf_zfree(voidpf context, voidpf address) {
  job* j = context;
  if (!address) return;
  zblock* b = (zblock*)address - 1;
  j->allocated -= b->size;
  free(b);
}

/* Grows an arena block that is not yet linked into the arena list. *held is
 * the size already charged to the scratch budget. */
static bool grow_block(job* j, mem** block, size_t* held, size_t bytes) {
  size_t total = sizeof(mem) + bytes;
  if (total > *held &&
      total - *held > j->limits.maximum_plan_bytes - j->allocated) {
    stop(j, RMR_ERR_LIMIT, "PDF scratch limit");
    return false;
  }
  mem* m = rmr_realloc(*block, total);
  if (!m) {
    stop(j, RMR_ERR_OUT_OF_MEMORY, "PDF allocation failed");
    return false;
  }
  j->allocated = j->allocated - *held + total;
  *block = m;
  *held = total;
  return true;
}

enum { DECODED_STREAM_LIMIT = 64 << 20 };

/* Decode only the filters needed by xref/object streams. Flate output is
 * inflated once into a single growing buffer, bounded per stream and by the
 * caller's per-file decoded-byte budget, then adopted by the request arena. */
static bool decode_stream(job* j, size_t* budget, node* dict,
                          const uint8_t* raw, size_t length,
                          const uint8_t** out, size_t* size) {
  node* filter = get(dict, "Filter");
  *out = raw;
  *size = length;
  if (!filter) return true;
  if (filter->type == ARRAY && filter->child && !filter->child->next)
    filter = filter->child;
  if (!named(filter, "FlateDecode")) {
    stop(j, RMR_ERR_UNSUPPORTED, "PDF stream filter");
    return false;
  }
  if (length > UINT_MAX) {
    stop(j, RMR_ERR_LIMIT, "encoded PDF stream limit");
    return false;
  }

  z_stream codec = {.zalloc = pdf_zalloc,
                    .zfree = pdf_zfree,
                    .opaque = j,
                    .next_in = (Bytef*)(uintptr_t)raw,
                    .avail_in = (uInt)length};
  if (inflateInit(&codec) != Z_OK) {
    stop(j, RMR_ERR_OUT_OF_MEMORY, "zlib initialisation failed");
    return false;
  }

  mem* block = nullptr;
  size_t held = 0, capacity = 0;
  int status = Z_OK;

  while (tick(j)) {
    if (!codec.avail_out) {
      size_t next = capacity ? capacity * 2 : length < 1024 ? 1024 : length;
      if (next > DECODED_STREAM_LIMIT) next = DECODED_STREAM_LIMIT;
      if (next <= capacity) {
        stop(j, RMR_ERR_LIMIT, "decoded PDF stream limit");
        break;
      }
      if (next > *budget) {
        stop(j, RMR_ERR_LIMIT, "decoded PDF data budget");
        break;
      }
      if (!grow_block(j, &block, &held, next)) break;
      codec.next_out = (Bytef*)(block + 1) + capacity;
      codec.avail_out = (uInt)(next - capacity);
      capacity = next;
    }

    status = inflate(&codec, Z_NO_FLUSH);
    if (status != Z_OK && !(status == Z_BUF_ERROR && !codec.avail_out)) break;
  }

  inflateEnd(&codec);
  if (j->status == RMR_OK && status != Z_STREAM_END)
    stop(j,
         status == Z_MEM_ERROR ? RMR_ERR_OUT_OF_MEMORY
                               : RMR_ERR_INVALID_ARGUMENT,
         "invalid Flate stream");
  if (j->status != RMR_OK) {
    free(block);
    j->allocated -= held;
    return false;
  }

  block->next = j->memory;
  j->memory = block;
  *size = (size_t)codec.total_out;
  *budget -= *size;
  uint8_t* decoded = (uint8_t*)(block + 1);
  *out = decoded;

  node* params = get(dict, "DecodeParms");
  if (params && params->type == ARRAY && params->child && !params->child->next)
    params = params->child;
  if (!params || (params->type == SCALAR && params->length == 4 &&
                  !memcmp(params->raw, "null", 4)))
    return true;
  if (params->type != DICT) goto bad;

  uint64_t predictor = 1, columns = 1, colors = 1, bits = 8;
  if ((get(params, "Predictor") &&
       !uint_value(get(params, "Predictor"), &predictor)) ||
      (get(params, "Columns") &&
       !uint_value(get(params, "Columns"), &columns)) ||
      (get(params, "Colors") && !uint_value(get(params, "Colors"), &colors)) ||
      (get(params, "BitsPerComponent") &&
       !uint_value(get(params, "BitsPerComponent"), &bits)))
    goto bad;
  if (predictor == 1) return true;
  if (bits != 8 || !colors || !columns || columns > SIZE_MAX / colors) goto bad;
  size_t width = (size_t)(columns * colors);
  if (!width || width >= 64u * 1024u * 1024u) goto bad;

  if (predictor == 2) {
    if (*size % width) goto bad;
    for (size_t row = 0; row < *size; row += width) {
      if (!tick(j)) return false;
      for (size_t x = (size_t)colors; x < width; x++)
        decoded[row + x] =
            (uint8_t)(decoded[row + x] + decoded[row + x - colors]);
    }
    return true;
  }

  if (predictor < 10 || predictor > 15 || *size % (width + 1)) goto bad;
  size_t rows = *size / (width + 1);
  for (size_t row = 0; row < rows; row++) {
    if (!tick(j)) return false;
    unsigned row_filter = decoded[row * (width + 1)];
    if (row_filter > 4) goto bad;
    for (size_t x = 0; x < width; x++) {
      unsigned raw_value = decoded[row * (width + 1) + 1 + x];
      int left = x >= colors ? decoded[row * width + x - colors] : 0;
      int up = row ? decoded[(row - 1) * width + x] : 0;
      int corner =
          row && x >= colors ? decoded[(row - 1) * width + x - colors] : 0;
      int prediction = 0;
      if (row_filter == 1) prediction = left;
      if (row_filter == 2) prediction = up;
      if (row_filter == 3) prediction = (left + up) / 2;
      if (row_filter == 4) {
        int p = left + up - corner;
        int a = abs(p - left), b = abs(p - up), c = abs(p - corner);
        prediction = a <= b && a <= c ? left : b <= c ? up : corner;
      }
      decoded[row * width + x] = (uint8_t)(raw_value + (unsigned)prediction);
    }
  }

  *size = rows * width;
  return true;

bad:
  stop(j, RMR_ERR_INVALID_ARGUMENT, "invalid PDF predictor");
  return false;
}

/* Decoded object stream with its parsed (object number, offset) header. */
typedef struct {
  const uint8_t* data;
  size_t size, first, count;
  uint64_t* slots;
} packed;

/* Sorted cross-reference index: newest free entries shadow older live objects.
 * revision counts sections from the newest; table marks classic-table rows. */
typedef struct entry {
  uint32_t number, generation, mapped, container, index;
  size_t revision;
  size_t offset;
  bool live, reading, visited, table;
  node* object;
  const uint8_t* stream;
  size_t stream_size;
  packed* packed;
} entry;

typedef struct page {
  entry* object;
  node *resources, *media, *crop, *rotate;
  struct page* next;
} page;

/* Each cross-reference stream is decoded once and reused by both index passes.
 */
typedef struct {
  size_t offset, size;
  node* dict;
  const uint8_t* data;
} xref_stream;

enum { MAXIMUM_REVISIONS = 256, MAXIMUM_XREF_STREAMS = 2 * MAXIMUM_REVISIONS };
static const size_t decoded_file_budget = (size_t)256 << 20;

typedef struct {
  job* j;
  const uint8_t* bytes;
  size_t size, last, next_number;
  bool xref_streams;
  entry* entries;
  size_t count;
  node *trailer, *root;
  page *pages, *last_page;
  size_t page_count;
  size_t decoded_budget; /* Remaining decoded bytes for this file. */
  xref_stream* streams;
  size_t stream_count;
} file;

/* Index entry for object id, or null when the index has none. */
static entry* lookup(file* f, uint32_t id) {
  size_t lo = 0, hi = f->count;
  while (lo < hi) {
    size_t m = lo + (hi - lo) / 2;
    if (f->entries[m].number < id)
      lo = m + 1;
    else
      hi = m;
  }

  return lo < f->count && f->entries[lo].number == id ? &f->entries[lo]
                                                      : nullptr;
}

static node* resolve(file* f, node* n, unsigned depth);
static node* object(file* f, entry* e, unsigned depth);

/* An in-use xref row that names no object: offset 0 outside an object
 * stream. Such rows are skipped when every object is visited. */
static bool absent(const entry* e) {
  return e->live && !e->offset && !e->container;
}

/* Decodes an object stream and its header once per file. */
static packed* unpack(file* f, entry* container, unsigned depth) {
  node* d = object(f, container, depth);
  uint64_t count, first;
  if (!d || !container || container->container || container->generation ||
      !named(get(d, "Type"), "ObjStm") || !uint_value(get(d, "N"), &count) ||
      !uint_value(get(d, "First"), &first) || !count ||
      count > f->j->limits.maximum_elements)
    goto bad;

  packed* p = take(f->j, sizeof *p);
  if (!p || !decode_stream(f->j, &f->decoded_budget, d, container->stream,
                           container->stream_size, &p->data, &p->size))
    return nullptr;

  /* Every header pair needs at least four bytes ("1 0 "), bounding the table.
   */
  if (first > p->size || count > (first + 1) / 4) goto bad;
  p->first = (size_t)first;
  p->count = (size_t)count;

  p->slots = take(f->j, p->count * 2 * sizeof *p->slots);
  if (!p->slots) return nullptr;

  scan header = {f->j, p->data, p->first, 0};
  for (size_t i = 0; i < p->count; i++) {
    if (!tick(f->j)) return nullptr;
    uint64_t id, offset;
    if (!integer(&header, &id) || !integer(&header, &offset) || !id ||
        id > UINT32_MAX || offset >= p->size - p->first ||
        (i && offset <= p->slots[2 * i - 1]))
      goto bad;
    p->slots[2 * i] = id;
    p->slots[2 * i + 1] = offset;
  }

  container->packed = p;
  return p;

bad:
  stop(f->j, RMR_ERR_INVALID_ARGUMENT, "invalid PDF object stream");
  return nullptr;
}

/* Parses an indirect object once and caches it on the entry, reading it
 * from the file body or its object stream. For a stream object, also
 * records the raw stream bytes. Fails on free, missing or cyclic
 * objects. */
static node* object(file* f, entry* e, unsigned depth) {
  if (!e || !e->live) {
    stop(f->j, RMR_ERR_INVALID_ARGUMENT, "missing PDF object");
    return nullptr;
  }
  if (absent(e)) {
    stop(f->j, RMR_ERR_INVALID_ARGUMENT, "reference to an absent PDF object");
    return nullptr;
  }
  if (e->object) return e->object;
  if (e->reading || depth >= 64) {
    stop(f->j, RMR_ERR_INVALID_ARGUMENT, "cyclic PDF object");
    return nullptr;
  }

  e->reading = true;
  if (e->container) {
    entry* container = lookup(f, e->container);
    packed* p = container ? container->packed : nullptr;
    if (!p && !(p = unpack(f, container, depth + 1))) return nullptr;
    if (e->index >= p->count || p->slots[2 * (size_t)e->index] != e->number)
      goto bad;
    uint64_t selected = p->slots[2 * (size_t)e->index + 1];
    uint64_t end = (size_t)e->index + 1 < p->count
                       ? p->slots[2 * (size_t)e->index + 3]
                       : p->size - p->first;
    scan body = {f->j, p->data + p->first + selected, (size_t)(end - selected),
                 0};

    node* n = parse(&body, 0);
    ws(&body);
    if (!n || body.pos != body.size) goto bad;

    e->object = n;
    e->reading = false;
    return n;
  }

  scan s = {f->j, f->bytes, f->size, e->offset};
  uint64_t id, gen;
  if (!integer(&s, &id) || !integer(&s, &gen) || id != e->number ||
      gen != e->generation || !word(&s, "obj"))
    goto bad;

  node* n = parse(&s, 0);
  if (!n) return nullptr;

  if (n->type == DICT && word(&s, "stream")) {
    if (s.pos < f->size && s.data[s.pos] == '\r')
      s.pos++;
    else if (s.pos >= f->size || s.data[s.pos] != '\n')
      goto bad;
    if (s.pos < f->size && s.data[s.pos] == '\n') s.pos++;
    uint64_t length;
    node* len = resolve(f, get(n, "Length"), depth + 1);
    if (!uint_value(len, &length) || length > f->size - s.pos) goto bad;
    e->stream = f->bytes + s.pos;
    e->stream_size = (size_t)length;
    s.pos += (size_t)length;
    if (!word(&s, "endstream")) goto bad;
  }

  if (!word(&s, "endobj")) goto bad;

  e->object = n;
  e->reading = false;
  return n;

bad:
  stop(f->j, RMR_ERR_INVALID_ARGUMENT, "invalid PDF object or stream length");
  return nullptr;
}

/* Follows references until a direct object; other nodes pass through. */
static node* resolve(file* f, node* n, unsigned depth) {
  if (!n || n->type != REF) return n;
  if (depth >= 64) {
    stop(f->j, RMR_ERR_LIMIT, "PDF reference depth");
    return nullptr;
  }
  entry* e = lookup(f, n->number);
  if (!e || e->generation != n->generation) {
    stop(f->j, RMR_ERR_INVALID_ARGUMENT, "PDF reference generation mismatch");
    return nullptr;
  }

  node* v = object(f, e, depth);
  return v ? resolve(f, v, depth + 1) : nullptr;
}

/* PDF decimal numbers, without locale-dependent conversion or exponent syntax.
 */
static bool decimal(node* n, double* out) {
  if (!n || n->type != SCALAR || !n->length) return false;
  size_t i = 0;
  bool negative = false, dot = false, digit = false;
  if (n->raw[i] == '+' || n->raw[i] == '-') negative = n->raw[i++] == '-';
  double value = 0, fraction = .1;

  for (; i < n->length; i++) {
    unsigned c = n->raw[i];
    if (c == '.' && !dot) {
      dot = true;
      continue;
    }
    if (c < '0' || c > '9') return false;
    digit = true;
    if (dot) {
      value += (double)(c - '0') * fraction;
      fraction *= .1;
    } else
      value = value * 10 + (double)(c - '0');
  }

  *out = negative ? -value : value;
  return digit && isfinite(value);
}

/* True if both values resolve to arrays of four equal numbers. */
static bool equal_boxes(file* f, node* a, node* b) {
  a = resolve(f, a, 0);
  b = resolve(f, b, 0);
  if (!a || !b || a->type != ARRAY || b->type != ARRAY) return false;
  a = a->child;
  b = b->child;

  for (size_t i = 0; i < 4; i++) {
    double x, y;
    if (!a || !b || !decimal(resolve(f, a, 0), &x) ||
        !decimal(resolve(f, b, 0), &y) || x != y)
      return false;
    a = a->next;
    b = b->next;
  }

  return !a && !b;
}

/* qsort order: object number, newest revision, table rows before stream
 * rows, then original row order. */
static int entry_order(const void* a, const void* b) {
  const entry *x = a, *y = b;
  if (x->number != y->number) return x->number < y->number ? -1 : 1;
  if (x->revision != y->revision) return x->revision < y->revision ? -1 : 1;
  if (x->table != y->table) return x->table ? -1 : 1;
  return (x->mapped > y->mapped) - (x->mapped < y->mapped);
}

/* Parses and decodes the cross-reference stream object at offset; cached
 * per file so both index passes share it. */
static xref_stream* load_xref_stream(file* f, size_t offset) {
  for (size_t i = 0; i < f->stream_count; i++)
    if (f->streams[i].offset == offset) return &f->streams[i];
  if (!f->streams &&
      !(f->streams = take(f->j, MAXIMUM_XREF_STREAMS * sizeof *f->streams)))
    return nullptr;

  scan s = {f->j, f->bytes, f->size, offset};
  uint64_t number, gen, length, size;
  if (f->stream_count == MAXIMUM_XREF_STREAMS || !integer(&s, &number) ||
      !number || number > UINT32_MAX || !integer(&s, &gen) || gen > 65535 ||
      !word(&s, "obj"))
    goto bad;

  node* d = parse(&s, 0);
  if (!d || d->type != DICT || !named(get(d, "Type"), "XRef") ||
      !uint_value(get(d, "Length"), &length) ||
      !uint_value(get(d, "Size"), &size) || !size || size > UINT32_MAX ||
      !word(&s, "stream"))
    goto bad;
  if (s.pos >= s.size || (s.data[s.pos] != '\r' && s.data[s.pos] != '\n'))
    goto bad;
  if (s.data[s.pos] == '\r') s.pos++;
  if (s.pos < s.size && s.data[s.pos] == '\n') s.pos++;
  if (length > s.size - s.pos) goto bad;

  xref_stream x = {.offset = offset, .dict = d};
  if (!decode_stream(f->j, &f->decoded_budget, d, s.data + s.pos,
                     (size_t)length, &x.data, &x.size))
    return nullptr;

  s.pos += (size_t)length;
  if (!word(&s, "endstream") || !word(&s, "endobj")) goto bad;
  f->streams[f->stream_count] = x;
  return &f->streams[f->stream_count++];

bad:
  stop(f->j, RMR_ERR_INVALID_ARGUMENT, "invalid PDF cross-reference stream");
  return nullptr;
}

/* Counts the rows of the xref stream at offset, also storing them when
 * fill is set. Returns the stream dictionary, which acts as the trailer. */
static node* stream_section(file* f, size_t offset, bool fill, size_t* used,
                            size_t revision) {
  xref_stream* x = load_xref_stream(f, offset);
  if (!x) return nullptr;
  node* d = x->dict;
  const uint8_t* data = x->data;
  size_t count = x->size;

  uint64_t size;
  if (!uint_value(get(d, "Size"), &size)) goto bad;

  node* w = get(d, "W");
  if (!w || w->type != ARRAY) goto bad;
  uint64_t widths[3];
  node* v = w->child;
  for (unsigned i = 0; i < 3; i++) {
    if (!v || !uint_value(v, &widths[i]) || widths[i] > 8) goto bad;
    v = v->next;
  }
  size_t stride = (size_t)(widths[0] + widths[1] + widths[2]);
  if (v || !stride) goto bad;

  node* index = get(d, "Index");
  if (index && (index->type != ARRAY || !index->child)) goto bad;
  node* range = index ? index->child : nullptr;
  size_t cursor = 0;

  do {
    uint64_t first = 0, n = size;
    if (range) {
      if (!uint_value(range, &first) || !range->next ||
          !uint_value(range->next, &n))
        goto bad;
      range = range->next->next;
    }
    if (first > size || n > size - first || n > (count - cursor) / stride)
      goto bad;

    for (uint64_t i = 0; i < n; i++) {
      if (!tick(f->j)) return nullptr;
      uint64_t fields[3] = {widths[0] ? 0 : 1, 0, 0};
      for (unsigned k = 0; k < 3; k++)
        for (uint64_t j = 0; j < widths[k]; j++)
          fields[k] = (fields[k] << 8) | data[cursor++];
      entry e = {.number = (uint32_t)(first + i), .revision = revision};
      if (fields[0] && !e.number) goto bad;
      if (fields[0] == 0) {
        if (fields[2] > 65535) goto bad;
      } else if (fields[0] == 1) {
        /* Offset 0 with generation 0 is an absent object, as in a table. */
        if ((!fields[1] && fields[2]) || fields[1] >= f->size ||
            fields[2] > 65535)
          goto bad;
        e.live = true;
        e.offset = (size_t)fields[1];
        e.generation = (uint32_t)fields[2];
      } else if (fields[0] == 2) {
        if (!fields[1] || fields[1] >= size || fields[2] > UINT32_MAX) goto bad;
        e.live = true;
        e.container = (uint32_t)fields[1];
        e.index = (uint32_t)fields[2];
      } else
        goto bad;
      if (*used == SIZE_MAX) goto bad;
      if (fill) {
        if (*used >= f->count) goto bad;
        f->entries[*used] = e;
      }
      (*used)++;
    }
  } while (range);

  if (cursor != count) goto bad;
  return d;

bad:
  stop(f->j, RMR_ERR_INVALID_ARGUMENT, "invalid PDF cross-reference stream");
  return nullptr;
}

/* Build xrefs in two passes. No allocation proportional to a hostile /Size. */
static bool sections(file* f, bool fill) {
  size_t offset = f->last, used = 0, iterations = 0;
  size_t visited[MAXIMUM_REVISIONS];

  while (f->j->status == RMR_OK) {
    if (++iterations > MAXIMUM_REVISIONS) {
      stop(f->j, RMR_ERR_LIMIT, "PDF revision limit");
      break;
    }
    for (size_t i = 0; i + 1 < iterations; i++)
      if (visited[i] == offset) goto bad;
    visited[iterations - 1] = offset;

    scan s = {f->j, f->bytes, f->size, offset};
    node* d = nullptr;
    bool classic = word(&s, "xref");
    if (!classic) {
      f->xref_streams = true;
      d = stream_section(f, offset, fill, &used, iterations);
      if (!d) return false;
    }

    while (classic && !word(&s, "trailer")) {
      if (!tick(f->j)) return false;
      uint64_t first, count;
      if (!integer(&s, &first) || !integer(&s, &count) || first > UINT32_MAX ||
          count > UINT32_MAX - first)
        goto bad;
      if (count > f->j->limits.maximum_elements) {
        stop(f->j, RMR_ERR_LIMIT, "PDF xref entry limit");
        return false;
      }

      for (uint64_t i = 0; i < count; i++) {
        uint64_t at, gen;
        if (!tick(f->j)) return false;
        if (!integer(&s, &at) || !integer(&s, &gen) || gen > 65535) goto bad;
        bool live = word(&s, "n");
        if (!live && !word(&s, "f")) goto bad;
        /* Quartz writes an in-use row at offset 0 for an object number it
         * never emits. The row is kept as an absent object: harmless while
         * unreferenced, an error once anything resolves it. */
        if (live && ((at == 0 && gen != 0) || at >= f->size || first + i == 0))
          goto bad;
        if (used == SIZE_MAX) goto bad;
        if (fill) {
          if (used >= f->count) goto bad;
          f->entries[used] = (entry){.number = (uint32_t)(first + i),
                                     .generation = (uint32_t)gen,
                                     .offset = (size_t)at,
                                     .live = live,
                                     .table = true,
                                     .revision = iterations};
        }
        used++;
      }
    }

    if (classic) d = parse(&s, 0);
    if (!d || d->type != DICT) goto bad;

    if (get(d, "Encrypt")) {
      stop(f->j, RMR_ERR_UNSUPPORTED, "encrypted PDF");
      return false;
    }

    if (classic && get(d, "XRefStm")) {
      f->xref_streams = true;
      uint64_t hybrid;
      if (!uint_value(get(d, "XRefStm"), &hybrid) || !hybrid ||
          hybrid >= f->size || hybrid == offset)
        goto bad;
      /* Companion rows share this revision; open_file prefers the table. */
      node* extra = stream_section(f, (size_t)hybrid, fill, &used, iterations);
      if (!extra) return false;
      if (get(extra, "Encrypt")) {
        stop(f->j, RMR_ERR_UNSUPPORTED, "encrypted PDF");
        return false;
      }
    }

    if (fill) {
      if (!f->trailer)
        f->trailer = d;
      else { /* Carry missing trailer values from earlier revisions. */
        for (node* k = d->child; k; k = k->next->next)
          if ((named(k, "Root") && !get(f->trailer, "Root")) ||
              (named(k, "Info") && !get(f->trailer, "Info")) ||
              (named(k, "ID") && !get(f->trailer, "ID"))) {
            node *key = take(f->j, sizeof *key),
                 *value = take(f->j, sizeof *value);
            if (!key || !value) return false;
            *key = *k;
            *value = *k->next;
            value->next = f->trailer->child;
            key->next = value;
            f->trailer->child = key;
          }
      }
    }

    uint64_t prev;
    node* p = get(d, "Prev");
    if (!p) break;
    if (!uint_value(p, &prev) || prev == 0 || prev >= f->size) goto bad;
    offset = (size_t)prev;
  }

  if (!fill) f->count = used;
  return f->j->status == RMR_OK;

bad:
  stop(f->j, RMR_ERR_INVALID_ARGUMENT, "invalid PDF cross-reference table");
  return false;
}

/* Walks the page tree depth-first, appending each page with its inherited
 * Resources, MediaBox, CropBox and Rotate. Refuses cycles, bad nodes and
 * a /UserUnit other than 1. */
static bool tree(file* f, node* ref, node* resources, node* media, node* crop,
                 node* rotate, unsigned depth) {
  if (!tick(f->j)) return false;
  if (depth >= 64 || !ref || ref->type != REF) goto bad;
  entry* e = lookup(f, ref->number);
  if (!e || e->visited || e->generation != ref->generation) goto bad;
  e->visited = true;
  node* d = object(f, e, 0);
  if (!d || d->type != DICT) goto bad;

  node* v;
  if ((v = get(d, "Resources"))) resources = v;
  if ((v = get(d, "MediaBox"))) media = v;
  if ((v = get(d, "CropBox"))) crop = v;
  if ((v = get(d, "Rotate"))) rotate = v;

  if (get(d, "UserUnit")) {
    double unit;
    if (!decimal(resolve(f, get(d, "UserUnit"), 0), &unit) || unit != 1) {
      stop(f->j, RMR_ERR_UNSUPPORTED, "non-unit PDF UserUnit");
      return false;
    }
  }

  if (named(get(d, "Type"), "Pages")) {
    node* kids = resolve(f, get(d, "Kids"), 0);
    if (!kids || kids->type != ARRAY) goto bad;
    for (node* k = kids->child; k; k = k->next)
      if (!tree(f, k, resources, media, crop, rotate, depth + 1)) return false;
  } else if (named(get(d, "Type"), "Page")) {
    if (f->page_count >= f->j->limits.maximum_draws) {
      stop(f->j, RMR_ERR_LIMIT, "PDF page limit");
      return false;
    }
    page* p = take(f->j, sizeof *p);
    if (!p) return false;
    *p = (page){e, resources, media, crop, rotate, nullptr};
    if (f->last_page)
      f->last_page->next = p;
    else
      f->pages = p;
    f->last_page = p;
    f->page_count++;
  } else
    goto bad;

  return true;

bad:
  stop(f->j, RMR_ERR_INVALID_ARGUMENT, "invalid or cyclic PDF page tree");
  return false;
}

/* Indexes every xref revision of a PDF (newest row per object wins),
 * resolves the catalog and collects the pages. Refuses encryption and
 * catalog /Perms. Allocations live in the job's arena. */
static bool open_file(file* f, job* j, const uint8_t* bytes, size_t size) {
  *f = (file){.j = j,
              .bytes = bytes,
              .size = size,
              .decoded_budget = decoded_file_budget};

  if (size > j->limits.maximum_pdf_bytes) {
    stop(j, RMR_ERR_LIMIT, "PDF input size limit");
    return false;
  }

  if (!bytes || size < 9 || memcmp(bytes, "%PDF-", 5)) goto bad;

  size_t at = size - 9;
  bool found = false;
  for (;;) {
    if (!memcmp(bytes + at, "startxref", 9)) {
      found = true;
      break;
    }
    if (at == 0 || size - at > 65536) break;
    at--;
  }
  if (!found) goto bad;

  scan s = {j, bytes, size, at + 9};
  uint64_t offset;
  if (!integer(&s, &offset) || offset == 0 || offset >= size) goto bad;
  f->last = (size_t)offset;

  if (!sections(f, false)) return false;
  if (f->count > SIZE_MAX / sizeof(entry) || f->count > UINT32_MAX) goto bad;

  f->entries = take(j, f->count * sizeof(entry));
  if (!f->entries) return false;

  if (!sections(f, true)) return false;

  for (size_t i = 0; i < f->count; i++) f->entries[i].mapped = (uint32_t)i;

  /* Rows sort by object, then newest revision, then classic table before
   * stream rows; a row repeated within one section is adjacent. */
  qsort(f->entries, f->count, sizeof(entry), entry_order);
  size_t unique = 0;
  for (size_t i = 0; i < f->count;) {
    size_t end = i + 1;
    while (end < f->count && f->entries[end].number == f->entries[i].number) {
      if (!tick(j)) return false;
      if (f->entries[end].revision == f->entries[end - 1].revision &&
          f->entries[end].table == f->entries[end - 1].table)
        goto bad;
      end++;
    }
    /* Hybrid files (ISO 32000-1 7.5.8.4): the table wins; its companion
     * stream supplies an object only where the table marks it free. */
    size_t best = i;
    if (f->entries[i].table && !f->entries[i].live && i + 1 < end &&
        f->entries[i + 1].revision == f->entries[i].revision)
      best = i + 1;
    f->entries[unique] = f->entries[best];
    f->entries[unique++].mapped = 0;
    i = end;
  }

  f->count = unique;
  uint64_t declared;
  if (!uint_value(get(f->trailer, "Size"), &declared) ||
      declared > UINT32_MAX || !declared)
    goto bad;
  f->next_number = (size_t)declared;
  if (unique && f->entries[unique - 1].number >= f->next_number)
    f->next_number = (size_t)f->entries[unique - 1].number + 1;

  node* root = get(f->trailer, "Root");
  f->root = root && root->type == REF ? resolve(f, root, 0) : nullptr;
  if (!f->root || f->root->type != DICT) goto bad;

  if (get(f->root, "Perms")) {
    stop(j, RMR_ERR_UNSUPPORTED,
         "certified/rights-managed PDF requires signature permission handling");
    return false;
  }

  return tree(f, get(f->root, "Pages"), nullptr, nullptr, nullptr, nullptr, 0);

bad:
  stop(j, RMR_ERR_INVALID_ARGUMENT, "invalid PDF file");
  return false;
}

typedef struct update {
  uint32_t id, gen;
  size_t offset;
  struct update* next;
} update;

/* Output already moved out of a chunked writer's buffer: mapped pages of
 * their own rather than heap blocks, so that releasing a chunk while the
 * output is assembled returns its memory at once. */
typedef struct chunk {
  struct chunk* next;
  size_t size, mapped;
  uint8_t bytes[];
} chunk;

/* A chunk able to hold size bytes; null when no memory is left. */
static chunk* chunk_new(size_t size) {
  size_t mapped = sizeof(chunk) + size;
#ifdef _WIN32
  chunk* c =
      VirtualAlloc(nullptr, mapped, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
  void* p = mmap(nullptr, mapped, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0);
  chunk* c = p == MAP_FAILED ? nullptr : p;
#endif
  if (c) *c = (chunk){.size = size, .mapped = mapped};
  return c;
}

static void chunk_free(chunk* c) {
#ifdef _WIN32
  VirtualFree(c, 0, MEM_RELEASE);
#else
  munmap(c, c->mapped);
#endif
}

typedef rmr_sha256 digester;
typedef rmr_sha256_digest digest;

/* Adds n bytes to the digest. */
static void absorb(digester* h, const void* bytes, size_t n) {
  rmr_sha256_update(h, bytes, n);
}

/* Adds v as eight bytes. */
static void absorb_size(digester* h, uint64_t v) {
  uint8_t b[8];
  for (unsigned k = 0; k < 8; k++) b[k] = (uint8_t)(v >> (8 * k));
  absorb(h, b, sizeof b);
}

/* The first eight bytes of d, where a table of digests starts looking. */
static size_t digest_slot(const digest* d) {
  uint64_t v = 0;
  for (unsigned k = 0; k < 8; k++) v |= (uint64_t)d->bytes[k] << (8 * k);
  return (size_t)v;
}

/* Objects an update has imported, by digest: an object imported again,
 * identical down to the objects it refers to, is referred to rather than
 * written again. Only SHA-256 digests and object numbers are kept, in an
 * open addressing table charged to j's scratch budget: 18 MiB at most, and no
 * more than a quarter of the budget, so that the update's own records keep
 * the rest. Past MAXIMUM_SHARED entries, or once the table may not grow,
 * objects are no longer recorded, those recorded still shared. */
typedef struct {
  digest d;
  uint32_t number;
} shared_object;

typedef struct ledger {
  job* j;
  shared_object* slots;
  size_t capacity, count;
} ledger;

enum { MAXIMUM_SHARED = 1 << 18 };

/* The number of the object recorded under d; 0 when there is none. */
static uint32_t ledger_find(const ledger* l, digest d) {
  if (!l->capacity) return 0;
  for (size_t at = digest_slot(&d) & (l->capacity - 1); l->slots[at].number;
       at = (at + 1) & (l->capacity - 1))
    if (!memcmp(l->slots[at].d.bytes, d.bytes, sizeof d.bytes))
      return l->slots[at].number;
  return 0;
}

/* Records object number under d, unless the ledger is full. */
static void ledger_add(ledger* l, digest d, uint32_t number) {
  job* j = l->j;
  if (l->count >= MAXIMUM_SHARED || ledger_find(l, d)) return;
  if (l->count + 1 > l->capacity / 2) {
    size_t capacity = l->capacity ? l->capacity * 2 : 1024,
           bytes = capacity * sizeof *l->slots,
           held = l->capacity * sizeof *l->slots;
    if (bytes > j->limits.maximum_plan_bytes / 4 ||
        bytes - held > j->limits.maximum_plan_bytes - j->allocated)
      return;
    shared_object* grown = rmr_calloc(capacity, sizeof *grown);
    if (!grown) {
      stop(j, RMR_ERR_OUT_OF_MEMORY, "PDF allocation failed");
      return;
    }
    for (size_t i = 0; i < l->capacity; i++)
      if (l->slots[i].number) {
        size_t at = digest_slot(&l->slots[i].d) & (capacity - 1);
        while (grown[at].number) at = (at + 1) & (capacity - 1);
        grown[at] = l->slots[i];
      }
    free(l->slots);
    j->allocated = j->allocated - held + bytes;
    l->slots = grown;
    l->capacity = capacity;
  }
  size_t at = digest_slot(&d) & (l->capacity - 1);
  while (l->slots[at].number) at = (at + 1) & (l->capacity - 1);
  l->slots[at] = (shared_object){d, number};
  l->count++;
}

/* renumber, when set, is the file whose objects are written under new numbers
 * (its entries' mapped field), trailer values included. records, when set,
 * is the job holding the xref records instead of j. A chunked writer moves
 * each full megabytes of output aside instead of growing one buffer, so that
 * a long file never needs room for twice its size. A writer with an output
 * hands its bytes on to it instead, a chunk's worth at a time, keeping none
 * once they are written. shared, when set, is the ledger of imported objects
 * that imports refer to instead of writing them again. */
typedef struct {
  job* j;
  rmr_buffer data;
  size_t capacity, next_number;
  update* updates;
  size_t count;
  file* renumber;
  job* records;
  bool chunked;
  chunk *chunks, *last_chunk;
  size_t flushed;
  const rmr_output* output;
  ledger* shared;
} writer;

enum { CHUNK_BYTES = 4 << 20 };

/* Offset in the output of the next byte written. */
static size_t position(const writer* w) { return w->flushed + w->data.size; }

/* Hands n bytes, which follow those buffered, to the writer's output. */
static void emit(writer* w, const void* bytes, size_t n) {
  if (w->j->status != RMR_OK || !n) return;
  rmr_status s = w->output->write(w->output->context, bytes, n);
  if (s != RMR_OK)
    stop(w->j, s, "PDF output write failed");
  else
    w->flushed += n;
}

/* Hands what is buffered to the writer's output. */
static void drain(writer* w) {
  size_t n = w->data.size;
  w->data.size = 0;
  emit(w, w->data.bytes, n);
}

/* Appends bytes to the output, growing it up to maximum_pdf_bytes. */
static void put(writer* w, const void* bytes, size_t n) {
  if (w->j->status != RMR_OK || !n) return;
  if (n > w->j->limits.maximum_pdf_bytes - position(w)) {
    stop(w->j, RMR_ERR_LIMIT, "PDF output size limit");
    return;
  }

  /* Output goes on once a chunk's worth is buffered; as much again or more
   * goes on at once, unbuffered. */
  if (w->output && w->data.size + n > CHUNK_BYTES) {
    drain(w);
    if (n >= CHUNK_BYTES) {
      emit(w, bytes, n);
      return;
    }
    if (w->j->status != RMR_OK) return;
  }

  if (w->chunked && !w->output && w->data.size >= CHUNK_BYTES &&
      n > w->capacity - w->data.size) {
    chunk* c = chunk_new(w->data.size);
    if (!c) {
      stop(w->j, RMR_ERR_OUT_OF_MEMORY, "PDF output allocation");
      return;
    }
    memcpy(c->bytes, w->data.bytes, w->data.size);
    if (w->last_chunk)
      w->last_chunk->next = c;
    else
      w->chunks = c;
    w->last_chunk = c;
    w->flushed += w->data.size;
    w->data.size = 0;
  }

  size_t needed = w->data.size + n;
  if (needed > w->capacity) {
    size_t cap = w->capacity ? w->capacity : 4096;
    while (cap < needed) {
      if (cap > w->j->limits.maximum_pdf_bytes / 2) {
        cap = w->j->limits.maximum_pdf_bytes;
        break;
      }
      cap *= 2;
    }
    if (cap > w->j->limits.maximum_pdf_bytes)
      cap = w->j->limits.maximum_pdf_bytes;
    void* p = rmr_realloc(w->data.bytes, cap);
    if (!p) {
      stop(w->j, RMR_ERR_OUT_OF_MEMORY, "PDF output allocation");
      return;
    }
    w->data.bytes = p;
    w->capacity = cap;
  }

  memcpy(w->data.bytes + w->data.size, bytes, n);
  w->data.size += n;
}

/* Appends a NUL-terminated string. */
static void text(writer* w, const char* s) { put(w, s, strlen(s)); }

/* Appends printf-style text; output longer than 511 bytes fails the
 * job. */
static void format(writer* w, const char* fmt, ...) {
  char b[512];
  va_list a;
  va_start(a, fmt);
  int n = vsnprintf(b, sizeof b, fmt, a);
  va_end(a);

  if (n < 0 || (size_t)n >= sizeof b) {
    stop(w->j, RMR_ERR_LIMIT, "PDF number formatting");
    return;
  }
  text(w, b);
}

/* C numeric locale may use a comma. PDF decimal tokens always use a dot. */
static void real(writer* w, double v) {
  if (!isfinite(v) || fabs(v) > w->j->limits.maximum_coordinate) {
    stop(w->j, RMR_ERR_LIMIT, "PDF coordinate limit");
    return;
  }

  char scientific[64], digits[24], fixed[400];
  int n = snprintf(scientific, sizeof scientific, "%.17e", fabs(v));
  if (n < 0 || (size_t)n >= sizeof scientific) {
    stop(w->j, RMR_ERR_LIMIT, "PDF number formatting");
    return;
  }

  char* exponent = strchr(scientific, 'e');
  if (!exponent) {
    stop(w->j, RMR_ERR_INVALID_ARGUMENT, "PDF numeric locale");
    return;
  }

  size_t nd = 0;
  for (char* p = scientific; p < exponent; p++)
    if (*p >= '0' && *p <= '9') {
      if (nd == sizeof digits) {
        stop(w->j, RMR_ERR_LIMIT, "PDF decimal digits");
        return;
      }
      digits[nd++] = *p;
    }

  int point = 1 + (int)strtol(exponent + 1, nullptr, 10);
  size_t used = 0;
  if (v < 0) fixed[used++] = '-';

  if (point <= 0) {
    fixed[used++] = '0';
    fixed[used++] = '.';
    for (int i = 0; i < -point; i++) fixed[used++] = '0';
    memcpy(fixed + used, digits, nd);
    used += nd;
  } else {
    for (int i = 0; i < point; i++)
      fixed[used++] = (size_t)i < nd ? digits[i] : '0';
    if ((size_t)point < nd) {
      fixed[used++] = '.';
      memcpy(fixed + used, digits + point, nd - (size_t)point);
      used += nd - (size_t)point;
    }
  }

  fixed[used] = 0;
  if (strchr(fixed, '.')) {
    while (used && fixed[used - 1] == '0') fixed[--used] = 0;
    if (used && fixed[used - 1] == '.') fixed[--used] = 0;
  }

  text(w, fixed);
}

/* Next unused object number; 0 after failing the job on exhaustion. */
static uint32_t reserve(writer* w) {
  if (w->next_number >= UINT32_MAX) {
    stop(w->j, RMR_ERR_LIMIT, "PDF object number limit");
    return 0;
  }
  return (uint32_t)w->next_number++;
}

/* Opens indirect object id and records its offset for the xref. */
static void begin(writer* w, uint32_t id, uint32_t gen) {
  if (!tick(w->j)) return;
  if (!id) {
    stop(w->j, RMR_ERR_INVALID_ARGUMENT, "PDF object number 0 is reserved");
    return;
  }
  update* u = take(w->records ? w->records : w->j, sizeof *u);
  if (!u) {
    if (w->records) stop(w->j, w->records->status, w->records->reason);
    return;
  }

  *u = (update){id, gen, position(w), w->updates};
  w->updates = u;
  w->count++;
  format(w, "%u %u obj\n", id, gen);
}

/* Closes the current indirect object. */
static void end(writer* w) { text(w, "\nendobj\n"); }

/* Serializes a node. With import set, references are renumbered to the
 * object numbers assigned to that file's imported objects. */
static void value(writer* w, file* import, node* n, unsigned depth) {
  if (!n) {
    text(w, "null");
    return;
  }
  if (!tick(w->j)) return;
  if (depth >= 64) {
    stop(w->j, RMR_ERR_LIMIT, "PDF write nesting");
    return;
  }

  if (n->type == REF && import) {
    entry* e = lookup(import, n->number);
    if (!e || !e->live || !e->mapped || e->generation != n->generation) {
      stop(w->j, RMR_ERR_INVALID_ARGUMENT, "missing imported reference");
      return;
    }
    format(w, "%u 0 R", e->mapped);
  } else if (n->type == ARRAY || n->type == DICT) {
    text(w, n->type == ARRAY ? "[" : "<<");
    for (node* c = n->child; c; c = c->next) {
      value(w, import, c, depth + 1);
      text(w, " ");
    }
    text(w, n->type == ARRAY ? "]" : ">>");
  } else
    put(w, n->raw, n->length);
}

/* Writes the entries of dictionary d except the count keys in skip. */
static void fields(writer* w, file* import, node* d, const char* const* skip,
                   size_t count) {
  if (!d) return;
  if (d->type != DICT) {
    stop(w->j, RMR_ERR_INVALID_ARGUMENT, "expected PDF dictionary");
    return;
  }

  for (node* k = d->child; k; k = k->next->next) {
    bool omit = false;
    for (size_t i = 0; i < count; i++)
      if (named(k, skip[i])) omit = true;
    if (!omit) {
      value(w, import, k, 0);
      text(w, " ");
      value(w, import, k->next, 0);
      text(w, "\n");
    }
  }
}

/* Writes a new unfiltered stream object holding s; returns its number. */
static uint32_t stream_text(writer* w, const char* s) {
  uint32_t id = reserve(w);
  begin(w, id, 0);
  format(w, "<< /Length %zu >>\nstream\n", strlen(s));
  text(w, s);
  text(w, "\nendstream");
  end(w);
  return id;
}

/* Entry of the indirect content stream that contents refers to, either
 * directly or as a one-element array; null after failing the job
 * otherwise. */
static entry* single_stream(file* f, node* contents) {
  if (!contents) return nullptr;

  node* v = resolve(f, contents, 0);
  if (v && v->type == ARRAY) {
    if (!v->child || v->child->next) {
      stop(f->j, RMR_ERR_UNSUPPORTED, "overlay PDF needs one content stream");
      return nullptr;
    }
    contents = v->child;
  }

  if (contents->type != REF) {
    stop(f->j, RMR_ERR_INVALID_ARGUMENT, "content stream must be indirect");
    return nullptr;
  }

  entry* e = lookup(f, contents->number);
  if (!e || e->generation != contents->generation || !object(f, e, 0) ||
      !e->stream) {
    stop(f->j, RMR_ERR_INVALID_ARGUMENT, "invalid content stream");
    return nullptr;
  }

  return e;
}

static bool import_reachable(writer* w, file* f, node* const* roots,
                             size_t count);
static size_t dict_values(node* d, const char* except, node** out, size_t room);

/* Imports a one-page overlay file and wraps its page as a Form XObject: the
 * objects its resources, box, group and content stream's other entries refer
 * to come along, not the file's catalog, page tree, page or content stream
 * object, which the form replaces. Returns the form's object number, or 0
 * on failure. */
static uint32_t form(writer* w, file* f) {
  page* p = f->pages;
  uint64_t rotation = 0;
  if (p->rotate &&
      (!uint_value(resolve(f, p->rotate, 0), &rotation) || rotation % 360)) {
    stop(w->j, RMR_ERR_UNSUPPORTED, "rotated overlay PDF");
    return 0;
  }
  entry* e = single_stream(f, get(p->object->object, "Contents"));
  if (!e) {
    stop(w->j, RMR_ERR_INVALID_ARGUMENT, "overlay PDF has no content stream");
    return 0;
  }
  node* box = resolve(f, p->media, 0);
  if (!box || box->type != ARRAY) {
    stop(w->j, RMR_ERR_INVALID_ARGUMENT, "overlay has no media box");
    return 0;
  }

  node* group = get(p->object->object, "Group");
  node* roots[64];
  size_t n = 0;
  roots[n++] = p->resources;
  roots[n++] = box;
  if (group) roots[n++] = group;
  n += dict_values(e->object, "Length", roots + n, 64 - n);
  if (!import_reachable(w, f, roots, n)) return 0;

  uint32_t id = reserve(w);
  begin(w, id, 0);
  text(w, "<< /Type /XObject /Subtype /Form /FormType 1\n/BBox ");
  value(w, f, box, 0);
  text(w, "\n/Resources ");
  value(w, f, p->resources, 0);
  const char* skip[] = {"Type",      "Subtype", "FormType", "BBox",
                        "Resources", "Matrix",  "Group",    "Length"};
  text(w, "\n");
  fields(w, f, e->object, skip, sizeof skip / sizeof *skip);

  if (group) {
    text(w, "/Group ");
    value(w, f, group, 0);
    text(w, "\n");
  }

  format(w, "/Length %zu >>\nstream\n", e->stream_size);
  put(w, e->stream, e->stream_size);
  text(w, "\nendstream");
  end(w);
  return id;
}

/* Writes a page's existing content-stream references, each checked to be
 * an indirect stream. */
static void contents(writer* w, file* f, node* n) {
  if (!n) return;
  node* v = resolve(f, n, 0);
  if (!v || (v->type == SCALAR && v->length == 4 && !memcmp(v->raw, "null", 4)))
    return;

  if (v->type == ARRAY) {
    for (node* c = v->child; c; c = c->next) {
      if (!single_stream(f, c)) return;
      value(w, nullptr, c, 0);
      text(w, " ");
    }
  } else {
    if (!single_stream(f, n)) return;
    value(w, nullptr, n, 0);
    text(w, " ");
  }
}

/* Writes a rectangle as a PDF [llx lly urx ury] array. */
static void rectangle(writer* w, rmr_rect b) {
  text(w, "[");
  real(w, b.x);
  text(w, " ");
  real(w, b.y);
  text(w, " ");
  real(w, b.x + b.width);
  text(w, " ");
  real(w, b.y + b.height);
  text(w, "]");
}

enum { MAXIMUM_PARTS = 8 };

/* Rewrites source page p so that it draws the parts, one-page overlay files
 * each imported with its objects as a Form XObject, in order under the
 * first part's transform, after its own contents between the streams open
 * ("q") and close ("Q"). With the first part's replace_box, also replaces
 * MediaBox/CropBox and keeps the old MediaBox as ArtBox. */
static void overlay(writer* w, file* source, page* p,
                    const rmr_pdf_overlay* parts, size_t count, uint32_t open,
                    uint32_t close) {
  const rmr_pdf_overlay* o = &parts[0];
  uint32_t forms[MAXIMUM_PARTS];
  for (size_t i = 0; i < count && w->j->status == RMR_OK; i++) {
    file imported;
    if (!open_file(&imported, w->j, parts[i].pdf, parts[i].size)) return;
    if (imported.page_count != 1) {
      stop(w->j, RMR_ERR_INVALID_ARGUMENT, "overlay PDF must have one page");
      return;
    }
    forms[i] = form(w, &imported);
  }
  if (w->j->status != RMR_OK) return;

  node *resources = resolve(source, p->resources, 0),
       *objects = resolve(source, get(resources, "XObject"), 0);
  if ((resources && resources->type != DICT) ||
      (objects && objects->type != DICT)) {
    stop(w->j, RMR_ERR_INVALID_ARGUMENT, "invalid page resources");
    return;
  }

  char names[MAXIMUM_PARTS][32];
  for (size_t i = 0; i < count; i++) {
    unsigned suffix = 0;
    do {
      if (!tick(w->j)) return;
      snprintf(names[i], sizeof names[i], "RMR%u_%u", forms[i], suffix++);
    } while (get(objects, names[i]));
  }

  /* The overlay lives in its own Form resource scope, so its gs/image/font
   * names can never replace names used by the original page. */
  uint32_t invoke = reserve(w);
  writer content = {.j = w->j};
  text(&content, "q\n");
  double m[] = {o->transform.a, o->transform.b,  o->transform.c,
                o->transform.d, o->transform.tx, o->transform.ty};
  for (size_t i = 0; i < 6; i++) {
    real(&content, m[i]);
    text(&content, " ");
  }
  text(&content, "cm\n");
  for (size_t i = 0; i < count; i++) format(&content, "/%s Do\n", names[i]);
  text(&content, "Q\n");
  begin(w, invoke, 0);
  format(w, "<< /Length %zu >>\nstream\n", content.data.size);
  put(w, content.data.bytes, content.data.size);
  text(w, "\nendstream");
  end(w);
  free(content.data.bytes);

  begin(w, p->object->number, p->object->generation);
  text(w, "<<\n");
  const char* skip[] = {"Contents", "Resources", "MediaBox", "CropBox",
                        "ArtBox"};
  fields(w, nullptr, p->object->object, skip, o->replace_box ? 5 : 2);

  if (o->replace_box) {
    text(w, "/MediaBox ");
    rectangle(w, o->box);
    text(w, "\n/CropBox ");
    if (p->crop && !equal_boxes(source, p->crop, p->media))
      value(w, nullptr, p->crop, 0);
    else
      rectangle(w, o->box);
    if (p->media) {
      text(w, "\n/ArtBox ");
      value(w, nullptr, p->media, 0);
    }
    text(w, "\n");
  }

  format(w, "/Contents [%u 0 R ", open);
  contents(w, source, get(p->object->object, "Contents"));
  format(w, "%u 0 R %u 0 R]\n/Resources <<\n", close, invoke);
  const char* xskip[] = {"XObject"};
  fields(w, nullptr, resources, xskip, 1);
  text(w, "/XObject <<\n");
  fields(w, nullptr, objects, nullptr, 0);
  for (size_t i = 0; i < count; i++)
    format(w, "/%s %u 0 R\n", names[i], forms[i]);
  text(w, ">>\n>>\n>>");
  end(w);
}

/* A new trailer or xref stream dictionary carries only document identity
 * from the source trailer; stream keys such as /F or /DL never transfer. */
static void trailer_fields(writer* w, node* trailer) {
  static const char* const keep[] = {"Root", "Info", "ID"};
  for (node* k = trailer->child; k; k = k->next->next)
    for (size_t i = 0; i < sizeof keep / sizeof *keep; i++)
      if (named(k, keep[i])) {
        value(w, nullptr, k, 0);
        text(w, " ");
        value(w, w->renumber, k->next, 0);
        text(w, "\n");
      }
}

/* qsort order of written objects by object number. */
static int update_order(const void* a, const void* b) {
  const update *x = a, *y = b;
  return (x->id > y->id) - (x->id < y->id);
}

/* Writes the cross-reference section and trailer for the objects written
 * so far: a complete table for a fresh file, otherwise an update chained
 * to the source's last xref, as a table, stream or both (hybrid). */
static void finish(writer* w, file* f) {
  size_t offset = position(w);
  bool hybrid = f->xref_streams && get(f->trailer, "XRefStm") != nullptr;
  size_t hybrid_offset = offset;
  if (f->xref_streams) begin(w, reserve(w), 0);

  if (w->count > SIZE_MAX / sizeof(update)) {
    stop(w->j, RMR_ERR_LIMIT, "xref allocation limit");
    return;
  }

  update* list = take(w->j, w->count * sizeof *list);
  if (!list) return;
  size_t i = 0;
  for (update* u = w->updates; u; u = u->next) list[i++] = *u;
  qsort(list, w->count, sizeof *list, update_order);

  if (!f->last) {
    /* A fresh file has no preceding xref to supply missing rows. Include
     * free entries for holes left by packed/xref objects or source gaps. */
    format(w, "xref\n0 %zu\n0000000000 65535 f \n", w->next_number);
    i = 0;

    for (size_t id = 1; id < w->next_number && tick(w->j); id++) {
      if (i < w->count && list[i].id == id) {
        if (list[i].offset > 9999999999ULL) {
          stop(w->j, RMR_ERR_LIMIT, "classic xref offset limit");
          return;
        }
        format(w, "%010zu %05u n \n", list[i].offset, list[i].gen);
        i++;
      } else
        text(w, "0000000000 65535 f \n");
    }

    format(w, "trailer\n<< /Size %zu\n", w->next_number);
    trailer_fields(w, f->trailer);
    format(w, ">>\nstartxref\n%zu\n%%%%EOF\n", offset);
    return;
  }

  if (f->xref_streams) {
    if (w->count > SIZE_MAX / 11) {
      stop(w->j, RMR_ERR_LIMIT, "xref stream length");
      return;
    }

    format(w, "<< /Type /XRef /Size %zu ", w->next_number);
    if (!hybrid) format(w, "/Prev %zu ", f->last);
    text(w, "/W [1 8 2] /Index [");
    for (i = 0; i < w->count; i++) format(w, "%u 1 ", list[i].id);
    format(w, "] /Length %zu\n", w->count * 11);
    trailer_fields(w, f->trailer);

    text(w, ">>\nstream\n");
    for (i = 0; i < w->count; i++) {
      uint8_t row[11] = {1};
      uint64_t at = list[i].offset;
      for (unsigned k = 0; k < 8; k++) row[8 - k] = (uint8_t)(at >> (k * 8));
      row[9] = (uint8_t)(list[i].gen >> 8);
      row[10] = (uint8_t)list[i].gen;
      put(w, row, sizeof row);
    }
    text(w, "\nendstream");
    end(w);

    if (!hybrid) {
      format(w, "startxref\n%zu\n%%%%EOF\n", offset);
      return;
    }
    offset = position(w);
  }

  text(w, "xref\n");
  for (i = 0; i < w->count; i++) {
    if (list[i].offset > 9999999999ULL) {
      stop(w->j, RMR_ERR_LIMIT, "classic xref offset limit");
      return;
    }
    format(w, "%u 1\n%010zu %05u n \n", list[i].id, list[i].offset,
           list[i].gen);
  }

  format(w, "trailer\n<< /Size %zu /Prev %zu\n", w->next_number, f->last);
  if (hybrid) format(w, "/XRefStm %zu\n", hybrid_offset);
  trailer_fields(w, f->trailer);
  format(w, ">>\nstartxref\n%zu\n%%%%EOF\n", offset);
}

/* Frees every arena block owned by the job. */
static void release(job* j) {
  while (j->memory) {
    mem* next = j->memory->next;
    free(j->memory);
    j->memory = next;
  }
}

/* A page's inherited MediaBox, normalised to positive size, and /Rotate
 * reduced to 0-270. False if either is malformed or not a multiple of
 * 90. */
static bool geometry(file* f, const page* p, rmr_pdf_page_geometry* out) {
  *out = (rmr_pdf_page_geometry){0};

  if (p->media) {
    node *box = resolve(f, p->media, 0),
         *c = box && box->type == ARRAY ? box->child : nullptr;
    double v[4];
    for (size_t k = 0; k < 4; k++, c = c->next)
      if (!c || !decimal(resolve(f, c, 0), &v[k])) return false;
    if (c) return false;
    out->has_media = true;
    out->media = (rmr_rect){fmin(v[0], v[2]), fmin(v[1], v[3]),
                            fabs(v[2] - v[0]), fabs(v[3] - v[1])};
  }

  if (p->rotate) {
    double r;
    if (!decimal(resolve(f, p->rotate, 0), &r) || fabs(r) > 1e9 ||
        fmod(r, 90) != 0)
      return false;
    long quarter = (long)r % 360;
    out->rotate = (int32_t)(quarter < 0 ? quarter + 360 : quarter);
  }

  return true;
}

rmr_status rmr_pdf_pages(const uint8_t* source, size_t size,
                         const rmr_limits* given, rmr_pdf_page_geometry* pages,
                         size_t capacity, size_t* count, rmr_error* error) {
  if (!source || !size || !count || (capacity && !pages))
    return rmr_fail(error, RMR_ERR_INVALID_ARGUMENT, 0,
                    "missing PDF page input");

  job j = {.limits = given ? *given : rmr_limits_default()};
  file f;

  if (tick(&j) && open_file(&f, &j, source, size)) {
    size_t i = 0;
    for (page* p = f.pages; p && tick(&j); p = p->next, i++) {
      rmr_pdf_page_geometry g;
      if (!geometry(&f, p, &g))
        stop(&j, RMR_ERR_INVALID_ARGUMENT, "invalid PDF page box or rotation");
      else if (i < capacity)
        pages[i] = g;
    }
    if (j.status == RMR_OK) *count = f.page_count;
  }

  release(&j);
  return j.status == RMR_OK
             ? RMR_OK
             : rmr_fail(error, j.status, 0,
                        j.reason ? j.reason : "PDF inspection failed");
}

/* Writes t applied to p as two PDF numbers, each followed by a space. */
static void clip_point(writer* w, rmr_transform t, rmr_point p) {
  real(w, t.a * p.x + t.c * p.y + t.tx);
  text(w, " ");
  real(w, t.b * p.x + t.d * p.y + t.ty);
  text(w, " ");
}

/* Appends an incremental update to a one-page PDF that clips the page's
 * existing contents to path, mapped through transform. On success out
 * receives a new caller-owned buffer; on failure it is unchanged and the
 * status is returned without a message. */
rmr_status rmr_pdf_clip(const uint8_t* pdf, size_t size, rmr_path_view path,
                        rmr_transform transform, const rmr_limits* limits,
                        rmr_buffer* out) {
  double determinant = transform.a * transform.d - transform.b * transform.c;
  if (!pdf || !size || !out || (path.count && !path.elements) ||
      !isfinite(determinant) || determinant == 0 || !isfinite(transform.tx) ||
      !isfinite(transform.ty))
    return RMR_ERR_INVALID_ARGUMENT;

  job j = {.limits = limits ? *limits : rmr_limits_default()};
  writer w = {.j = &j}, c = {.j = &j};
  file f;
  if (path.count > j.limits.maximum_elements)
    stop(&j, RMR_ERR_LIMIT, "clip path limit");

  if (tick(&j) && open_file(&f, &j, pdf, size)) {
    if (f.page_count != 1)
      stop(&j, RMR_ERR_INVALID_ARGUMENT, "clip requires one page");
    w.next_number = f.next_number;
    put(&w, pdf, size);
    text(&w, "\n");
    text(&c, "q\n");

    for (size_t i = 0; i < path.count && tick(&j); i++) {
      const rmr_element* v = &path.elements[i];
      switch (v->kind) {
        case RMR_MOVE:
        case RMR_LINE:
          clip_point(&c, transform, v->end);
          text(&c, v->kind == RMR_MOVE ? "m\n" : "l\n");
          break;
        case RMR_CURVE:
          clip_point(&c, transform, v->control1);
          clip_point(&c, transform, v->control2);
          clip_point(&c, transform, v->end);
          text(&c, "c\n");
          break;
        case RMR_CLOSE:
          text(&c, "h\n");
          break;
        default:
          stop(&j, RMR_ERR_INVALID_ARGUMENT, "invalid clip operation");
      }
    }

    text(&c, "W n\n");
    uint32_t start = reserve(&w);
    begin(&w, start, 0);
    format(&w, "<< /Length %zu >>\nstream\n", c.data.size);
    put(&w, c.data.bytes, c.data.size);
    text(&w, "\nendstream");
    end(&w);
    uint32_t close = stream_text(&w, "Q\n");

    page* p = f.pages;
    if (p && j.status == RMR_OK) {
      begin(&w, p->object->number, p->object->generation);
      text(&w, "<<\n");
      const char* skip[] = {"Contents"};
      fields(&w, nullptr, p->object->object, skip, 1);
      format(&w, "/Contents [%u 0 R ", start);
      contents(&w, &f, get(p->object->object, "Contents"));
      format(&w, "%u 0 R] >>", close);
      end(&w);
      finish(&w, &f);
    }
  }

  free(c.data.bytes);
  if (j.status == RMR_OK)
    *out = w.data;
  else
    free(w.data.bytes);
  release(&j);
  return j.status;
}

/* Object numbers of the output's pages by index: the entries of numbers and
 * generations, or first + index when numbers is null. */
typedef struct {
  const uint32_t *numbers, *generations;
  uint32_t first;
  size_t count;
} page_refs;

/* Writes a /Dest [page /XYZ x y null] entry; fails the job if the page
 * index is out of range. */
static void destination(writer* w, page_refs pages, rmr_pdf_destination d) {
  if (d.page >= pages.count) {
    stop(w->j, RMR_ERR_INVALID_ARGUMENT, "navigation target outside document");
    return;
  }

  format(w, "/Dest [%u %u R /XYZ ",
         pages.numbers ? pages.numbers[d.page] : pages.first + (uint32_t)d.page,
         pages.generations ? pages.generations[d.page] : 0);
  real(w, d.point.x);
  text(w, " ");
  real(w, d.point.y);
  text(w, " null]\n");
}

/* Writes link annotation object id: rect, to destination d. */
static void link_object(writer* w, uint32_t id, rmr_rect rect, page_refs pages,
                        rmr_pdf_destination d) {
  begin(w, id, 0);
  text(w, "<< /Type /Annot /Subtype /Link /Border [0 0 0] /Rect ");
  rectangle(w, rect);
  text(w, "\n");
  destination(w, pages, d);
  text(w, ">>");
  end(w);
}

typedef struct {
  uint32_t id;
  size_t first, last, prev, next, visible;
} outline_entry;

/* Writes the outline's items and, when it has any, its /Outlines root;
 * returns the root's object number, 0 without items or after failing the
 * job. Outline parents must precede their children. */
static uint32_t outline_objects(writer* w, const rmr_pdf_outline* outline,
                                size_t outline_count, page_refs pages) {
  job* j = w->j;
  outline_entry* tree = take(j, (outline_count + 1) * sizeof *tree);
  if (!tree) return 0;

  for (size_t i = 0; i <= outline_count && tick(j); i++)
    tree[i] =
        (outline_entry){reserve(w), SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, 0};

  for (size_t i = 0; i < outline_count && tick(j); i++) {
    size_t parent = outline[i].parent;
    if (parent != SIZE_MAX && parent >= i) {
      stop(j, RMR_ERR_INVALID_ARGUMENT, "invalid outline parent");
      break;
    }
    if (parent == SIZE_MAX) parent = outline_count;
    size_t prev = tree[parent].last;
    tree[i].prev = prev;
    if (prev != SIZE_MAX)
      tree[prev].next = i;
    else
      tree[parent].first = i;
    tree[parent].last = i;
  }

  for (size_t i = outline_count; i > 0 && tick(j);) {
    --i;
    size_t parent =
        outline[i].parent == SIZE_MAX ? outline_count : outline[i].parent;
    tree[parent].visible += 1 + (outline[i].open ? tree[i].visible : 0);
  }

  for (size_t i = 0; i < outline_count && tick(j); i++) {
    const rmr_pdf_outline* o = &outline[i];
    if ((o->title_size && !o->title) || o->title_size % 2) {
      stop(j, RMR_ERR_INVALID_ARGUMENT, "invalid outline title");
      break;
    }
    begin(w, tree[i].id, 0);
    text(w, "<< /Title <FEFF");
    for (size_t k = 0; k < o->title_size && tick(j); k += 2)
      format(w, "%02X%02X", o->title[k + 1], o->title[k]);
    size_t parent = o->parent == SIZE_MAX ? outline_count : o->parent;
    format(w, "> /Parent %u 0 R\n", tree[parent].id);
    if (tree[i].prev != SIZE_MAX)
      format(w, "/Prev %u 0 R\n", tree[tree[i].prev].id);
    if (tree[i].next != SIZE_MAX)
      format(w, "/Next %u 0 R\n", tree[tree[i].next].id);
    if (tree[i].first != SIZE_MAX)
      format(w, "/First %u 0 R /Last %u 0 R /Count %s%zu\n",
             tree[tree[i].first].id, tree[tree[i].last].id, o->open ? "" : "-",
             tree[i].visible);
    if (o->destination.page != SIZE_MAX) destination(w, pages, o->destination);
    text(w, ">>");
    end(w);
  }

  if (!outline_count || j->status != RMR_OK) return 0;
  outline_entry* root = &tree[outline_count];
  begin(w, root->id, 0);
  format(w, "<< /Type /Outlines /First %u 0 R /Last %u 0 R /Count %zu >>",
         tree[root->first].id, tree[root->last].id, root->visible);
  end(w);
  return root->id;
}

/* Appends an incremental update that adds local link annotations to their
 * pages and, when outline_count is nonzero, installs a new document
 * outline in place of /Outlines. Outline parents must precede their
 * children. On success out receives a new caller-owned buffer; on
 * failure it is unchanged. */
rmr_status rmr_pdf_navigation(const uint8_t* pdf, size_t size,
                              const rmr_pdf_local_link* links,
                              size_t link_count, const rmr_pdf_outline* outline,
                              size_t outline_count, const rmr_limits* limits,
                              rmr_cancel_fn cancel, void* context,
                              rmr_buffer* out) {
  if (!pdf || !size || !out || (link_count && !links) ||
      (outline_count && !outline))
    return RMR_ERR_INVALID_ARGUMENT;

  job j = {.limits = limits ? *limits : rmr_limits_default(),
           .cancel = cancel,
           .context = context};
  writer w = {.j = &j};
  file f;
  if (link_count > SIZE_MAX / sizeof(uint32_t) ||
      outline_count >= SIZE_MAX / sizeof(outline_entry) ||
      link_count > j.limits.maximum_elements ||
      outline_count >= j.limits.maximum_elements)
    stop(&j, RMR_ERR_LIMIT, "navigation limit");

  if (tick(&j) && open_file(&f, &j, pdf, size)) {
    w.next_number = f.next_number;
    put(&w, pdf, size);
    text(&w, "\n");
    if (f.page_count > SIZE_MAX / sizeof(page*))
      stop(&j, RMR_ERR_LIMIT, "navigation page limit");

    page** pages = take(&j, f.page_count * sizeof *pages);
    uint32_t* numbers = take(&j, f.page_count * sizeof *numbers);
    uint32_t* generations = take(&j, f.page_count * sizeof *generations);
    uint32_t* ids = take(&j, link_count * sizeof *ids);

    if (j.status == RMR_OK) {
      size_t at = 0;
      for (page* p = f.pages; p; p = p->next, at++) {
        pages[at] = p;
        numbers[at] = p->object->number;
        generations[at] = p->object->generation;
      }
      page_refs refs = {numbers, generations, 0, f.page_count};

      for (size_t i = 0; i < link_count && tick(&j); i++) {
        if (links[i].page >= f.page_count) {
          stop(&j, RMR_ERR_INVALID_ARGUMENT, "link outside document");
          break;
        }
        ids[i] = reserve(&w);
        link_object(&w, ids[i], links[i].rect, refs, links[i].destination);
      }

      for (size_t pi = 0; pi < f.page_count && tick(&j); pi++) {
        bool found = false;
        for (size_t i = 0; i < link_count && tick(&j); i++)
          found |= links[i].page == pi;
        if (!found) continue;
        page* p = pages[pi];
        begin(&w, p->object->number, p->object->generation);
        text(&w, "<<\n");
        const char* skip[] = {"Annots"};
        fields(&w, nullptr, p->object->object, skip, 1);
        text(&w, "/Annots [");
        node* existing = resolve(&f, get(p->object->object, "Annots"), 0);
        if (existing && existing->type != ARRAY)
          stop(&j, RMR_ERR_INVALID_ARGUMENT, "invalid annotations");
        for (node* n = existing ? existing->child : nullptr; n && tick(&j);
             n = n->next) {
          value(&w, nullptr, n, 0);
          text(&w, " ");
        }
        for (size_t i = 0; i < link_count && tick(&j); i++)
          if (links[i].page == pi) format(&w, "%u 0 R ", ids[i]);
        text(&w, "] >>");
        end(&w);
      }

      uint32_t root = outline_objects(&w, outline, outline_count, refs);
      if (root) {
        node* ref = get(f.trailer, "Root");
        begin(&w, ref->number, ref->generation);
        text(&w, "<<\n");
        const char* skip[] = {"Outlines"};
        fields(&w, nullptr, f.root, skip, 1);
        format(&w, "/Outlines %u 0 R >>", root);
        end(&w);
      }

      if (j.status == RMR_OK) finish(&w, &f);
    }
  }

  if (j.status == RMR_OK)
    *out = w.data;
  else
    free(w.data.bytes);
  release(&j);
  return j.status;
}

/* Rewriting keeps object identities and generations, but discards historical
 * revisions and expands packed objects. It is deliberately separate from the
 * incremental writer: rewriting can never preserve signed byte ranges. */
/* A fresh file keeps the source's declared version, a %PDF-<major>.<minor>
 * line, and falls back to 1.7 when the header line is not in that form. */
static void header(writer* w, const uint8_t* source, size_t size) {
  size_t end = 5, major = 0, minor = 0;
  while (end < size && end < 16 && source[end] >= '0' && source[end] <= '9')
    end++, major++;
  if (end < size && source[end] == '.')
    for (end++;
         end < size && end < 16 && source[end] >= '0' && source[end] <= '9';
         end++)
      minor++;

  bool valid = major && minor && end < size &&
               (source[end] == '\r' || source[end] == '\n');
  if (valid)
    put(w, source, end);
  else
    text(w, "%PDF-1.7");
  text(w, "\n%\342\343\317\323\n");
}

rmr_status rmr_pdf_rewrite(const uint8_t* source, size_t size,
                           const rmr_limits* limits, rmr_cancel_fn cancel,
                           void* context, rmr_buffer* out, rmr_error* error) {
  if (!out)
    return rmr_fail(error, RMR_ERR_INVALID_ARGUMENT, 0,
                    "missing PDF rewrite input");
  return rmr_pdf_rewrite_output(source, size, limits, cancel, context, nullptr,
                                out, error);
}

rmr_status rmr_pdf_rewrite_output(const uint8_t* source, size_t size,
                                  const rmr_limits* limits,
                                  rmr_cancel_fn cancel, void* context,
                                  const rmr_output* output, rmr_buffer* out,
                                  rmr_error* error) {
  if (!source || !size || (!out && !output))
    return rmr_fail(error, RMR_ERR_INVALID_ARGUMENT, 0,
                    "missing PDF rewrite input");

  job j = {.limits = limits ? *limits : rmr_limits_default(),
           .cancel = cancel,
           .context = context};
  writer w = {.j = &j, .output = output};
  file f;

  if (tick(&j) && open_file(&f, &j, source, size)) {
    w.next_number = f.next_number;
    header(&w, source, size);

    for (size_t i = 0; i < f.count && tick(&j); i++) {
      entry* e = &f.entries[i];
      if (!e->live || absent(e)) continue;
      node* n = object(&f, e, 0);
      if (!n) break;
      node* type = n->type == DICT ? get(n, "Type") : nullptr;
      if (e->stream && (named(type, "ObjStm") || named(type, "XRef"))) continue;
      begin(&w, e->number, e->generation);
      value(&w, nullptr, n, 0);
      if (e->stream) {
        text(&w, "\nstream\n");
        put(&w, e->stream, e->stream_size);
        text(&w, "\nendstream");
      }
      end(&w);
    }

    if (j.status == RMR_OK) {
      f.last = 0;
      f.xref_streams = false;
      finish(&w, &f);
    }
  }

  if (output) {
    drain(&w);
    free(w.data.bytes);
  } else if (j.status == RMR_OK)
    *out = w.data;
  else
    free(w.data.bytes);
  release(&j);
  return j.status == RMR_OK
             ? RMR_OK
             : rmr_fail(error, j.status, 0,
                        j.reason ? j.reason : "PDF rewrite failed");
}

/* Page selection preserves native coordinates. Every output page owns its
 * annotation objects, while copied widgets share their original field value. */
typedef struct {
  node *original, *dict;
  entry* entry;
  uint32_t id;
} selected_annot;

typedef struct {
  page* original;
  node* dict;
  uint32_t id;
  selected_annot* annots;
  size_t count;
} selected_page;

/* New arena node of the given kind whose raw text copies text_value. */
static node* select_token(job* j, kind type, const char* text_value) {
  node* n = take(j, sizeof *n);
  size_t size = strlen(text_value);
  uint8_t* raw = take(j, size);
  if (!n || !raw) return nullptr;

  memcpy(raw, text_value, size);
  *n = (node){.type = type, .raw = raw, .length = size};
  return n;
}

/* New reference node for id and generation. */
static node* select_ref(job* j, uint32_t id, uint32_t generation) {
  char raw[48];
  snprintf(raw, sizeof raw, "%u %u R", id, generation);
  node* n = select_token(j, REF, raw);
  if (n) {
    n->number = id;
    n->generation = generation;
  }
  return n;
}

/* Deep copy of a node into the arena, with its sibling link cleared. */
static node* select_clone(job* j, const node* n, unsigned depth) {
  if (!n || !tick(j)) return nullptr;
  if (depth >= 64) {
    stop(j, RMR_ERR_LIMIT, "page copy nesting");
    return nullptr;
  }
  node* copy = take(j, sizeof *copy);
  if (!copy) return nullptr;

  *copy = *n;
  copy->child = copy->next = nullptr;
  node** tail = &copy->child;

  for (const node* c = n->child; c && j->status == RMR_OK; c = c->next) {
    *tail = select_clone(j, c, depth + 1);
    if (*tail) tail = &(*tail)->next;
  }

  return copy;
}

/* Replaces the value of key in dict, or prepends the key if absent. */
static void select_set(job* j, node* dict, const char* key, node* v) {
  if (!dict || !v || j->status != RMR_OK) return;

  for (node* k = dict->child; k; k = k->next->next)
    if (named(k, key)) {
      v->next = k->next->next;
      k->next = v;
      return;
    }

  char raw[64];
  snprintf(raw, sizeof raw, "/%s", key);
  node* k = select_token(j, NAME, raw);
  if (k) {
    v->next = dict->child;
    k->next = v;
    dict->child = k;
  }
}

/* Unlinks the entry for key from dict, if present. */
static void select_remove(node* dict, const char* key) {
  if (!dict) return;
  for (node** at = &dict->child; *at; at = &(*at)->next->next)
    if (named(*at, key)) {
      *at = (*at)->next->next;
      return;
    }
}

typedef struct {
  node value;
  node* tail;
} selection_array;

/* New empty array that select_append extends in constant time. */
static node* select_array(job* j) {
  selection_array* a = take(j, sizeof *a);
  if (a) a->value.type = ARRAY;
  return a ? &a->value : nullptr;
}

/* Only arrays created by select_array are appended to. */
static void select_append(node* array, node* item) {
  if (!array || !item) return;
  selection_array* a = (selection_array*)array;
  if (a->tail)
    a->tail->next = item;
  else
    array->child = item;
  a->tail = item;
}

/* True if a widget dictionary also carries field keys (merged field). */
static bool select_merged(node* dict) {
  return get(dict, "T") || get(dict, "FT") || get(dict, "V") || get(dict, "DV");
}

/* Page and annotation references are remapped without traversing indirect
 * objects. Self-links and popup/reply relations use the current page copy. */
static void select_refs(file* f, node* n, const selected_page* local,
                        unsigned depth) {
  if (!n || !tick(f->j)) return;
  if (depth >= 64) {
    stop(f->j, RMR_ERR_LIMIT, "page reference nesting");
    return;
  }

  if (n->type == REF) {
    entry* e = lookup(f, n->number);
    if (!e || !e->live || e->generation != n->generation) return;
    uint32_t id = e->mapped;
    if (local) {
      if (e == local->original->object) id = local->id;
      for (size_t i = 0; i < local->count && tick(f->j); i++)
        if (e == local->annots[i].entry) id = local->annots[i].id;
    }
    bool is_page = e->object && named(get(e->object, "Type"), "Page");

    if (id || is_page) {
      node* replacement =
          id ? select_ref(f->j, id, 0) : select_token(f->j, SCALAR, "null");
      if (replacement) {
        replacement->next = n->next;
        *n = *replacement;
      }
    }
  } else if (n->type == DICT) {
    bool widget = named(get(n, "Subtype"), "Widget");
    for (node* k = n->child; k && f->j->status == RMR_OK; k = k->next->next)
      if (!(widget && named(k, "Parent")))
        select_refs(f, k->next, local, depth + 1);
  } else
    for (node* c = n->child; c && f->j->status == RMR_OK; c = c->next)
      select_refs(f, c, local, depth + 1);
}

/* Rebuilds a form field array for the selected pages. Widgets become their
 * page copies and merged field/widgets keep the field with per-copy kids.
 * Fields left without widgets are dropped unless no source page held
 * them. Retained fields are marked 2 in seen. */
static node* select_fields(file* f, node* array, node** widgets, uint8_t* seen,
                           const uint8_t* attached, unsigned depth) {
  array = resolve(f, array, 0);
  if (!array || array->type != ARRAY || depth >= 64) {
    stop(f->j, RMR_ERR_INVALID_ARGUMENT, "invalid form field tree");
    return nullptr;
  }
  node* result = select_array(f->j);

  for (node* ref = array->child; ref && tick(f->j); ref = ref->next) {
    entry* e = ref->type == REF ? lookup(f, ref->number) : nullptr;
    node* dict = resolve(f, ref, 0);
    if (!e || !dict || dict->type != DICT || seen[e - f->entries]) {
      stop(f->j, RMR_ERR_INVALID_ARGUMENT, "invalid or repeated form field");
      break;
    }
    size_t index = (size_t)(e - f->entries);
    seen[index] = 1;

    if (named(get(dict, "Subtype"), "Widget")) {
      node* copies = widgets[index];
      if (!copies || !copies->child) {
        /* Fields with no widget in any source page can carry hidden
         * values used by calculations; page removal must not drop them. */
        if (!attached[index]) {
          seen[index] = 2;
          select_append(result, select_clone(f->j, ref, 0));
        }
        continue;
      }
      if (!select_merged(dict)) {
        for (node* c = copies->child; c && tick(f->j); c = c->next)
          select_append(result, select_clone(f->j, c, 0));
        seen[index] = 2;
        continue;
      }
      /* Split a merged field/widget: retain its field identity and
       * values, and give each selected page its own child widget. */
      const char* keys[] = {
          "Type",   "Subtype", "Rect", "P", "AP",           "AS", "F",    "BS",
          "Border", "MK",      "H",    "A", "StructParent", "OC", "Popup"};
      for (size_t k = 0; k < sizeof keys / sizeof *keys; k++)
        select_remove(dict, keys[k]);
      select_set(f->j, dict, "Kids", select_clone(f->j, copies, 0));
    } else if (get(dict, "Kids")) {
      node* kids = select_fields(f, get(dict, "Kids"), widgets, seen, attached,
                                 depth + 1);
      if (!kids || !kids->child) continue;
      select_set(f->j, dict, "Kids", kids);
    }

    seen[index] = 2;
    select_append(result, select_clone(f->j, ref, 0));
  }

  return result;
}

/* Page labels are keyed by page index. Each output page keeps the label its
 * source page showed: it starts a range copied from the source range, with
 * /St advanced to the source page's position in that range. */
typedef struct label_range {
  uint64_t key;
  node* dict;
  struct label_range* next;
} label_range;

/* Collects every (first page index, label dictionary) pair of a page
 * label number tree into a list. */
static void select_label_ranges(file* f, node* tree, label_range** out,
                                unsigned depth) {
  tree = resolve(f, tree, 0);
  if (!tree || !tick(f->j)) return;
  if (depth >= 64 || tree->type != DICT) {
    stop(f->j, depth >= 64 ? RMR_ERR_LIMIT : RMR_ERR_INVALID_ARGUMENT,
         "invalid page labels");
    return;
  }
  node *nums = resolve(f, get(tree, "Nums"), 0),
       *kids = resolve(f, get(tree, "Kids"), 0);
  if ((nums && nums->type != ARRAY) || (kids && kids->type != ARRAY)) {
    stop(f->j, RMR_ERR_INVALID_ARGUMENT, "invalid page labels");
    return;
  }

  for (node* k = nums ? nums->child : nullptr; k && tick(f->j);
       k = k->next->next) {
    uint64_t key;
    node* d = k->next ? resolve(f, k->next, 0) : nullptr;
    label_range* r = d && d->type == DICT && uint_value(k, &key)
                         ? take(f->j, sizeof *r)
                         : nullptr;
    if (!r) {
      if (f->j->status == RMR_OK)
        stop(f->j, RMR_ERR_INVALID_ARGUMENT, "invalid page labels");
      return;
    }
    *r = (label_range){key, d, *out};
    *out = r;
  }
  for (node* k = kids ? kids->child : nullptr; k && f->j->status == RMR_OK;
       k = k->next)

    select_label_ranges(f, k, out, depth + 1);
}

/* New /PageLabels number tree giving each selected page the label it had
 * in the source; null on failure. */
static node* select_labels(file* f, const size_t* indices, size_t count) {
  job* j = f->j;
  label_range* ranges = nullptr;
  select_label_ranges(f, get(f->root, "PageLabels"), &ranges, 0);
  node *nums = select_array(j), *labels = take(j, sizeof *labels);
  const label_range* previous = nullptr;

  for (size_t i = 0; i < count && j->status == RMR_OK && tick(j); i++) {
    const label_range* best = nullptr;
    for (const label_range* r = ranges; r && tick(j); r = r->next)
      if (r->key <= indices[i] && (!best || r->key > best->key)) best = r;
    if (i && best == previous && indices[i] == indices[i - 1] + 1) continue;
    previous = best;
    char raw[32];
    snprintf(raw, sizeof raw, "%zu", i);
    select_append(nums, select_token(j, SCALAR, raw));
    if (!best) {
      select_append(nums, select_token(j, DICT, ""));
      continue;
    }

    uint64_t start = 1;
    node* st = resolve(f, get(best->dict, "St"), 0);
    if (st && (!uint_value(st, &start) || !start || start > INT32_MAX)) {
      stop(j, RMR_ERR_INVALID_ARGUMENT, "invalid page label start");
      break;
    }
    if (indices[i] - best->key > INT32_MAX - start) {
      stop(j, RMR_ERR_LIMIT, "page label start limit");
      break;
    }
    start += indices[i] - best->key;

    node* d = select_clone(j, best->dict, 0);
    snprintf(raw, sizeof raw, "%llu", (unsigned long long)start);
    select_set(j, d, "St", select_token(j, SCALAR, raw));
    select_append(nums, d);
  }

  if (!labels || j->status != RMR_OK) return nullptr;
  *labels = (node){.type = DICT};
  select_set(j, labels, "Nums", nums);
  return labels;
}

rmr_status rmr_pdf_select_pages(const uint8_t* source, size_t size,
                                const size_t* indices, size_t count,
                                const rmr_limits* limits, rmr_cancel_fn cancel,
                                void* context, rmr_buffer* out,
                                rmr_error* error) {
  if (!source || !size || !indices || !count || !out)
    return rmr_fail(error, RMR_ERR_INVALID_ARGUMENT, 0,
                    "missing PDF selection input");

  job j = {.limits = limits ? *limits : rmr_limits_default(),
           .cancel = cancel,
           .context = context};
  writer w = {.j = &j};
  file f;
  if (count > j.limits.maximum_draws ||
      count > SIZE_MAX / sizeof(selected_page))
    stop(&j, RMR_ERR_LIMIT, "PDF selection page limit");

  if (tick(&j) && open_file(&f, &j, source, size)) {
    w.next_number = f.next_number;
    if (f.page_count > SIZE_MAX / sizeof(page*) ||
        f.count > SIZE_MAX / sizeof(node*))
      stop(&j, RMR_ERR_LIMIT, "PDF selection object limit");

    selected_page* copies = take(&j, count * sizeof *copies);
    page** pages = take(&j, f.page_count * sizeof *pages);
    node** widgets = take(&j, f.count * sizeof *widgets);
    uint8_t* seen = take(&j, f.count);
    uint8_t* attached = take(&j, f.count);
    uint32_t tree_id = reserve(&w);
    node* kids = select_array(&j);

    /* Load objects before installing new references: every imported
     * object remains bounded by the same parser and work limits. */
    for (size_t i = 0; i < f.count && tick(&j); i++)
      if (f.entries[i].live && !absent(&f.entries[i]))
        object(&f, &f.entries[i], 0);

    if (j.status == RMR_OK) {
      size_t pi = 0;
      for (page* p = f.pages; p && tick(&j); p = p->next) {
        pages[pi++] = p;
        node* annots = resolve(&f, get(p->object->object, "Annots"), 0);
        if (annots && annots->type != ARRAY) {
          stop(&j, RMR_ERR_INVALID_ARGUMENT, "invalid annotations");
          break;
        }
        for (node* n = annots ? annots->child : nullptr; n && tick(&j);
             n = n->next) {
          entry* e = n->type == REF ? lookup(&f, n->number) : nullptr;
          if (e && e->generation == n->generation) attached[e - f.entries] = 1;
        }
      }

      for (size_t i = 0; i < count && tick(&j); i++) {
        if (indices[i] >= f.page_count) {
          stop(&j, RMR_ERR_INVALID_ARGUMENT, "selected page out of range");
          break;
        }
        selected_page* p = &copies[i];
        p->original = pages[indices[i]];
        p->id = reserve(&w);
        if (!p->original->object->mapped) p->original->object->mapped = p->id;
        select_append(kids, select_ref(&j, p->id, 0));
        node* annots =
            resolve(&f, get(p->original->object->object, "Annots"), 0);
        if (annots && annots->type != ARRAY) {
          stop(&j, RMR_ERR_INVALID_ARGUMENT, "invalid annotations");
          break;
        }

        for (node* n = annots ? annots->child : nullptr; n && tick(&j);
             n = n->next)
          p->count++;
        if (p->count > SIZE_MAX / sizeof *p->annots) {
          stop(&j, RMR_ERR_LIMIT, "annotation count limit");
          break;
        }

        p->annots = take(&j, p->count * sizeof *p->annots);
        size_t k = 0;
        for (node* n = annots ? annots->child : nullptr; n && tick(&j);
             n = n->next, k++) {
          selected_annot* a = &p->annots[k];
          a->original = resolve(&f, n, 0);
          a->entry = n->type == REF ? lookup(&f, n->number) : nullptr;
          if (!a->original || a->original->type != DICT) {
            stop(&j, RMR_ERR_INVALID_ARGUMENT, "invalid annotation dictionary");
            break;
          }
          a->id = reserve(&w);
          a->dict = select_clone(&j, a->original, 0);
          if (named(get(a->original, "Subtype"), "Widget")) {
            if (!a->entry) {
              stop(&j, RMR_ERR_INVALID_ARGUMENT, "widget must be indirect");
              break;
            }
            size_t index = (size_t)(a->entry - f.entries);
            if (!widgets[index]) widgets[index] = select_array(&j);
            select_append(widgets[index], select_ref(&j, a->id, 0));
          } else if (a->entry && !a->entry->mapped)
            a->entry->mapped = a->id;
        }
      }
    }

    node* form =
        j.status == RMR_OK ? resolve(&f, get(f.root, "AcroForm"), 0) : nullptr;
    if (form && (form->type != DICT || get(form, "XFA")))
      stop(&j, RMR_ERR_UNSUPPORTED,
           "page selection requires an AcroForm without XFA");

    /* Clone pages before modifying merged field dictionaries. */
    for (size_t i = 0; i < count && tick(&j); i++) {
      selected_page* p = &copies[i];
      p->dict = select_clone(&j, p->original->object->object, 0);
      select_set(&j, p->dict, "Parent", select_ref(&j, tree_id, 0));
      const char* names[] = {"Resources", "MediaBox", "CropBox", "Rotate"};
      node* inherited[] = {p->original->resources, p->original->media,
                           p->original->crop, p->original->rotate};
      for (size_t k = 0; k < 4; k++)
        if (inherited[k])
          select_set(&j, p->dict, names[k], select_clone(&j, inherited[k], 0));

      node* annots = select_array(&j);
      for (size_t k = 0; k < p->count && tick(&j); k++) {
        selected_annot* a = &p->annots[k];
        select_refs(&f, a->dict, p, 0);
        select_set(&j, a->dict, "P", select_ref(&j, p->id, 0));
        if (named(get(a->original, "Subtype"), "Widget") &&
            select_merged(a->original)) {
          const char* keys[] = {"FT", "T", "TU", "TM", "Ff", "V", "DV", "Kids"};
          for (size_t q = 0; q < sizeof keys / sizeof *keys; q++)
            select_remove(a->dict, keys[q]);
          select_set(&j, a->dict, "Parent",
                     select_ref(&j, a->entry->number, a->entry->generation));
        }
        select_append(annots, select_ref(&j, a->id, 0));
      }

      select_set(&j, p->dict, "Annots", annots);
      select_refs(&f, p->dict, p, 0);
    }

    if (form && j.status == RMR_OK) {
      node* fields =
          select_fields(&f, get(form, "Fields"), widgets, seen, attached, 0);
      select_set(&j, form, "Fields", fields);
      node* order = resolve(&f, get(form, "CO"), 0);

      if (order) {
        node* retained = select_array(&j);
        if (order->type != ARRAY)
          stop(&j, RMR_ERR_INVALID_ARGUMENT, "invalid calculation order");
        for (node* n = order->child; n && tick(&j); n = n->next) {
          entry* e = n->type == REF ? lookup(&f, n->number) : nullptr;
          if (e && seen[e - f.entries] == 2)
            select_append(retained, select_clone(&j, n, 0));
        }
        select_set(&j, form, "CO", retained);
      }
    }

    for (size_t i = 0; i < f.count && tick(&j); i++)
      if (widgets[i] && !seen[i])
        stop(&j, RMR_ERR_INVALID_ARGUMENT,
             "widget missing from form field tree");

    if (get(f.root, "PageLabels"))
      select_set(&j, f.root, "PageLabels", select_labels(&f, indices, count));
    select_set(&j, f.root, "Pages", select_ref(&j, tree_id, 0));

    if (j.status == RMR_OK) {
      header(&w, source, size);
      begin(&w, tree_id, 0);
      format(&w, "<< /Type /Pages /Count %zu /Kids ", count);
      value(&w, nullptr, kids, 0);
      text(&w, " >>");
      end(&w);
    }

    for (size_t i = 0; i < count && tick(&j); i++) {
      selected_page* p = &copies[i];
      begin(&w, p->id, 0);
      value(&w, nullptr, p->dict, 0);
      end(&w);
      for (size_t k = 0; k < p->count && tick(&j); k++) {
        begin(&w, p->annots[k].id, 0);
        value(&w, nullptr, p->annots[k].dict, 0);
        end(&w);
      }
    }

    for (size_t i = 0; i < f.count && tick(&j); i++) {
      entry* e = &f.entries[i];
      if (!e->live || absent(e)) continue;
      node* type = get(e->object, "Type");
      if (e->stream && (named(type, "ObjStm") || named(type, "XRef"))) continue;
      select_refs(&f, e->object, nullptr, 0);
      begin(&w, e->number, e->generation);
      value(&w, nullptr, e->object, 0);
      if (e->stream) {
        text(&w, "\nstream\n");
        put(&w, e->stream, e->stream_size);
        text(&w, "\nendstream");
      }
      end(&w);
    }

    if (j.status == RMR_OK) {
      f.last = 0;
      f.xref_streams = false;
      finish(&w, &f);
    }
  }

  if (j.status == RMR_OK)
    *out = w.data;
  else
    free(w.data.bytes);
  release(&j);
  return j.status == RMR_OK
             ? RMR_OK
             : rmr_fail(error, j.status, 0,
                        j.reason ? j.reason : "PDF page selection failed");
}

/* A fresh file written as its parts arrive (rmr_pdf_edit.h). Its writer
 * keeps output in chunks and records its xref rows in the builder's job;
 * each imported PDF is parsed under a job of its own, released once copied,
 * so neither memory nor work limits grow with the number of pages. */
struct rmr_pdf_builder {
  job j;
  writer w;
  uint32_t tree, first_page, open, close;
  size_t page_count;
  char version[3]; /* The header's, "1.7" until a base's replaces it. */
};

rmr_status rmr_pdf_builder_create(const rmr_limits* limits,
                                  rmr_cancel_fn cancel, void* context,
                                  rmr_pdf_builder** out) {
  if (!out) return RMR_ERR_INVALID_ARGUMENT;
  rmr_pdf_builder* b = rmr_calloc(1, sizeof *b);
  if (!b) return RMR_ERR_OUT_OF_MEMORY;
  b->j = (job){.limits = limits ? *limits : rmr_limits_default(),
               .cancel = cancel,
               .context = context};
  b->w = (writer){.j = &b->j, .next_number = 1, .chunked = true};
  /* rmr_pdf_builder_merge gives the file its base's version. */
  memcpy(b->version, "1.7", 3);
  text(&b->w, "%PDF-1.7\n%\342\343\317\323\n");
  if (b->j.status != RMR_OK) {
    rmr_status s = b->j.status;
    rmr_pdf_builder_free(b);
    return s;
  }
  *out = b;
  return RMR_OK;
}

void rmr_pdf_builder_free(rmr_pdf_builder* b) {
  if (!b) return;
  free(b->w.data.bytes);
  for (chunk* c = b->w.chunks; c;) {
    chunk* next = c->next;
    chunk_free(c);
    c = next;
  }
  release(&b->j);
  free(b);
}

void rmr_pdf_builder_output(rmr_pdf_builder* b, const rmr_output* output) {
  if (b->w.chunks)
    stop(&b->j, RMR_ERR_INVALID_ARGUMENT, "PDF output already collected");
  else
    b->w.output = output;
}

uint32_t rmr_pdf_builder_reserve(rmr_pdf_builder* b) {
  return b->j.status == RMR_OK ? reserve(&b->w) : 0;
}

/* The builder's failure as an rmr_status, filling error. */
static rmr_status built(rmr_pdf_builder* b, rmr_error* error) {
  job* j = &b->j;
  return j->status == RMR_OK
             ? RMR_OK
             : rmr_fail(error, j->status, 0,
                        j->reason ? j->reason : "PDF assembly failed");
}

rmr_status rmr_pdf_builder_append(rmr_pdf_builder* b, const uint8_t* bytes,
                                  size_t size, const uint32_t* numbers,
                                  const size_t* offsets, size_t count,
                                  rmr_error* error) {
  job* j = &b->j;
  writer* w = &b->w;
  if ((size && !bytes) || (count && (!numbers || !offsets)))
    stop(j, RMR_ERR_INVALID_ARGUMENT, "missing PDF objects");
  update* list =
      count && j->status == RMR_OK ? take(j, count * sizeof *list) : nullptr;

  size_t at = position(w);
  for (size_t i = 0; i < count && j->status == RMR_OK; i++) {
    if (!numbers[i] || numbers[i] >= w->next_number || offsets[i] >= size ||
        (i && offsets[i] <= offsets[i - 1])) {
      stop(j, RMR_ERR_INVALID_ARGUMENT, "invalid appended PDF object");
      break;
    }
    list[i] = (update){numbers[i], 0, at + offsets[i], w->updates};
    w->updates = &list[i];
    w->count++;
  }
  put(w, bytes, size);
  return built(b, error);
}

/* Moves the output, chunks first, into one exactly sized buffer, freeing
 * each chunk as it is copied; with an output, hands it the rest instead. */
static void assemble(writer* w, rmr_buffer* out) {
  if (w->output) {
    drain(w);
    return;
  }
  size_t size = position(w);
  uint8_t* bytes = rmr_alloc(size);
  if (!bytes) {
    stop(w->j, RMR_ERR_OUT_OF_MEMORY, "PDF output allocation");
    return;
  }
  size_t at = 0;
  for (chunk* c = w->chunks; c;) {
    chunk* next = c->next;
    memcpy(bytes + at, c->bytes, c->size);
    at += c->size;
    chunk_free(c);
    c = next;
  }
  w->chunks = w->last_chunk = nullptr;
  memcpy(bytes + at, w->data.bytes, w->data.size);
  free(w->data.bytes);
  w->data = (rmr_buffer){0};
  w->capacity = w->flushed = 0;
  *out = (rmr_buffer){bytes, size};
}

/* Writes resources (a resolved dictionary or null) with the objects of each
 * kind in lists added under RMRx<n>, RMRg<n> and RMRc<n> names. */
static void resources_with(writer* w, file* f, node* resources,
                           const uint32_t* const lists[3],
                           const size_t counts[3]) {
  static const char* const kinds[] = {"XObject", "ExtGState", "ColorSpace"};
  static const char prefixes[] = {'x', 'g', 'c'};
  bool written[3] = {};
  if (resources && resources->type != DICT) {
    stop(w->j, RMR_ERR_INVALID_ARGUMENT, "invalid page resources");
    return;
  }

  text(w, "<<\n");
  for (node* k = resources ? resources->child : nullptr; k && tick(w->j);
       k = k->next->next) {
    size_t slot = 0;
    while (slot < 3 && !named(k, kinds[slot])) slot++;
    value(w, nullptr, k, 0);
    text(w, " ");
    if (slot == 3 || !counts[slot]) {
      value(w, f, k->next, 0);
      text(w, "\n");
      continue;
    }

    node* existing = resolve(f, k->next, 0);
    if (!existing || existing->type != DICT) {
      stop(w->j, RMR_ERR_INVALID_ARGUMENT, "invalid page resources");
      return;
    }
    text(w, "<<\n");
    fields(w, f, existing, nullptr, 0);
    for (size_t i = 0; i < counts[slot] && tick(w->j); i++) {
      char name[24];
      snprintf(name, sizeof name, "RMR%c%u", prefixes[slot], lists[slot][i]);
      if (get(existing, name)) {
        stop(w->j, RMR_ERR_INVALID_ARGUMENT, "PDF resource name in use");
        return;
      }
      format(w, "/%s %u 0 R\n", name, lists[slot][i]);
    }
    text(w, ">>\n");
    written[slot] = true;
  }

  for (size_t slot = 0; slot < 3; slot++) {
    if (written[slot] || !counts[slot]) continue;
    format(w, "/%s <<\n", kinds[slot]);
    for (size_t i = 0; i < counts[slot] && tick(w->j); i++)
      format(w, "/RMR%c%u %u 0 R\n", prefixes[slot], lists[slot][i],
             lists[slot][i]);
    text(w, ">>\n");
  }
  text(w, ">>");
}

/* The content streams "q" and "Q" that wrap a page's own content when more
 * is drawn after it, written once per file. */
static void wrapping(rmr_pdf_builder* b) {
  if (!b->open) {
    b->open = stream_text(&b->w, "q\n");
    b->close = stream_text(&b->w, "Q\n");
  }
}

/* Writes a page's existing content references (renumbered through f). */
static void page_contents(writer* w, file* f, node* old) {
  node* v = resolve(f, old, 0);
  if (v && v->type == ARRAY)
    for (node* c = v->child; c && tick(w->j); c = c->next) {
      value(w, f, c, 0);
      text(w, " ");
    }
  else if (old && !(v && v->type == SCALAR && v->length == 4 &&
                    !memcmp(v->raw, "null", 4))) {
    value(w, f, old, 0);
    text(w, " ");
  }
}

/* Writes the base page p (its object renumbered) with o's content stream
 * before its own, if any, and its own content between the streams open ("q")
 * and close ("Q"), so that o's content stream after them, if any, starts from
 * the initial graphics state, and with o's resources added to its inherited
 * ones. */
static void extend_page(rmr_pdf_builder* b, file* f, page* p,
                        const rmr_pdf_page_objects* o) {
  writer* w = &b->w;
  entry* e = p->object;
  begin(w, e->mapped, 0);
  text(w, "<<\n");
  const char* skip[] = {"Contents", "Resources"};
  fields(w, f, e->object, skip, 2);
  text(w, "/Contents [");
  if (o->before) format(w, "%u 0 R ", o->before);
  if (o->contents) format(w, "%u 0 R ", b->open);
  page_contents(w, f, get(e->object, "Contents"));
  if (o->contents) format(w, "%u 0 R %u 0 R", b->close, o->contents);
  text(w, "]\n/Resources ");
  const uint32_t* const lists[3] = {o->xobjects, o->states, o->spaces};
  const size_t counts[3] = {o->xobject_count, o->state_count, o->space_count};
  resources_with(w, f, resolve(f, p->resources, 0), lists, counts);
  text(w, "\n>>");
  end(w);
}

/* Gives the file base's version when it has one of the same length, so that
 * recorded offsets stay valid. Once the header has gone to the output, the
 * version can no longer change. */
static void version(rmr_pdf_builder* b, const uint8_t* base, size_t size) {
  writer* w = &b->w;
  if (size > 9 && base[5] >= '0' && base[5] <= '9' && base[6] == '.' &&
      base[7] >= '0' && base[7] <= '9' &&
      (base[8] == '\n' || base[8] == '\r')) {
    if (w->output && w->flushed) {
      if (memcmp(b->version, base + 5, 3))
        stop(w->j, RMR_ERR_BACKEND, "PDF version changed after output began");
      return;
    }
    uint8_t* header = w->chunks ? w->chunks->bytes : w->data.bytes;
    memcpy(header + 5, base + 5, 3);
    memcpy(b->version, base + 5, 3);
  }
}

void rmr_pdf_builder_version(rmr_pdf_builder* b, const uint8_t* base,
                             size_t size) {
  if (base && b->j.status == RMR_OK) version(b, base, size);
}

rmr_status rmr_pdf_builder_merge(rmr_pdf_builder* b, const uint8_t* base,
                                 size_t size, const rmr_pdf_page_objects* pages,
                                 size_t count, rmr_buffer* out,
                                 rmr_error* error) {
  job* j = &b->j;
  writer* w = &b->w;
  file f;
  if (!base || !size || (!out && !w->output) || (count && !pages))
    stop(j, RMR_ERR_INVALID_ARGUMENT, "missing PDF assembly input");

  if (tick(j) && open_file(&f, j, base, size)) {
    version(b, base, size);
    if (f.count > SIZE_MAX / sizeof(void*) ||
        f.page_count > SIZE_MAX / sizeof(void*))
      stop(j, RMR_ERR_LIMIT, "PDF assembly object limit");
    page** list = take(j, f.page_count * sizeof *list);
    page** owners = take(j, f.count * sizeof *owners);
    const rmr_pdf_page_objects** extra = take(j, f.count * sizeof *extra);

    if (j->status == RMR_OK) {
      size_t at = 0;
      for (page* p = f.pages; p; p = p->next) list[at++] = p;
      for (size_t i = 0; i < count && tick(j); i++) {
        size_t index = 0;
        if (pages[i].page >= f.page_count ||
            (!pages[i].contents && !pages[i].before) ||
            extra[index = (size_t)(list[pages[i].page]->object - f.entries)]) {
          stop(j, RMR_ERR_INVALID_ARGUMENT, "invalid PDF page extension");
          break;
        }
        extra[index] = &pages[i];
        owners[index] = list[pages[i].page];
      }
    }

    for (size_t i = 0; i < f.count && tick(j); i++) {
      entry* e = &f.entries[i];
      if (!e->live || absent(e)) continue;
      node* n = object(&f, e, 0);
      if (!n) break;
      node* type = n->type == DICT ? get(n, "Type") : nullptr;
      if (!e->stream || !(named(type, "ObjStm") || named(type, "XRef")))
        e->mapped = reserve(w);
    }

    if (count && j->status == RMR_OK) wrapping(b);
    for (size_t i = 0; i < f.count && tick(j); i++) {
      entry* e = &f.entries[i];
      if (!e->mapped) continue;
      if (extra[i]) {
        extend_page(b, &f, owners[i], extra[i]);
        continue;
      }
      begin(w, e->mapped, 0);
      value(w, &f, e->object, 0);
      if (e->stream) {
        text(w, "\nstream\n");
        put(w, e->stream, e->stream_size);
        text(w, "\nendstream");
      }
      end(w);
    }

    if (j->status == RMR_OK) {
      w->renumber = &f;
      f.last = 0;
      f.xref_streams = false;
      finish(w, &f);
      w->renumber = nullptr;
    }
  }

  if (j->status == RMR_OK) assemble(w, out);
  rmr_status s = built(b, error);
  /* Spent either way. */
  stop(j, RMR_ERR_INVALID_ARGUMENT, "PDF builder already finished");
  return s;
}

uint32_t rmr_pdf_builder_pages(rmr_pdf_builder* b, size_t count) {
  if (b->tree || !count || count > b->j.limits.maximum_draws ||
      count >= UINT32_MAX - b->w.next_number) {
    stop(&b->j, RMR_ERR_LIMIT, "PDF page count limit");
    return 0;
  }
  b->tree = reserve(&b->w);
  b->first_page = (uint32_t)b->w.next_number;
  b->w.next_number += count;
  b->page_count = count;
  return b->first_page;
}

/* Parses pdf under a job of its own as the writer's job, so that its parse
 * and work stay bounded per import; xref rows still go to the builder. */
static bool import_begin(rmr_pdf_builder* b, job* scratch, file* f,
                         const uint8_t* pdf, size_t size) {
  *scratch = (job){.limits = b->j.limits};
  b->w.j = scratch;
  b->w.records = &b->j;
  if (b->j.status != RMR_OK) {
    stop(scratch, b->j.status, b->j.reason);
    return false;
  }
  return open_file(f, scratch, pdf, size);
}

/* Ends an import: its failure becomes the builder's. */
static void import_end(rmr_pdf_builder* b, job* scratch) {
  b->w.j = &b->j;
  b->w.records = nullptr;
  if (scratch->status != RMR_OK)
    stop(&b->j, scratch->status,
         scratch->reason ? scratch->reason : "PDF import failed");
  release(scratch);
}

/* The page's content as one stream: the raw bytes of its single stream, or
 * its streams decoded and joined by newlines into data. dict is null for
 * the joined data, which is unfiltered. */
static bool one_stream(file* f, node* contents, entry** single,
                       rmr_buffer* joined) {
  *single = nullptr;
  *joined = (rmr_buffer){0};
  node* v = resolve(f, contents, 0);
  if (!v) return true; /* An empty page. */
  if (v->type != ARRAY) {
    *single = single_stream(f, contents);
    return *single != nullptr;
  }
  if (v->child && !v->child->next) {
    *single = single_stream(f, v->child);
    return *single != nullptr;
  }

  size_t total = 0;
  for (node* c = v->child; c; c = c->next) {
    entry* e = single_stream(f, c);
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (!e || !decode_stream(f->j, &f->decoded_budget, e->object, e->stream,
                             e->stream_size, &data, &size))
      return false;
    uint8_t* grown = rmr_realloc(joined->bytes, total + size + 1);
    if (!grown) {
      stop(f->j, RMR_ERR_OUT_OF_MEMORY, "PDF content allocation");
      return false;
    }
    joined->bytes = grown;
    memcpy(grown + total, data, size);
    grown[total + size] = '\n';
    total += size + 1;
  }
  joined->size = total;
  return true;
}

/* Gives every object n refers to, directly or through other objects, a new
 * number (unless it has one) and remembers it on stack to visit. */
static bool reach(writer* w, file* f, node* n, entry** stack, size_t* top) {
  if (!n || !tick(f->j)) return f->j->status == RMR_OK;
  if (n->type == REF) {
    entry* e = lookup(f, n->number);
    /* A reference to nothing, such as an object Quartz reserves as a live
     * row at offset zero and never writes, fails when written. */
    if (e && e->live && !absent(e) && e->generation == n->generation &&
        !e->mapped) {
      e->mapped = reserve(w);
      stack[(*top)++] = e;
    }
    return true;
  }
  if (n->type == ARRAY || n->type == DICT)
    for (node* c = n->child; c; c = c->next)
      if (!reach(w, f, c, stack, top)) return false;
  return true;
}

/* Marks share() leaves on an import's objects. */
enum { SHARE_OPEN = 1, SHARE_DONE = 2, SHARE_ABLE = 4, SHARE_OWN = 8 };

/* share()'s marks and digests, by entry index of the imported file. */
typedef struct {
  uint8_t* marks;
  digest* sums;
} sharing;

/* Pushes on todo, grown as needed, each object n refers to that share() has
 * not yet opened. */
static bool push_refs(file* f, node* n, const uint8_t* marks, entry*** todo,
                      size_t* top, size_t* room) {
  if (!n || !tick(f->j)) return f->j->status == RMR_OK;
  if (n->type == REF) {
    entry* e = lookup(f, n->number);
    if (!e || !e->live || absent(e) || e->generation != n->generation ||
        marks[e - f->entries] & (SHARE_OPEN | SHARE_DONE))
      return true;
    if (*top == *room) {
      size_t grown = *room ? *room * 2 : 256;
      entry** more = take(f->j, grown * sizeof *more);
      if (!more) return false;
      if (*top) memcpy(more, *todo, *top * sizeof *more);
      *todo = more;
      *room = grown;
    }
    (*todo)[(*top)++] = e;
    return true;
  }
  if (n->type == ARRAY || n->type == DICT)
    for (node* c = n->child; c; c = c->next)
      if (!push_refs(f, c, marks, todo, top, room)) return false;
  return true;
}

/* Adds name token n, its #xx escapes decoded, to h. */
static void absorb_name(digester* h, const node* n) {
  size_t length = 0;
  for (size_t i = 1; i < n->length; i++, length++)
    if (n->raw[i] == '#') i += 2;
  absorb_size(h, length);
  for (size_t i = 1; i < n->length; i++) {
    uint8_t c = n->raw[i];
    if (c == '#') {
      c = (uint8_t)(hex(n->raw[i + 1]) * 16 + hex(n->raw[i + 2]));
      i += 2;
    }
    absorb(h, &c, 1);
  }
}

/* Adds n's canonical form to h: its kind, a dictionary's keys decoded and in
 * order, a reference as the digest of the object referred to. False, the job
 * still fine, when n refers to an object that cannot be shared. */
static bool canonical(file* f, const sharing* s, digester* h, node* n,
                      unsigned depth) {
  if (!tick(f->j) || depth >= 64) return false;
  uint8_t tag = (uint8_t)n->type;
  absorb(h, &tag, 1);
  if (n->type == REF) {
    entry* e = lookup(f, n->number);
    if (!e || e->generation != n->generation ||
        !(s->marks[e - f->entries] & SHARE_ABLE))
      return false;
    absorb(h, s->sums[e - f->entries].bytes, sizeof(digest));
    return true;
  }
  if (n->type == NAME) {
    absorb_name(h, n);
    return true;
  }
  if (n->type == SCALAR) {
    absorb_size(h, n->length);
    absorb(h, n->raw, n->length);
    return true;
  }

  size_t count = 0;
  for (node* c = n->child; c; c = c->next) count++;
  absorb_size(h, count);
  if (n->type == ARRAY) {
    for (node* c = n->child; c; c = c->next)
      if (!canonical(f, s, h, c, depth + 1)) return false;
    return true;
  }

  node *few[16], **keys = few;
  count /= 2;
  if (count > 16) {
    /* Sorting costs work as unique_keys() charges it. */
    size_t bits = 0;
    for (size_t k = count; k; k >>= 1) bits++;
    if (!charge(f->j, count * bits) ||
        !(keys = take(f->j, count * sizeof *keys)))
      return false;
  }
  size_t at = 0;
  for (node* k = n->child; k; k = k->next->next) keys[at++] = k;
  qsort(keys, count, sizeof *keys, key_order);
  for (size_t i = 0; i < count; i++) {
    absorb_name(h, keys[i]);
    if (!canonical(f, s, h, keys[i]->next, depth + 1)) return false;
  }
  return true;
}

/* Objects whose identity matters, never shared: the document's structure,
 * its pages, annotations and optional content. */
static bool distinct(node* n) {
  static const char* const kinds[] = {
      "Catalog", "Pages", "Page",           "Annot",      "OCG",
      "OCMD",    "Sig",   "StructTreeRoot", "StructElem", "Outlines"};
  node* type = get(n, "Type");
  for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++)
    if (named(type, kinds[i])) return true;
  return get(n, "Subtype") && get(n, "Rect"); /* An annotation untyped. */
}

/* The digest of imported object e: its canonical form, then its stream's
 * bytes as stored. False, the job still fine, when e cannot be shared. */
static bool object_digest(file* f, const sharing* s, entry* e, digest* out) {
  digester h;
  rmr_sha256_init(&h);
  absorb(&h, e->stream ? "S" : "O", 1);
  if (!canonical(f, s, &h, e->object, 0)) return false;
  if (e->stream) {
    if (!charge(f->j, e->stream_size / 4096)) return false;
    absorb_size(&h, e->stream_size);
    absorb(&h, e->stream, e->stream_size);
  }
  *out = rmr_sha256_final(&h);
  return true;
}

/* Ahead of an import into a writer keeping a ledger: digests every object the
 * roots reach, those it refers to first, and gives each the ledger has seen
 * the number recorded there, so that the import refers to it instead of
 * writing it again, nor what only it refers to. An object is shared when it
 * is neither a page's content nor of distinct identity, takes no part in a
 * reference cycle, and refers only to objects themselves shared. */
static bool share(writer* w, file* f, node* const* roots, size_t count,
                  sharing* s) {
  job* j = f->j;
  s->marks = take(j, f->count);
  s->sums = take(j, f->count * sizeof *s->sums);
  if (!s->marks || !s->sums) return false;
  for (page* p = f->pages; p; p = p->next) {
    node* c = get(p->object->object, "Contents");
    entry* e = c && c->type == REF ? lookup(f, c->number) : nullptr;
    if (e) s->marks[e - f->entries] |= SHARE_OWN;
    if (e && e->object) c = e->object;
    for (node* k = c && c->type == ARRAY ? c->child : nullptr; k; k = k->next)
      if (k->type == REF && (e = lookup(f, k->number)))
        s->marks[e - f->entries] |= SHARE_OWN;
  }

  entry** todo = nullptr;
  size_t top = 0, room = 0;
  for (size_t i = 0; i < count; i++)
    if (!push_refs(f, roots[i], s->marks, &todo, &top, &room)) return false;
  while (top && j->status == RMR_OK) {
    entry* e = todo[top - 1];
    size_t at = (size_t)(e - f->entries);
    if (!(s->marks[at] & (SHARE_OPEN | SHARE_DONE))) {
      s->marks[at] |= SHARE_OPEN;
      node* n = object(f, e, 0);
      if (!n || !push_refs(f, n, s->marks, &todo, &top, &room)) return false;
      continue;
    }
    top--;
    if (s->marks[at] & SHARE_DONE) continue;
    /* Every object e refers to is done, or open: a cycle through e. */
    s->marks[at] = (uint8_t)((s->marks[at] & ~SHARE_OPEN) | SHARE_DONE);
    if (s->marks[at] & SHARE_OWN || distinct(e->object) ||
        !object_digest(f, s, e, &s->sums[at]))
      continue;
    s->marks[at] |= SHARE_ABLE;
    uint32_t number = ledger_find(w->shared, s->sums[at]);
    if (number) e->mapped = number;
  }
  return j->status == RMR_OK;
}

/* Numbers and writes the objects the roots refer to and those they refer to
 * in turn: what a form or page imported from f draws with, without the
 * file's catalog, page tree or document information. Objects numbered
 * before the call (the page, its tree) are referred to, not written; with a
 * ledger, so are those an earlier import wrote, and the objects written are
 * recorded in it. */
static bool import_reachable(writer* w, file* f, node* const* roots,
                             size_t count) {
  sharing s = {0};
  if (w->shared && !share(w, f, roots, count, &s)) return false;
  entry** stack = take(f->j, (f->count + 1) * sizeof *stack);
  if (!stack) return false;
  size_t top = 0, written = 0;
  for (size_t i = 0; i < count; i++)
    if (!reach(w, f, roots[i], stack, &top)) return false;

  entry** order = take(f->j, (f->count + 1) * sizeof *order);
  if (!order) return false;
  while (top) {
    entry* e = stack[--top];
    order[written++] = e;
    node* n = object(f, e, 0);
    if (!n || !reach(w, f, n, stack, &top)) return false;
  }

  for (size_t i = 0; i < written && w->j->status == RMR_OK; i++) {
    entry* e = order[i];
    begin(w, e->mapped, 0);
    value(w, f, e->object, 0);
    if (e->stream) {
      text(w, "\nstream\n");
      put(w, e->stream, e->stream_size);
      text(w, "\nendstream");
    }
    end(w);
  }
  for (size_t i = 0; s.marks && i < written && w->j->status == RMR_OK; i++) {
    size_t at = (size_t)(order[i] - f->entries);
    if (s.marks[at] & SHARE_ABLE)
      ledger_add(w->shared, s.sums[at], order[i]->mapped);
  }
  return w->j->status == RMR_OK && f->j->status == RMR_OK;
}

/* The values of dictionary d other than its key `except`, as roots. */
static size_t dict_values(node* d, const char* except, node** out,
                          size_t room) {
  size_t n = 0;
  for (node* k = d && d->type == DICT ? d->child : nullptr; k && n < room;
       k = k->next->next)
    if (!named(k, except)) out[n++] = k->next;
  return n;
}

/* The objects a form made of page p needs: its resources and box, and the
 * filter parameters of its single content stream. */
static bool import_roots(writer* w, file* f, page* p, entry* single) {
  node* roots[64];
  size_t n = 0;
  roots[n++] = p->resources;
  roots[n++] = p->crop ? p->crop : p->media;
  if (single) n += dict_values(single->object, "Length", roots + n, 64 - n);
  return import_reachable(w, f, roots, n);
}

rmr_status rmr_pdf_builder_form(rmr_pdf_builder* b, const uint8_t* pdf,
                                size_t size, uint32_t* out, rmr_error* error) {
  job scratch;
  file f;
  writer* w = &b->w;
  rmr_buffer joined = {0};
  if (import_begin(b, &scratch, &f, pdf, size)) {
    page* p = f.pages;
    uint64_t rotation = 0;
    node* box = resolve(&f, p && p->crop ? p->crop : p ? p->media : nullptr, 0);
    entry* single = nullptr;
    if (f.page_count != 1)
      stop(&scratch, RMR_ERR_INVALID_ARGUMENT, "form PDF must have one page");
    else if (p->rotate && (!uint_value(resolve(&f, p->rotate, 0), &rotation) ||
                           rotation % 360))
      stop(&scratch, RMR_ERR_UNSUPPORTED, "rotated form PDF");
    else if (!box || box->type != ARRAY)
      stop(&scratch, RMR_ERR_INVALID_ARGUMENT, "form PDF has no media box");
    else if (one_stream(&f, get(p->object->object, "Contents"), &single,
                        &joined) &&
             import_roots(w, &f, p, single)) {
      uint32_t id = reserve(w);
      begin(w, id, 0);
      text(w, "<< /Type /XObject /Subtype /Form /FormType 1\n/BBox ");
      value(w, &f, box, 0);
      text(w, "\n/Matrix [1 0 0 1 0 0]\n/Resources ");
      node* resources = p->resources ? p->resources : nullptr;
      if (resources)
        value(w, &f, resources, 0);
      else
        text(w, "<< >>");
      text(w, "\n");
      /* A page's /Group is not carried over: as PDFium imports a page, the
       * form is no transparency group of its own. */
      if (single) {
        const char* skip[] = {"Type",      "Subtype", "FormType", "BBox",
                              "Resources", "Matrix",  "Group",    "Length"};
        fields(w, &f, single->object, skip, sizeof skip / sizeof *skip);
        format(w, "/Length %zu >>\nstream\n", single->stream_size);
        put(w, single->stream, single->stream_size);
      } else {
        format(w, "/Length %zu >>\nstream\n", joined.size);
        put(w, joined.bytes, joined.size);
      }
      text(w, "\nendstream");
      end(w);
      if (scratch.status == RMR_OK) *out = id;
    }
  }
  free(joined.bytes);
  import_end(b, &scratch);
  return built(b, error);
}

rmr_status rmr_pdf_builder_wrap(rmr_pdf_builder* b, double width, double height,
                                const uint32_t* forms, size_t count,
                                uint32_t* out, rmr_error* error) {
  writer* w = &b->w;
  writer content = {.j = &b->j};
  for (size_t i = 0; i < count; i++)
    format(&content, "q /RMRx%u Do Q\n", forms[i]);
  uint32_t id = reserve(w);
  begin(w, id, 0);
  text(w, "<< /Type /XObject /Subtype /Form /FormType 1 /BBox [0 0 ");
  real(w, width);
  text(w, " ");
  real(w, height);
  text(w, "]\n/Matrix [1 0 0 1 0 0]\n/Resources ");
  const uint32_t* const lists[3] = {forms, nullptr, nullptr};
  const size_t counts[3] = {count, 0, 0};
  resources_with(w, nullptr, nullptr, lists, counts);
  format(w, "\n/Length %zu >>\nstream\n", content.data.size);
  put(w, content.data.bytes, content.data.size);
  text(w, "\nendstream");
  end(w);
  free(content.data.bytes);
  if (b->j.status == RMR_OK) *out = id;
  return built(b, error);
}

rmr_status rmr_pdf_builder_page(rmr_pdf_builder* b, size_t index,
                                const uint8_t* pdf, size_t size, double width,
                                double height, const uint32_t* forms,
                                size_t count, const rmr_pdf_local_link* links,
                                size_t link_count, rmr_error* error) {
  writer* w = &b->w;
  if (index >= b->page_count || (link_count && !links) || (count && !forms)) {
    stop(&b->j, RMR_ERR_INVALID_ARGUMENT, "invalid PDF page");
    return built(b, error);
  }
  uint32_t number = b->first_page + (uint32_t)index;
  page_refs refs = {nullptr, nullptr, b->first_page, b->page_count};

  /* The forms drawn after the page's own content; a page of its own always
   * has a content stream, empty when it draws nothing. */
  uint32_t drawn = 0;
  if ((count || !pdf) && b->j.status == RMR_OK) {
    writer content = {.j = &b->j};
    for (size_t i = 0; i < count; i++)
      format(&content, "q /RMRx%u Do Q\n", forms[i]);
    drawn = reserve(w);
    begin(w, drawn, 0);
    format(w, "<< /Length %zu >>\nstream\n", content.data.size);
    put(w, content.data.bytes, content.data.size);
    text(w, "\nendstream");
    end(w);
    free(content.data.bytes);
    if (pdf) wrapping(b);
  }

  uint32_t* annots = nullptr;
  if (link_count && b->j.status == RMR_OK) {
    annots = take(&b->j, link_count * sizeof *annots);
    for (size_t i = 0; annots && i < link_count; i++) {
      annots[i] = reserve(w);
      link_object(w, annots[i], links[i].rect, refs, links[i].destination);
    }
  }
  const uint32_t* const lists[3] = {forms, nullptr, nullptr};
  const size_t counts[3] = {count, 0, 0};

  if (!pdf) {
    if (b->j.status == RMR_OK) {
      begin(w, number, 0);
      format(w, "<< /Type /Page /Parent %u 0 R /MediaBox [0 0 ", b->tree);
      real(w, width);
      text(w, " ");
      real(w, height);
      text(w, "]\n");
      if (drawn) format(w, "/Contents %u 0 R\n", drawn);
      text(w, "/Resources ");
      resources_with(w, nullptr, nullptr, lists, counts);
      if (link_count) {
        text(w, "\n/Annots [");
        for (size_t i = 0; i < link_count; i++) format(w, "%u 0 R ", annots[i]);
        text(w, "]");
      }
      text(w, " >>");
      end(w);
    }
    return built(b, error);
  }

  job scratch;
  file f;
  if (import_begin(b, &scratch, &f, pdf, size)) {
    if (f.page_count != 1)
      stop(&scratch, RMR_ERR_INVALID_ARGUMENT, "page PDF must have one page");
    page* p = f.pages;
    /* The page tree is replaced by the output's, the page becomes page
     * `number`; what the page refers to comes along, nothing else. */
    node* roots[64];
    size_t n = 0;
    if (scratch.status == RMR_OK) {
      p->object->mapped = number;
      for (node* tree = get(p->object->object, "Parent");
           tree && tree->type == REF;) {
        entry* e = lookup(&f, tree->number);
        if (!e || e->mapped) break;
        e->mapped = b->tree;
        node* d = object(&f, e, 0);
        tree = d ? get(d, "Parent") : nullptr;
      }
      roots[n++] = p->resources;
      roots[n++] = p->media;
      roots[n++] = p->crop;
      roots[n++] = p->rotate;
      n += dict_values(p->object->object, "Parent", roots + n, 64 - n);
    }
    if (scratch.status == RMR_OK && import_reachable(w, &f, roots, n)) {
      entry* e = p->object;
      begin(w, number, 0);
      format(w, "<<\n/Parent %u 0 R\n", b->tree);
      const char* skip[] = {"Parent",   "Contents", "Resources", "Annots",
                            "MediaBox", "CropBox",  "Rotate"};
      fields(w, &f, e->object, skip, sizeof skip / sizeof *skip);
      /* Inherited attributes are written on the page itself. */
      const char* names[] = {"MediaBox", "CropBox", "Rotate"};
      node* inherited[] = {p->media, p->crop, p->rotate};
      for (size_t k = 0; k < 3; k++)
        if (inherited[k]) {
          format(w, "/%s ", names[k]);
          value(w, &f, inherited[k], 0);
          text(w, "\n");
        }
      text(w, "/Contents [");
      if (drawn) format(w, "%u 0 R ", b->open);
      page_contents(w, &f, get(e->object, "Contents"));
      if (drawn) format(w, "%u 0 R %u 0 R", b->close, drawn);
      text(w, "]\n/Resources ");
      resources_with(w, &f, resolve(&f, p->resources, 0), lists, counts);
      node* existing = resolve(&f, get(e->object, "Annots"), 0);
      if (existing && existing->type != ARRAY)
        stop(&scratch, RMR_ERR_INVALID_ARGUMENT, "invalid annotations");
      if (existing || link_count) {
        text(w, "\n/Annots [");
        for (node* a = existing ? existing->child : nullptr;
             a && tick(&scratch); a = a->next) {
          value(w, &f, a, 0);
          text(w, " ");
        }
        for (size_t k = 0; k < link_count; k++) format(w, "%u 0 R ", annots[k]);
        text(w, "]");
      }
      text(w, "\n>>");
      end(w);
    }
  }
  import_end(b, &scratch);
  return built(b, error);
}

rmr_status rmr_pdf_builder_finish(rmr_pdf_builder* b,
                                  const rmr_pdf_outline* outline,
                                  size_t outline_count, rmr_buffer* out,
                                  rmr_error* error) {
  job* j = &b->j;
  writer* w = &b->w;
  if ((!out && !w->output) || !b->tree || (outline_count && !outline))
    stop(j, RMR_ERR_INVALID_ARGUMENT, "missing PDF pages");
  if (outline_count >= j->limits.maximum_elements)
    stop(j, RMR_ERR_LIMIT, "navigation limit");

  page_refs refs = {nullptr, nullptr, b->first_page, b->page_count};
  uint32_t outlines = j->status == RMR_OK
                          ? outline_objects(w, outline, outline_count, refs)
                          : 0;
  uint32_t catalog = j->status == RMR_OK ? reserve(w) : 0;
  if (j->status == RMR_OK) {
    begin(w, b->tree, 0);
    format(w, "<< /Type /Pages /Count %zu /Kids [", b->page_count);
    for (size_t i = 0; i < b->page_count && tick(j); i++)
      format(w, "%u 0 R ", b->first_page + (uint32_t)i);
    text(w, "] >>");
    end(w);
    begin(w, catalog, 0);
    format(w, "<< /Type /Catalog /Pages %u 0 R", b->tree);
    if (outlines) format(w, " /Outlines %u 0 R", outlines);
    text(w, " >>");
    end(w);
  }

  /* Every page must have been written; numbers reserved and left unused
   * become free entries. */
  if (j->status == RMR_OK) {
    update* list = take(j, w->count * sizeof *list);
    size_t i = 0;
    for (update* u = w->updates; list && u; u = u->next) list[i++] = *u;
    if (list) qsort(list, w->count, sizeof *list, update_order);
    size_t pages = 0;
    for (i = 0; list && i < w->count; i++)
      if (list[i].id >= b->first_page &&
          list[i].id - b->first_page < b->page_count)
        pages++;
    if (pages != b->page_count)
      stop(j, RMR_ERR_INVALID_ARGUMENT, "PDF pages missing");
    size_t offset = position(w);
    format(w, "xref\n0 %zu\n0000000000 65535 f \n", w->next_number);
    i = 0;
    for (size_t id = 1; list && id < w->next_number && j->status == RMR_OK;
         id++) {
      if (i < w->count && list[i].id == id) {
        if (list[i].offset > 9999999999ULL) {
          stop(j, RMR_ERR_LIMIT, "classic xref offset limit");
          break;
        }
        format(w, "%010zu 00000 n \n", list[i].offset);
        i++;
      } else
        text(w, "0000000000 65535 f \n");
    }
    format(w,
           "trailer\n<< /Size %zu /Root %u 0 R >>\nstartxref\n%zu\n%%%%EOF\n",
           w->next_number, catalog, offset);
  }

  if (j->status == RMR_OK) assemble(w, out);
  rmr_status s = built(b, error);
  stop(j, RMR_ERR_INVALID_ARGUMENT, "PDF builder already finished");
  return s;
}

/* An incremental update or a fresh rewrite written page by page
 * (rmr_pdf_edit.h). The source is parsed once under the update's own job,
 * which also keeps the xref rows; each page's overlays are parsed and
 * imported under a job of their own, released once they are written, so that
 * neither memory nor work grows with the number of pages, and a page costs no
 * more than its own overlays do. An object an overlay imports identical to
 * one an earlier page's did (a grain mask, a color profile) is referred to
 * instead of written again; only a ledger of digests and object numbers,
 * bounded, outlives the page. */
struct rmr_pdf_update {
  job j;
  writer w;
  file f;
  page** pages;  /* source pages by index */
  uint8_t* done; /* pages written: given overlays, or (fresh) at finish */
  uint8_t* skip; /* fresh: source objects written with their pages */
  uint32_t open, close;
  size_t updated, original;
  bool fresh, started;
  ledger shared; /* objects the pages' overlays imported */
};

/* The update's failure as an rmr_status, filling error. */
static rmr_status update_status(rmr_pdf_update* u, rmr_error* error) {
  job* j = &u->j;
  return j->status == RMR_OK ? RMR_OK
                             : rmr_fail(error, j->status, 0,
                                        j->reason  ? j->reason
                                        : u->fresh ? "PDF rewrite failed"
                                                   : "PDF update failed");
}

void rmr_pdf_update_free(rmr_pdf_update* u) {
  if (!u) return;
  free(u->w.data.bytes);
  for (chunk* c = u->w.chunks; c;) {
    chunk* next = c->next;
    chunk_free(c);
    c = next;
  }
  free(u->shared.slots);
  release(&u->j);
  free(u);
}

/* Writes source object e as it is. */
static void copy_object(writer* w, file* f, entry* e) {
  node* n = object(f, e, 0);
  if (!n) return;
  begin(w, e->number, e->generation);
  value(w, nullptr, n, 0);
  if (e->stream) {
    text(w, "\nstream\n");
    put(w, e->stream, e->stream_size);
    text(w, "\nendstream");
  }
  end(w);
}

rmr_status rmr_pdf_update_create(const uint8_t* source, size_t size, bool fresh,
                                 const rmr_limits* limits, rmr_cancel_fn cancel,
                                 void* context, const rmr_output* output,
                                 rmr_pdf_update** out, rmr_error* error) {
  if (!source || !size || !out)
    return rmr_fail(
        error, RMR_ERR_INVALID_ARGUMENT, 0,
        fresh ? "missing PDF rewrite input" : "missing PDF update input");
  rmr_pdf_update* u = rmr_calloc(1, sizeof *u);
  if (!u)
    return rmr_fail(error, RMR_ERR_OUT_OF_MEMORY, 0, "PDF update allocation");
  u->j = (job){.limits = limits ? *limits : rmr_limits_default(),
               .cancel = cancel,
               .context = context};
  /* Without an output the file is collected in chunks, assembled once. */
  u->shared.j = &u->j;
  u->w = (writer){
      .j = &u->j, .output = output, .chunked = !output, .shared = &u->shared};
  u->fresh = fresh;
  u->original = size;
  job* j = &u->j;
  file* f = &u->f;

  if (tick(j) && open_file(f, j, source, size)) {
    u->w.next_number = f->next_number;
    if (f->page_count > SIZE_MAX / sizeof(page*))
      stop(j, RMR_ERR_LIMIT, "PDF page limit");
    u->pages = take(j, f->page_count * sizeof *u->pages);
    u->done = take(j, f->page_count);
    if (fresh) u->skip = take(j, f->count);
    size_t at = 0;
    for (page* p = f->pages; p && j->status == RMR_OK; p = p->next) {
      u->pages[at++] = p;
      if (fresh) u->skip[p->object - f->entries] = 1;
    }
  }

  if (j->status == RMR_OK && !fresh) put(&u->w, source, size);
  /* A fresh file keeps object identities and generations, but not historical
   * revisions or object streams; its pages are written as they are given
   * their overlays, the rest now. */
  if (j->status == RMR_OK && fresh) {
    header(&u->w, source, size);
    for (size_t i = 0; i < f->count && tick(j); i++) {
      entry* e = &f->entries[i];
      if (!e->live || absent(e) || u->skip[i]) continue;
      node* n = object(f, e, 0);
      if (!n) break;
      node* type = n->type == DICT ? get(n, "Type") : nullptr;
      if (e->stream && (named(type, "ObjStm") || named(type, "XRef"))) continue;
      copy_object(&u->w, f, e);
    }
  }

  rmr_status s = update_status(u, error);
  if (s != RMR_OK) {
    rmr_pdf_update_free(u);
    return s;
  }
  *out = u;
  return RMR_OK;
}

rmr_status rmr_pdf_update_page(rmr_pdf_update* u, const rmr_pdf_overlay* parts,
                               size_t count, rmr_error* error) {
  job* j = &u->j;
  writer* w = &u->w;
  if (!parts || !count || count > MAXIMUM_PARTS)
    stop(j, RMR_ERR_INVALID_ARGUMENT, "missing PDF update input");
  else if (u->updated >= j->limits.maximum_draws)
    stop(j, RMR_ERR_LIMIT, "overlay count limit");
  if (!tick(j)) return update_status(u, error);

  const rmr_pdf_overlay* o = &parts[0];
  double determinant =
      o->transform.a * o->transform.d - o->transform.b * o->transform.c;
  bool valid =
      isfinite(determinant) && determinant != 0 &&
      (!o->replace_box ||
       (isfinite(o->box.x) && isfinite(o->box.y) && isfinite(o->box.width) &&
        isfinite(o->box.height) && o->box.width > 0 && o->box.height > 0));
  for (size_t i = 0; i < count; i++)
    valid = valid && parts[i].pdf && parts[i].size && parts[i].page == o->page;
  if (!valid)
    stop(j, RMR_ERR_INVALID_ARGUMENT, "invalid overlay transform/box");
  else if (o->page >= u->f.page_count)
    stop(j, RMR_ERR_INVALID_ARGUMENT, "overlay page outside source");
  else if (u->done[o->page])
    stop(j, RMR_ERR_INVALID_ARGUMENT, "duplicate page overlay");
  if (j->status != RMR_OK) return update_status(u, error);
  u->done[o->page] = 1;

  /* An incremental update starts on a line of its own. */
  if (!u->fresh && !u->started) text(w, "\n");
  u->started = true;
  if (!u->open) {
    u->open = stream_text(w, "q\n");
    u->close = stream_text(w, "Q\n");
  }

  /* The overlays' parse, import and work are this page's alone; their xref
   * rows stay with the update. */
  job scratch = {
      .limits = j->limits, .cancel = j->cancel, .context = j->context};
  if (j->status == RMR_OK) {
    w->j = &scratch;
    w->records = j;
    overlay(w, &u->f, u->pages[o->page], parts, count, u->open, u->close);
    w->j = j;
    w->records = nullptr;
    if (scratch.status != RMR_OK)
      stop(j, scratch.status,
           scratch.reason ? scratch.reason : "PDF update failed");
    release(&scratch);
  }
  if (j->status == RMR_OK) u->updated++;
  return update_status(u, error);
}

rmr_status rmr_pdf_update_finish(rmr_pdf_update* u, rmr_buffer* out,
                                 rmr_pdf_incremental_report* report,
                                 rmr_error* error) {
  job* j = &u->j;
  writer* w = &u->w;
  file* f = &u->f;
  if (!out && !w->output)
    stop(j, RMR_ERR_INVALID_ARGUMENT, "missing PDF update output");

  if (u->fresh) {
    for (size_t i = 0; i < f->page_count && tick(j); i++)
      if (!u->done[i]) copy_object(w, f, u->pages[i]->object);
    if (j->status == RMR_OK) {
      f->last = 0;
      f->xref_streams = false;
      finish(w, f);
    }
  } else if (u->started && j->status == RMR_OK)
    finish(w, f);

  /* Zero overlays leave an exact copy of the source. */
  if (j->status == RMR_OK) assemble(w, out);
  rmr_status s = update_status(u, error);
  if (s == RMR_OK && report)
    *report = (rmr_pdf_incremental_report){u->updated, u->original, !u->fresh};
  stop(j, RMR_ERR_INVALID_ARGUMENT, "PDF update already finished");
  return s;
}

rmr_status rmr_pdf_incremental(const uint8_t* source, size_t size,
                               const rmr_pdf_overlay* overlays, size_t count,
                               const rmr_limits* limits, rmr_cancel_fn cancel,
                               void* context, rmr_buffer* out,
                               rmr_pdf_incremental_report* report,
                               rmr_error* error) {
  if (!out)
    return rmr_fail(error, RMR_ERR_INVALID_ARGUMENT, 0,
                    "missing PDF update input");
  return rmr_pdf_incremental_output(source, size, overlays, count, limits,
                                    cancel, context, nullptr, out, report,
                                    error);
}

rmr_status rmr_pdf_incremental_output(const uint8_t* source, size_t size,
                                      const rmr_pdf_overlay* overlays,
                                      size_t count, const rmr_limits* given,
                                      rmr_cancel_fn cancel, void* context,
                                      const rmr_output* output, rmr_buffer* out,
                                      rmr_pdf_incremental_report* report,
                                      rmr_error* error) {
  if (!source || !size || (!out && !output) || (count && !overlays))
    return rmr_fail(error, RMR_ERR_INVALID_ARGUMENT, 0,
                    "missing PDF update input");
  rmr_limits limits = given ? *given : rmr_limits_default();
  if (count > limits.maximum_draws)
    return rmr_fail(error, RMR_ERR_LIMIT, 0, "overlay count limit");

  rmr_pdf_update* u = nullptr;
  rmr_status s = rmr_pdf_update_create(source, size, false, &limits, cancel,
                                       context, output, &u, error);
  for (size_t i = 0; s == RMR_OK && i < count; i++)
    s = rmr_pdf_update_page(u, &overlays[i], 1, error);
  if (s == RMR_OK) s = rmr_pdf_update_finish(u, out, report, error);
  rmr_pdf_update_free(u);
  return s;
}
