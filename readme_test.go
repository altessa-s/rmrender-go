// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

package rmrender_test

import (
	"os"
	"regexp"
	"strings"
	"testing"
)

// Every Go snippet of the README, but the import line, is the body of an
// Example: the README shows only code that compiles and runs.
func TestReadmeSnippetsAreExamples(t *testing.T) {
	readme, err := os.ReadFile("README.md")
	if err != nil {
		t.Fatal(err)
	}
	examples, err := os.ReadFile("example_test.go")
	if err != nil {
		t.Fatal(err)
	}
	bodies := map[string]bool{}
	for _, m := range regexp.MustCompile(`(?s)\nfunc Example\w*\(\) \{\n(.*?)\n\}\n`).FindAllSubmatch(examples, -1) {
		var lines []string
		for _, line := range strings.Split(string(m[1]), "\n") {
			if strings.HasPrefix(strings.TrimSpace(line), "// Output") {
				break
			}
			lines = append(lines, strings.TrimPrefix(line, "\t"))
		}
		bodies[strings.TrimSpace(strings.Join(lines, "\n"))] = true
	}
	snippets := regexp.MustCompile("(?s)```go\n(.*?)```").FindAllSubmatch(readme, -1)
	checked := 0
	for _, m := range snippets {
		snippet := strings.TrimSpace(string(m[1]))
		if strings.HasPrefix(snippet, "import") {
			continue
		}
		checked++
		if !bodies[snippet] {
			t.Errorf("README snippet is not the body of an Example:\n%s", snippet)
		}
	}
	if checked == 0 {
		t.Errorf("no README snippet was checked")
	}
}
