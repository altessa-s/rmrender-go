// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender

import "io"

// CVersion lets the external tests hold Version against the C library's.
var CVersion = cVersion

// PDFPageCount lets the external tests count the pages of a PDF with the C
// library's own parser.
var PDFPageCount = pdfPageCount

// WriteOutput passes p to w as an export's file is passed, in pieces.
func WriteOutput(w io.Writer, p []byte) error { return (&output{w: w}).write(p) }

// OutputPiece is the most a writer is handed at once.
const OutputPiece = outputPiece
