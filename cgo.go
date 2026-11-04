// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender

// -std=c2x and not -std=c23: GCC 13 knows the standard only by its draft
// name, while GCC 14 and clang 19 or newer take both. The libraries are
// written with nullptr, typed enums and constexpr, which clang 18 does not
// read, so no flag rescues an older compiler.
//
// clib_prelude.h is force-included into every C file of the package, these
// preambles too. It turns off floating-point contraction, as the libraries'
// own builds do, and includes clib_symbols.h, which renames each external
// symbol of the vendored libraries to rmrgo_<name>, so that this package
// links beside rmlines-go, rmdoc-go and rmmarks-go. See scripts/vendor.py.
//
// The backend is chosen by GOOS: CoreGraphics and CoreText on darwin, Cairo,
// Pango and PDFium on linux. On darwin the rmrender_cairo build tag selects
// the Cairo backend instead. PDFium's headers and library come from
// CGO_CFLAGS and CGO_LDFLAGS; see the README.

/*
#cgo CFLAGS: -std=c2x -include ${SRCDIR}/clib_prelude.h
#cgo CFLAGS: -I${SRCDIR}/internal/clib/rmrender/include
#cgo CFLAGS: -I${SRCDIR}/internal/clib/rmlines/include -I${SRCDIR}/internal/clib/rmlines/src
#cgo CFLAGS: -I${SRCDIR}/internal/clib/rmdoc/include -I${SRCDIR}/internal/clib/rmdoc/src
#cgo CFLAGS: -I${SRCDIR}/internal/clib/rmmarks/include -I${SRCDIR}/internal/clib/rmmarks/src
#cgo LDFLAGS: -lz -lm
#cgo darwin,!rmrender_cairo CFLAGS: -DRMRGO_APPLE -I${SRCDIR}/internal/clib/rmrender/backends/apple
#cgo darwin,!rmrender_cairo LDFLAGS: -framework CoreGraphics -framework CoreText -framework CoreFoundation
#cgo linux CFLAGS: -DRMRGO_CAIRO -I${SRCDIR}/internal/clib/rmrender/backends/linux
#cgo darwin,rmrender_cairo CFLAGS: -DRMRGO_CAIRO -I${SRCDIR}/internal/clib/rmrender/backends/linux
#cgo linux pkg-config: cairo pangocairo pangoft2 fontconfig harfbuzz gdk-pixbuf-2.0
#cgo darwin,rmrender_cairo pkg-config: cairo pangocairo pangoft2 fontconfig harfbuzz gdk-pixbuf-2.0
#cgo linux LDFLAGS: -lpdfium -lpthread
#cgo darwin,rmrender_cairo LDFLAGS: -lpdfium

#include "bridge.h"
*/
import "C"

import "unsafe"

// Version is the version of the vendored rmrender-lib.
const Version = "1.2.0"

// cVersion answers the C library's own version, so that a test can hold
// [Version] to it.
func cVersion() string {
	return C.GoString(C.rmr_version())
}

// pdfPageCount answers how many pages the PDF data has, read by the C
// library's own PDF parser, so that tests need no other.
func pdfPageCount(data []byte) (int, error) {
	if len(data) == 0 {
		return 0, errInvalid("empty PDF")
	}
	var count C.size_t
	var cerr C.rmr_error
	status := C.rmr_pdf_pages((*C.uint8_t)(unsafe.Pointer(&data[0])), C.size_t(len(data)), nil, nil, 0, &count, &cerr)
	if status != C.RMR_OK {
		return 0, cError(status, &cerr)
	}
	return goLength(count, "page count")
}
