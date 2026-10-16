package main

import (
	"crypto/sha256"
	"fmt"
	"net/http/httptest"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func sum(s string) string { return fmt.Sprintf("%x", sha256.Sum256([]byte(s))) }

// pruneDir makes a lib dir with a base manifest:
//
//	kept-1.0.tm      synced, still in the registry
//	gone-1.0.tm      synced, deleted from the registry, unchanged  -> prune
//	edited-1.0.tm    synced, deleted from the registry, edited     -> keep
//	handmade-1.0.tm  no base entry (never synced)                  -> untouched
//	vanished-1.0.tm  base entry only, file gone everywhere         -> stale entry
func pruneDir(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	write := func(f, s string) {
		if err := os.WriteFile(filepath.Join(dir, f), []byte(s), 0644); err != nil {
			t.Fatal(err)
		}
	}
	write("kept-1.0.tm", "kept")
	write("gone-1.0.tm", "gone")
	write("edited-1.0.tm", "edited locally")
	write("handmade-1.0.tm", "mine")
	m := &BaseManifest{Workgroup: "wg", Entries: map[string]BaseEntry{}}
	m.set("kept-1.0.tm", sum("kept"), "1.0", "sync")
	m.set("gone-1.0.tm", sum("gone"), "1.0", "sync")
	m.set("edited-1.0.tm", sum("edited"), "1.0", "sync")
	m.set("vanished-1.0.tm", sum("vanished"), "1.0", "sync")
	if err := writeBaseManifest(dir, m); err != nil {
		t.Fatal(err)
	}
	return dir
}

func TestPlanLibsPrune(t *testing.T) {
	dir := pruneDir(t)
	m := readBaseManifest(dir, "wg")
	cands, stale, err := planLibsPrune(dir, m, map[string]bool{"kept-1.0.tm": true})
	if err != nil {
		t.Fatal(err)
	}
	want := []pruneCandidate{{"edited-1.0.tm", "keep_modified"}, {"gone-1.0.tm", "prune"}}
	if !reflect.DeepEqual(cands, want) {
		t.Errorf("candidates = %v, want %v", cands, want)
	}
	if !reflect.DeepEqual(stale, []string{"vanished-1.0.tm"}) {
		t.Errorf("stale = %v", stale)
	}
}

func runPrune(t *testing.T, f *fakeRegistry, terminal bool, answer string, args ...string) int {
	t.Helper()
	srv := httptest.NewServer(f.handler())
	defer srv.Close()
	oldIn, oldTerm := confirmIn, isTerminal
	confirmIn = strings.NewReader(answer)
	isTerminal = func() bool { return terminal }
	defer func() { confirmIn, isTerminal = oldIn, oldTerm }()
	return runLibsPrune(&Config{Registry: srv.URL, Workgroup: "wg"}, args)
}

func exists(p string) bool { _, err := os.Stat(p); return err == nil }

func TestLibsPruneFlow(t *testing.T) {
	reg := func() *fakeRegistry { return &fakeRegistry{libs: testLibs("kept-1.0.tm", "other-1.0.tm")} }

	t.Run("dry run moves nothing", func(t *testing.T) {
		dir := pruneDir(t)
		if rc := runPrune(t, reg(), true, "", "--dir", dir, "--dry-run"); rc != 0 {
			t.Fatalf("rc=%d", rc)
		}
		if !exists(filepath.Join(dir, "gone-1.0.tm")) {
			t.Error("dry run moved a file")
		}
	})
	t.Run("no terminal and no --yes refuses", func(t *testing.T) {
		dir := pruneDir(t)
		if rc := runPrune(t, reg(), false, "", "--dir", dir); rc == 0 || !exists(filepath.Join(dir, "gone-1.0.tm")) {
			t.Fatalf("rc=%d, gone present=%v", rc, exists(filepath.Join(dir, "gone-1.0.tm")))
		}
	})
	t.Run("empty registry list refuses", func(t *testing.T) {
		dir := pruneDir(t)
		if rc := runPrune(t, &fakeRegistry{}, false, "", "--dir", dir, "--yes"); rc == 0 || !exists(filepath.Join(dir, "gone-1.0.tm")) {
			t.Fatalf("pruned against an empty registry (rc=%d)", rc)
		}
	})
	t.Run("--yes moves only the unchanged synced copy", func(t *testing.T) {
		root := t.TempDir()
		dir := filepath.Join(root, "lib")
		os.Rename(pruneDir(t), dir)
		if rc := runPrune(t, reg(), false, "", "--dir", dir, "--yes"); rc != 0 {
			t.Fatalf("rc=%d", rc)
		}
		if exists(filepath.Join(dir, "gone-1.0.tm")) {
			t.Error("gone-1.0.tm still in lib")
		}
		for _, f := range []string{"kept-1.0.tm", "edited-1.0.tm", "handmade-1.0.tm"} {
			if !exists(filepath.Join(dir, f)) {
				t.Errorf("%s was removed", f)
			}
		}
		moved, _ := filepath.Glob(filepath.Join(root, ".lib_pruned", "*", "gone-1.0.tm"))
		if len(moved) != 1 {
			t.Errorf("gone-1.0.tm not found under .lib_pruned: %v", moved)
		}
		m := readBaseManifest(dir, "wg")
		for _, k := range []string{"gone-1.0.tm", "vanished-1.0.tm"} {
			if _, ok := m.Entries[k]; ok {
				t.Errorf("%s still in the manifest", k)
			}
		}
		for _, k := range []string{"kept-1.0.tm", "edited-1.0.tm"} {
			if _, ok := m.Entries[k]; !ok {
				t.Errorf("%s dropped from the manifest", k)
			}
		}
	})
}

// After a registry delete, a dservctl sync must keep the base entry of a
// lib whose file is still on disk (prune needs it) and drop the entry of
// one that is gone.
func TestSyncKeepsBaseEntryWhileFileExists(t *testing.T) {
	dir := pruneDir(t)
	libs := testLibs("kept-1.0.tm")
	libs[0]["checksum"] = sum("kept")
	srv := httptest.NewServer((&fakeRegistry{libs: libs}).handler())
	defer srv.Close()
	cfg := &Config{Registry: srv.URL, Workgroup: "wg"}
	r := syncLibsToDir(cfg, NewRegistryClient(cfg), dir, false, false)
	if r.errors != 0 {
		t.Fatalf("sync errors: %d", r.errors)
	}
	// gone-1.0.tm is an unchanged synced copy; edited-1.0.tm is edited.
	if r.prunable != 1 {
		t.Errorf("sync reported %d prunable lib(s), want 1", r.prunable)
	}
	m := readBaseManifest(dir, "wg")
	if _, ok := m.Entries["gone-1.0.tm"]; !ok {
		t.Error("sync dropped the base entry of a file still on disk")
	}
	if _, ok := m.Entries["vanished-1.0.tm"]; ok {
		t.Error("sync kept the base entry of a file that is gone")
	}
}
