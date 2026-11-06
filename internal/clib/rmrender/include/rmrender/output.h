/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_OUTPUT_H
#define RMR_OUTPUT_H
#include "backend.h"

typedef struct {
  uint8_t* pixels;
  size_t width, height, stride;
} rmr_bitmap;

/* Premultiplied RGBA8, tightly packed, first row at top. */
/* Frees pixels and zeroes bitmap; a null bitmap is a no-op. */
RMR_API void rmr_bitmap_free(rmr_bitmap* bitmap);

/* How PDF output draws grained ink (pencils, brushes, ballpoints): its 1-bit
 * grain is a stencil mask clipped to the stroke's outline. MASK draws opaque
 * ink's stencil straight onto the page, half the bytes of GROUP; GROUP wraps
 * each stroke in a transparency group of its own, as rmrender 1.4 did on
 * Cairo, where viewers antialias the clipped edge twice and draw it lighter.
 * Translucent ink is grouped either way. The Apple backend always draws the
 * MASK way; it accepts both values. */
typedef enum rmr_pdf_grain : uint32_t {
  RMR_PDF_GRAIN_MASK,
  RMR_PDF_GRAIN_GROUP
} rmr_pdf_grain;

/* PDF drawing options for the *_with_options entry points. Start from
 * rmr_pdf_options_default(), which fills abi_version and struct_size, and
 * change the fields needed. A null options pointer selects the defaults. A
 * newer library reads only the fields within struct_size and gives fields
 * added after them their default; a wrong abi_version, a struct_size smaller
 * than this first version or an unknown value returns INVALID_ARGUMENT. */
#define RMR_PDF_OPTIONS_ABI 1u
typedef struct {
  uint32_t abi_version;
  size_t struct_size;
  rmr_pdf_grain grain;
} rmr_pdf_options;

/* {RMR_PDF_OPTIONS_ABI, sizeof(rmr_pdf_options), RMR_PDF_GRAIN_MASK}. */
RMR_API rmr_pdf_options rmr_pdf_options_default(void);

#endif
