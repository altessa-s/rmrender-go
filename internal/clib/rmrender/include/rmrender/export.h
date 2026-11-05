/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMRENDER_EXPORT_H
#define RMRENDER_EXPORT_H
/* RMR_API marks the exported functions. On Windows the build defines
 * RMRENDER_BUILDING inside a DLL and gives its users RMRENDER_SHARED; a static
 * library needs neither. Elsewhere symbols are hidden unless marked. */
#if defined(_WIN32) && defined(RMRENDER_BUILDING)
#define RMR_API __declspec(dllexport)
#elif defined(_WIN32) && defined(RMRENDER_SHARED)
#define RMR_API __declspec(dllimport)
#elif defined(_WIN32)
#define RMR_API
#elif defined(__GNUC__) || defined(__clang__)
#define RMR_API __attribute__((visibility("default")))
#else
#define RMR_API
#endif

#endif
