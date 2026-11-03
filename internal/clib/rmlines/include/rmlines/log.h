/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Where the parser's diagnostics go.
 *
 * This library parses bytes and does nothing else: it has no logger of its
 * own, no opinion about files or the system log, and no dependency on the
 * host that gives it a page. What it does have is a handful of lines worth
 * saying out loud — a stroke it could not read, a paragraph style this
 * parser does not know, an image whose name is missing from the attachment
 * map. Each one is a piece of a page that will not be drawn, and each one is
 * the kind of thing that is found months later, in someone's notebook,
 * because nothing ever said it.
 *
 * So the lines are written to whatever the host installed, and to nothing at
 * all until it does. Not called is the supported state — a corpus test wants
 * no log — and the default is silence rather than `fprintf(stderr, …)`,
 * because a library that writes to a terminal nobody is watching is a library
 * that has to be worked around.
 *
 * Nothing here is ever a secret: every value in every message is a count, a
 * block type or a raw style number. The page's writing, its text and its file
 * names do not reach a sink, so the host is free to install one that writes a
 * file and keep it. */

#ifndef RMLINES_LOG_H
#define RMLINES_LOG_H

#include "rmlines/export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How much the line matters. Two levels and not five: a piece of the page was
 * lost (RM_LOG_ERROR), or the format held something this parser does not know
 * and read past (RM_LOG_DEBUG). */
typedef enum rm_log_level : int {
  RM_LOG_DEBUG,
  RM_LOG_ERROR,
} rm_log_level;

/* What a host installs. Called on whichever thread parsed the page — the
 * parser runs wherever the work is, and a sink that touches shared state must
 * say so itself. `message` is NUL-terminated and is not owned by the sink: it
 * lives only for the duration of the call. */
typedef void (*rm_log_sink)(rm_log_level level, const char* message,
                            void* context);

/* Name the sink for this process. Once, at its start, from the composition
 * root; a later call replaces the earlier one. */
RM_API void rm_log_install(rm_log_sink sink, void* context);

/* Go back to silence. For a test that installed a sink and must not leave it
 * behind for the next one. */
RM_API void rm_log_silence(void);

#ifdef __cplusplus
}
#endif

#endif /* RMLINES_LOG_H */
