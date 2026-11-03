/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_BACKEND_H
#define RMR_BACKEND_H
#include "rmrender.h"
#define RMR_BACKEND_ABI 7u
#define RMR_CAP_PDF (UINT64_C(1) << 11)
#define RMR_CAP_TEXT (UINT64_C(1) << 10)
#define RMR_CAP_FILL_STROKE (UINT64_C(1) << 8)
#define RMR_CAP_MITER (UINT64_C(1) << 9)
#define RMR_CAP_IMAGE (UINT64_C(1) << 7)
#define RMR_CAP_ERASE (UINT64_C(1) << 5)
#define RMR_CAP_GROUP (UINT64_C(1) << 6)
#define RMR_CAP_MASK (UINT64_C(1) << 4)
#define RMR_CAP_FILL (UINT64_C(1) << 0)
#define RMR_CAP_STROKE (UINT64_C(1) << 1)
#define RMR_CAP_DARKEN (UINT64_C(1) << 2)
#define RMR_CAP_MULTIPLY (UINT64_C(1) << 3)
/* TEXT means native Helvetica line shaping/fallback and baseline drawing,
 * compatible with the text metrics provider used to build the plan.
 * FILL_STROKE means one native nonzero fill/outline operation with separate
 * colors/alphas. MITER means miter joins with limit 10 for stroked geometry.
 * IMAGE means PNG/JPEG decoding and affine placement through the path clip.
 * ERASE means ordered contour subtraction, including native stroked-outline
 * conversion when needed; GROUP means isolated transparency for contiguous
 * tokens. A backend must close active groups on end AND abort. Never advertise
 * a capability that is unavailable on the current runtime. No native objects
 * appear in this interface. Callbacks see borrowed immutable commands only
 * during the call. State lives in context, not global variables. On ANY failure
 * after begin is invoked, abort is called exactly once, including a failed
 * begin/end. No end after failed draw. Abort must tolerate partial init. The
 * backend is responsible for transactional output and releasing its resources.
 * Callbacks return a status; execution identifies the failing draw in error. */
typedef struct {
  uint32_t abi_version;
  size_t struct_size;
  uint64_t capabilities;
  void* context;
  rmr_status (*begin)(void* context, rmr_rect viewport);
  rmr_status (*draw)(void* context, const rmr_draw* draw);
  rmr_status (*end)(void* context);
  void (*abort)(void* context);
} rmr_backend;

typedef bool (*rmr_cancel_fn)(void* context);

/* Checks ABI and ALL required capabilities before touching the backend.
 * Cancellation is observed before begin, each draw, and end. Callbacks must not
 * destroy the plan or backend; nested use of the same backend is unsupported.
 */
[[nodiscard]] RMR_API rmr_status rmr_plan_execute(const rmr_plan* plan,
                                                  const rmr_backend* backend,
                                                  rmr_cancel_fn cancel,
                                                  void* cancel_context,
                                                  rmr_error* error);

#endif
