/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* What the shared library lets out.
 *
 * Everything inside is compiled with hidden visibility, so the plumbing —
 * the arena, matching scratch storage and Unicode helpers — does not reach a
 * consumer's symbol table and cannot be linked against by accident. What is
 * marked `RMD_API` below is the contract, and only that. */

#ifndef RMDOC_EXPORT_H
#define RMDOC_EXPORT_H

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(RMDOC_BUILDING_SHARED)
#define RMD_API __declspec(dllexport)
#elif defined(RMDOC_USING_SHARED)
#define RMD_API __declspec(dllimport)
#else
#define RMD_API
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define RMD_API __attribute__((visibility("default")))
#else
#define RMD_API
#endif

#endif /* RMDOC_EXPORT_H */
