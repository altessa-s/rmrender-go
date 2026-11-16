/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <limits.h>
#include <math.h>
#include <rmrender/apple_pdf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/rmr_pdf_edit.h"
#include "../common/rmr_pdf_options.h"
#include "../common/rmr_pdf_output.h"
#include "rmr_apple_internal.h"
#include "rmr_pdf_platform.h"
#include "rmr_pdf_writer.h"

struct rmr_apple_pdf_source {
  CFDataRef data;
  CGPDFDocumentRef document;
};

/* Fill `e` with `s` and `m`, if non-null; always returns `s`. */
static rmr_status fail(rmr_error* e, rmr_status s, const char* m) {
  if (e) {
    *e = (rmr_error){.code = s};
    snprintf(e->message, sizeof e->message, "%s", m);
  }
  return s;
}

/* Release source and its underlying document and bytes; a no-op for
 * nullptr. */
void rmr_apple_pdf_source_free(rmr_apple_pdf_source* s) {
  if (s) {
    if (s->document) CGPDFDocumentRelease(s->document);
    if (s->data) CFRelease(s->data);
    free(s);
  }
}

/* Open bytes as a PDF source, rejecting encrypted documents, documents that
 * cannot be unlocked, and pages with a UserUnit other than 1. */
rmr_status rmr_apple_pdf_source_create(const uint8_t* bytes, size_t size,
                                       const rmr_limits* given,
                                       rmr_apple_pdf_source** out,
                                       rmr_error* e) {
  if (!bytes || !size || !out)
    return fail(e, RMR_ERR_INVALID_ARGUMENT, "missing PDF bytes");

  rmr_limits l = given ? *given : rmr_limits_default();
  if (size > l.maximum_pdf_bytes || size > LONG_MAX)
    return fail(e, RMR_ERR_LIMIT, "source PDF limit");
  rmr_apple_pdf_source* s = calloc(1, sizeof *s);
  if (!s) return fail(e, RMR_ERR_OUT_OF_MEMORY, "source allocation failed");

  s->data = CFDataCreate(kCFAllocatorDefault, bytes, (CFIndex)size);
  CGDataProviderRef provider =
      s->data ? CGDataProviderCreateWithCFData(s->data) : nullptr;
  s->document = provider ? CGPDFDocumentCreateWithProvider(provider) : nullptr;
  if (provider) CGDataProviderRelease(provider);

  rmr_status status = !s->data       ? RMR_ERR_OUT_OF_MEMORY
                      : !s->document ? RMR_ERR_BACKEND
                      : CGPDFDocumentIsEncrypted(s->document) &&
                              !CGPDFDocumentIsUnlocked(s->document)
                          ? RMR_ERR_UNSUPPORTED
                          : RMR_OK;
  if (status == RMR_OK &&
      CGPDFDocumentGetNumberOfPages(s->document) > l.maximum_draws)
    status = RMR_ERR_LIMIT;

  for (size_t i = 0;
       status == RMR_OK && i < CGPDFDocumentGetNumberOfPages(s->document);
       i++) {
    CGPDFDictionaryRef page =
        CGPDFPageGetDictionary(CGPDFDocumentGetPage(s->document, i + 1));
    CGPDFObjectRef object = nullptr;
    if (CGPDFDictionaryGetObject(page, "UserUnit", &object)) {
      CGPDFReal unit = 0;
      if (!CGPDFDictionaryGetNumber(page, "UserUnit", &unit) || unit != 1)
        status = RMR_ERR_UNSUPPORTED;
    }
  }

  if (status != RMR_OK) {
    rmr_apple_pdf_source_free(s);
    return fail(e, status, "PDF could not be opened");
  }

  *out = s;
  return RMR_OK;
}

/* Number of pages in source, or 0 for nullptr. */
size_t rmr_apple_pdf_source_count(const rmr_apple_pdf_source* s) {
  return s ? CGPDFDocumentGetNumberOfPages(s->document) : 0;
}

rmr_status rmr_apple_pdf_source_size(const rmr_apple_pdf_source* s, size_t page,
                                     rmr_point* out, rmr_error* e) {
  if (!s || !out || page >= rmr_apple_pdf_source_count(s))
    return fail(e, RMR_ERR_INVALID_ARGUMENT, "source page out of range");

  CGPDFPageRef p = CGPDFDocumentGetPage(s->document, page + 1);
  CGRect r = CGPDFPageGetBoxRect(p, kCGPDFMediaBox);
  if (!isfinite(r.size.width) || !isfinite(r.size.height) ||
      r.size.width <= 0 || r.size.height <= 0)
    return fail(e, RMR_ERR_BACKEND, "invalid source media box");

  int rotation = CGPDFPageGetRotationAngle(p) % 180;
  *out = rotation ? (rmr_point){r.size.height, r.size.width}
                  : (rmr_point){r.size.width, r.size.height};
  return RMR_OK;
}

typedef struct {
  rmr_buffer bytes;
  size_t capacity, limit;
  rmr_status status;
} sink;

/* CGDataConsumer callback: append bytes to the sink buffer, growing it and
 * enforcing the sink's limit. */
static size_t put(void* context, const void* bytes, size_t size) {
  sink* s = context;
  if (s->status != RMR_OK) return 0;

  if (size > s->limit - s->bytes.size) {
    s->status = RMR_ERR_LIMIT;
    return 0;
  }

  size_t need = s->bytes.size + size;
  if (need > s->capacity) {
    size_t next =
        s->capacity && s->capacity <= s->limit / 2 ? s->capacity * 2 : need;
    if (next < need) next = need;
    void* p = realloc(s->bytes.bytes, next);
    if (!p) {
      s->status = RMR_ERR_OUT_OF_MEMORY;
      return 0;
    }
    s->bytes.bytes = p;
    s->capacity = next;
  }

  if (size) memcpy(s->bytes.bytes + s->bytes.size, bytes, size);
  s->bytes.size = need;
  return size;
}

/* Compose a source page's native drawing transform with its .rm placement
 * (source_rect) and output sheet transform into one page-to-output matrix.
 */
static CGAffineTransform transform(const rmr_apple_pdf_source* s,
                                   const rmr_pdf_page* p) {
  CGPDFPageRef page = CGPDFDocumentGetPage(s->document, p->source_page + 1);
  CGAffineTransform native = CGPDFPageGetDrawingTransform(
      page, kCGPDFMediaBox, CGRectMake(0, 0, 1, 1), 0, false);
  rmr_rect r = p->source_rect;
  CGAffineTransform rm =
      CGAffineTransformMake(r.width, 0, 0, -r.height, r.x, r.y + r.height);
  rmr_transform t = p->sheet.transform;
  return CGAffineTransformConcat(
      CGAffineTransformConcat(native, rm),
      CGAffineTransformMake(t.a, t.b, t.c, t.d, t.tx, t.ty));
}

/* Named destination identifying output page's own top-left anchor. */
static CFStringRef destination_name(size_t page) {
  return CFStringCreateWithFormat(kCFAllocatorDefault, nullptr,
                                  CFSTR("rmrender-page-%zu"), page);
}

/* Decode a 4-number PDF array as a normalized (non-negative size) CGRect. */
static bool array_rect(CGPDFArrayRef a, CGRect* out) {
  CGPDFReal v[4];
  if (!a || CGPDFArrayGetCount(a) < 4) return false;

  for (size_t i = 0; i < 4; i++)
    if (!CGPDFArrayGetNumber(a, i, &v[i]) || !isfinite(v[i])) return false;

  *out = CGRectMake(fmin(v[0], v[2]), fmin(v[1], v[3]), fabs(v[2] - v[0]),
                    fabs(v[3] - v[1]));
  return true;
}

/* Copy a PDF string or name object's text; nullptr if object is neither. */
static CFStringRef pdf_name(CGPDFObjectRef object) {
  CGPDFStringRef string = nullptr;
  const char* name = nullptr;
  if (CGPDFObjectGetValue(object, kCGPDFObjectTypeString, &string))
    return CGPDFStringCopyTextString(string);
  if (CGPDFObjectGetValue(object, kCGPDFObjectTypeName, &name))
    return CFStringCreateWithCString(kCFAllocatorDefault, name,
                                     kCFStringEncodingUTF8);
  return nullptr;
}

/* Look up name's value in a PDF name tree (Names/Kids), or nullptr if
 * absent; work/maximum and depth bound the traversal. */
static CGPDFObjectRef named_tree(CGPDFDictionaryRef tree, CFStringRef name,
                                 size_t* work, size_t maximum, unsigned depth) {
  if (depth > 64 || *work >= maximum) return nullptr;

  (*work)++;
  CGPDFArrayRef names = nullptr, kids = nullptr;
  if (CGPDFDictionaryGetArray(tree, "Names", &names)) {
    size_t count = CGPDFArrayGetCount(names);
    for (size_t i = 0; i + 1 < count; i += 2) {
      if (*work >= maximum) return nullptr;
      (*work)++;
      CGPDFObjectRef key = nullptr, value = nullptr;
      if (!CGPDFArrayGetObject(names, i, &key)) continue;
      CFStringRef text = pdf_name(key);
      bool equal = text && CFEqual(text, name);
      if (text) CFRelease(text);
      if (equal && CGPDFArrayGetObject(names, i + 1, &value)) return value;
    }
  }

  if (CGPDFDictionaryGetArray(tree, "Kids", &kids)) {
    size_t count = CGPDFArrayGetCount(kids);
    for (size_t i = 0; i < count; i++) {
      if (*work >= maximum) return nullptr;
      (*work)++;
      CGPDFDictionaryRef kid = nullptr;
      if (!CGPDFArrayGetDictionary(kids, i, &kid)) continue;
      CGPDFObjectRef found = named_tree(kid, name, work, maximum, depth + 1);
      if (found) return found;
    }
  }

  return nullptr;
}

/* Resolve a destination reference (explicit array, action dictionary, or
 * named destination) to its explicit destination array, or nullptr. */
static CGPDFArrayRef destination_array(const rmr_apple_pdf_source* s,
                                       CGPDFObjectRef object, size_t* work,
                                       size_t maximum, unsigned depth) {
  if (!object || depth > 16 || *work >= maximum) return nullptr;
  (*work)++;
  CGPDFArrayRef array = nullptr;
  CGPDFDictionaryRef dictionary = nullptr;
  if (CGPDFObjectGetValue(object, kCGPDFObjectTypeArray, &array)) return array;

  if (CGPDFObjectGetValue(object, kCGPDFObjectTypeDictionary, &dictionary)) {
    CGPDFObjectRef value = nullptr;
    if (CGPDFDictionaryGetObject(dictionary, "D", &value))
      return destination_array(s, value, work, maximum, depth + 1);
    return nullptr;
  }

  CFStringRef name = pdf_name(object);
  if (!name) return nullptr;
  CGPDFDictionaryRef catalog = CGPDFDocumentGetCatalog(s->document),
                     destinations = nullptr, names = nullptr;
  CGPDFObjectRef found = nullptr;
  if (CGPDFDictionaryGetDictionary(catalog, "Dests", &destinations)) {
    char key[1024];
    if (CFStringGetCString(name, key, sizeof key, kCFStringEncodingUTF8))
      CGPDFDictionaryGetObject(destinations, key, &found);
  }
  if (!found && CGPDFDictionaryGetDictionary(catalog, "Names", &names) &&
      CGPDFDictionaryGetDictionary(names, "Dests", &destinations))
    found = named_tree(destinations, name, work, maximum, 0);

  CFRelease(name);
  return found ? destination_array(s, found, work, maximum, depth + 1)
               : nullptr;
}

typedef struct {
  size_t page, origin, annotation;
  CGPoint point;
} local_destination;

typedef struct {
  local_destination* items;
  size_t count, capacity, maximum;
} local_destinations;

/* Named destination identifying one source link annotation's target point.
 */
static CFStringRef local_name(size_t origin, size_t annotation) {
  return CFStringCreateWithFormat(kCFAllocatorDefault, nullptr,
                                  CFSTR("rmrender-link-%zu-%zu"), origin,
                                  annotation);
}

/* Append item to list, growing its storage up to list->maximum entries. */
static rmr_status destination_append(local_destinations* list,
                                     local_destination item) {
  if (list->count == list->maximum) return RMR_ERR_LIMIT;

  if (list->count == list->capacity) {
    size_t next = list->capacity ? list->capacity * 2 : 16;
    if (next > list->maximum) next = list->maximum;
    void* memory = realloc(list->items, next * sizeof *list->items);
    if (!memory) return RMR_ERR_OUT_OF_MEMORY;
    list->items = memory;
    list->capacity = next;
  }

  list->items[list->count++] = item;
  return RMR_OK;
}

/* qsort comparator: order local_destination entries by output page. */
static int destination_order(const void* a, const void* b) {
  const local_destination *x = a, *y = b;
  return x->page < y->page ? -1 : x->page > y->page ? 1 : 0;
}

/* Resolve a GoTo destination object to its output page index and point in
 * that page's output space, or SIZE_MAX if it names no visible output page.
 */
static size_t explicit_destination(const rmr_apple_pdf_source* s,
                                   CGPDFObjectRef object,
                                   const rmr_pdf_page* pages, size_t count,
                                   size_t* work, size_t maximum,
                                   CGPoint* point) {
  CGPDFArrayRef array = destination_array(s, object, work, maximum, 0);
  if (!array || CGPDFArrayGetCount(array) < 1) return SIZE_MAX;

  CGPDFDictionaryRef target = nullptr;
  if (!CGPDFArrayGetDictionary(array, 0, &target)) return SIZE_MAX;

  for (size_t i = 0; i < count; i++) {
    if (*work >= maximum) return SIZE_MAX;
    (*work)++;
    if (pages[i].has_source &&
        CGPDFPageGetDictionary(CGPDFDocumentGetPage(
            s->document, pages[i].source_page + 1)) == target) {
      CGRect media = CGPDFPageGetBoxRect(
          CGPDFDocumentGetPage(s->document, pages[i].source_page + 1),
          kCGPDFMediaBox);
      CGPoint position =
          CGPointMake(CGRectGetMinX(media), CGRectGetMaxY(media));
      const char* mode = nullptr;
      CGPDFReal value;

      if (CGPDFArrayGetName(array, 1, &mode)) {
        if (!strcmp(mode, "XYZ") || !strcmp(mode, "FitV") ||
            !strcmp(mode, "FitBV") || !strcmp(mode, "FitR"))
          if (CGPDFArrayGetNumber(array, 2, &value) && isfinite(value))
            position.x = value;
        size_t at = !strcmp(mode, "XYZ") ? 3 : !strcmp(mode, "FitR") ? 5 : 2;
        if (!strcmp(mode, "XYZ") || !strcmp(mode, "FitH") ||
            !strcmp(mode, "FitBH") || !strcmp(mode, "FitR"))
          if (CGPDFArrayGetNumber(array, at, &value) && isfinite(value))
            position.y = value;
      }

      *point = CGPointApplyAffineTransform(position, transform(s, &pages[i]));
      return i;
    }
  }
  return SIZE_MAX;
}

/* Carry one source page's Link annotations into the output: URIs and
 * resolvable GoTo targets become real or collected destinations, everything
 * else is counted as dropped in report. */
static rmr_status links(CGContextRef cg, const rmr_apple_pdf_source* s,
                        const rmr_pdf_page* pages, size_t count, size_t index,
                        size_t* work, size_t maximum, rmr_pdf_report* report,
                        local_destinations* collect) {
  const rmr_pdf_page* p = &pages[index];
  CGPDFPageRef page = CGPDFDocumentGetPage(s->document, p->source_page + 1);
  CGPDFArrayRef annotations = nullptr;
  if (!CGPDFDictionaryGetArray(CGPDFPageGetDictionary(page), "Annots",
                               &annotations))
    return RMR_OK;

  size_t n = CGPDFArrayGetCount(annotations);
  if (n > maximum - *work) return RMR_ERR_LIMIT;
  *work += n;
  CGAffineTransform t = transform(s, p);

  for (size_t i = 0; i < n; i++) {
    CGPDFDictionaryRef a = nullptr;
    const char* subtype = nullptr;
    if (!CGPDFArrayGetDictionary(annotations, i, &a) ||
        !CGPDFDictionaryGetName(a, "Subtype", &subtype) ||
        strcmp(subtype, "Link"))
      continue;

    CGPDFArrayRef rectangle = nullptr;
    CGRect rect;
    if (!CGPDFDictionaryGetArray(a, "Rect", &rectangle) ||
        !array_rect(rectangle, &rect)) {
      report->links_dropped++;
      continue;
    }
    rect = CGRectApplyAffineTransform(rect, t);

    CGPDFDictionaryRef action = nullptr;
    const char* kind = nullptr;
    CGPDFObjectRef dest = nullptr;
    if (CGPDFDictionaryGetDictionary(a, "A", &action))
      CGPDFDictionaryGetName(action, "S", &kind);

    if (kind && !strcmp(kind, "URI")) {
      CGPDFStringRef uri = nullptr;
      if (CGPDFDictionaryGetString(action, "URI", &uri)) {
        CFStringRef string = CGPDFStringCopyTextString(uri);
        CFURLRef url =
            string ? CFURLCreateWithString(kCFAllocatorDefault, string, nullptr)
                   : nullptr;
        if (string) CFRelease(string);
        if (url) {
          if (cg) CGPDFContextSetURLForRect(cg, url, rect);
          CFRelease(url);
          report->links_preserved++;
          continue;
        }
      }
    } else if (!kind || !strcmp(kind, "GoTo")) {
      if (action) CGPDFDictionaryGetObject(action, "D", &dest);
      if (!dest) CGPDFDictionaryGetObject(a, "Dest", &dest);

      CGPoint point;
      size_t target =
          explicit_destination(s, dest, pages, count, work, maximum, &point);

      if (target != SIZE_MAX) {
        if (collect) {
          rmr_status status = destination_append(
              collect, (local_destination){target, index, i, point});
          if (status != RMR_OK) return status;
          continue;
        }

        CFStringRef name = local_name(index, i);
        if (!name) return RMR_ERR_OUT_OF_MEMORY;
        CGPDFContextSetDestinationForRect(cg, name, rect);
        CFRelease(name);
        report->links_preserved++;
        continue;
      }

      if (*work >= maximum) return RMR_ERR_LIMIT;
    }

    report->links_dropped++;
  }

  return RMR_OK;
}

/* Outline entries kept from a source PDF and how deep they may nest; a file
 * past either is refused with RMR_ERR_LIMIT, as the Cairo backend refuses
 * it. */
enum { OUTLINE_ENTRIES = 65536, OUTLINE_DEPTH = 128 };

/* Rebuild one level of the PDF outline tree into output, remapping each
 * item's page destination or splicing its children up when the destination
 * does not survive; *entries counts the items read. */
static rmr_status outline_children(const rmr_apple_pdf_source* s,
                                   CFArrayRef input, CFMutableArrayRef output,
                                   const rmr_pdf_page* pages, size_t count,
                                   size_t* work, size_t maximum,
                                   size_t* entries, unsigned depth) {
  if (depth > OUTLINE_DEPTH) return RMR_ERR_LIMIT;

  if (!input || CFGetTypeID(input) != CFArrayGetTypeID()) return RMR_OK;

  CFIndex n = CFArrayGetCount(input);
  for (CFIndex i = 0; i < n; i++) {
    if (*work >= maximum) return RMR_ERR_LIMIT;
    (*work)++;

    CFDictionaryRef old = CFArrayGetValueAtIndex(input, i);
    if (CFGetTypeID(old) != CFDictionaryGetTypeID()) continue;
    if (++*entries > OUTLINE_ENTRIES) return RMR_ERR_LIMIT;

    CFMutableArrayRef children =
        CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    if (!children) return RMR_ERR_OUT_OF_MEMORY;

    rmr_status status = outline_children(
        s, CFDictionaryGetValue(old, kCGPDFOutlineChildren), children, pages,
        count, work, maximum, entries, depth + 1);
    if (status != RMR_OK) {
      CFRelease(children);
      return status;
    }

    CFTypeRef target = CFDictionaryGetValue(old, kCGPDFOutlineDestination);
    size_t mapped = SIZE_MAX;
    CFNumberRef number = nullptr;

    if (target && CFGetTypeID(target) == CFNumberGetTypeID()) {
      int64_t original = 0;
      if (CFNumberGetValue(target, kCFNumberSInt64Type, &original) &&
          original > 0)
        for (size_t j = 0; j < count; j++) {
          if (*work >= maximum) {
            CFRelease(children);
            return RMR_ERR_LIMIT;
          }
          (*work)++;
          if (pages[j].has_source &&
              pages[j].source_page == (uint64_t)original - 1) {
            mapped = j;
            break;
          }
        }
      if (mapped != SIZE_MAX) {
        int64_t value = (int64_t)mapped + 1;
        number =
            CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &value);
      }
      target = number;
    } else if (!target || CFGetTypeID(target) != CFURLGetTypeID())
      target = nullptr;

    if (target) {
      CFMutableDictionaryRef item =
          CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, old);

      if (!item) {
        if (number) CFRelease(number);
        CFRelease(children);
        return RMR_ERR_OUT_OF_MEMORY;
      }

      CFDictionarySetValue(item, kCGPDFOutlineDestination, target);
      CFDictionaryRemoveValue(item, kCGPDFOutlineChildren);
      if (CFArrayGetCount(children))
        CFDictionarySetValue(item, kCGPDFOutlineChildren, children);

      CFDictionaryRef old_rect =
          CFDictionaryGetValue(old, kCGPDFOutlineDestinationRect);
      CGRect rect;
      if (mapped != SIZE_MAX && old_rect &&
          CFGetTypeID(old_rect) == CFDictionaryGetTypeID() &&
          CGRectMakeWithDictionaryRepresentation(old_rect, &rect)) {
        rect = CGRectApplyAffineTransform(rect, transform(s, &pages[mapped]));
        CFDictionaryRef new_rect = CGRectCreateDictionaryRepresentation(rect);
        if (new_rect) {
          CFDictionarySetValue(item, kCGPDFOutlineDestinationRect, new_rect);
          CFRelease(new_rect);
        }
      }

      CFArrayAppendValue(output, item);
      CFRelease(item);
    } else
      for (CFIndex j = 0; j < CFArrayGetCount(children); j++)
        CFArrayAppendValue(output, CFArrayGetValueAtIndex(children, j));

    if (number) CFRelease(number);
    CFRelease(children);
  }

  return RMR_OK;
}

/* The source's outline entries seen so far, hashed by dictionary. */
typedef struct {
  CGPDFDictionaryRef* slots;
  size_t capacity, count;
} seen_entries;

/* Records d, false when it was already there or (with *status set) memory
 * runs out. */
static bool first_sight(seen_entries* t, CGPDFDictionaryRef d,
                        rmr_status* status) {
  *status = RMR_OK;
  if (2 * (t->count + 1) > t->capacity) {
    size_t capacity = t->capacity ? 2 * t->capacity : 64;
    CGPDFDictionaryRef* slots = calloc(capacity, sizeof *slots);
    if (!slots) {
      *status = RMR_ERR_OUT_OF_MEMORY;
      return false;
    }
    for (size_t i = 0; i < t->capacity; i++)
      if (t->slots[i]) {
        size_t k =
            ((uint64_t)(uintptr_t)t->slots[i] * 0x9e3779b97f4a7c15u >> 32) &
            (capacity - 1);
        while (slots[k]) k = (k + 1) & (capacity - 1);
        slots[k] = t->slots[i];
      }
    free(t->slots);
    t->slots = slots;
    t->capacity = capacity;
  }
  size_t k =
      ((uint64_t)(uintptr_t)d * 0x9e3779b97f4a7c15u >> 32) & (t->capacity - 1);
  for (; t->slots[k]; k = (k + 1) & (t->capacity - 1))
    if (t->slots[k] == d) return false;
  t->slots[k] = d;
  t->count++;
  return true;
}

/* Whether CoreGraphics may be given the source's outline. It loops forever
 * on an outline whose entries link back to one another, so the links are
 * walked here first, as the Cairo backend walks them: depth first, an
 * entry's children before its next sibling, each charged to work against
 * maximum, and the first of these met decides. An entry met again drops the
 * outline (*usable false); an entry past OUTLINE_ENTRIES or nested deeper
 * than OUTLINE_DEPTH, or work past maximum, refuses the file with
 * RMR_ERR_LIMIT. */
static rmr_status outline_shape(CGPDFDocumentRef document, size_t maximum,
                                bool* usable) {
  size_t work = 0;
  *usable = false;
  CGPDFDictionaryRef catalog = CGPDFDocumentGetCatalog(document), root;
  if (!catalog || !CGPDFDictionaryGetDictionary(catalog, "Outlines", &root))
    return RMR_OK;
  typedef struct {
    CGPDFDictionaryRef entry;
    size_t depth;
  } pending;
  pending* stack = nullptr;
  size_t height = 0, room = 0;
  seen_entries seen = {0};
  rmr_status status = RMR_OK;
  bool cyclic = !first_sight(&seen, root, &status);
  CGPDFDictionaryRef first;
  if (status == RMR_OK && CGPDFDictionaryGetDictionary(root, "First", &first)) {
    stack = malloc(16 * sizeof *stack);
    room = 16;
    if (!stack)
      status = RMR_ERR_OUT_OF_MEMORY;
    else
      stack[height++] = (pending){first, 0};
  }
  while (status == RMR_OK && !cyclic && height) {
    pending p = stack[--height];
    if (++work > maximum) {
      status = RMR_ERR_LIMIT;
      break;
    }
    if (!first_sight(&seen, p.entry, &status)) {
      cyclic = status == RMR_OK;
      break;
    }
    if (seen.count - 1 > OUTLINE_ENTRIES || p.depth > OUTLINE_DEPTH) {
      status = RMR_ERR_LIMIT;
      break;
    }
    CGPDFDictionaryRef next, child;
    bool has_next = CGPDFDictionaryGetDictionary(p.entry, "Next", &next),
         has_child = CGPDFDictionaryGetDictionary(p.entry, "First", &child);
    if (height + 2 > room) {
      size_t more = 2 * room;
      pending* grown = realloc(stack, more * sizeof *stack);
      if (!grown) {
        status = RMR_ERR_OUT_OF_MEMORY;
        break;
      }
      stack = grown;
      room = more;
    }
    if (has_next) stack[height++] = (pending){next, p.depth};
    if (has_child) stack[height++] = (pending){child, p.depth + 1};
  }
  free(stack);
  free(seen.slots);
  *usable = status == RMR_OK && !cyclic;
  return status;
}

rmr_status rmr_apple_pdf_overlay_marks(
    const rmr_apple_pdf_source* source, const rmr_pdf_page* pages, size_t count,
    const rmr_pdf_marks* marks, const rmr_limits* given,
    const rmr_pdf_options* options, rmr_cancel_fn cancel, void* context,
    const rmr_output* output, rmr_buffer* out, rmr_pdf_report* report,
    rmr_error* e) {
  if (!pages || !count || (!out && !output))
    return fail(e, RMR_ERR_INVALID_ARGUMENT, "missing PDF output pages");
  /* Grain masks go straight onto the page either way. */
  rmr_pdf_options checked;
  if (!rmr_pdf_options_read(options, &checked))
    return fail(e, RMR_ERR_INVALID_ARGUMENT, "invalid PDF options");

  rmr_limits l = given ? *given : rmr_limits_default();
  if (count > l.maximum_draws || count > INT64_MAX)
    return fail(e, RMR_ERR_LIMIT, "PDF page count limit");

  for (size_t i = 0; i < count; i++) {
    const rmr_pdf_page* p = &pages[i];
    rmr_transform t = p->sheet.transform;
    if ((!marks && !p->marks) || !isfinite(p->sheet.width) ||
        !isfinite(p->sheet.height) || p->sheet.width <= 0 ||
        p->sheet.height <= 0 || p->sheet.width > 10000000 ||
        p->sheet.height > 10000000 || !isfinite(t.a) || t.a <= 0 || t.b != 0 ||
        t.c != 0 || t.d != -t.a || !isfinite(t.tx) || !isfinite(t.ty))
      return fail(e, RMR_ERR_INVALID_ARGUMENT, "invalid output PDF sheet");
    if (p->has_source) {
      rmr_rect r = p->source_rect;
      if (!source || p->source_page >= rmr_apple_pdf_source_count(source) ||
          !isfinite(r.x) || !isfinite(r.y) || !isfinite(r.width) ||
          !isfinite(r.height) || r.width <= 0 || r.height <= 0 ||
          fabs(r.x) > l.maximum_coordinate ||
          fabs(r.x + r.width) > l.maximum_coordinate ||
          fabs(r.y) > l.maximum_coordinate ||
          fabs(r.y + r.height) > l.maximum_coordinate)
        return fail(e, RMR_ERR_INVALID_ARGUMENT,
                    "invalid PDF source placement");
    }
  }

  local_destinations targets = {.maximum = l.maximum_plan_bytes /
                                           sizeof(local_destination)};
  if (targets.maximum > l.maximum_elements)
    targets.maximum = l.maximum_elements;
  size_t work = 0;
  for (size_t i = 0; source && i < count; i++)
    if (pages[i].has_source && pages[i].source_visible) {
      if (cancel && cancel(context)) {
        free(targets.items);
        return fail(e, RMR_ERR_CANCELLED, "PDF collection cancelled");
      }
      rmr_pdf_report ignored = {0};
      rmr_status collected = links(nullptr, source, pages, count, i, &work,
                                   l.maximum_elements, &ignored, &targets);
      if (collected != RMR_OK) {
        free(targets.items);
        return fail(e, collected, "PDF destination collection failed");
      }
    }

  /* The outline is checked before anything is written, as on Cairo. */
  bool usable = false;
  if (source) {
    rmr_status shape =
        outline_shape(source->document, l.maximum_elements, &usable);
    if (shape != RMR_OK) {
      free(targets.items);
      return fail(e, shape,
                  shape == RMR_ERR_LIMIT ? "PDF outline limit"
                                         : "PDF outline allocation failed");
    }
  }

  if (targets.count > 1)
    qsort(targets.items, targets.count, sizeof *targets.items,
          destination_order);
  size_t next_target = 0;
  /* Pages whose marks the writer can serialize get them from it, merged into
   * CoreGraphics' output once it is closed; CoreGraphics draws the rest. */
  rmr_apple_writer* writer = nullptr;
  rmr_status created =
      rmr_apple_writer_create(&l, checked.decimals, true, &writer);
  if (created != RMR_OK) {
    free(targets.items);
    /* A limit below the file header is the limit, as on Cairo. */
    return fail(e, created,
                created == RMR_ERR_LIMIT ? "PDF output byte limit"
                                         : "PDF sink allocation failed");
  }
  sink drawn = {.limit = l.maximum_pdf_bytes};
  CGDataConsumerCallbacks callbacks = {put, nullptr};
  CGDataConsumerRef consumer = CGDataConsumerCreate(&drawn, &callbacks);
  if (!consumer) {
    rmr_apple_writer_free(writer);
    free(targets.items);
    return fail(e, RMR_ERR_OUT_OF_MEMORY, "PDF sink allocation failed");
  }

  CGContextRef cg = CGPDFContextCreate(consumer, nullptr, nullptr);
  CGDataConsumerRelease(consumer);
  if (!cg) {
    rmr_apple_writer_free(writer);
    free(targets.items);
    free(drawn.bytes.bytes);
    return fail(e, RMR_ERR_BACKEND, "PDF context unavailable");
  }
  /* The writer's pages go to output as they are deflated, under the header
   * CoreGraphics has just written; its own output is merged after them. */
  rmr_output_guard guard = {0}; /* output, closed once the export fails */
  if (output) {
    rmr_output_guard_init(&guard, output);
    rmr_apple_writer_output(writer, &guard.output, drawn.bytes.bytes,
                            drawn.bytes.size);
  }

  rmr_status status = RMR_OK;
  bool reported = false; /* set once a marks or drawing call has filled e */
  bool written = false;  /* set once writing the output has failed */
  size_t completed = 0;  /* settled pages already reported */
  rmr_pdf_report result = {0};
  for (size_t i = 0; i < count; i++) {
    if (cancel && cancel(context)) {
      status = RMR_ERR_CANCELLED;
      break;
    }

    const rmr_pdf_page* p = &pages[i];
    CGRect box = CGRectMake(0, 0, p->sheet.width, p->sheet.height);
    rmr_transform placement = p->sheet.transform;
    CGRect art = CGRectApplyAffineTransform(
        CGRectMake(p->source_rect.x, p->source_rect.y, p->source_rect.width,
                   p->source_rect.height),
        CGAffineTransformMake(placement.a, placement.b, placement.c,
                              placement.d, placement.tx, placement.ty));
    if (CGRectIsEmpty(art)) art = box;

    CFDataRef data =
        CFDataCreate(kCFAllocatorDefault, (const UInt8*)&box, sizeof box);
    CFDataRef art_data =
        CFDataCreate(kCFAllocatorDefault, (const UInt8*)&art, sizeof art);
    CGRect crop = box;
    if (source && p->has_source && p->source_visible) {
      CGPDFPageRef original =
          CGPDFDocumentGetPage(source->document, p->source_page + 1);
      CGRect original_crop = CGPDFPageGetBoxRect(original, kCGPDFCropBox);
      if (!CGRectEqualToRect(original_crop,
                             CGPDFPageGetBoxRect(original, kCGPDFMediaBox)))
        crop = CGRectApplyAffineTransform(original_crop, transform(source, p));
    }
    CFDataRef crop_data =
        CFDataCreate(kCFAllocatorDefault, (const UInt8*)&crop, sizeof crop);

    const void* keys[] = {kCGPDFContextMediaBox, kCGPDFContextArtBox,
                          kCGPDFContextCropBox};
    const void* values[] = {data, art_data, crop_data};
    CFDictionaryRef info =
        data && art_data && crop_data
            ? CFDictionaryCreate(kCFAllocatorDefault, keys, values, 3,
                                 &kCFTypeDictionaryKeyCallBacks,
                                 &kCFTypeDictionaryValueCallBacks)
            : nullptr;

    if (data) CFRelease(data);
    if (art_data) CFRelease(art_data);
    if (crop_data) CFRelease(crop_data);
    if (!info) {
      status = RMR_ERR_OUT_OF_MEMORY;
      break;
    }

    CGPDFContextBeginPage(cg, info);
    CFRelease(info);
    CFStringRef name = destination_name(i);
    if (!name)
      status = RMR_ERR_OUT_OF_MEMORY;
    else {
      CGPDFContextAddDestinationAtPoint(cg, name,
                                        CGPointMake(0, p->sheet.height));
      CFRelease(name);
    }

    while (status == RMR_OK && next_target < targets.count &&
           targets.items[next_target].page == i) {
      local_destination item = targets.items[next_target++];
      CFStringRef target_name = local_name(item.origin, item.annotation);
      if (!target_name)
        status = RMR_ERR_OUT_OF_MEMORY;
      else {
        CGPDFContextAddDestinationAtPoint(cg, target_name, item.point);
        CFRelease(target_name);
      }
    }

    if (status == RMR_OK && p->has_source && p->source_visible) {
      CGContextSaveGState(cg);
      CGContextConcatCTM(cg, transform(source, p));
      CGContextDrawPDFPage(
          cg, CGPDFDocumentGetPage(source->document, p->source_page + 1));
      CGContextRestoreGState(cg);
      status = links(cg, source, pages, count, i, &work, l.maximum_elements,
                     &result, nullptr);
    }

    const rmr_plan* plan = nullptr;
    if (status == RMR_OK) {
      status = rmr_pdf_marks_acquire(marks, pages, i, &plan, e);
      reported = status != RMR_OK;
    }
    if (status == RMR_OK) {
      /* CoreGraphics draws what only it can, up to the last such draw; the
       * writer draws the rest after it, and the page's template up to the
       * first such draw before it. */
      size_t split = rmr_apple_writer_split(plan), all = rmr_plan_count(plan);
      size_t lead = split ? rmr_apple_writer_lead(plan) : 0;
      if (split || !all)
        status = rmr_apple_draw_context_range(cg, plan, lead, split, &p->sheet,
                                              &l, rmr_apple_writer_mask, writer,
                                              cancel, context, e);
      if (status == RMR_OK && (lead || split < all))
        status = rmr_apple_writer_page(writer, i, plan, lead, split, &p->sheet,
                                       cancel, context, e);
      reported = status != RMR_OK;
      rmr_pdf_marks_release(marks, i);
    }
    if (status == RMR_OK) {
      status = rmr_apple_writer_flush(writer, false);
      written = status != RMR_OK;
    }

    CGPDFContextEndPage(cg);
    if (status != RMR_OK || drawn.status != RMR_OK) break;
    if (marks && marks->progress)
      rmr_pdf_marks_progress_until(
          marks, &completed, rmr_apple_writer_watermark(writer, i + 1), count);
  }

  /* The writer may still be deflating pages before the failing one: a
   * failure writing them is earlier in the file and is the one reported,
   * whichever thread got there first. They are finished with the output
   * closed, as the export fails either way. A cancellation stops at once. */
  if (status != RMR_OK && status != RMR_ERR_CANCELLED && !written) {
    guard.closed = true;
    rmr_status first = rmr_apple_writer_flush(writer, true);
    if (first != RMR_OK) {
      status = first;
      written = true;
      reported = false;
    }
  }

  if (status == RMR_OK && drawn.status == RMR_OK && marks && marks->progress) {
    status = rmr_apple_writer_flush(writer, true);
    written = status != RMR_OK;
    if (status == RMR_OK)
      rmr_pdf_marks_progress_until(marks, &completed, count, count);
  }

  if (status == RMR_OK && source && usable) {
    CFDictionaryRef old = CGPDFDocumentGetOutline(source->document);
    if (old) {
      CFMutableArrayRef children =
          CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
      if (!children)
        status = RMR_ERR_OUT_OF_MEMORY;
      else {
        size_t entries = 0;
        status = outline_children(
            source, CFDictionaryGetValue(old, kCGPDFOutlineChildren), children,
            pages, count, &work, l.maximum_elements, &entries, 0);
        /* Past the outline's own bounds rather than the work limit. */
        if (status == RMR_ERR_LIMIT &&
            (entries > OUTLINE_ENTRIES || work < l.maximum_elements)) {
          fail(e, status, "PDF outline limit");
          reported = true;
        }
        const void *keys[] = {kCGPDFOutlineChildren}, *values[] = {children};
        CFDictionaryRef root = CFDictionaryCreate(
            kCFAllocatorDefault, keys, values, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (!root)
          status = RMR_ERR_OUT_OF_MEMORY;
        else {
          if (status == RMR_OK) CGPDFContextSetOutline(cg, root);
          CFRelease(root);
        }
        CFRelease(children);
      }
    }
  }

  CGPDFContextClose(cg);
  CGContextRelease(cg);
  free(targets.items);

  if (status == RMR_OK && drawn.status != RMR_OK) {
    status = drawn.status;
    written = true;
  }
  if (status == RMR_OK && rmr_apple_writer_pages(writer)) {
    rmr_buffer merged = {0};
    status = rmr_apple_writer_merge(writer, drawn.bytes.bytes, drawn.bytes.size,
                                    &merged);
    written = status != RMR_OK;
    free(drawn.bytes.bytes);
    drawn.bytes = merged;
  } else if (status == RMR_OK && output) {
    /* CoreGraphics drew every page: its output is the file. */
    status = guard.output.write(&guard, drawn.bytes.bytes, drawn.bytes.size);
    written = status != RMR_OK;
  }
  /* A failure writing the file is named as on Cairo: the caller's output
   * refusing it, else the document's own report of its cause, else
   * CoreGraphics' share passing maximum_pdf_bytes. */
  char cause[sizeof e->message] = "PDF overlay failed";
  const char* document = rmr_apple_writer_failure(writer);
  if (written)
    snprintf(cause, sizeof cause, "%s",
             output && guard.status != RMR_OK ? "PDF output write failed"
             : document                       ? document
             : status == RMR_ERR_LIMIT        ? "PDF output byte limit"
                                              : "PDF overlay failed");
  rmr_apple_writer_free(writer);

  if (status != RMR_OK) {
    free(drawn.bytes.bytes);
    return reported ? status : fail(e, status, cause);
  }

  if (output)
    free(drawn.bytes.bytes);
  else
    *out = drawn.bytes;
  if (report) *report = result;
  return RMR_OK;
}

rmr_status rmr_apple_pdf_overlay(const rmr_apple_pdf_source* source,
                                 const rmr_pdf_page* pages, size_t count,
                                 const rmr_limits* limits, rmr_cancel_fn cancel,
                                 void* context, rmr_buffer* out,
                                 rmr_pdf_report* report, rmr_error* e) {
  return rmr_apple_pdf_overlay_marks(source, pages, count, nullptr, limits,
                                     nullptr, cancel, context, nullptr, out,
                                     report, e);
}

rmr_status rmr_apple_pdf_overlay_with_options(
    const rmr_apple_pdf_source* source, const rmr_pdf_page* pages, size_t count,
    const rmr_limits* limits, const rmr_pdf_options* options,
    rmr_cancel_fn cancel, void* context, rmr_buffer* out,
    rmr_pdf_report* report, rmr_error* e) {
  return rmr_apple_pdf_overlay_marks(source, pages, count, nullptr, limits,
                                     options, cancel, context, nullptr, out,
                                     report, e);
}

/* The incremental writer edits the page tree it parses itself, while sizes and
 * transforms come from CoreGraphics. Both readers must see the same pages. */
static rmr_status agree(const rmr_apple_pdf_source* source,
                        rmr_pdf_page_geometry* geometry, size_t count,
                        const rmr_limits* limits, rmr_error* e) {
  size_t parsed = 0;
  rmr_status status = rmr_pdf_pages(CFDataGetBytePtr(source->data),
                                    (size_t)CFDataGetLength(source->data),
                                    limits, geometry, count, &parsed, e);
  if (status != RMR_OK) return status;

  if (parsed != count)
    return fail(e, RMR_ERR_UNSUPPORTED,
                "PDF readers disagree on the page count");

  for (size_t i = 0; i < count; i++) {
    CGPDFPageRef page = CGPDFDocumentGetPage(source->document, i + 1);
    int rotation = CGPDFPageGetRotationAngle(page) % 360;
    if ((rotation < 0 ? rotation + 360 : rotation) != geometry[i].rotate)
      return fail(e, RMR_ERR_UNSUPPORTED,
                  "PDF readers disagree on a page rotation");
    if (!geometry[i].has_media) continue;
    CGRect media = CGPDFPageGetBoxRect(page, kCGPDFMediaBox);
    rmr_rect expected = geometry[i].media;
    if (fabs(CGRectGetMinX(media) - expected.x) > 0.01 ||
        fabs(CGRectGetMinY(media) - expected.y) > 0.01 ||
        fabs(CGRectGetWidth(media) - expected.width) > 0.01 ||
        fabs(CGRectGetHeight(media) - expected.height) > 0.01)
      return fail(e, RMR_ERR_UNSUPPORTED,
                  "PDF readers disagree on a page media box");
  }

  return RMR_OK;
}

/* A page of an update whose overlay is being made: CoreGraphics' share, drawn
 * at once (empty when it has none), and whether the writer's share is queued
 * behind it. */
typedef struct {
  rmr_pdf_overlay overlay;
  rmr_buffer native;
  bool written;
} pending_page;

/* Pages pending in order, at most window + 1 of them, the oldest at head. */
typedef struct {
  pending_page* items;
  size_t head, count, capacity;
} pending_pages;

enum { PENDING_WINDOW = 16 };

/* Hands the oldest pending pages to the update, each once its writer's share
 * is back: every page when all is set, otherwise while more than the window
 * are pending and then those already back. Failures report as
 * rmr_apple_pdf_sheets reports them. */
static rmr_status settle(pending_pages* q, rmr_apple_writer* writer,
                         rmr_pdf_update* update, bool all, rmr_error* e) {
  while (q->count) {
    pending_page* item = &q->items[q->head];
    rmr_buffer written = {0};
    if (item->written) {
      bool wait = all || q->count > PENDING_WINDOW;
      size_t tag = 0;
      rmr_status status = RMR_OK;
      if (!rmr_apple_writer_next_overlay(writer, wait, &written, &tag,
                                         &status)) {
        if (status == RMR_OK && !wait) return RMR_OK;
        return fail(e, status == RMR_OK ? RMR_ERR_BACKEND : status,
                    "PDF output memory limit or allocation failure");
      }
      if (tag != item->overlay.page) {
        rmr_buffer_free(&written);
        return fail(e, RMR_ERR_BACKEND, "PDF overlay out of order");
      }
    }

    rmr_pdf_overlay parts[2];
    size_t count = 0;
    if (item->native.size) {
      parts[count] = item->overlay;
      parts[count].pdf = item->native.bytes;
      parts[count++].size = item->native.size;
    }
    if (written.size) {
      parts[count] = item->overlay;
      parts[count].pdf = written.bytes;
      parts[count++].size = written.size;
    }
    rmr_status status =
        count ? rmr_pdf_update_page(update, parts, count, e) : RMR_OK;
    rmr_buffer_free(&written);
    rmr_buffer_free(&item->native);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    if (status != RMR_OK) return status;
  }
  return RMR_OK;
}

rmr_status rmr_apple_pdf_update_marks(
    const rmr_apple_pdf_source* source, const rmr_pdf_page* pages, size_t count,
    const rmr_pdf_marks* marks, const rmr_limits* given,
    const rmr_pdf_options* options, rmr_cancel_fn cancel, void* context,
    bool fresh, const rmr_output* output, rmr_buffer* out,
    rmr_pdf_incremental_report* report, rmr_error* e) {
  if (!source || !pages || (!out && !output) || !count)
    return fail(e, RMR_ERR_INVALID_ARGUMENT, "missing incremental pages");
  rmr_pdf_options checked;
  if (!rmr_pdf_options_read(options, &checked))
    return fail(e, RMR_ERR_INVALID_ARGUMENT, "invalid PDF options");

  rmr_limits limits = given ? *given : rmr_limits_default();
  if (count != rmr_apple_pdf_source_count(source))
    return fail(e, RMR_ERR_UNSUPPORTED,
                "incremental output preserves page count/order");

  if (count > limits.maximum_draws ||
      count > limits.maximum_plan_bytes / sizeof(rmr_pdf_page_geometry))
    return fail(e, RMR_ERR_LIMIT, "incremental page limit");

  rmr_pdf_page_geometry* geometry = calloc(count, sizeof *geometry);
  pending_pages queue = {.capacity = PENDING_WINDOW + 2};
  queue.items = calloc(queue.capacity, sizeof *queue.items);
  if (!geometry || !queue.items) {
    free(geometry);
    free(queue.items);
    return fail(e, RMR_ERR_OUT_OF_MEMORY, "incremental page allocation");
  }

  limits.maximum_plan_bytes -= count * sizeof *geometry;
  rmr_status status = agree(source, geometry, count, &limits, e);
  /* Each page's overlay goes into the update once made, so that at most a
   * window of them is held however long the document. Of a page's marks,
   * CoreGraphics draws what only it can, up to the last such draw that
   * paints, and the writer the rest, in order, on its own page over
   * CoreGraphics'; a page whose text layer has no glyphs is the writer's
   * alone. */
  rmr_apple_writer* writer = nullptr;
  rmr_pdf_update* update = nullptr;
  bool settled = false;         /* set once settling the queue has failed */
  size_t completed = 0;         /* settled pages already reported */
  rmr_output_guard guard = {0}; /* output, closed once the export fails */
  if (output) rmr_output_guard_init(&guard, output);
  if (status == RMR_OK && rmr_apple_writer_create(&limits, checked.decimals,
                                                  false, &writer) != RMR_OK)
    status = fail(e, RMR_ERR_OUT_OF_MEMORY, "incremental page allocation");
  if (status == RMR_OK)
    status = rmr_pdf_update_create(
        CFDataGetBytePtr(source->data), (size_t)CFDataGetLength(source->data),
        fresh, &limits, cancel, context, output ? &guard.output : nullptr,
        &update, e);

  for (size_t i = 0; status == RMR_OK && i < count; i++) {
    const rmr_pdf_page* p = &pages[i];
    if (cancel && cancel(context)) {
      status = fail(e, RMR_ERR_CANCELLED, "PDF update cancelled");
      break;
    }

    if (!p->has_source || !p->source_visible || p->source_page != i) {
      status = fail(
          e, RMR_ERR_UNSUPPORTED,
          "incremental output cannot hide, insert, reorder or remove pages");
      break;
    }

    rmr_rect r = p->source_rect;
    if ((!marks && !p->marks) || !isfinite(r.x) || !isfinite(r.y) ||
        !isfinite(r.width) || !isfinite(r.height) || r.width <= 0 ||
        r.height <= 0) {
      status = fail(e, RMR_ERR_INVALID_ARGUMENT, "invalid source placement");
      break;
    }

    const rmr_plan* plan = nullptr;
    status = rmr_pdf_marks_acquire(marks, pages, i, &plan, e);
    if (status != RMR_OK) break;
    if (!rmr_plan_count(plan)) {
      rmr_pdf_marks_release(marks, i);
      rmr_pdf_marks_progress_until(
          marks, &completed,
          queue.count ? queue.items[queue.head].overlay.page : i + 1, count);
      continue;
    }

    CGPDFPageRef original = CGPDFDocumentGetPage(source->document, i + 1);
    CGAffineTransform t = transform(source, p);
    double determinant = t.a * t.d - t.b * t.c;
    if (!isfinite(determinant) || fabs(determinant) < 1e-12) {
      rmr_pdf_marks_release(marks, i);
      status = fail(e, RMR_ERR_INVALID_ARGUMENT, "singular PDF placement");
      break;
    }

    t = CGAffineTransformInvert(t);
    CGRect box = CGRectApplyAffineTransform(
        CGRectMake(0, 0, p->sheet.width, p->sheet.height), t);
    CGRect media = CGPDFPageGetBoxRect(original, kCGPDFMediaBox);
    bool expand = CGRectGetMinX(box) < CGRectGetMinX(media) - 0.01 ||
                  CGRectGetMinY(box) < CGRectGetMinY(media) - 0.01 ||
                  CGRectGetMaxX(box) > CGRectGetMaxX(media) + 0.01 ||
                  CGRectGetMaxY(box) > CGRectGetMaxY(media) + 0.01;
    box = CGRectUnion(box, media);

    pending_page* item =
        &queue.items[(queue.head + queue.count) % queue.capacity];
    *item = (pending_page){
        .overlay = {.page = i,
                    .transform = {t.a, t.b, t.c, t.d, t.tx, t.ty},
                    .replace_box = expand,
                    .box = {box.origin.x, box.origin.y, box.size.width,
                            box.size.height}}};
    size_t split = rmr_apple_writer_split(plan), all = rmr_plan_count(plan);
    if (split)
      status = rmr_apple_pdf_sheet_range(
          plan, 0, split, &p->sheet, &limits,
          split < all ? rmr_apple_writer_mask : nullptr, writer, cancel,
          context, &item->native, e);
    if (status == RMR_OK && split < all) {
      status = rmr_apple_writer_overlay(writer, i, plan, split, &p->sheet,
                                        cancel, context, e);
      item->written = status == RMR_OK;
    }
    rmr_pdf_marks_release(marks, i);
    if (status != RMR_OK) {
      rmr_buffer_free(&item->native);
      break;
    }
    queue.count++;
    status = settle(&queue, writer, update, false, e);
    settled = status != RMR_OK;
    if (status == RMR_OK)
      rmr_pdf_marks_progress_until(
          marks, &completed,
          queue.count ? queue.items[queue.head].overlay.page : i + 1, count);
  }

  /* Pages before the failing one may still be queued: a failure adding them
   * to the update is earlier in the file and is the one reported, whichever
   * thread finished first. They are added with the output closed, as the
   * export fails either way. A cancellation stops at once. */
  if (status != RMR_OK && status != RMR_ERR_CANCELLED && !settled && update) {
    rmr_error earlier = {0};
    guard.closed = true;
    rmr_status first = settle(&queue, writer, update, true, &earlier);
    if (first != RMR_OK) {
      status = first;
      if (e) *e = earlier;
    }
  }
  if (status == RMR_OK) {
    status = settle(&queue, writer, update, true, e);
    if (status == RMR_OK)
      rmr_pdf_marks_progress_until(marks, &completed, count, count);
  }
  for (; queue.count; queue.count--) {
    rmr_buffer_free(&queue.items[queue.head].native);
    queue.head = (queue.head + 1) % queue.capacity;
  }
  rmr_apple_writer_free(writer);

  if (status == RMR_OK) status = rmr_pdf_update_finish(update, out, report, e);
  rmr_pdf_update_free(update);
  free(queue.items);
  free(geometry);
  return status;
}

rmr_status rmr_apple_pdf_incremental_marks(
    const rmr_apple_pdf_source* source, const rmr_pdf_page* pages, size_t count,
    const rmr_pdf_marks* marks, const rmr_limits* limits,
    const rmr_pdf_options* options, rmr_cancel_fn cancel, void* context,
    const rmr_output* output, rmr_buffer* out,
    rmr_pdf_incremental_report* report, rmr_error* e) {
  return rmr_apple_pdf_update_marks(source, pages, count, marks, limits,
                                    options, cancel, context, false, output,
                                    out, report, e);
}

rmr_status rmr_apple_pdf_incremental(
    const rmr_apple_pdf_source* source, const rmr_pdf_page* pages, size_t count,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* context,
    rmr_buffer* out, rmr_pdf_incremental_report* report, rmr_error* e) {
  return rmr_apple_pdf_incremental_marks(source, pages, count, nullptr, limits,
                                         nullptr, cancel, context, nullptr, out,
                                         report, e);
}

rmr_status rmr_apple_pdf_incremental_with_options(
    const rmr_apple_pdf_source* source, const rmr_pdf_page* pages, size_t count,
    const rmr_limits* limits, const rmr_pdf_options* options,
    rmr_cancel_fn cancel, void* context, rmr_buffer* out,
    rmr_pdf_incremental_report* report, rmr_error* e) {
  return rmr_apple_pdf_incremental_marks(source, pages, count, nullptr, limits,
                                         options, cancel, context, nullptr, out,
                                         report, e);
}

const uint8_t* rmr_apple_pdf_source_data(const rmr_apple_pdf_source* s) {
  return CFDataGetBytePtr(s->data);
}

size_t rmr_apple_pdf_source_length(const rmr_apple_pdf_source* s) {
  return (size_t)CFDataGetLength(s->data);
}

rmr_status rmr_apple_pdf_fail(rmr_error* e, rmr_status s, const char* message) {
  return fail(e, s, message);
}
