/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Content-addressed font files for Fontconfig, which loads fonts by path. On
 * Windows FreeType and cairo keep loaded files open, so they cannot be deleted
 * while a font map lives; a cache shared by every font map and run keeps the
 * files bounded by the distinct fonts supplied instead. */
#ifndef RMR_FONT_CACHE_H
#define RMR_FONT_CACHE_H
#include <stddef.h>
#include <stdint.h>

/* Creates directory (UTF-8) if missing and returns it in a spelling that
 * Fontconfig and FreeType open through the ANSI file APIs: the path itself
 * when it is ASCII, otherwise its Windows 8.3 alias when that is ASCII. Returns
 * nullptr when the directory cannot be created or has no ASCII spelling. The
 * caller frees the result with g_free. */
char* rmr_font_cache_directory(const char* directory);

/* Returns the path of directory/<SHA-256 of bytes>.ttf, first writing bytes
 * there through a temporary file and rename unless a file of that size is
 * already present; nullptr on failure. The caller frees it with g_free. */
char* rmr_font_cache_file(const char* directory, const uint8_t* bytes,
                          size_t size);

#endif
