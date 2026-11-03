/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Packed ink tiles, generated into rmr_marker_tiles.c and rmr_pencil_tiles.c
 * by scripts/embed-marker.py and scripts/embed-pencil.py. Each tile is 64x64,
 * MSB first, row zero at top; 1 means ink. */

#ifndef RMR_TILES_H
#define RMR_TILES_H

#include <stdint.h>

/* Four tiles for the marker ink family. */
extern const uint8_t rmr_marker_tiles[4][512];

/* Four tiles per pencil family. */
extern const uint8_t rmr_pencil_tiles[12][512];

#endif
