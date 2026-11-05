/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "rmr_font_cache.h"

#include <glib.h>
#include <glib/gstdio.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* Whether path holds only ASCII bytes. */
static bool ascii(const char* path) {
  for (const char* c = path; *c; c++)
    if ((unsigned char)*c >= 0x80) return false;
  return true;
}

#ifdef _WIN32
/* The 8.3 alias of the existing UTF-8 path in UTF-8, or nullptr when the
 * volume has none. */
static char* short_path(const char* path) {
  gunichar2* wide = g_utf8_to_utf16(path, -1, nullptr, nullptr, nullptr);
  if (!wide) return nullptr;
  char* result = nullptr;
  DWORD n = GetShortPathNameW((const wchar_t*)wide, nullptr, 0);
  wchar_t* alias = n ? g_try_new(wchar_t, n) : nullptr;
  if (alias) {
    DWORD m = GetShortPathNameW((const wchar_t*)wide, alias, n);
    if (m && m < n)
      result = g_utf16_to_utf8((const gunichar2*)alias, -1, nullptr, nullptr,
                               nullptr);
  }
  g_free(alias);
  g_free(wide);
  return result;
}
#endif

char* rmr_font_cache_directory(const char* directory) {
  if (!directory || g_mkdir_with_parents(directory, 0700) != 0) return nullptr;
  if (ascii(directory)) return g_strdup(directory);
#ifdef _WIN32
  char* alias = short_path(directory);
  if (alias && ascii(alias)) return alias;
  g_free(alias);
#endif
  return nullptr;
}

/* Whether path is a regular file of exactly size bytes. */
static bool present(const char* path, size_t size) {
  GStatBuf status;
  return g_file_test(path, G_FILE_TEST_IS_REGULAR) &&
         g_stat(path, &status) == 0 && status.st_size >= 0 &&
         (uint64_t)status.st_size == (uint64_t)size;
}

char* rmr_font_cache_file(const char* directory, const uint8_t* bytes,
                          size_t size) {
  if (!directory || !bytes || !size || size > G_MAXSSIZE) return nullptr;
  char* digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, bytes, size);
  if (!digest) return nullptr;
  char* name = g_strconcat(digest, ".ttf", nullptr);
  char* path = g_build_filename(directory, name, nullptr);
  g_free(name);
  g_free(digest);

  /* An existing file is reused, never rewritten: Windows cannot replace a
   * file another font map still has open. GLib owns the temporary file, the
   * write and the rename, so no descriptor crosses C runtimes. A concurrent
   * writer that wins the rename leaves the same content. */
  if (present(path, size) ||
      g_file_set_contents_full(path, (const gchar*)bytes, (gssize)size,
                               G_FILE_SET_CONTENTS_CONSISTENT, 0600, nullptr) ||
      present(path, size))
    return path;
  g_free(path);
  return nullptr;
}
