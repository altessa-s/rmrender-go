// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

// Force-included into every C file of the package, the vendored sources and
// the cgo preambles alike (see cgo.go), ahead of anything else.

#ifndef RMRENDER_GO_CLIB_PRELUDE_H
#define RMRENDER_GO_CLIB_PRELUDE_H

// The libraries build with -ffp-contract=off, so that no multiply and add
// fuse and geometry comes out the same on every architecture. cgo does not
// pass that flag, so it is said here: GCC contracts nothing in the ISO mode
// -std=c2x selects, and clang takes the standard pragma.
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

// The libraries are compiled into the package as static code. On Windows
// these defines would declare every API function dllexport or dllimport, as
// for rmrender-lib's DLL build, and the link would look for DLLs that do not
// exist.
#if defined(RMRENDER_BUILDING) || defined(RMRENDER_SHARED) || \
    defined(RMLINES_BUILDING_SHARED) || defined(RMLINES_USING_SHARED) || \
    defined(RMDOC_BUILDING_SHARED) || defined(RMDOC_USING_SHARED) || \
    defined(RMMARKS_BUILDING_SHARED) || defined(RMMARKS_USING_SHARED)
#error "rmrender-go compiles the C libraries in statically; do not define their DLL macros"
#endif

// Every external symbol of the vendored libraries, renamed.
#include "clib_symbols.h"

#endif
