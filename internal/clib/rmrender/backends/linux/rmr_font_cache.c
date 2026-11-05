/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include "rmr_font_cache.h"

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
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

/* The outcome of giving a file a name that may already be taken. */
typedef enum { PLACED, TAKEN, FAILED } placement;

#ifdef _WIN32
/* The calling process's id. */
static unsigned long process(void) { return GetCurrentProcessId(); }

/* Sets error to the failed step and the Windows error code with its text. */
static void windows_error(GError** error, const char* step, DWORD code) {
  char* text = g_win32_error_message((gint)code);
  g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
              "%s: Windows error %lu: %s", step, (unsigned long)code, text);
  g_free(text);
}

/* Moves the file from to the name to unless that name exists. */
static placement place(const char* from, const char* to, GError** error) {
  gunichar2* source = g_utf8_to_utf16(from, -1, nullptr, nullptr, nullptr);
  gunichar2* target = g_utf8_to_utf16(to, -1, nullptr, nullptr, nullptr);
  placement result = FAILED;
  if (!source || !target)
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                "move: path is not UTF-8");
  else if (MoveFileExW((const wchar_t*)source, (const wchar_t*)target, 0))
    result = PLACED;
  else {
    DWORD code = GetLastError();
    if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS)
      result = TAKEN;
    else
      windows_error(error, "move", code);
  }
  g_free(target);
  g_free(source);
  return result;
}

/* Whether the file at path opens for reading as the C runtime, FreeType and
 * Fontconfig open it: sharing reads and writes but not deletion. While a
 * mover's MoveFileExW is renaming the file it holds it open for deleting, so
 * such an open fails with a sharing violation even though the complete file
 * is already visible under its name. The wait lasts as long as that call, so
 * a bounded retry is enough. */
static bool readable(const char* path, GError** error) {
  gunichar2* wide = g_utf8_to_utf16(path, -1, nullptr, nullptr, nullptr);
  if (!wide) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                "open: path is not UTF-8");
    return false;
  }
  DWORD code = ERROR_SUCCESS;
  for (int attempt = 0; attempt < 200; attempt++) {
    HANDLE file = CreateFileW((const wchar_t*)wide, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
      g_free(wide);
      return true;
    }
    code = GetLastError();
    if (code != ERROR_SHARING_VIOLATION) break;
    Sleep(attempt < 10 ? 1u : 10u);
  }
  g_free(wide);
  windows_error(error, "open", code);
  return false;
}
#else
/* The calling process's id. */
static unsigned long process(void) { return (unsigned long)getpid(); }

/* Links the name to to the file from unless that name exists; unlike rename,
 * link never replaces. The caller removes from. */
static placement place(const char* from, const char* to, GError** error) {
  if (link(from, to) == 0) return PLACED;
  int code = errno;
  if (code == EEXIST) return TAKEN;
  g_set_error(error, G_FILE_ERROR, (gint)g_file_error_from_errno(code),
              "link: %s", g_strerror(code));
  return FAILED;
}

/* POSIX opens never conflict with a link or rename in progress. */
static bool readable(const char* path, GError** error) {
  (void)path;
  (void)error;
  return true;
}
#endif

/* The path of directory/<SHA-256 of bytes><extension>, holding bytes; see
 * rmr_font_cache_file. */
static char* cache(const char* directory, const char* extension,
                   const uint8_t* bytes, size_t size, GError** error) {
  if (!directory || !bytes || !size || size > G_MAXSSIZE) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "invalid bytes");
    return nullptr;
  }
  char* digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, bytes, size);
  if (!digest) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOMEM, "no checksum");
    return nullptr;
  }
  char* name = g_strconcat(digest, extension, nullptr);
  char* path = g_build_filename(directory, name, nullptr);
  g_free(name);
  g_free(digest);

  /* A file under the final name is complete and never replaced: Windows fails
   * to open a file while it is being replaced, and to replace one that is
   * open. The bytes go to a temporary file unique to this call (the process
   * id separates forks sharing a random state), which takes the final name
   * only if no other writer got there first. GLib opens, writes and closes
   * it, so no descriptor crosses C runtimes. */
  placement placed = TAKEN;
  if (!present(path, size)) {
    char* temporary =
        g_strdup_printf("%s.%lu-%08x%08x.tmp", path, process(),
                        (unsigned)g_random_int(), (unsigned)g_random_int());
    placed = FAILED;
    if (g_file_set_contents_full(temporary, (const gchar*)bytes, (gssize)size,
                                 G_FILE_SET_CONTENTS_DURABLE, 0600, error)) {
      placed = place(temporary, path, error);
      /* Placed files never change size, so a wrong-sized one predates this
       * protocol or was damaged outside it: replace it once. */
      if (placed == TAKEN && !present(path, size) && g_unlink(path) == 0)
        placed = place(temporary, path, error);
    } else
      g_prefix_error(error, "write: ");
    /* Gone already where placing moved it. */
    g_unlink(temporary);
    g_free(temporary);
    if (placed == TAKEN && !present(path, size)) {
      g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_EXIST,
                  "a file of another size holds the name");
      placed = FAILED;
    }
  }

  /* The name holds a complete file that nobody replaces or deletes; on
   * Windows a concurrent mover may still be finishing with it. */
  if (placed != FAILED && readable(path, error)) return path;
  g_free(path);
  return nullptr;
}

char* rmr_font_cache_file(const char* directory, const uint8_t* bytes,
                          size_t size, GError** error) {
  return cache(directory, ".ttf", bytes, size, error);
}

char* rmr_font_cache_config(const char* directory, GError** error) {
  static const char config[] = "<?xml version=\"1.0\"?>\n<fontconfig/>\n";
  return cache(directory, ".conf", (const uint8_t*)config, sizeof config - 1,
               error);
}
