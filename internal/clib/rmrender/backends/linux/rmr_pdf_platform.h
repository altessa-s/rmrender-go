/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* What backends/common/rmr_pdf_recompose.c needs from the Linux backend: the
 * names of its PDF entry points and access to an opened source's bytes. */

#ifndef RMR_PDF_PLATFORM_H
#define RMR_PDF_PLATFORM_H
#include <rmrender/linux_pdf.h>

#include "rmr_linux_internal.h"

#define RMR_PDF_NATIVE(name) rmr_linux_##name
#define RMR_PDF_BYTES rmr_linux_pdf_source_data
#define RMR_PDF_SIZE rmr_linux_pdf_source_length
#define RMR_PDF_FAIL rmr_linux_error

/* The source PDF's bytes, borrowed for the life of the source. */
const uint8_t* rmr_linux_pdf_source_data(const rmr_linux_pdf_source* s);

/* Length in bytes of the source PDF data returned by
 * rmr_linux_pdf_source_data. */
size_t rmr_linux_pdf_source_length(const rmr_linux_pdf_source* s);

#endif
