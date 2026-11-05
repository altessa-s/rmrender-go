/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* POSIX newlocale/strtod_l on Apple and Linux, the CRT's _create_locale and
 * _strtod_l on Windows. A private C locale keeps numeric conversion
 * independent of the application's locale, without setlocale. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rmr_locale.h"

#include <locale.h>
#include <stdlib.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif

#ifdef _WIN32
rmr_locale rmr_locale_create(void) {
  return (rmr_locale)_create_locale(LC_NUMERIC, "C");
}

void rmr_locale_free(rmr_locale locale) { _free_locale((_locale_t)locale); }

double rmr_locale_number(const char* text, char** end, rmr_locale locale) {
  return _strtod_l(text, end, (_locale_t)locale);
}
#else
rmr_locale rmr_locale_create(void) {
  return (rmr_locale)newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
}

void rmr_locale_free(rmr_locale locale) { freelocale((locale_t)locale); }

double rmr_locale_number(const char* text, char** end, rmr_locale locale) {
  return strtod_l(text, end, (locale_t)locale);
}
#endif
