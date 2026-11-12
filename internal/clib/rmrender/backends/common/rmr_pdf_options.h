/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Reading the caller's rmr_pdf_options, shared by the Apple and Cairo PDF
 * entry points. */

#ifndef RMR_PDF_OPTIONS_H
#define RMR_PDF_OPTIONS_H
#include <rmrender/output.h>
#include <stddef.h>

/* The options given (the defaults when null) into *out: fields beyond the
 * caller's struct_size keep their default. False for a wrong abi_version, a
 * struct_size short of the first version or an unknown value. */
static inline bool rmr_pdf_options_read(const rmr_pdf_options* given,
                                        rmr_pdf_options* out) {
  *out = rmr_pdf_options_default();
  if (!given) return true;
  if (given->abi_version != RMR_PDF_OPTIONS_ABI ||
      given->struct_size <
          offsetof(rmr_pdf_options, grain) + sizeof given->grain ||
      given->grain > RMR_PDF_GRAIN_GROUP)
    return false;
  out->grain = given->grain;
  if (given->struct_size >=
      offsetof(rmr_pdf_options, decimals) + sizeof given->decimals) {
    if (given->decimals > 6) return false;
    out->decimals = given->decimals;
  }
  if (given->struct_size >=
      offsetof(rmr_pdf_options, unreadable) + sizeof given->unreadable) {
    if (given->unreadable > RMR_UNREADABLE_PLACEHOLDER) return false;
    out->unreadable = given->unreadable;
  }
  if (given->struct_size >= offsetof(rmr_pdf_options, unreadable_context) +
                                sizeof given->unreadable_context) {
    out->on_unreadable = given->on_unreadable;
    out->unreadable_context = given->unreadable_context;
  }
  return true;
}

#endif
