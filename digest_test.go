// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"crypto/sha256"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

// clibDigest answers the digest scripts/vendor.py writes into clib_digest.h,
// computed the way the script computes it: for every regular file under
// internal/clib, its SHA-256 in hex, a space, its path relative to
// internal/clib with forward slashes and a newline, in the order Python sorts
// pathlib paths, component by component, and the SHA-256 of all those lines.
// Component order is not string order: "a/x" sorts before "a-b/x", because
// "a" sorts before "a-b", though "/" sorts after "-".
func clibDigest(t *testing.T) string {
	t.Helper()
	const root = "internal/clib"
	var paths [][]string
	err := filepath.WalkDir(root, func(path string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if entry.IsDir() {
			return nil
		}
		// pathlib's is_file follows a link to a file; the tree holds none, and
		// a link anywhere in it is refused rather than guessed at.
		if !entry.Type().IsRegular() {
			return fmt.Errorf("%s is not a regular file", path)
		}
		relative, err := filepath.Rel(root, path)
		if err != nil {
			return err
		}
		paths = append(paths, strings.Split(filepath.ToSlash(relative), "/"))
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	slices.SortFunc(paths, slices.Compare)
	h := sha256.New()
	for _, parts := range paths {
		relative := strings.Join(parts, "/")
		data, err := os.ReadFile(filepath.Join(root, filepath.FromSlash(relative)))
		if err != nil {
			t.Fatal(err)
		}
		fmt.Fprintf(h, "%x %s\n", sha256.Sum256(data), relative)
	}
	return fmt.Sprintf("%x", h.Sum(nil))
}

// clib_digest.h records, beside the upstream version the vendored sources
// were taken at, a digest of every file under internal/clib. Holding the
// committed tree to that digest ties it to the recorded version: a hand edit
// or a partial update of the vendored sources fails this test instead of
// shipping unnoticed. scripts/vendor.py changes the tree and the digest
// together.
func TestVendoredDigest(t *testing.T) {
	header, err := os.ReadFile("clib_digest.h")
	if err != nil {
		t.Fatal(err)
	}
	want := fmt.Sprintf(`#define RMRENDER_GO_CLIB_DIGEST "%s"`, clibDigest(t))
	if !strings.Contains(string(header), want) {
		t.Fatalf("clib_digest.h does not hold the digest of internal/clib: want %s; re-run scripts/vendor.py", want)
	}
}
