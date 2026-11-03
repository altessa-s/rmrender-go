/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* What the shared library lets out.
 *
 * Everything inside is compiled with hidden visibility, so the plumbing —
 * the arena, the hash map, the byte reader — does not reach a consumer's
 * symbol table and cannot be linked against by accident. What is marked
 * `RM_API` below is the contract, and only that. */

#ifndef RMLINES_EXPORT_H
#define RMLINES_EXPORT_H

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(RMLINES_BUILDING_SHARED)
#define RM_API __declspec(dllexport)
#elif defined(RMLINES_USING_SHARED)
#define RM_API __declspec(dllimport)
#else
#define RM_API
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define RM_API __attribute__((visibility("default")))
#else
#define RM_API
#endif

#endif /* RMLINES_EXPORT_H */
