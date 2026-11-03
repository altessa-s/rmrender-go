/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMR_FILES_H
#define RMR_FILES_H
#include <rmrender/rmlines.h>

/* Read `directory/name<extension>` whole, up to `maximum` bytes including the
 * path. A missing file is RMR_OK with an empty buffer. Shared by the template
 * store and the platform font loaders, so exported from the RMLines adapter;
 * not part of the public API. */
RMR_API rmr_status rmr_read_resource(const char* directory, const char* name,
                                     size_t length, const char* extension,
                                     size_t maximum, rmr_buffer* out);

#endif
