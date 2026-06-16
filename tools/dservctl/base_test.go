package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestBaseDecide(t *testing.T) {
	cases := []struct {
		base, local, reg, want string
	}{
		{"abc", "abc", "abc", "unchanged"},
		{"abc", "abc", "xyz", "pull"},       // local stale, registry moved
		{"abc", "xyz", "abc", "keep_local"}, // local edits, registry unchanged
		{"abc", "def", "xyz", "conflict"},   // both moved
		{"", "xyz", "abc", "cold"},          // no base, differs
		{"", "abc", "abc", "unchanged"},     // no base but already equal
	}
	for _, c := range cases {
		if got := baseDecide(c.base, c.local, c.reg); got != c.want {
			t.Errorf("baseDecide(%q,%q,%q)=%q want %q", c.base, c.local, c.reg, got, c.want)
		}
	}
}

// A real manifest as written by the Tcl ess_sync side (verbatim).
const tclWrittenManifest = `{"schemaVersion":1,"workgroup":"brown-sheinberg","defaultVersion":"main","entries":{"search.tcl":{"checksum":"127707c6343f9c03a6a6e04f926e8a12a1531b4e5d744223563ffa84357420ee","version":"main","syncedAt":1781652661,"syncedBy":"sync"},"circles/circles.tcl":{"checksum":"20abb5a10b66303d68b75557544663ca21af1a3dba753813e7214fc6c6d92a71","version":"main","syncedAt":1781652661,"syncedBy":"sync"}}}`

// Go must read a Tcl-written manifest, and its own write-back must preserve
// every entry — this is what guarantees the two tools share one base.
func TestReadTclManifestRoundTrip(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, baseManifestFile), []byte(tclWrittenManifest), 0644); err != nil {
		t.Fatal(err)
	}

	m := readBaseManifest(dir, "brown-sheinberg")
	if len(m.Entries) != 2 {
		t.Fatalf("expected 2 entries, got %d", len(m.Entries))
	}
	if m.get("search.tcl") != "127707c6343f9c03a6a6e04f926e8a12a1531b4e5d744223563ffa84357420ee" {
		t.Errorf("system-level key checksum wrong: %q", m.get("search.tcl"))
	}
	if m.get("circles/circles.tcl") != "20abb5a10b66303d68b75557544663ca21af1a3dba753813e7214fc6c6d92a71" {
		t.Errorf("protocol key checksum wrong: %q", m.get("circles/circles.tcl"))
	}

	// Write back and re-read; entries must be identical.
	if err := writeBaseManifest(dir, m); err != nil {
		t.Fatal(err)
	}
	m2 := readBaseManifest(dir, "brown-sheinberg")
	if len(m2.Entries) != len(m.Entries) {
		t.Fatalf("round-trip entry count drift: %d -> %d", len(m.Entries), len(m2.Entries))
	}
	for k, e := range m.Entries {
		if m2.Entries[k] != e {
			t.Errorf("round-trip entry %q changed: %+v -> %+v", k, e, m2.Entries[k])
		}
	}
}

// A manifest from a different workgroup must be ignored (cold start), never
// trusted — otherwise stale foreign checksums would cause false conflicts.
func TestWorkgroupMismatchIgnored(t *testing.T) {
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, baseManifestFile), []byte(tclWrittenManifest), 0644)
	m := readBaseManifest(dir, "some-other-lab")
	if len(m.Entries) != 0 {
		t.Errorf("expected empty manifest on workgroup mismatch, got %d entries", len(m.Entries))
	}
}

// Persist a Go-written manifest to a fixed path so an external strict parser
// (Python) can confirm it is valid JSON with the camelCase keys the Tcl
// reader expects. Set DSERVCTL_INTEROP_OUT to enable.
func TestEmitInteropArtifact(t *testing.T) {
	out := os.Getenv("DSERVCTL_INTEROP_OUT")
	if out == "" {
		t.Skip("DSERVCTL_INTEROP_OUT not set")
	}
	m := readBaseManifestFromString(t, tclWrittenManifest)
	m.set("new/added.tcl", "deadbeef", "main", "dservctl")
	if err := writeBaseManifest(out, m); err != nil {
		t.Fatal(err)
	}
}

func readBaseManifestFromString(t *testing.T, s string) *BaseManifest {
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, baseManifestFile), []byte(s), 0644)
	return readBaseManifest(dir, "brown-sheinberg")
}
