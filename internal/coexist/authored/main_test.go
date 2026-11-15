// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package main

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
)

// The committed pages are what the generator writes, byte for byte, so they
// can be regenerated from the tree and are not edited by hand.
func TestCommittedPagesAreReproduced(t *testing.T) {
	for name, page := range pages {
		t.Run(name, func(t *testing.T) {
			want, err := os.ReadFile(filepath.Join("..", "..", "..", "testdata", "authored", name))
			if err != nil {
				t.Fatal(err)
			}
			got, err := page()
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(got, want) {
				t.Fatalf("the generator writes %d bytes that differ from the committed %d", len(got), len(want))
			}
			again, err := page()
			if err != nil || !bytes.Equal(again, got) {
				t.Fatalf("a second run differs: %v", err)
			}
		})
	}
}
