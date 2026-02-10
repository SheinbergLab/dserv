// ess_browse.go - Browse endpoint and handler for ESS Script Registry
//
// Provides endpoints for browsing and comparing ESS experiment scripts:
//
//   Registry server (has ESS database):
//     GET /api/v1/ess/browse/{workgroup}?version=main&base=  — full script tree
//
//   Any agent (client or server):
//     GET /api/ess/browse-config  — tells the browse page where to find the registry
//
// When a `base` version is specified (e.g., ?version=dave-dev&base=main),
// each script is annotated with a diff status relative to the base version,
// enabling overlay visualization in the browse UI.

package main

import (
	"net/http"
	"strings"
)

// ============ Browse Models ============

// BrowseScript is a lightweight script entry for tree display
type BrowseScript struct {
	Protocol   string `json:"protocol"`
	Type       string `json:"type"`
	Filename   string `json:"filename"`
	Checksum   string `json:"checksum"`
	UpdatedAt  int64  `json:"updatedAt"`
	UpdatedBy  string `json:"updatedBy"`
	DiffStatus string `json:"diffStatus,omitempty"` // "modified", "added", "same", "deleted"
}

// BrowseSystem represents a system with its scripts grouped by protocol
type BrowseSystem struct {
	Name        string                    `json:"name"`
	Version     string                    `json:"version"`
	Versions    []string                  `json:"versions"`
	Description string                    `json:"description,omitempty"`
	Author      string                    `json:"author,omitempty"`
	ForkedFrom  string                    `json:"forkedFrom,omitempty"`
	UpdatedAt   int64                     `json:"updatedAt"`
	UpdatedBy   string                    `json:"updatedBy"`
	Scripts     map[string][]BrowseScript `json:"scripts"` // key: protocol ("_system" for system-level)
	DiffSummary *DiffSummary              `json:"diffSummary,omitempty"`
}

// DiffSummary provides counts of changed scripts for a system
type DiffSummary struct {
	Modified int `json:"modified"`
	Added    int `json:"added"`
	Deleted  int `json:"deleted"`
	Same     int `json:"same"`
}

// BrowseTree is the response for the browse endpoint
type BrowseTree struct {
	Workgroup   string          `json:"workgroup"`
	Version     string          `json:"version"`
	BaseVersion string          `json:"baseVersion,omitempty"`
	Systems     []*BrowseSystem `json:"systems"`
}

// ============ Browse Logic ============

// GetBrowseTree returns the full script tree for a workgroup.
// If baseVersion is non-empty, scripts are annotated with diff status.
func (r *ESSRegistry) GetBrowseTree(workgroup, version, baseVersion string) (*BrowseTree, error) {
	if version == "" {
		version = "main"
	}

	systems, err := r.ListSystems(workgroup)
	if err != nil {
		return nil, err
	}

	tree := &BrowseTree{
		Workgroup:   workgroup,
		Version:     version,
		BaseVersion: baseVersion,
		Systems:     make([]*BrowseSystem, 0),
	}

	// Group systems by name to find all versions
	versionsByName := make(map[string][]string)
	systemsByNameVersion := make(map[string]*ESSSystem)
	for _, sys := range systems {
		versionsByName[sys.Name] = append(versionsByName[sys.Name], sys.Version)
		systemsByNameVersion[sys.Name+"/"+sys.Version] = sys
	}

	seen := make(map[string]bool)
	for _, sys := range systems {
		if seen[sys.Name] {
			continue
		}
		seen[sys.Name] = true

		targetSys := systemsByNameVersion[sys.Name+"/"+version]
		if targetSys == nil {
			targetSys = systemsByNameVersion[sys.Name+"/main"]
		}
		if targetSys == nil {
			targetSys = sys
		}

		scripts, err := r.getBrowseScripts(targetSys.ID)
		if err != nil {
			return nil, err
		}

		var diffSummary *DiffSummary
		if baseVersion != "" && baseVersion != version {
			baseSys := systemsByNameVersion[sys.Name+"/"+baseVersion]
			if baseSys != nil {
				diffSummary = r.annotateDiffs(scripts, baseSys.ID)
			}
		}

		browseSys := &BrowseSystem{
			Name:        targetSys.Name,
			Version:     targetSys.Version,
			Versions:    versionsByName[targetSys.Name],
			Description: targetSys.Description,
			Author:      targetSys.Author,
			ForkedFrom:  targetSys.ForkedFrom,
			UpdatedAt:   targetSys.UpdatedAt.Unix(),
			UpdatedBy:   targetSys.UpdatedBy,
			Scripts:     scripts,
			DiffSummary: diffSummary,
		}

		tree.Systems = append(tree.Systems, browseSys)
	}

	return tree, nil
}

// getBrowseScripts fetches script metadata (no content) grouped by protocol
func (r *ESSRegistry) getBrowseScripts(systemID int64) (map[string][]BrowseScript, error) {
	rows, err := r.db.Query(`
		SELECT protocol, type, filename, checksum, updated_at, updated_by
		FROM ess_scripts WHERE system_id = ? ORDER BY protocol, type`, systemID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	scripts := make(map[string][]BrowseScript)
	for rows.Next() {
		var s BrowseScript
		if err := rows.Scan(&s.Protocol, &s.Type, &s.Filename,
			&s.Checksum, &s.UpdatedAt, &s.UpdatedBy); err != nil {
			return nil, err
		}
		key := s.Protocol
		if key == "" {
			key = "_system"
		}
		scripts[key] = append(scripts[key], s)
	}
	return scripts, nil
}

// annotateDiffs compares scripts against a base version and sets DiffStatus.
// Also injects "deleted" entries for scripts present in base but not in target.
func (r *ESSRegistry) annotateDiffs(scripts map[string][]BrowseScript, baseSystemID int64) *DiffSummary {
	type baseInfo struct {
		checksum  string
		filename  string
		updatedAt int64
		updatedBy string
	}
	baseScripts := make(map[string]baseInfo) // key: "protocol/type"

	rows, err := r.db.Query(`
		SELECT protocol, type, filename, checksum, updated_at, updated_by
		FROM ess_scripts WHERE system_id = ? ORDER BY protocol, type`, baseSystemID)
	if err != nil {
		return nil
	}
	defer rows.Close()

	for rows.Next() {
		var protocol, stype, filename, checksum, updatedBy string
		var updatedAt int64
		if err := rows.Scan(&protocol, &stype, &filename, &checksum, &updatedAt, &updatedBy); err != nil {
			continue
		}
		key := protocol + "/" + stype
		baseScripts[key] = baseInfo{checksum, filename, updatedAt, updatedBy}
	}

	summary := &DiffSummary{}
	seenBaseKeys := make(map[string]bool)

	for proto, scriptList := range scripts {
		for i := range scriptList {
			s := &scriptList[i]
			rawProto := s.Protocol
			key := rawProto + "/" + s.Type

			base, inBase := baseScripts[key]
			seenBaseKeys[key] = true

			if !inBase {
				s.DiffStatus = "added"
				summary.Added++
			} else if s.Checksum != base.checksum {
				s.DiffStatus = "modified"
				summary.Modified++
			} else {
				s.DiffStatus = "same"
				summary.Same++
			}
		}
		scripts[proto] = scriptList
	}

	// Inject deleted entries (in base but not in target)
	for key, base := range baseScripts {
		if seenBaseKeys[key] {
			continue
		}
		summary.Deleted++

		parts := strings.SplitN(key, "/", 2)
		proto := parts[0]
		stype := parts[1]

		groupKey := proto
		if groupKey == "" {
			groupKey = "_system"
		}

		scripts[groupKey] = append(scripts[groupKey], BrowseScript{
			Protocol:   proto,
			Type:       stype,
			Filename:   base.filename,
			Checksum:   base.checksum,
			UpdatedAt:  base.updatedAt,
			UpdatedBy:  base.updatedBy,
			DiffStatus: "deleted",
		})
	}

	return summary
}

// ============ HTTP Handlers ============

// RegisterBrowseHandlers registers browse endpoints on the registry server
// (the machine that has the ESS database).
func (r *ESSRegistry) RegisterBrowseHandlers(mux *http.ServeMux, authMiddleware func(http.HandlerFunc) http.HandlerFunc) {
	mux.HandleFunc("/api/v1/ess/browse/", authMiddleware(r.handleBrowse))
}

// GET /api/v1/ess/browse/{workgroup}?version=main&base=
func (r *ESSRegistry) handleBrowse(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/browse/")
	parts := strings.Split(path, "/")

	if len(parts) < 1 || parts[0] == "" {
		http.Error(w, "Invalid path: need workgroup", http.StatusBadRequest)
		return
	}

	workgroup := parts[0]
	version := req.URL.Query().Get("version")
	if version == "" {
		version = "main"
	}
	baseVersion := req.URL.Query().Get("base")

	tree, err := r.GetBrowseTree(workgroup, version, baseVersion)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	workgroups, err := r.ListWorkgroups()
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	writeJSON(w, 200, map[string]interface{}{
		"tree":       tree,
		"workgroups": workgroups,
	})
}

// ListWorkgroups returns all distinct workgroups (excluding _templates)
func (r *ESSRegistry) ListWorkgroups() ([]string, error) {
	rows, err := r.db.Query(`SELECT DISTINCT workgroup FROM ess_systems 
	                          WHERE workgroup != ? ORDER BY workgroup`, TemplatesWorkgroup)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var workgroups []string
	for rows.Next() {
		var wg string
		if err := rows.Scan(&wg); err != nil {
			return nil, err
		}
		workgroups = append(workgroups, wg)
	}
	return workgroups, nil
}

// ============ Agent-side config endpoint ============
// This lives on the Agent, not the ESSRegistry, because every agent
// can serve it regardless of whether it has a local ESS database.

// handleESSBrowseConfig returns the registry URL and workgroup so the
// browse page knows where to send its ESS API requests.
//
// GET /api/ess/browse-config
//
// Response:
//   {
//     "registryURL": "https://dserv.net",
//     "workgroup": "brown-sheinberg",
//     "local": false
//   }
//
// When the agent IS the registry server (has --ess-registry), it returns
// local=true and no registryURL, telling the page to use relative URLs.
func (a *Agent) handleESSBrowseConfig(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	resp := map[string]interface{}{
		"workgroup": a.cfg.Workgroup,
	}

	if a.cfg.ESSRegistryPath != "" {
		// This agent IS the registry — browse page should use relative URLs
		resp["local"] = true
		resp["registryURL"] = ""
	} else if len(a.cfg.RegistryURLs) > 0 {
		// This agent talks to a remote registry — tell the page where
		resp["local"] = false
		resp["registryURL"] = normalizeURL(a.cfg.RegistryURLs[0])
	} else {
		resp["local"] = false
		resp["registryURL"] = ""
		resp["error"] = "No ESS registry configured"
	}

	writeJSON(w, 200, resp)
}
