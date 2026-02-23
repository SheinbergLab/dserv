// ess_scaffold_handlers.go - HTTP handlers for scaffold operations
//
// Endpoints:
//   POST /api/v1/ess/scaffold/protocol   - Create a new protocol (clone or skeleton)
//   POST /api/v1/ess/scaffold/system     - Create a new system (clone, template, or skeleton)
//   GET  /api/v1/ess/scaffold/info/{workgroup}/{system} - List protocols available to clone

package main

import (
	"encoding/json"
	"net/http"
	"strings"
)

// POST /api/v1/ess/scaffold/protocol
//
// Clone an existing protocol or create from skeleton.
// This is the common operation for adding new protocols to stable systems.
//
// Request body:
//
//	{
//	    "workgroup": "sheinberg",
//	    "system": "match_to_sample",
//	    "protocol": "sizematch",
//	    "fromProtocol": "colormatch",
//	    "createdBy": "dls"
//	}
//
// Omit fromProtocol to create from skeleton.
func (r *ESSRegistry) handleScaffoldProtocol(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var scaffoldReq ScaffoldProtocolRequest
	if err := json.NewDecoder(req.Body).Decode(&scaffoldReq); err != nil {
		writeJSON(w, 400, map[string]string{"error": "Invalid JSON: " + err.Error()})
		return
	}

	// Validate required fields
	if scaffoldReq.Workgroup == "" {
		writeJSON(w, 400, map[string]string{"error": "workgroup is required"})
		return
	}
	if scaffoldReq.System == "" {
		writeJSON(w, 400, map[string]string{"error": "system is required"})
		return
	}
	if scaffoldReq.Protocol == "" {
		writeJSON(w, 400, map[string]string{"error": "protocol is required"})
		return
	}

	// Validate protocol name (alphanumeric, underscores, hyphens)
	if !isValidName(scaffoldReq.Protocol) {
		writeJSON(w, 400, map[string]string{"error": "Invalid protocol name: use only letters, numbers, underscores, hyphens"})
		return
	}

	result, err := r.ScaffoldProtocol(scaffoldReq)
	if err != nil {
		// Distinguish client errors from server errors
		errStr := err.Error()
		if strings.Contains(errStr, "already exists") ||
			strings.Contains(errStr, "not found") ||
			strings.Contains(errStr, "no scripts") {
			writeJSON(w, 409, map[string]string{"error": errStr})
		} else {
			writeJSON(w, 500, map[string]string{"error": errStr})
		}
		return
	}

	writeJSON(w, 201, map[string]interface{}{
		"success":  true,
		"result":   result,
	})
}

// POST /api/v1/ess/scaffold/system
//
// Clone an existing system, create from template, or create from skeleton.
//
// Clone from same workgroup:
//
//	{
//	    "workgroup": "sheinberg",
//	    "system": "my_new_task",
//	    "fromSystem": "match_to_sample",
//	    "createdBy": "dls"
//	}
//
// Clone from different workgroup:
//
//	{
//	    "workgroup": "sheinberg",
//	    "system": "my_new_task",
//	    "fromSystem": "match_to_sample",
//	    "fromWorkgroup": "otherlab",
//	    "createdBy": "dls"
//	}
//
// From template zoo:
//
//	{
//	    "workgroup": "sheinberg",
//	    "system": "my_new_task",
//	    "template": "match_to_sample",
//	    "createdBy": "dls"
//	}
//
// From skeleton (blank):
//
//	{
//	    "workgroup": "sheinberg",
//	    "system": "my_new_task",
//	    "protocol": "first_proto",
//	    "description": "A new experiment",
//	    "createdBy": "dls"
//	}
func (r *ESSRegistry) handleScaffoldSystem(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var scaffoldReq ScaffoldSystemRequest
	if err := json.NewDecoder(req.Body).Decode(&scaffoldReq); err != nil {
		writeJSON(w, 400, map[string]string{"error": "Invalid JSON: " + err.Error()})
		return
	}

	// Validate required fields
	if scaffoldReq.Workgroup == "" {
		writeJSON(w, 400, map[string]string{"error": "workgroup is required"})
		return
	}
	if scaffoldReq.System == "" {
		writeJSON(w, 400, map[string]string{"error": "system is required"})
		return
	}

	if !isValidName(scaffoldReq.System) {
		writeJSON(w, 400, map[string]string{"error": "Invalid system name: use only letters, numbers, underscores, hyphens"})
		return
	}

	result, err := r.ScaffoldSystem(scaffoldReq)
	if err != nil {
		errStr := err.Error()
		if strings.Contains(errStr, "already exists") ||
			strings.Contains(errStr, "not found") {
			writeJSON(w, 409, map[string]string{"error": errStr})
		} else {
			writeJSON(w, 500, map[string]string{"error": errStr})
		}
		return
	}

	writeJSON(w, 201, map[string]interface{}{
		"success":  true,
		"result":   result,
	})
}

// GET /api/v1/ess/scaffold/info/{workgroup}/{system}
//
// Returns information useful for scaffold UI: available protocols to clone from,
// available templates, etc.
func (r *ESSRegistry) handleScaffoldInfo(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/scaffold/info/")
	parts := strings.Split(path, "/")

	if len(parts) < 2 || parts[0] == "" || parts[1] == "" {
		// No system specified — return available templates
		templates, err := r.ListTemplates()
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}

		templateNames := make([]map[string]string, 0)
		for _, t := range templates {
			templateNames = append(templateNames, map[string]string{
				"name":        t.Name,
				"version":     t.Version,
				"description": t.Description,
			})
		}

		writeJSON(w, 200, map[string]interface{}{
			"templates": templateNames,
		})
		return
	}

	workgroup := parts[0]
	systemName := parts[1]

	sys, err := r.GetSystem(workgroup, systemName, "main")
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	if sys == nil {
		writeJSON(w, 404, map[string]string{"error": "System not found"})
		return
	}

	// Get protocols with their script types
	scripts, err := r.GetScripts(sys.ID)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	type ProtoInfo struct {
		Name    string   `json:"name"`
		Scripts []string `json:"scripts"` // script types present
	}

	protoMap := make(map[string][]string)
	for _, s := range scripts {
		if s.Protocol != "" {
			protoMap[s.Protocol] = append(protoMap[s.Protocol], s.Type)
		}
	}

	var protocols []ProtoInfo
	for name, types := range protoMap {
		protocols = append(protocols, ProtoInfo{Name: name, Scripts: types})
	}

	// Also get available templates
	templates, _ := r.ListTemplates()
	templateNames := make([]string, 0)
	for _, t := range templates {
		templateNames = append(templateNames, t.Name)
	}

	writeJSON(w, 200, map[string]interface{}{
		"system":    systemName,
		"workgroup": workgroup,
		"protocols": protocols,
		"templates": templateNames,
	})
}

// isValidName checks that a name is safe for use as a system/protocol identifier
func isValidName(name string) bool {
	if name == "" || len(name) > 128 {
		return false
	}
	for _, c := range name {
		if !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '_' || c == '-') {
			return false
		}
	}
	// Must start with a letter or underscore
	first := name[0]
	return (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_'
}
