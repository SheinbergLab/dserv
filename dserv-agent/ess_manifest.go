// ess_manifest.go - Lightweight manifest endpoint for efficient sync
//
// Returns checksums for all scripts in a system version without content,
// allowing clients to compare locally and pull only what's changed.

package main

import (
	"encoding/json"
	"net/http"
	"strings"
	"time"
)

// ScriptManifestEntry is a lightweight script descriptor (no content)
type ScriptManifestEntry struct {
	Protocol  string `json:"protocol"`
	Type      string `json:"type"`
	Filename  string `json:"filename"`
	Checksum  string `json:"checksum"`
	UpdatedAt int64  `json:"updatedAt"`
}

// LibManifestEntry is a lightweight lib descriptor (no content)
type LibManifestEntry struct {
	Name      string `json:"name"`
	Version   string `json:"version"`
	Filename  string `json:"filename"`
	Checksum  string `json:"checksum"`
	UpdatedAt int64  `json:"updatedAt"`
}

// SystemManifest is the response for a manifest request
type SystemManifest struct {
	Workgroup string                `json:"workgroup"`
	System    string                `json:"system"`
	Version   string                `json:"version"`
	UpdatedAt int64                 `json:"updatedAt"`
	Scripts   []ScriptManifestEntry `json:"scripts"`
}

// GetManifest returns checksums for all scripts in a system version
func (r *ESSRegistry) GetManifest(workgroup, system, version string) (*SystemManifest, error) {
	sys, err := r.GetSystem(workgroup, system, version)
	if err != nil {
		return nil, err
	}
	if sys == nil {
		return nil, nil
	}

	rows, err := r.db.Query(`
		SELECT protocol, type, filename, checksum, updated_at
		FROM ess_scripts WHERE system_id = ? ORDER BY protocol, type`, sys.ID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	manifest := &SystemManifest{
		Workgroup: workgroup,
		System:    system,
		Version:   sys.Version,
		UpdatedAt: sys.UpdatedAt.Unix(),
	}

	for rows.Next() {
		var entry ScriptManifestEntry
		if err := rows.Scan(&entry.Protocol, &entry.Type, &entry.Filename,
			&entry.Checksum, &entry.UpdatedAt); err != nil {
			return nil, err
		}
		manifest.Scripts = append(manifest.Scripts, entry)
	}

	return manifest, nil
}

// GetWorkgroupManifest returns manifests for all systems in a workgroup
func (r *ESSRegistry) GetWorkgroupManifest(workgroup, version string) ([]SystemManifest, error) {
	systems, err := r.ListSystems(workgroup)
	if err != nil {
		return nil, err
	}

	var manifests []SystemManifest
	for _, sys := range systems {
		// Skip if version specified and doesn't match
		if version != "" && version != "latest" && sys.Version != version {
			continue
		}

		manifest, err := r.GetManifest(workgroup, sys.Name, sys.Version)
		if err != nil {
			continue
		}
		if manifest != nil {
			manifests = append(manifests, *manifest)
		}
	}

	return manifests, nil
}

// GetLibsManifest returns checksums for all libs in a workgroup (no content)
func (r *ESSRegistry) GetLibsManifest(workgroup string) ([]LibManifestEntry, error) {
	rows, err := r.db.Query(`
		SELECT name, version, filename, checksum, updated_at
		FROM ess_libs WHERE workgroup = ? ORDER BY name, version`, workgroup)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var entries []LibManifestEntry
	for rows.Next() {
		var entry LibManifestEntry
		if err := rows.Scan(&entry.Name, &entry.Version, &entry.Filename,
			&entry.Checksum, &entry.UpdatedAt); err != nil {
			return nil, err
		}
		entries = append(entries, entry)
	}
	return entries, nil
}

// GET /api/v1/ess/manifest/{workgroup}/{system}[?version=main]
// GET /api/v1/ess/manifest/{workgroup}?version=main  (all systems in workgroup)
func (r *ESSRegistry) handleManifest(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/manifest/")
	parts := strings.Split(path, "/")

	if len(parts) < 1 || parts[0] == "" {
		http.Error(w, "Invalid path: need workgroup[/system]", http.StatusBadRequest)
		return
	}

	workgroup := parts[0]
	version := req.URL.Query().Get("version")
	if version == "" {
		version = "main"
	}

	// Single system manifest
	if len(parts) >= 2 && parts[1] != "" {
		system := parts[1]
		manifest, err := r.GetManifest(workgroup, system, version)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		if manifest == nil {
			writeJSON(w, 404, map[string]string{"error": "System not found"})
			return
		}
		writeJSON(w, 200, manifest)
		return
	}

	// Workgroup-wide manifest
	manifests, err := r.GetWorkgroupManifest(workgroup, version)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	// Include libs manifest
	libs, err := r.GetLibsManifest(workgroup)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	writeJSON(w, 200, map[string]interface{}{
		"workgroup": workgroup,
		"version":   version,
		"systems":   manifests,
		"libs":      libs,
		"timestamp": time.Now().Unix(),
	})
}

// POST /api/v1/ess/sync/{workgroup}/{system}
// Client sends its local checksums, server returns only the scripts that differ
//
// Request body:
//   { "checksums": { "colormatch/variants": "a3f2...", "_system/system": "b7c1..." } }
//
// Response:
//   { "stale": [ {script with content}, ... ], "extra": ["colormatch/oldtype", ...] }
//
type SyncRequest struct {
	Checksums map[string]string `json:"checksums"` // key: "protocol/type", value: checksum
	Version   string            `json:"version"`
}

type SyncResponse struct {
	System    string       `json:"system"`
	Version   string       `json:"version"`
	Stale     []*ESSScript `json:"stale"`              // Scripts that need updating (includes content)
	Extra     []string     `json:"extra,omitempty"`     // Keys client has that server doesn't
	Unchanged int          `json:"unchanged"`           // Count of matching scripts
}

func (r *ESSRegistry) handleSync(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/sync/")
	parts := strings.Split(path, "/")

	if len(parts) < 2 {
		http.Error(w, "Invalid path: need workgroup/system", http.StatusBadRequest)
		return
	}

	workgroup := parts[0]
	systemName := parts[1]

	var syncReq SyncRequest
	if err := json.NewDecoder(req.Body).Decode(&syncReq); err != nil {
		writeJSON(w, 400, map[string]string{"error": "Invalid JSON"})
		return
	}

	version := syncReq.Version
	if version == "" {
		version = "main"
	}

	sys, err := r.GetSystem(workgroup, systemName, version)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	if sys == nil {
		writeJSON(w, 404, map[string]string{"error": "System not found"})
		return
	}

	scripts, err := r.GetScripts(sys.ID)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	resp := SyncResponse{
		System:  systemName,
		Version: version,
	}

	// Track which server scripts the client has
	serverKeys := make(map[string]bool)

	for _, script := range scripts {
		key := script.Protocol + "/" + script.Type
		if script.Protocol == "" {
			key = "_system/" + script.Type
		}
		serverKeys[key] = true

		clientChecksum, exists := syncReq.Checksums[key]
		if !exists || clientChecksum != script.Checksum {
			resp.Stale = append(resp.Stale, script)
		} else {
			resp.Unchanged++
		}
	}

	// Find keys client has that server doesn't
	for key := range syncReq.Checksums {
		if !serverKeys[key] {
			resp.Extra = append(resp.Extra, key)
		}
	}

	writeJSON(w, 200, resp)
}
