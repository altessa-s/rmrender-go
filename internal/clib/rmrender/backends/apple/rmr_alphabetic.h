/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Unicode scalar properties, generated into rmr_alphabetic.c by
 * scripts/generate-unicode.swift. */

#ifndef RMR_ALPHABETIC_H
#define RMR_ALPHABETIC_H

#include <stddef.h>
#include <stdint.h>

/* Whether the scalar has the Alphabetic property. */
bool rmr_alphabetic(uint32_t c);

/* Whether the scalar is a nonspacing mark (Mn). */
bool rmr_nonspacing(uint32_t c);

#endif
