package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"sync"
	"testing"
)

func testLibs(files ...string) []map[string]interface{} {
	var out []map[string]interface{}
	for _, f := range files {
		base := strings.TrimSuffix(f, ".tm")
		i := strings.LastIndex(base, "-")
		out = append(out, map[string]interface{}{"name": base[:i], "version": base[i+1:], "filename": f})
	}
	return out
}

func targetFiles(ts []libTarget) []string {
	var out []string
	for _, t := range ts {
		out = append(out, t.filename)
	}
	return out
}

func TestResolveLibTargets(t *testing.T) {
	libs := testLibs("planko-3.0.tm", "planko-2.0.tm", "planko_bank_a_000-1.0.tm",
		"planko_bank_a_001-1.0.tm", "planko_bank_store-1.0.tm", "runner-1.0.tm")
	cases := []struct {
		name    string
		names   []string
		globs   []string
		want    []string
		wantErr string
	}{
		{"filename", []string{"runner-1.0.tm"}, nil, []string{"runner-1.0.tm"}, ""},
		{"bare name, one version", []string{"runner"}, nil, []string{"runner-1.0.tm"}, ""},
		{"bare name, several versions", []string{"planko"}, nil, nil, "has 2 versions"},
		{"unknown name", []string{"nope"}, nil, nil, "no library"},
		{"glob on filename", nil, []string{"planko_bank_*"}, []string{
			"planko_bank_a_000-1.0.tm", "planko_bank_a_001-1.0.tm", "planko_bank_store-1.0.tm"}, ""},
		{"glob on name", nil, []string{"planko_bank_a_00?"}, []string{
			"planko_bank_a_000-1.0.tm", "planko_bank_a_001-1.0.tm"}, ""},
		{"glob matching nothing", nil, []string{"sling_*"}, nil, "matches no library"},
		{"bad glob", nil, []string{"[oops"}, nil, "bad pattern"},
		{"overlap de-duplicated", []string{"planko_bank_store-1.0.tm"}, []string{"planko_bank_store*"},
			[]string{"planko_bank_store-1.0.tm"}, ""},
		{"one bad argument fails all", []string{"runner", "nope"}, nil, nil, "no library"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			got, err := resolveLibTargets(libs, c.names, c.globs)
			if c.wantErr != "" {
				if err == nil || !strings.Contains(err.Error(), c.wantErr) {
					t.Fatalf("err = %v, want containing %q", err, c.wantErr)
				}
				return
			}
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			if f := targetFiles(got); !reflect.DeepEqual(f, c.want) {
				t.Errorf("got %v, want %v", f, c.want)
			}
		})
	}
}

// fakeRegistry serves GET /libs and DELETE /lib/{wg}/{name}/{version},
// recording deletes; failOn names a lib whose delete returns 500.
type fakeRegistry struct {
	mu      sync.Mutex
	libs    []map[string]interface{}
	deleted []string
	failOn  string
}

func (f *fakeRegistry) handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/api/v1/ess/libs", func(w http.ResponseWriter, r *http.Request) {
		f.mu.Lock()
		defer f.mu.Unlock()
		json.NewEncoder(w).Encode(map[string]interface{}{"libs": f.libs})
	})
	mux.HandleFunc("/api/v1/ess/lib/", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodDelete {
			http.Error(w, "method", 405)
			return
		}
		parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/api/v1/ess/lib/"), "/")
		file := parts[1] + "-" + parts[2] + ".tm"
		f.mu.Lock()
		defer f.mu.Unlock()
		if parts[1] == f.failOn {
			w.WriteHeader(500)
			json.NewEncoder(w).Encode(map[string]string{"error": "boom"})
			return
		}
		f.deleted = append(f.deleted, parts[0]+"/"+file)
		json.NewEncoder(w).Encode(map[string]interface{}{"success": true})
	})
	return mux
}

func runDelete(t *testing.T, f *fakeRegistry, terminal bool, answer string, args ...string) int {
	t.Helper()
	srv := httptest.NewServer(f.handler())
	defer srv.Close()
	oldIn, oldTerm := confirmIn, isTerminal
	confirmIn = strings.NewReader(answer)
	isTerminal = func() bool { return terminal }
	defer func() { confirmIn, isTerminal = oldIn, oldTerm }()
	cfg := &Config{Registry: srv.URL, Workgroup: "wg"}
	return runLibsDelete(cfg, args)
}

func TestLibsDeleteFlow(t *testing.T) {
	all := []string{"planko_bank_a_000-1.0.tm", "planko_bank_a_001-1.0.tm", "runner-1.0.tm"}

	t.Run("dry run deletes nothing", func(t *testing.T) {
		f := &fakeRegistry{libs: testLibs(all...)}
		if rc := runDelete(t, f, true, "", "--dry-run", "--match", "planko_bank_*"); rc != 0 || len(f.deleted) != 0 {
			t.Fatalf("rc=%d deleted=%v", rc, f.deleted)
		}
	})
	t.Run("no terminal and no --yes refuses", func(t *testing.T) {
		f := &fakeRegistry{libs: testLibs(all...)}
		if rc := runDelete(t, f, false, "", "--match", "planko_bank_*"); rc == 0 || len(f.deleted) != 0 {
			t.Fatalf("rc=%d deleted=%v", rc, f.deleted)
		}
	})
	t.Run("answering no deletes nothing", func(t *testing.T) {
		f := &fakeRegistry{libs: testLibs(all...)}
		if rc := runDelete(t, f, true, "n\n", "--match", "planko_bank_*"); rc == 0 || len(f.deleted) != 0 {
			t.Fatalf("rc=%d deleted=%v", rc, f.deleted)
		}
	})
	t.Run("answering yes deletes the matches only", func(t *testing.T) {
		f := &fakeRegistry{libs: testLibs(all...)}
		if rc := runDelete(t, f, true, "y\n", "--match", "planko_bank_*"); rc != 0 {
			t.Fatalf("rc=%d", rc)
		}
		sort.Strings(f.deleted)
		want := []string{"wg/planko_bank_a_000-1.0.tm", "wg/planko_bank_a_001-1.0.tm"}
		if !reflect.DeepEqual(f.deleted, want) {
			t.Errorf("deleted %v, want %v", f.deleted, want)
		}
	})
	t.Run("--yes with --dir prunes the base manifest, keeps local files", func(t *testing.T) {
		dir := t.TempDir()
		m := &BaseManifest{Workgroup: "wg", Entries: map[string]BaseEntry{}}
		for _, f := range all {
			m.set(f, "abc", "1.0", "sync")
		}
		if err := writeBaseManifest(dir, m); err != nil {
			t.Fatal(err)
		}
		local := filepath.Join(dir, "planko_bank_a_000-1.0.tm")
		os.WriteFile(local, []byte("# x\n"), 0644)

		f := &fakeRegistry{libs: testLibs(all...)}
		if rc := runDelete(t, f, false, "", "--yes", "--dir", dir, "--match", "planko_bank_*"); rc != 0 {
			t.Fatalf("rc=%d", rc)
		}
		got := readBaseManifest(dir, "wg")
		if _, ok := got.Entries["runner-1.0.tm"]; !ok || len(got.Entries) != 1 {
			t.Errorf("manifest entries after delete = %v, want only runner-1.0.tm", got.Entries)
		}
		if _, err := os.Stat(local); err != nil {
			t.Errorf("local file was removed: %v", err)
		}
	})
	t.Run("a failed delete is reported and kept in the manifest", func(t *testing.T) {
		dir := t.TempDir()
		m := &BaseManifest{Workgroup: "wg", Entries: map[string]BaseEntry{}}
		for _, f := range all {
			m.set(f, "abc", "1.0", "sync")
		}
		writeBaseManifest(dir, m)
		f := &fakeRegistry{libs: testLibs(all...), failOn: "planko_bank_a_001"}
		if rc := runDelete(t, f, false, "", "--yes", "--dir", dir, "--match", "planko_bank_*"); rc == 0 {
			t.Fatalf("rc=0 despite a failed delete")
		}
		got := readBaseManifest(dir, "wg")
		if _, ok := got.Entries["planko_bank_a_001-1.0.tm"]; !ok {
			t.Errorf("failed delete was dropped from the manifest")
		}
		if _, ok := got.Entries["planko_bank_a_000-1.0.tm"]; ok {
			t.Errorf("successful delete still in the manifest")
		}
	})
	t.Run("unknown name aborts before deleting anything", func(t *testing.T) {
		f := &fakeRegistry{libs: testLibs(all...)}
		if rc := runDelete(t, f, false, "", "--yes", "runner", "nope"); rc == 0 || len(f.deleted) != 0 {
			t.Fatalf("rc=%d deleted=%v", rc, f.deleted)
		}
	})
}
