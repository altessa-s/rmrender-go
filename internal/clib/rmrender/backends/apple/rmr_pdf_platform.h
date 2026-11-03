/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* What backends/common/rmr_pdf_recompose.c needs from the Apple backend: the
 * names of its PDF entry points and access to an opened source's bytes. */

#ifndef RMR_PDF_PLATFORM_H
#define RMR_PDF_PLATFORM_H
#include <rmrender/apple_pdf.h>

#define RMR_PDF_NATIVE(name) rmr_apple_##name
#define RMR_PDF_BYTES rmr_apple_pdf_source_data
#define RMR_PDF_SIZE rmr_apple_pdf_source_length
#define RMR_PDF_FAIL rmr_apple_pdf_fail

/* The source PDF's bytes, borrowed for the life of the source. */
const uint8_t* rmr_apple_pdf_source_data(const rmr_apple_pdf_source* s);
size_t rmr_apple_pdf_source_length(const rmr_apple_pdf_source* s);

/* Fill `e` with `s` and `message`. */
rmr_status rmr_apple_pdf_fail(rmr_error* e, rmr_status s, const char* message);

#endif
