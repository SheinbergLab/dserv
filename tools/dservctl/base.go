package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"time"
)

// Base-manifest (.sync_base.json) support — the client-side half of the
// merge-ancestor tracking implemented in Tcl's ess_sync (_base_* procs).
//
// The "base" is the registry checksum the last time we pulled a file (or
// the checksum we last pushed). Comparing base vs local vs registry lets
// sync/push tell a stale local copy (safe to overwrite) from genuine
// unpushed edits (preserve) from a true conflict (both changed).
//
// One manifest lives in each directory it describes, keyed by path
// relative to that directory (e.g. "<proto>/<file>" or "<file>") — the
// same scheme dservctl already uses for relpaths and the same on-disk
// format ess_sync reads/writes, so on a combined dev==target rig the two
// tools share a single base per system.
//
// dserv runs as root and chowns its writes back to the tree owner so this
// file stays user-owned; dservctl runs as the user and creates it
// user-owned 0644. Root can always overwrite user-owned files, so no
// chown is needed here — but if a manifest were ever left root-owned,
// writeBaseManifest's rename would fail and the caller surfaces it.

const baseManifestSchema = 1
const baseManifestFile = ".sync_base.json"

// BaseEntry is one tracked file's merge-ancestor record.
type BaseEntry struct {
	Checksum string `json:"checksum"`
	Version  string `json:"version"`
	SyncedAt int64  `json:"syncedAt"`
	SyncedBy string `json:"syncedBy"`
}

// BaseManifest is the per-directory .sync_base.json shared with ess_sync.
type BaseManifest struct {
	SchemaVersion  int                  `json:"schemaVersion"`
	Workgroup      string               `json:"workgroup"`
	DefaultVersion string               `json:"defaultVersion"`
	Entries        map[string]BaseEntry `json:"entries"`
}

func baseManifestPath(dir string) string {
	return filepath.Join(dir, baseManifestFile)
}

// readBaseManifest loads the manifest in dir. A missing file, parse error,
// or workgroup mismatch yields an empty (cold-start) manifest — matching
// the Tcl reader so both tools degrade identically rather than trusting
// stale or foreign-workgroup data.
func readBaseManifest(dir, workgroup string) *BaseManifest {
	empty := &BaseManifest{
		SchemaVersion:  baseManifestSchema,
		Workgroup:      workgroup,
		DefaultVersion: "main",
		Entries:        map[string]BaseEntry{},
	}

	data, err := os.ReadFile(baseManifestPath(dir))
	if err != nil {
		return empty
	}
	var m BaseManifest
	if err := json.Unmarshal(data, &m); err != nil {
		return empty
	}
	if m.Workgroup != "" && m.Workgroup != workgroup {
		return empty
	}
	if m.Entries == nil {
		m.Entries = map[string]BaseEntry{}
	}
	if m.SchemaVersion == 0 {
		m.SchemaVersion = baseManifestSchema
	}
	if m.DefaultVersion == "" {
		m.DefaultVersion = "main"
	}
	return &m
}

// writeBaseManifest writes atomically (temp + rename). The temp name is
// PID-tagged so concurrent/leftover temps don't collide.
func writeBaseManifest(dir string, m *BaseManifest) error {
	if m.SchemaVersion == 0 {
		m.SchemaVersion = baseManifestSchema
	}
	if m.DefaultVersion == "" {
		m.DefaultVersion = "main"
	}
	if m.Entries == nil {
		m.Entries = map[string]BaseEntry{}
	}

	// Encode without HTML escaping so output matches the Tcl writer
	// byte-for-byte for path keys; trailing newline trimmed to match
	// ess_sync's puts -nonewline.
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(m); err != nil {
		return err
	}
	data := bytes.TrimRight(buf.Bytes(), "\n")

	path := baseManifestPath(dir)
	tmp := fmt.Sprintf("%s.tmp.%d", path, os.Getpid())
	if err := os.WriteFile(tmp, data, 0644); err != nil {
		return err
	}
	if err := os.Rename(tmp, path); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

func (m *BaseManifest) get(key string) string {
	if e, ok := m.Entries[key]; ok {
		return e.Checksum
	}
	return ""
}

func (m *BaseManifest) set(key, checksum, version, by string) {
	if version == "" {
		version = m.DefaultVersion
	}
	m.Entries[key] = BaseEntry{
		Checksum: checksum,
		Version:  version,
		SyncedAt: time.Now().Unix(),
		SyncedBy: by,
	}
}

func (m *BaseManifest) unset(key string) {
	delete(m.Entries, key)
}

// baseDecide is the 3-way decision shared with ess_sync's _base_decide.
// "" means absent. Returns: unchanged | pull | keep_local | conflict | cold
func baseDecide(base, local, registry string) string {
	if local == registry {
		return "unchanged"
	}
	if base == "" {
		return "cold"
	}
	if base == local {
		return "pull"
	}
	if base == registry {
		return "keep_local"
	}
	return "conflict"
}

// hashBytes returns the lowercase hex SHA-256 of data (matches the
// registry's checksum and ess_sync's sha256).
func hashBytes(data []byte) string {
	return fmt.Sprintf("%x", sha256.Sum256(data))
}

// scriptFilename reconstructs a script's filename from (system, protocol,
// type) using the same convention as ess_sync's _script_filename. Used to
// derive the manifest key when only protocol+type are known (e.g. delete).
func scriptFilename(system, protocol, stype string) string {
	if protocol == "" || protocol == "_" || protocol == "_system" {
		switch stype {
		case "system":
			return system + ".tcl"
		case "extract":
			return system + "_extract.tcl"
		case "viewer":
			return system + "_viewer.js"
		default:
			return system + "_" + stype + ".tcl"
		}
	}
	switch stype {
	case "protocol":
		return protocol + ".tcl"
	case "viewer":
		return protocol + "_viewer.js"
	default:
		return protocol + "_" + stype + ".tcl"
	}
}

// versionOrMain defaults an empty version to "main".
func versionOrMain(v string) string {
	if v == "" {
		return "main"
	}
	return v
}

// syncedByOrDefault returns the user for syncedBy, or a tool marker.
func syncedByOrDefault(user string) string {
	if user != "" {
		return user
	}
	return "dservctl"
}
