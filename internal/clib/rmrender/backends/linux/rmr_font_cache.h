/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Content-addressed font files for Fontconfig, which loads fonts by path. On
 * Windows FreeType and cairo keep loaded files open, so they cannot be deleted
 * while a font map lives; a cache shared by every font map and run keeps the
 * files bounded by the distinct fonts supplied instead. */
#ifndef RMR_FONT_CACHE_H
#define RMR_FONT_CACHE_H
#include <glib.h>
#include <stddef.h>
#include <stdint.h>

/* Creates directory (UTF-8) if missing and returns it in a spelling that
 * Fontconfig and FreeType open through the ANSI file APIs: the path itself
 * when it is ASCII, otherwise its Windows 8.3 alias when that is ASCII. Returns
 * nullptr when the directory cannot be created or has no ASCII spelling. The
 * caller frees the result with g_free. */
char* rmr_font_cache_directory(const char* directory);

/* Returns the path of directory/<SHA-256 of bytes>.ttf, which holds bytes. A
 * file of that size there is used as is; otherwise bytes are written to a
 * unique temporary file that takes the name only if it is still free, so a
 * cached file is never replaced while another thread or process reads it.
 * On Windows it returns only once the file opens for reading. Safe to call
 * concurrently. Returns nullptr on failure, setting the optional error to the
 * failed step and the system's reason; the caller frees the result with
 * g_free. */
char* rmr_font_cache_file(const char* directory, const uint8_t* bytes,
                          size_t size, GError** error);

#endif
