// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"debug/elf"
	"debug/macho"
	"debug/pe"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// Mach-O symbol type bits, from <mach-o/nlist.h>.
const (
	machoExternal        = 0x01 // N_EXT
	machoType            = 0x0e // N_TYPE
	machoSection         = 0x0e // N_SECT, under N_TYPE
	machoPrivateExternal = 0x10 // N_PEXT
)

// linkedSymbols answers the names of the symbols the linker could see from
// another object: those an unstripped test binary of this package defines
// with external linkage, or with hidden visibility, without the leading
// underscore of Mach-O, read from its Mach-O, ELF or PE (COFF) symbol table.
//
// The binary is built here, not taken from os.Executable: go test links its
// binary without a symbol table on some systems, and a test that skipped
// there would check nothing where the linkers differ most. It is built with
// this binary's backend tag, so that each backend's sources are checked.
//
// Hidden symbols count as well as external ones: compiled with hidden
// visibility, the vendored code would still collide with the same library
// linked under its own names in one program, since hiding acts only when a
// shared object is made. In the executable such a symbol shows as private
// external on Mach-O, and on ELF keeps its hidden visibility, whether the
// linker leaves its binding global or makes it local; the test takes every
// form. Static symbols are left out; they cannot collide.
//
// The binary is built by the toolchain running this test, from this file's
// package directory, so that a prebuilt test binary run from anywhere checks
// the same package, as long as the source tree is where it was built from.
func linkedSymbols(t *testing.T) []string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "symbols.test")
	cmd := exec.Command(goTool(), "test", "-c", "-tags="+testTags, "-ldflags=-s=false -w=false", "-o", path, ".")
	cmd.Dir = packageDir(t)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("building the test binary in %s: %v\n%s", cmd.Dir, err, out)
	}
	var names []string
	switch runtime.GOOS {
	case "darwin":
		f, err := macho.Open(path)
		if err != nil {
			t.Fatal(err)
		}
		defer f.Close()
		if f.Symtab == nil {
			t.Fatal("the test binary has no symbol table")
		}
		for _, s := range f.Symtab.Syms {
			// Defined in a section of this file, and external or private
			// external; a static symbol carries neither bit.
			if s.Type&machoType == machoSection && s.Sect != 0 &&
				s.Type&(machoExternal|machoPrivateExternal) != 0 {
				names = append(names, strings.TrimPrefix(s.Name, "_"))
			}
		}
	case "linux":
		f, err := elf.Open(path)
		if err != nil {
			t.Fatal(err)
		}
		defer f.Close()
		syms, err := f.Symbols()
		if err != nil {
			t.Fatalf("the test binary has no symbol table: %v", err)
		}
		for _, s := range syms {
			// A hidden symbol is local in the linked executable but keeps
			// its hidden visibility, which tells it from a static one.
			bind := elf.ST_BIND(s.Info)
			hidden := bind == elf.STB_LOCAL && elf.ST_VISIBILITY(s.Other) == elf.STV_HIDDEN
			if s.Section != elf.SHN_UNDEF && (bind == elf.STB_GLOBAL || bind == elf.STB_WEAK || hidden) {
				names = append(names, s.Name)
			}
		}
	case "windows":
		f, err := pe.Open(path)
		if err != nil {
			t.Fatal(err)
		}
		defer f.Close()
		if len(f.Symbols) == 0 {
			t.Fatal("the test binary has no symbol table")
		}
		for _, s := range f.Symbols {
			// External (storage class 2) and defined in a section of
			// this file; x86-64 and arm64 COFF names carry no leading
			// underscore. COFF has no hidden visibility.
			if s.StorageClass == 2 && s.SectionNumber > 0 {
				names = append(names, s.Name)
			}
		}
	default:
		t.Skip("no symbol reader for " + runtime.GOOS)
	}
	return names
}

// goTool answers the go command of the toolchain running this test, or the
// one on PATH when that toolchain's is not on disk.
func goTool() string {
	name := "go"
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	//nolint:staticcheck // GOROOT is wanted here: the toolchain this binary was built with, when it is on disk.
	if root := runtime.GOROOT(); root != "" {
		if tool := filepath.Join(root, "bin", name); fileExists(tool) {
			return tool
		}
	}
	return "go"
}

// packageDir answers the directory of this package's sources, the directory
// of this file as it was compiled, failing the test when the sources are not
// there.
func packageDir(t *testing.T) string {
	t.Helper()
	_, file, _, ok := runtime.Caller(0)
	dir := filepath.Dir(file)
	if !ok || !filepath.IsAbs(file) || !fileExists(filepath.Join(dir, "symbols_test.go")) {
		t.Fatalf("the package sources are not at %q, where this test binary was built from: "+
			"the test builds the package from source, so run it beside its source tree", dir)
	}
	return dir
}

// fileExists reports whether path names a regular file.
func fileExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.Mode().IsRegular()
}

// The vendored libraries are linked under rmrgo_ names only, so that the
// C libraries rmlines-go, rmdoc-go and rmmarks-go compile under their own
// names can share a program with this package. A symbol scripts/vendor.py
// did not rename would show here under its own name.
func TestVendoredSymbolsArePrefixed(t *testing.T) {
	renamed := 0
	for _, name := range linkedSymbols(t) {
		if strings.HasPrefix(name, "rmrgo_rm") {
			renamed++
			continue
		}
		for _, prefix := range []string{"rm_", "rmd_", "rmm_", "rmr_"} {
			if strings.HasPrefix(name, prefix) {
				t.Errorf("%s is linked under its own name", name)
			}
		}
	}
	if renamed < 100 {
		t.Fatalf("only %d renamed symbols found", renamed)
	}
}
