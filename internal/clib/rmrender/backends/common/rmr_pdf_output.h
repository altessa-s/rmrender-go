/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* The caller's rmr_output, shared by the Apple and Cairo document exports:
 * checking it, and passing writes on to it so that its failure is known
 * however deep in the export it happens. */

#ifndef RMR_PDF_OUTPUT_H
#define RMR_PDF_OUTPUT_H
#include <rmrender/output.h>
#include <stddef.h>

/* False for a wrong abi_version, a struct_size short of the first version or
 * a null write. */
static inline bool rmr_output_valid(const rmr_output* output) {
  return output && output->abi_version == RMR_OUTPUT_ABI &&
         output->struct_size >=
             offsetof(rmr_output, context) + sizeof output->context &&
         output->write;
}

/* The caller's output and how its last write went. */
typedef struct {
  const rmr_output* given;
  rmr_status status;
  rmr_output output; /* Writes through rmr_output_guarded_write. */
} rmr_output_guard;

/* rmr_output write passing bytes on to the guarded output while it has not
 * failed, remembering its failure. */
static inline rmr_status rmr_output_guarded_write(void* context,
                                                  const uint8_t* bytes,
                                                  size_t size) {
  rmr_output_guard* g = context;
  if (g->status == RMR_OK)
    g->status = g->given->write(g->given->context, bytes, size);
  return g->status;
}

/* Guards given, which rmr_output_valid accepts, in g; g->output is what the
 * export writes to. g must not move while the export runs. */
static inline void rmr_output_guard_init(rmr_output_guard* g,
                                         const rmr_output* given) {
  *g = (rmr_output_guard){.given = given};
  g->output = (rmr_output){RMR_OUTPUT_ABI, sizeof(rmr_output),
                           rmr_output_guarded_write, g};
}

#endif
