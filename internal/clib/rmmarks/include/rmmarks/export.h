/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* What the shared library lets out.
 *
 * Everything inside is compiled with hidden visibility, so the plumbing —
 * the arena, matching scratch storage and Unicode helpers — does not reach a
 * consumer's symbol table and cannot be linked against by accident. What is
 * marked `RMM_API` below is the contract, and only that. */

#ifndef RMMARKS_EXPORT_H
#define RMMARKS_EXPORT_H

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(RMMARKS_BUILDING_SHARED)
#define RMM_API __declspec(dllexport)
#elif defined(RMMARKS_USING_SHARED)
#define RMM_API __declspec(dllimport)
#else
#define RMM_API
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define RMM_API __attribute__((visibility("default")))
#else
#define RMM_API
#endif

#endif /* RMMARKS_EXPORT_H */
