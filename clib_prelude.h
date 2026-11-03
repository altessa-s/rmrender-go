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

// Every external symbol of the vendored libraries, renamed.
#include "clib_symbols.h"

#endif
