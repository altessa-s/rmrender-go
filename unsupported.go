// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

//go:build !cgo || !(darwin || linux)

package rmrender

// The package binds C libraries and draws through a native backend:
// CoreGraphics on darwin, Cairo and PDFium on linux. Without cgo, or on
// another system, it does not build, and says so here rather than with a
// missing symbol.
var _ = rmrender_needs_cgo_on_darwin_or_linux
