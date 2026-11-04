/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* The tablet's file format, both ways: reading what a reMarkable tablet
 * writes — the `.rm` v6 stream, the page and its strokes, typed text and its
 * paragraphs — writing what it accepts, and erasing from a page it wrote, in
 * place. Nothing here draws, and nothing here has heard of the cloud, the
 * index or a file manager.
 *
 * A standalone C23 library with no dependency beyond the C standard
 * library. */

#ifndef RMLINES_H
#define RMLINES_H

#include "rmlines/editor.h"
#include "rmlines/log.h"
#include "rmlines/parser.h"
#include "rmlines/types.h"
#include "rmlines/writer.h"

/* The one place the version is written. The build reads these three numbers
 * out of this file for `project(VERSION)`, the package config and the
 * pkg-config file, and the string below is spelled from them — so a release
 * is one edit, and the library cannot report a version its package does not
 * carry. */
#define RMLINES_VERSION_MAJOR 1
#define RMLINES_VERSION_MINOR 1
#define RMLINES_VERSION_PATCH 0

#define RMLINES_STRINGIFY_(x) #x
#define RMLINES_STRINGIFY(x) RMLINES_STRINGIFY_(x)
#define RMLINES_VERSION_STRING                                        \
  RMLINES_STRINGIFY(RMLINES_VERSION_MAJOR)                            \
  "." RMLINES_STRINGIFY(RMLINES_VERSION_MINOR) "." RMLINES_STRINGIFY( \
      RMLINES_VERSION_PATCH)

#endif /* RMLINES_H */
