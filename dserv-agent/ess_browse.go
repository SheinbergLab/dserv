// ess_browse.go - Browse endpoint and handler for ESS Script Registry
//
// Provides endpoints for browsing and comparing ESS experiment scripts:
//   - GET /api/v1/ess/browse/{workgroup}?version=main&base=  — full script tree
//   - GET /api/v1/ess/browse/config                          — default workgroup for client mode
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

	// Get all systems for this workgroup
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

	// For each unique system name, get the requested version
	seen := make(map[string]bool)
	for _, sys := range systems {
		if seen[sys.Name] {
			continue
		}
		seen[sys.Name] = true

		// Find the target version
		targetSys := systemsByNameVersion[sys.Name+"/"+version]
		if targetSys == nil {
			targetSys = systemsByNameVersion[sys.Name+"/main"]
		}
		if targetSys == nil {
			targetSys = sys
		}

		// Get scripts (metadata only — no content)
		scripts, err := r.getBrowseScripts(targetSys.ID)
		if err != nil {
			return nil, err
		}

		// If base version specified and differs from current, compute diff
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
	// Build checksum map from base version
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

	// Track which base scripts we've seen
	seenBaseKeys := make(map[string]bool)

	// Annotate each script in the target version
	for proto, scriptList := range scripts {
		for i := range scriptList {
			s := &scriptList[i]
			rawProto := s.Protocol // empty string for _system
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

	// Find scripts in base that are not in target (deleted in overlay)
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

// RegisterBrowseHandlers registers the browse endpoint and config endpoint.
// defaultWorkgroup is the workgroup from the agent's --workgroup flag (may be empty on the registry server).
func (r *ESSRegistry) RegisterBrowseHandlers(mux *http.ServeMux, authMiddleware func(http.HandlerFunc) http.HandlerFunc, defaultWorkgroup string) {
	mux.HandleFunc("/api/v1/ess/browse/config", authMiddleware(func(w http.ResponseWriter, req *http.Request) {
		r.handleBrowseConfig(w, req, defaultWorkgroup)
	}))
	mux.HandleFunc("/api/v1/ess/browse/", authMiddleware(r.handleBrowse))
}

// GET /api/v1/ess/browse/config
// Returns default workgroup and available workgroups for UI bootstrapping.
// On a client-mode agent, defaultWorkgroup will be set from --workgroup,
// so the browse page can auto-select it without user interaction.
func (r *ESSRegistry) handleBrowseConfig(w http.ResponseWriter, req *http.Request, defaultWorkgroup string) {
	if req.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	workgroups, err := r.ListWorkgroups()
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	writeJSON(w, 200, map[string]interface{}{
		"defaultWorkgroup": defaultWorkgroup,
		"workgroups":       workgroups,
	})
}

// GET /api/v1/ess/browse/{workgroup}?version=main&base=
// Returns the full script tree for a workgroup.
// If `base` is specified, scripts are annotated with diff status relative to
// the base version (typically "main"), enabling overlay diff visualization.
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

	// Include available workgroups for the selector
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
