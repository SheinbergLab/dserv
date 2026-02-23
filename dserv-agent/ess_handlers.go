// ess_handlers.go - HTTP handlers for ESS Registry API

package main

import (
	"archive/zip"
	"encoding/json"
	"fmt"
	"net/http"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// RegisterHandlers registers all ESS registry HTTP endpoints
func (r *ESSRegistry) RegisterHandlers(mux *http.ServeMux, authMiddleware func(http.HandlerFunc) http.HandlerFunc) {
	// Templates (global/zoo)
	mux.HandleFunc("/api/v1/ess/templates", authMiddleware(r.handleListTemplates))

	// Systems
	mux.HandleFunc("/api/v1/ess/systems", authMiddleware(r.handleListSystems))
	mux.HandleFunc("/api/v1/ess/system/", authMiddleware(r.handleSystem))

	// Scripts
	mux.HandleFunc("/api/v1/ess/script/", authMiddleware(r.handleScript))
	mux.HandleFunc("/api/v1/ess/scripts/", authMiddleware(r.handleScripts))

	// Libraries
	mux.HandleFunc("/api/v1/ess/libs", authMiddleware(r.handleListLibs))
	mux.HandleFunc("/api/v1/ess/lib/", authMiddleware(r.handleLib))

	// Projects
	mux.HandleFunc("/api/v1/ess/projects", authMiddleware(r.handleListProjects))
	mux.HandleFunc("/api/v1/ess/project/", authMiddleware(r.handleProject))

	// Scaffold
	mux.HandleFunc("/api/v1/ess/scaffold/protocol", authMiddleware(r.handleScaffoldProtocol))
	mux.HandleFunc("/api/v1/ess/scaffold/system", authMiddleware(r.handleScaffoldSystem))
	mux.HandleFunc("/api/v1/ess/scaffold/info/", authMiddleware(r.handleScaffoldInfo))
	
	// Operations
	mux.HandleFunc("/api/v1/ess/add-to-workgroup", authMiddleware(r.handleAddToWorkgroup))
	mux.HandleFunc("/api/v1/ess/lock", authMiddleware(r.handleLock))
	mux.HandleFunc("/api/v1/ess/locks", authMiddleware(r.handleListLocks))

	// History
	mux.HandleFunc("/api/v1/ess/history/", authMiddleware(r.handleHistory))

	// Users
	mux.HandleFunc("/api/v1/ess/users", authMiddleware(r.handleListUsers))
	mux.HandleFunc("/api/v1/ess/user/", authMiddleware(r.handleUser))

	// Sandbox/Version Management
	mux.HandleFunc("/api/v1/ess/sandbox/", authMiddleware(r.handleSandbox))

	
	mux.HandleFunc("/api/v1/ess/manifest/", authMiddleware(r.handleManifest))
	mux.HandleFunc("/api/v1/ess/sync/", authMiddleware(r.handleSync))

	// Export
	mux.HandleFunc("/api/v1/ess/export/", authMiddleware(r.handleExport))
	
	// Admin
	mux.HandleFunc("/api/v1/ess/admin/seed-templates", authMiddleware(r.handleSeedTemplates))
	mux.HandleFunc("/api/v1/ess/admin/backup", authMiddleware(r.handleBackup))
	mux.HandleFunc("/api/v1/ess/admin/backups", authMiddleware(r.handleListBackups))
}

// GET /api/v1/ess/templates
func (r *ESSRegistry) handleListTemplates(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	systems, err := r.ListTemplates()
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	writeJSON(w, 200, map[string]interface{}{"systems": systems})
}

// GET /api/v1/ess/systems?workgroup=xxx
func (r *ESSRegistry) handleListSystems(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	workgroup := req.URL.Query().Get("workgroup")
	if workgroup == "" {
		http.Error(w, "workgroup parameter required", http.StatusBadRequest)
		return
	}

	systems, err := r.ListSystems(workgroup)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	writeJSON(w, 200, map[string]interface{}{
		"workgroup": workgroup,
		"systems":   systems,
	})
}

// GET /api/v1/ess/system/{workgroup}/{name}[/{version}]
func (r *ESSRegistry) handleSystem(w http.ResponseWriter, req *http.Request) {
	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/system/")
	parts := strings.Split(path, "/")

	if len(parts) < 2 {
		http.Error(w, "Invalid path: need workgroup/name", http.StatusBadRequest)
		return
	}

	workgroup := parts[0]
	name := parts[1]
	version := ""
	if len(parts) > 2 {
		version = parts[2]
	}

	switch req.Method {
	case http.MethodGet:
		sys, err := r.GetSystem(workgroup, name, version)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		if sys == nil {
			writeJSON(w, 404, map[string]string{"error": "System not found"})
			return
		}
		scripts, _ := r.GetScripts(sys.ID)
		writeJSON(w, 200, map[string]interface{}{"system": sys, "scripts": scripts})

	default:
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
	}
}

// GET/PUT /api/v1/ess/script/{workgroup}/{system}/{protocol}/{type}
func (r *ESSRegistry) handleScript(w http.ResponseWriter, req *http.Request) {
	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/script/")
	parts := strings.Split(path, "/")

	if len(parts) < 4 {
		http.Error(w, "Invalid path: need workgroup/system/protocol/type", http.StatusBadRequest)
		return
	}

	workgroup := parts[0]
	systemName := parts[1]
	protocol := parts[2]
	scriptType := parts[3]

	if protocol == "_" || protocol == "system" {
		protocol = ""
	}

	sys, err := r.GetSystem(workgroup, systemName, "")
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	if sys == nil {
		writeJSON(w, 404, map[string]string{"error": "System not found"})
		return
	}

	switch req.Method {
	case http.MethodGet:
		script, err := r.GetScript(sys.ID, protocol, scriptType)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		if script == nil {
			writeJSON(w, 404, map[string]string{"error": "Script not found"})
			return
		}
		writeJSON(w, 200, script)

	case http.MethodPut:
		var saveReq SaveScriptRequest
		if err := json.NewDecoder(req.Body).Decode(&saveReq); err != nil {
			writeJSON(w, 400, map[string]string{"error": "Invalid JSON"})
			return
		}
		if saveReq.Content == "" {
			writeJSON(w, 400, map[string]string{"error": "Content required"})
			return
		}

		var filename string
		if protocol == "" {
			switch scriptType {
			case ScriptTypeExtract:
				filename = fmt.Sprintf("%s_extract.tcl", systemName)
			case ScriptTypeAnalyze:
				filename = fmt.Sprintf("%s_analyze.tcl", systemName)
			default:
				filename = fmt.Sprintf("%s.tcl", systemName)
			}			
		} else {
			switch scriptType {
			case ScriptTypeProtocol:
				filename = fmt.Sprintf("%s.tcl", protocol)
			default:
				filename = fmt.Sprintf("%s_%s.tcl", protocol, scriptType)
			}
		}

		script := &ESSScript{
			SystemID:  sys.ID,
			Protocol:  protocol,
			Type:      scriptType,
			Filename:  filename,
			Content:   saveReq.Content,
			UpdatedBy: saveReq.UpdatedBy,
		}

		if err := r.SaveScript(script, saveReq.ExpectedChecksum, saveReq.Comment); err != nil {
			if strings.Contains(err.Error(), "conflict") {
				writeJSON(w, 409, map[string]string{"error": err.Error()})
			} else {
				writeJSON(w, 500, map[string]string{"error": err.Error()})
			}
			return
		}

		writeJSON(w, 200, map[string]interface{}{
			"success":  true,
			"checksum": script.Checksum,
			"script":   script,
		})

	default:
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
	}
}

// GET /api/v1/ess/scripts/{workgroup}/{system}
func (r *ESSRegistry) handleScripts(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/scripts/")
	parts := strings.Split(path, "/")

	if len(parts) < 2 {
		http.Error(w, "Invalid path: need workgroup/system", http.StatusBadRequest)
		return
	}

	workgroup := parts[0]
	systemName := parts[1]

	sys, err := r.GetSystem(workgroup, systemName, "")
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

	grouped := make(map[string][]*ESSScript)
	for _, s := range scripts {
		key := s.Protocol
		if key == "" {
			key = "_system"
		}
		grouped[key] = append(grouped[key], s)
	}

	writeJSON(w, 200, map[string]interface{}{"system": sys, "scripts": grouped})
}

// GET /api/v1/ess/libs?workgroup=xxx
func (r *ESSRegistry) handleListLibs(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	workgroup := req.URL.Query().Get("workgroup")
	if workgroup == "" {
		workgroup = TemplatesWorkgroup
	}

	libs, err := r.ListLibs(workgroup)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	type LibSummary struct {
		ID         int64     `json:"id"`
		Workgroup  string    `json:"workgroup"`
		Name       string    `json:"name"`
		Version    string    `json:"version"`
		Filename   string    `json:"filename"`
		Checksum   string    `json:"checksum"`
		ForkedFrom string    `json:"forkedFrom,omitempty"`
		CreatedAt  time.Time `json:"createdAt"`
		UpdatedAt  time.Time `json:"updatedAt"`
	}

	summaries := make([]LibSummary, len(libs))
	for i, lib := range libs {
		summaries[i] = LibSummary{
			ID: lib.ID, Workgroup: lib.Workgroup, Name: lib.Name,
			Version: lib.Version, Filename: lib.Filename, Checksum: lib.Checksum,
			ForkedFrom: lib.ForkedFrom, CreatedAt: lib.CreatedAt, UpdatedAt: lib.UpdatedAt,
		}
	}

	writeJSON(w, 200, map[string]interface{}{"workgroup": workgroup, "libs": summaries})
}

// GET/PUT/DELETE /api/v1/ess/lib/{workgroup}/{name}/{version}
func (r *ESSRegistry) handleLib(w http.ResponseWriter, req *http.Request) {
	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/lib/")
	parts := strings.Split(path, "/")

	if len(parts) < 2 {
		http.Error(w, "Invalid path: need workgroup/name[/version]", http.StatusBadRequest)
		return
	}

	workgroup := parts[0]
	name := parts[1]
	version := ""
	if len(parts) > 2 {
		version = parts[2]
	}

	switch req.Method {
	case http.MethodGet:
		lib, err := r.GetLib(workgroup, name, version)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		if lib == nil {
			writeJSON(w, 404, map[string]string{"error": "Library not found"})
			return
		}
		writeJSON(w, 200, lib)

	case http.MethodPut:
		if version == "" {
			writeJSON(w, 400, map[string]string{"error": "Version required for PUT"})
			return
		}
		var saveReq SaveLibRequest
		if err := json.NewDecoder(req.Body).Decode(&saveReq); err != nil {
			writeJSON(w, 400, map[string]string{"error": "Invalid JSON"})
			return
		}
		if saveReq.Content == "" {
			writeJSON(w, 400, map[string]string{"error": "Content required"})
			return
		}

		filename := fmt.Sprintf("%s-%s.tm", name, version)
		lib := &ESSLib{
			Workgroup: workgroup,
			Name:      name,
			Version:   version,
			Filename:  filename,
			Content:   saveReq.Content,
		}

		id, err := r.SaveLib(lib)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, 200, map[string]interface{}{
			"success":  true,
			"id":       id,
			"checksum": lib.Checksum,
			"lib":      lib,
		})

	case http.MethodDelete:
		if version == "" {
			writeJSON(w, 400, map[string]string{"error": "Version required for DELETE"})
			return
		}
		if err := r.DeleteLib(workgroup, name, version); err != nil {
			if strings.Contains(err.Error(), "not found") {
				writeJSON(w, 404, map[string]string{"error": err.Error()})
			} else {
				writeJSON(w, 500, map[string]string{"error": err.Error()})
			}
			return
		}
		writeJSON(w, 200, map[string]interface{}{"success": true})

	default:
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
	}
}

// GET /api/v1/ess/projects?workgroup=xxx
func (r *ESSRegistry) handleListProjects(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	workgroup := req.URL.Query().Get("workgroup")
	if workgroup == "" {
		http.Error(w, "workgroup parameter required", http.StatusBadRequest)
		return
	}

	projects, err := r.ListProjects(workgroup)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	writeJSON(w, 200, map[string]interface{}{"workgroup": workgroup, "projects": projects})
}

// GET/PUT/DELETE /api/v1/ess/project/{workgroup}/{name}
func (r *ESSRegistry) handleProject(w http.ResponseWriter, req *http.Request) {
	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/project/")
	parts := strings.Split(path, "/")

	if len(parts) < 2 {
		http.Error(w, "Invalid path: need workgroup/name", http.StatusBadRequest)
		return
	}

	workgroup := parts[0]
	name := parts[1]

	switch req.Method {
	case http.MethodGet:
		project, err := r.GetProject(workgroup, name)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		if project == nil {
			writeJSON(w, 404, map[string]string{"error": "Project not found"})
			return
		}
		writeJSON(w, 200, project)

	case http.MethodPut:
		var project ESSProject
		if err := json.NewDecoder(req.Body).Decode(&project); err != nil {
			writeJSON(w, 400, map[string]string{"error": "Invalid JSON"})
			return
		}
		project.Workgroup = workgroup
		project.Name = name

		id, err := r.SaveProject(&project)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, 200, map[string]interface{}{"success": true, "id": id})

	case http.MethodDelete:
		if err := r.DeleteProject(workgroup, name); err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, 200, map[string]interface{}{"success": true})

	default:
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
	}
}

// POST /api/v1/ess/add-to-workgroup
func (r *ESSRegistry) handleAddToWorkgroup(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var addReq AddToWorkgroupRequest
	if err := json.NewDecoder(req.Body).Decode(&addReq); err != nil {
		writeJSON(w, 400, map[string]string{"error": "Invalid JSON"})
		return
	}

	if addReq.TemplateSystem == "" || addReq.TargetWorkgroup == "" {
		writeJSON(w, 400, map[string]string{"error": "templateSystem and targetWorkgroup required"})
		return
	}

	if addReq.TemplateVersion == "" {
		addReq.TemplateVersion = "latest"
	}
	if addReq.AddedBy == "" {
		addReq.AddedBy = "unknown"
	}

	sys, err := r.AddToWorkgroup(addReq)
	if err != nil {
		if strings.Contains(err.Error(), "already exists") {
			writeJSON(w, 409, map[string]string{"error": err.Error()})
		} else if strings.Contains(err.Error(), "not found") {
			writeJSON(w, 404, map[string]string{"error": err.Error()})
		} else {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
		}
		return
	}

	writeJSON(w, 200, map[string]interface{}{
		"success": true,
		"message": fmt.Sprintf("Added %s to %s", sys.Name, addReq.TargetWorkgroup),
		"system":  sys,
	})
}

// POST/DELETE /api/v1/ess/lock
func (r *ESSRegistry) handleLock(w http.ResponseWriter, req *http.Request) {
	var lockReq LockRequest
	if err := json.NewDecoder(req.Body).Decode(&lockReq); err != nil {
		writeJSON(w, 400, map[string]string{"error": "Invalid JSON"})
		return
	}

	if lockReq.Key == "" || lockReq.LockedBy == "" {
		writeJSON(w, 400, map[string]string{"error": "key and lockedBy required"})
		return
	}

	switch req.Method {
	case http.MethodPost:
		lock, err := r.AcquireLock(lockReq.Key, lockReq.LockedBy)
		if err != nil {
			writeJSON(w, 409, map[string]interface{}{"error": err.Error(), "lock": lock, "locked": true})
			return
		}
		writeJSON(w, 200, map[string]interface{}{"success": true, "lock": lock})

	case http.MethodDelete:
		if err := r.ReleaseLock(lockReq.Key, lockReq.LockedBy); err != nil {
			writeJSON(w, 400, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, 200, map[string]interface{}{"success": true})

	default:
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
	}
}

// GET /api/v1/ess/locks
func (r *ESSRegistry) handleListLocks(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	locks, err := r.ListLocks()
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	writeJSON(w, 200, map[string]interface{}{"locks": locks})
}

// GET /api/v1/ess/history/{workgroup}/{system}/{protocol}/{type}
func (r *ESSRegistry) handleHistory(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/history/")
	parts := strings.Split(path, "/")

	if len(parts) < 4 {
		http.Error(w, "Invalid path: need workgroup/system/protocol/type", http.StatusBadRequest)
		return
	}

	workgroup := parts[0]
	systemName := parts[1]
	protocol := parts[2]
	scriptType := parts[3]

	if protocol == "_" || protocol == "system" {
		protocol = ""
	}

	sys, err := r.GetSystem(workgroup, systemName, "")
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	if sys == nil {
		writeJSON(w, 404, map[string]string{"error": "System not found"})
		return
	}

	script, err := r.GetScript(sys.ID, protocol, scriptType)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	if script == nil {
		writeJSON(w, 404, map[string]string{"error": "Script not found"})
		return
	}

	history, err := r.GetScriptHistory(script.ID, 0)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	writeJSON(w, 200, map[string]interface{}{"script": script, "history": history})
}

// GET /api/v1/ess/users?workgroup=xxx
func (r *ESSRegistry) handleListUsers(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	workgroup := req.URL.Query().Get("workgroup")
	if workgroup == "" {
		http.Error(w, "workgroup parameter required", http.StatusBadRequest)
		return
	}

	users, err := r.ListUsers(workgroup)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	writeJSON(w, 200, map[string]interface{}{"workgroup": workgroup, "users": users})
}

// GET/POST/DELETE /api/v1/ess/user/{workgroup}/{username}
func (r *ESSRegistry) handleUser(w http.ResponseWriter, req *http.Request) {
	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/user/")
	parts := strings.Split(path, "/")

	if len(parts) < 1 {
		http.Error(w, "Invalid path: need workgroup[/username]", http.StatusBadRequest)
		return
	}

	workgroup := parts[0]
	username := ""
	if len(parts) > 1 {
		username = parts[1]
	}

	switch req.Method {
	case http.MethodGet:
		if username == "" {
			http.Error(w, "Username required for GET", http.StatusBadRequest)
			return
		}
		user, err := r.GetUser(workgroup, username)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		if user == nil {
			writeJSON(w, 404, map[string]string{"error": "User not found"})
			return
		}
		writeJSON(w, 200, user)

	case http.MethodPost:
		var userReq UserRequest
		if err := json.NewDecoder(req.Body).Decode(&userReq); err != nil {
			writeJSON(w, 400, map[string]string{"error": "Invalid JSON"})
			return
		}
		if userReq.Username == "" {
			writeJSON(w, 400, map[string]string{"error": "username required"})
			return
		}
		user := &ESSUser{
			Workgroup: workgroup,
			Username:  userReq.Username,
			FullName:  userReq.FullName,
			Email:     userReq.Email,
			Role:      userReq.Role,
		}
		id, err := r.SaveUser(user)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, 200, map[string]interface{}{"success": true, "id": id, "user": user})

	case http.MethodDelete:
		if username == "" {
			http.Error(w, "Username required for DELETE", http.StatusBadRequest)
			return
		}
		if err := r.DeleteUser(workgroup, username); err != nil {
			if strings.Contains(err.Error(), "not found") {
				writeJSON(w, 404, map[string]string{"error": err.Error()})
			} else {
				writeJSON(w, 500, map[string]string{"error": err.Error()})
			}
			return
		}
		writeJSON(w, 200, map[string]interface{}{"success": true})

	default:
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
	}
}


// handleSandbox routes sandbox operations based on the action in the path
func (r *ESSRegistry) handleSandbox(w http.ResponseWriter, req *http.Request) {
	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/sandbox/")
	parts := strings.Split(path, "/")

	if len(parts) < 3 {
		http.Error(w, "Invalid path", http.StatusBadRequest)
		return
	}

	// parts[0] = workgroup
	// parts[1] = system
	// parts[2] = action ("create", "promote", "sync", "versions") or version (for DELETE)

	action := parts[2]
	switch action {
	case "create":
		r.handleCreateSandbox(w, req)
	case "promote":
		r.handlePromoteSandbox(w, req)
	case "sync":
		r.handleSyncSandbox(w, req)
	case "versions":
		r.handleListVersions(w, req)
	default:
		// If not a known action, might be DELETE with version
		if req.Method == http.MethodDelete {
			r.handleDeleteSandbox(w, req)
		} else {
			http.Error(w, "Unknown action", http.StatusNotFound)
		}
	}
}

// POST /api/v1/ess/admin/seed-templates
func (r *ESSRegistry) handleSeedTemplates(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var seedReq SeedTemplatesRequest
	if err := json.NewDecoder(req.Body).Decode(&seedReq); err != nil {
		writeJSON(w, 400, map[string]string{"error": "Invalid JSON"})
		return
	}

	if seedReq.SourcePath == "" {
		writeJSON(w, 400, map[string]string{"error": "sourcePath required"})
		return
	}

	seeded, err := r.SeedFromFilesystem(seedReq.SourcePath, seedReq.Systems)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	libs, _ := r.ListLibs(TemplatesWorkgroup)
	libNames := make([]string, len(libs))
	for i, lib := range libs {
		libNames[i] = fmt.Sprintf("%s-%s", lib.Name, lib.Version)
	}
	sort.Strings(libNames)

	writeJSON(w, 200, map[string]interface{}{
		"success": true,
		"systems": seeded,
		"libs":    libNames,
		"message": fmt.Sprintf("Seeded %d systems and %d libs", len(seeded), len(libs)),
	})
}

// POST /api/v1/ess/admin/backup
func (r *ESSRegistry) handleBackup(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	// Backup directory is sibling to database
	backupDir := filepath.Join(filepath.Dir(r.dbPath), "backups")

	backup, err := r.Backup(backupDir)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	writeJSON(w, 200, map[string]interface{}{
		"success": true,
		"backup":  backup,
	})
}

// GET /api/v1/ess/admin/backups
func (r *ESSRegistry) handleListBackups(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	backupDir := filepath.Join(filepath.Dir(r.dbPath), "backups")

	backups, err := r.ListBackups(backupDir)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	writeJSON(w, 200, map[string]interface{}{
		"backups": backups,
		"count":   len(backups),
	})
}

// GET /api/v1/ess/export/{workgroup}[/{system}]?format=zip
//
// Exports a workgroup (or single system) as a zip file with the
// standard filesystem layout:
//
//   lib/
//     planko-3.0.tm
//   match_to_sample/
//     match_to_sample.tcl
//     colormatch/
//       colormatch.tcl
//       colormatch_variants.tcl
//
func (r *ESSRegistry) handleExport(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/export/")
	parts := strings.Split(path, "/")

	if len(parts) < 1 || parts[0] == "" {
		http.Error(w, "Invalid path: need workgroup[/system]", http.StatusBadRequest)
		return
	}

	workgroup := parts[0]
	systemFilter := ""
	if len(parts) > 1 && parts[1] != "" {
		systemFilter = parts[1]
	}

	// Gather systems
	var systems []*ESSSystem
	if systemFilter != "" {
		sys, err := r.GetSystem(workgroup, systemFilter, "")
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		if sys == nil {
			writeJSON(w, 404, map[string]string{"error": "System not found"})
			return
		}
		systems = []*ESSSystem{sys}
	} else {
		var err error
		systems, err = r.ListSystems(workgroup)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
	}

	// Build zip filename
	zipName := workgroup
	if systemFilter != "" {
		zipName = systemFilter
	}
	zipName = strings.ReplaceAll(zipName, "/", "-")

	w.Header().Set("Content-Type", "application/zip")
	w.Header().Set("Content-Disposition", fmt.Sprintf(`attachment; filename="%s.zip"`, zipName))

	zw := zip.NewWriter(w)
	defer zw.Close()

	fileCount := 0

	// Add system scripts
	for _, sys := range systems {
		scripts, err := r.GetScripts(sys.ID)
		if err != nil {
			continue
		}
		for _, script := range scripts {
			if script.Content == "" {
				continue
			}

			var zipPath string
			if script.Protocol == "" {
				// System-level: system/filename
				zipPath = filepath.Join(sys.Name, script.Filename)
			} else {
				// Protocol-level: system/protocol/filename
				zipPath = filepath.Join(sys.Name, script.Protocol, script.Filename)
			}

			header := &zip.FileHeader{
				Name:     zipPath,
				Method:   zip.Deflate,
				Modified: script.UpdatedAt,
			}
			fw, err := zw.CreateHeader(header)
			if err != nil {
				continue
			}
			fw.Write([]byte(script.Content))
			fileCount++
		}
	}

	// Add libs (only for full workgroup export or if explicitly requested)
	if systemFilter == "" {
		libs, _ := r.ListLibs(workgroup)
		for _, libSummary := range libs {
			fullLib, err := r.GetLib(workgroup, libSummary.Name, libSummary.Version)
			if err != nil || fullLib == nil || fullLib.Content == "" {
				continue
			}

			zipPath := filepath.Join("lib", fullLib.Filename)
			header := &zip.FileHeader{
				Name:     zipPath,
				Method:   zip.Deflate,
				Modified: fullLib.UpdatedAt,
			}
			fw, err := zw.CreateHeader(header)
			if err != nil {
				continue
			}
			fw.Write([]byte(fullLib.Content))
			fileCount++
		}
	}
}
