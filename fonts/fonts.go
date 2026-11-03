// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

// Package fonts embeds the open fonts rmrender-lib lays out typed text with:
// Noto Sans in four weights and italic, EB Garamond, Noto Sans for Arabic,
// Devanagari, Hebrew, Japanese, Korean and Thai, and Noto Emoji. They carry
// the SIL Open Font License, whose texts sit beside them as *-OFL.txt.
//
// The files are about 27 MB, which is why they live in a package of their
// own: a program that draws no typed text need not link them.
//
//	f, err := rmrender.NewFonts(fonts.FS)
package fonts

import "embed"

// FS holds the fonts as <name>.ttf at its root, with their licences.
//
//go:embed *.ttf *-OFL.txt NotoEmoji-SOURCE.md
var FS embed.FS
