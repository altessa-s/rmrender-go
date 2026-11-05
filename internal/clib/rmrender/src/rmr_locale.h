/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* A private C numeric locale: parsing must not change or depend on the
 * caller's process-wide locale. */
#ifndef RMR_LOCALE_H
#define RMR_LOCALE_H

/* An opaque handle to the platform locale (locale_t or _locale_t). */
typedef struct rmr_locale_state* rmr_locale;

/* Creates a "C" numeric locale; returns nullptr when none is available. */
rmr_locale rmr_locale_create(void);

/* Releases a locale from rmr_locale_create. */
void rmr_locale_free(rmr_locale locale);

/* Parses a decimal number like strtod, with locale's radix character. */
double rmr_locale_number(const char* text, char** end, rmr_locale locale);

#endif
