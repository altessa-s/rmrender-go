/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMRENDER_EXPORT_H
#define RMRENDER_EXPORT_H
#if defined(__GNUC__) || defined(__clang__)
#define RMR_API __attribute__((visibility("default")))
#else
#define RMR_API
#endif

#endif
