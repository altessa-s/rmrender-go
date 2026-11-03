/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_APPLE_CONTEXT_H
#define RMR_APPLE_CONTEXT_H
#include <CoreGraphics/CoreGraphics.h>

#include "apple.h"

/* Apple integration boundary only: draw into a borrowed, open native context.
 * sheet.transform maps .rm coordinates into the context's current user space.
 * Existing context state is restored. No page is begun/ended and no context
 * is closed. Cancellation/errors may leave partial content in the context. */
[[nodiscard]] RMR_API rmr_status rmr_apple_draw_context(
    CGContextRef context, const rmr_plan* plan, const rmr_sheet* sheet,
    const rmr_limits* limits, rmr_cancel_fn cancel, void* cancel_context,
    rmr_error* error);

#endif
