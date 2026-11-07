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
  /* Read when struct_size covers it. 0, the default, writes ink paths at full
   * precision, exactly as CoreGraphics would. 1 to 6 round the Apple backend's
   * ink path coordinates to that many decimal places of a point (stroke
   * coordinates, in page units, to as many places, which is finer): 2 is
   * invisible at print resolution and makes ink-heavy files about a quarter
   * smaller. Pages holding text, images, embedded PDFs or transparency groups
   * are drawn by CoreGraphics at full precision; the Cairo backend ignores this
   * field. */
  uint32_t decimals;
} rmr_pdf_options;

/* {RMR_PDF_OPTIONS_ABI, sizeof(rmr_pdf_options), RMR_PDF_GRAIN_MASK, 0}. */
RMR_API rmr_pdf_options rmr_pdf_options_default(void);

/* Where the *_to_output entry points write a PDF: write receives the file's
 * bytes in order, on the calling thread, as the export completes them, never
 * with size 0; bytes are borrowed for the call only. It returns RMR_OK to go
 * on; any other status stops the export, which returns that status with the
 * message "PDF output write failed" and never calls write again. write must
 * not call back into rmrender. Set abi_version to RMR_OUTPUT_ABI and
 * struct_size to sizeof(rmr_output); a wrong abi_version, a struct_size
 * smaller than this first version or a null write returns INVALID_ARGUMENT
 * before anything is written.
 *
 * The file is the one the buffer entry points return, byte for byte, but what
 * has been written no longer takes memory. On any failure, cancellation
 * included, write has received part of a file, which the caller discards
 * (write to a temporary file, renamed once the export succeeds).
 * maximum_pdf_bytes still bounds the whole file as it bounds a buffer, and the
 * inputs and intermediates it bounds stay bounded by it: raise it to write
 * larger files. */
#define RMR_OUTPUT_ABI 1u
typedef struct {
  uint32_t abi_version;
  size_t struct_size;
  rmr_status (*write)(void* context, const uint8_t* bytes, size_t size);
  void* context;
} rmr_output;

#endif
