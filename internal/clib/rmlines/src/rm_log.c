/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "rm_internal.h"

/* The sink and its context move as one: read separately, a line could reach a
 * new sink with the old sink's context. The pair is published behind a single
 * atomic pointer, and a replacement allocates a new pair rather than editing
 * the live one. */
typedef struct rm_log_binding {
  rm_log_sink sink;
  void* context;
} rm_log_binding;

static _Atomic(rm_log_binding*) rm_log_installed = nullptr;

/* Every binding ever installed, kept so that a line already on its way to one
 * does not read freed memory. `rm_log_install` is documented as a
 * once-at-startup call, so the list is a handful of entries at worst — and a
 * leak of a handful of pointers beats a use-after-free in a library that
 * cannot know what threads its host is running. */
typedef struct rm_log_retired {
  struct rm_log_retired* next;
  rm_log_binding binding;
} rm_log_retired;

static _Atomic(rm_log_retired*) rm_log_history = nullptr;

void rm_log_install(rm_log_sink sink, void* context) {
  if (sink == nullptr) {
    rm_log_silence();
    return;
  }
  rm_log_retired* entry = malloc(sizeof *entry);
  if (entry == nullptr) return;
  entry->binding = (rm_log_binding){.sink = sink, .context = context};
  rm_log_retired* head =
      atomic_load_explicit(&rm_log_history, memory_order_relaxed);
  do {
    entry->next = head;
  } while (!atomic_compare_exchange_weak_explicit(&rm_log_history, &head, entry,
                                                  memory_order_release,
                                                  memory_order_relaxed));
  atomic_store_explicit(&rm_log_installed, &entry->binding,
                        memory_order_release);
}

void rm_log_silence(void) {
  atomic_store_explicit(&rm_log_installed, nullptr, memory_order_release);
}

bool rm_log_wanted(void) {
  return atomic_load_explicit(&rm_log_installed, memory_order_acquire) !=
         nullptr;
}

void rm_log_writef(rm_log_level level, const char* format, ...) {
  rm_log_binding* binding =
      atomic_load_explicit(&rm_log_installed, memory_order_acquire);
  if (binding == nullptr) return;

  /* One line, one stack buffer. The messages are counts, block types and
   * raw style numbers — never a page's text — so a line that does not fit
   * is rare; it goes to the heap rather than out cut short, so that every
   * line is handed over whole. */
  char message[512];
  va_list args;
  va_start(args, format);
  va_list again;
  va_copy(again, args);
  int written = vsnprintf(message, sizeof message, format, args);
  va_end(args);
  if (written < 0) {
    va_end(again);
    return;
  }
  if ((size_t)written < sizeof message) {
    va_end(again);
    binding->sink(level, message, binding->context);
    return;
  }
  char* whole = malloc((size_t)written + 1u);
  if (whole == nullptr) {
    va_end(again);
    /* Out of memory: the line cut short beats no line at all. */
    binding->sink(level, message, binding->context);
    return;
  }
  (void)vsnprintf(whole, (size_t)written + 1u, format, again);
  va_end(again);
  binding->sink(level, whole, binding->context);
  free(whole);
}
