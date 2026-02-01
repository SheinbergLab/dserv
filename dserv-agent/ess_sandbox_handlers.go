// ess_sandbox_handlers.go - Handlers for sandbox/version management

package main

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"time"
)

// ============ Request/Response Types ============

type CreateSandboxRequest struct {
	FromVersion string `json:"fromVersion"` // Version to copy from (usually "main")
	ToVersion   string `json:"toVersion"`   // New version name (usually username)
	Username    string `json:"username"`
	Comment     string `json:"comment,omitempty"`
}

type PromoteSandboxRequest struct {
	FromVersion string `json:"fromVersion"` // Sandbox version (e.g., "david")
	ToVersion   string `json:"toVersion"`   // Target version (usually "main")
	Username    string `json:"username"`
	Comment     string `json:"comment,omitempty"`
}

type SyncSandboxRequest struct {
	FromVersion string `json:"fromVersion"` // Version to pull from (usually "main")
	ToVersion   string `json:"toVersion"`   // Sandbox to update (e.g., "david")
	Username    string `json:"username"`
	Conflicts   string `json:"conflicts,omitempty"` // "overwrite", "keep", "merge" (for future)
}

type VersionInfo struct {
	Version     string    `json:"version"`
	Description string    `json:"description,omitempty"`
	UpdatedAt   time.Time `json:"updatedAt"`
	UpdatedBy   string    `json:"updatedBy"`
	ScriptCount int       `json:"scriptCount"`
	IsMain      bool      `json:"isMain"`
	IsSandbox   bool      `json:"isSandbox"`
}

// ============ Handlers ============

// POST /api/v1/ess/sandbox/{workgroup}/{system}/create
// Creates a new sandbox by copying an existing version
func (reg *ESSRegistry) handleCreateSandbox(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	// Parse path: /api/v1/ess/sandbox/{workgroup}/{system}/create
	parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/api/v1/ess/sandbox/"), "/")
	if len(parts) < 3 {
		http.Error(w, "Invalid path", http.StatusBadRequest)
		return
	}
	workgroup := parts[0]
	system := parts[1]

	var req CreateSandboxRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid request body", http.StatusBadRequest)
		return
	}

	// Validate
	if req.FromVersion == "" {
		req.FromVersion = "main"
	}
	if req.ToVersion == "" {
		http.Error(w, "toVersion required", http.StatusBadRequest)
		return
	}
	if req.Username == "" {
		http.Error(w, "username required", http.StatusBadRequest)
		return
	}

	// Check if target version already exists
	var existingID int64
	err := reg.db.QueryRow(`
		SELECT id FROM ess_systems 
		WHERE workgroup = ? AND name = ? AND version = ?`,
		workgroup, system, req.ToVersion).Scan(&existingID)

	if err == nil {
		http.Error(w, fmt.Sprintf("Version '%s' already exists", req.ToVersion), http.StatusConflict)
		return
	} else if err != sql.ErrNoRows {
		http.Error(w, "Database error", http.StatusInternalServerError)
		return
	}

	// Get source system
	var sourceSystemID int64
	var sourceDesc, sourceAuthor string
	err = reg.db.QueryRow(`
		SELECT id, description, author FROM ess_systems 
		WHERE workgroup = ? AND name = ? AND version = ?`,
		workgroup, system, req.FromVersion).Scan(&sourceSystemID, &sourceDesc, &sourceAuthor)

	if err == sql.ErrNoRows {
		http.Error(w, fmt.Sprintf("Source version '%s' not found", req.FromVersion), http.StatusNotFound)
		return
	} else if err != nil {
		http.Error(w, "Database error", http.StatusInternalServerError)
		return
	}

	// Create new system entry
	now := time.Now()
	description := fmt.Sprintf("Sandbox: %s", req.Comment)
	if description == "Sandbox: " {
		description = fmt.Sprintf("Sandbox from %s", req.FromVersion)
	}

	result, err := reg.db.Exec(`
		INSERT INTO ess_systems (workgroup, name, version, description, author, forked_from, forked_at, created_at, updated_at, updated_by)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		workgroup, system, req.ToVersion, description, sourceAuthor, req.FromVersion, now, now, now, req.Username)

	if err != nil {
		http.Error(w, "Failed to create sandbox", http.StatusInternalServerError)
		return
	}

	newSystemID, _ := result.LastInsertId()

	// Copy all scripts from source to new version
	_, err = reg.db.Exec(`
		INSERT INTO ess_scripts (system_id, protocol, type, filename, content, checksum, updated_at, updated_by)
		SELECT ?, protocol, type, filename, content, checksum, ?, ?
		FROM ess_scripts WHERE system_id = ?`,
		newSystemID, now, req.Username, sourceSystemID)

	if err != nil {
		// Rollback - delete the system we just created
		reg.db.Exec("DELETE FROM ess_systems WHERE id = ?", newSystemID)
		http.Error(w, "Failed to copy scripts", http.StatusInternalServerError)
		return
	}

	// Get script count
	var scriptCount int
	reg.db.QueryRow("SELECT COUNT(*) FROM ess_scripts WHERE system_id = ?", newSystemID).Scan(&scriptCount)

	response := map[string]interface{}{
		"success":     true,
		"version":     req.ToVersion,
		"systemId":    newSystemID,
		"scriptCount": scriptCount,
		"forkedFrom":  req.FromVersion,
		"createdAt":   now,
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(response)
}

// POST /api/v1/ess/sandbox/{workgroup}/{system}/promote
// Promotes a sandbox version to another version (typically main)
func (reg *ESSRegistry) handlePromoteSandbox(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/api/v1/ess/sandbox/"), "/")
	if len(parts) < 3 {
		http.Error(w, "Invalid path", http.StatusBadRequest)
		return
	}
	workgroup := parts[0]
	system := parts[1]

	var req PromoteSandboxRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid request body", http.StatusBadRequest)
		return
	}

	// Validate
	if req.FromVersion == "" || req.ToVersion == "" {
		http.Error(w, "fromVersion and toVersion required", http.StatusBadRequest)
		return
	}
	if req.Username == "" {
		http.Error(w, "username required", http.StatusBadRequest)
		return
	}

	// Get source system ID
	var sourceSystemID int64
	err := reg.db.QueryRow(`
		SELECT id FROM ess_systems 
		WHERE workgroup = ? AND name = ? AND version = ?`,
		workgroup, system, req.FromVersion).Scan(&sourceSystemID)

	if err == sql.ErrNoRows {
		http.Error(w, fmt.Sprintf("Source version '%s' not found", req.FromVersion), http.StatusNotFound)
		return
	} else if err != nil {
		http.Error(w, "Database error", http.StatusInternalServerError)
		return
	}

	// Check if target exists
	var targetSystemID int64
	err = reg.db.QueryRow(`
		SELECT id FROM ess_systems 
		WHERE workgroup = ? AND name = ? AND version = ?`,
		workgroup, system, req.ToVersion).Scan(&targetSystemID)

	now := time.Now()

	if err == sql.ErrNoRows {
		// Target doesn't exist - create it (first-time promotion)
		result, err := reg.db.Exec(`
			INSERT INTO ess_systems (workgroup, name, version, description, author, forked_from, created_at, updated_at, updated_by)
			SELECT workgroup, name, ?, description, author, ?, ?, ?, ?
			FROM ess_systems WHERE id = ?`,
			req.ToVersion, req.FromVersion, now, now, req.Username, sourceSystemID)

		if err != nil {
			http.Error(w, "Failed to create target version", http.StatusInternalServerError)
			return
		}
		targetSystemID, _ = result.LastInsertId()
	} else if err != nil {
		http.Error(w, "Database error", http.StatusInternalServerError)
		return
	}

	// Save current target scripts to history (if target existed)
	if targetSystemID > 0 {
		reg.db.Exec(`
			INSERT INTO ess_script_history (script_id, content, checksum, saved_at, saved_by, comment)
			SELECT id, content, checksum, ?, ?, ?
			FROM ess_scripts WHERE system_id = ?`,
			now, req.Username, fmt.Sprintf("Before promotion from %s", req.FromVersion), targetSystemID)
	}

	// Delete old target scripts
	reg.db.Exec("DELETE FROM ess_scripts WHERE system_id = ?", targetSystemID)

	// Copy scripts from source to target
	_, err = reg.db.Exec(`
		INSERT INTO ess_scripts (system_id, protocol, type, filename, content, checksum, updated_at, updated_by)
		SELECT ?, protocol, type, filename, content, checksum, ?, ?
		FROM ess_scripts WHERE system_id = ?`,
		targetSystemID, now, req.Username, sourceSystemID)

	if err != nil {
		http.Error(w, "Failed to promote scripts", http.StatusInternalServerError)
		return
	}

	// Update target system metadata
	_, err = reg.db.Exec(`
		UPDATE ess_systems 
		SET updated_at = ?, updated_by = ?, forked_from = ?
		WHERE id = ?`,
		now, req.Username, req.FromVersion, targetSystemID)

	if err != nil {
		http.Error(w, "Failed to update target metadata", http.StatusInternalServerError)
		return
	}

	// Get script count
	var scriptCount int
	reg.db.QueryRow("SELECT COUNT(*) FROM ess_scripts WHERE system_id = ?", targetSystemID).Scan(&scriptCount)

	response := map[string]interface{}{
		"success":     true,
		"fromVersion": req.FromVersion,
		"toVersion":   req.ToVersion,
		"scriptCount": scriptCount,
		"promotedAt":  now,
		"promotedBy":  req.Username,
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(response)
}

// POST /api/v1/ess/sandbox/{workgroup}/{system}/sync
// Syncs a sandbox with another version (pulls changes from main into sandbox)
func (reg *ESSRegistry) handleSyncSandbox(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/api/v1/ess/sandbox/"), "/")
	if len(parts) < 3 {
		http.Error(w, "Invalid path", http.StatusBadRequest)
		return
	}
	workgroup := parts[0]
	system := parts[1]

	var req SyncSandboxRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid request body", http.StatusBadRequest)
		return
	}

	// Validate
	if req.FromVersion == "" {
		req.FromVersion = "main"
	}
	if req.ToVersion == "" {
		http.Error(w, "toVersion required", http.StatusBadRequest)
		return
	}
	if req.Username == "" {
		http.Error(w, "username required", http.StatusBadRequest)
		return
	}
	if req.Conflicts == "" {
		req.Conflicts = "overwrite" // Default: overwrite sandbox with source
	}

	// Get source and target system IDs
	var sourceSystemID, targetSystemID int64
	err := reg.db.QueryRow(`
		SELECT id FROM ess_systems 
		WHERE workgroup = ? AND name = ? AND version = ?`,
		workgroup, system, req.FromVersion).Scan(&sourceSystemID)

	if err == sql.ErrNoRows {
		http.Error(w, fmt.Sprintf("Source version '%s' not found", req.FromVersion), http.StatusNotFound)
		return
	}

	err = reg.db.QueryRow(`
		SELECT id FROM ess_systems 
		WHERE workgroup = ? AND name = ? AND version = ?`,
		workgroup, system, req.ToVersion).Scan(&targetSystemID)

	if err == sql.ErrNoRows {
		http.Error(w, fmt.Sprintf("Target version '%s' not found", req.ToVersion), http.StatusNotFound)
		return
	}

	now := time.Now()

	// Save current target to history before overwriting
	reg.db.Exec(`
		INSERT INTO ess_script_history (script_id, content, checksum, saved_at, saved_by, comment)
		SELECT id, content, checksum, ?, ?, ?
		FROM ess_scripts WHERE system_id = ?`,
		now, req.Username, fmt.Sprintf("Before sync from %s", req.FromVersion), targetSystemID)

	// Delete target scripts
	reg.db.Exec("DELETE FROM ess_scripts WHERE system_id = ?", targetSystemID)

	// Copy from source
	_, err = reg.db.Exec(`
		INSERT INTO ess_scripts (system_id, protocol, type, filename, content, checksum, updated_at, updated_by)
		SELECT ?, protocol, type, filename, content, checksum, ?, ?
		FROM ess_scripts WHERE system_id = ?`,
		targetSystemID, now, req.Username, sourceSystemID)

	if err != nil {
		http.Error(w, "Failed to sync scripts", http.StatusInternalServerError)
		return
	}

	// Update target metadata
	reg.db.Exec(`
		UPDATE ess_systems 
		SET updated_at = ?, updated_by = ?
		WHERE id = ?`,
		now, req.Username, targetSystemID)

	var scriptCount int
	reg.db.QueryRow("SELECT COUNT(*) FROM ess_scripts WHERE system_id = ?", targetSystemID).Scan(&scriptCount)

	response := map[string]interface{}{
		"success":     true,
		"fromVersion": req.FromVersion,
		"toVersion":   req.ToVersion,
		"scriptCount": scriptCount,
		"syncedAt":    now,
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(response)
}

// DELETE /api/v1/ess/sandbox/{workgroup}/{system}/{version}
// Deletes a sandbox version
func (reg *ESSRegistry) handleDeleteSandbox(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodDelete {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/api/v1/ess/sandbox/"), "/")
	if len(parts) < 3 {
		http.Error(w, "Invalid path", http.StatusBadRequest)
		return
	}
	workgroup := parts[0]
	system := parts[1]
	version := parts[2]

	// Protect main version
	if version == "main" {
		http.Error(w, "Cannot delete main version", http.StatusForbidden)
		return
	}

	// Get system ID
	var systemID int64
	err := reg.db.QueryRow(`
		SELECT id FROM ess_systems 
		WHERE workgroup = ? AND name = ? AND version = ?`,
		workgroup, system, version).Scan(&systemID)

	if err == sql.ErrNoRows {
		http.Error(w, "Version not found", http.StatusNotFound)
		return
	} else if err != nil {
		http.Error(w, "Database error", http.StatusInternalServerError)
		return
	}

	// Delete scripts
	reg.db.Exec("DELETE FROM ess_scripts WHERE system_id = ?", systemID)

	// Delete system
	_, err = reg.db.Exec("DELETE FROM ess_systems WHERE id = ?", systemID)
	if err != nil {
		http.Error(w, "Failed to delete version", http.StatusInternalServerError)
		return
	}

	response := map[string]interface{}{
		"success": true,
		"version": version,
		"deleted": true,
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(response)
}

// GET /api/v1/ess/systems/{workgroup}/{system}/versions
// Lists all versions of a system
func (reg *ESSRegistry) handleListVersions(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/api/v1/ess/systems/"), "/")
	if len(parts) < 3 {
		http.Error(w, "Invalid path", http.StatusBadRequest)
		return
	}
	workgroup := parts[0]
	system := parts[1]

	rows, err := reg.db.Query(`
		SELECT s.version, s.description, s.updated_at, s.updated_by,
		       (SELECT COUNT(*) FROM ess_scripts WHERE system_id = s.id) as script_count
		FROM ess_systems s
		WHERE s.workgroup = ? AND s.name = ?
		ORDER BY 
			CASE WHEN s.version = 'main' THEN 0 ELSE 1 END,
			s.updated_at DESC`,
		workgroup, system)

	if err != nil {
		http.Error(w, "Database error", http.StatusInternalServerError)
		return
	}
	defer rows.Close()

	versions := []VersionInfo{}
	for rows.Next() {
		var v VersionInfo
		if err := rows.Scan(&v.Version, &v.Description, &v.UpdatedAt, &v.UpdatedBy, &v.ScriptCount); err != nil {
			continue
		}
		v.IsMain = (v.Version == "main")
		v.IsSandbox = (v.Version != "main" && !strings.HasPrefix(v.Version, "v"))
		versions = append(versions, v)
	}

	response := map[string]interface{}{
		"workgroup": workgroup,
		"system":    system,
		"versions":  versions,
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(response)
}

// ============ Helper to register routes ============

func (reg *ESSRegistry) registerSandboxRoutes() {
	// These should be called from your main route setup
	// Example usage in your existing handler setup:
	//
	// http.HandleFunc("/api/v1/ess/sandbox/", func(w http.ResponseWriter, r *http.Request) {
	//     path := strings.TrimPrefix(r.URL.Path, "/api/v1/ess/sandbox/")
	//     parts := strings.Split(path, "/")
	//     
	//     if len(parts) >= 3 {
	//         action := parts[2]
	//         switch action {
	//         case "create":
	//             reg.handleCreateSandbox(w, r)
	//         case "promote":
	//             reg.handlePromoteSandbox(w, r)
	//         case "sync":
	//             reg.handleSyncSandbox(w, r)
	//         case "versions":
	//             reg.handleListVersions(w, r)
	//         default:
	//             if r.Method == http.MethodDelete {
	//                 reg.handleDeleteSandbox(w, r)
	//             }
	//         }
	//     }
	// })
}
