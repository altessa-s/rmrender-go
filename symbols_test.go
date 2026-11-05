// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"debug/elf"
	"debug/macho"
	"debug/pe"
	"os"
	"runtime"
	"strings"
	"testing"
)

// definedSymbols answers the names of the symbols the running executable
// defines, without the leading underscore of Mach-O, read from its Mach-O,
// ELF or PE (COFF) symbol table.
func definedSymbols(t *testing.T) []string {
	t.Helper()
	path, err := os.Executable()
	if err != nil {
		t.Fatal(err)
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
			t.Skip("the executable has no symbol table")
		}
		for _, s := range f.Symtab.Syms {
			// N_SECT: defined in a section of this file.
			if s.Type&0x0e == 0x0e && s.Sect != 0 {
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
			t.Skip("the executable has no symbol table")
		}
		for _, s := range syms {
			if s.Section != elf.SHN_UNDEF {
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
			t.Skip("the executable has no symbol table")
		}
		for _, s := range f.Symbols {
			// External (storage class 2) and defined in a section of
			// this file; x86-64 COFF names carry no leading underscore.
			if s.StorageClass == 2 && s.SectionNumber > 0 {
				names = append(names, s.Name)
			}
		}
	default:
		t.Skip("no symbol reader for " + runtime.GOOS)
	}
	return names
}

// The vendored libraries are linked under rmrgo_ names only, so that the
// C libraries rmlines-go, rmdoc-go and rmmarks-go compile under their own
// names can share a program with this package. A symbol scripts/vendor.py
// did not rename would show here under its own name.
func TestVendoredSymbolsArePrefixed(t *testing.T) {
	renamed := 0
	for _, name := range definedSymbols(t) {
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
