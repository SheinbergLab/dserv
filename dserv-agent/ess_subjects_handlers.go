// ess_subjects_handlers.go - HTTP handlers for workgroup-scoped subjects.
//
//   /api/v1/ess/subjects?workgroup=X   - GET list (?all=1 includes inactive), POST create
//   /api/v1/ess/subject/{workgroup}/{name} - GET, PUT (update), DELETE
//
// Routes are registered from RegisterConfigsHandlers (ess_configs_handlers.go).

package main

import (
	"encoding/json"
	"net/http"
	"strings"
)

// handleListSubjects: GET (list for a workgroup) or POST (create).
func (r *ESSRegistry) handleListSubjects(w http.ResponseWriter, req *http.Request) {
	switch req.Method {
	case http.MethodGet:
		workgroup := req.URL.Query().Get("workgroup")
		if workgroup == "" {
			http.Error(w, "workgroup parameter required", http.StatusBadRequest)
			return
		}
		includeInactive := req.URL.Query().Get("all") == "1"
		subjects, err := r.ListSubjects(workgroup, includeInactive)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		if subjects == nil {
			subjects = []*ESSSubject{}
		}
		writeJSON(w, 200, subjects)

	case http.MethodPost:
		// Upsert: create if absent, else replace the mutable fields (so
		// `dservctl subjects add` is idempotent, matching `users add`).
		workgroup := req.URL.Query().Get("workgroup")
		if workgroup == "" {
			http.Error(w, "workgroup parameter required", http.StatusBadRequest)
			return
		}
		var sr SubjectRequest
		if err := json.NewDecoder(req.Body).Decode(&sr); err != nil {
			writeJSON(w, 400, map[string]string{"error": "Invalid JSON"})
			return
		}
		if sr.Name == "" {
			writeJSON(w, 400, map[string]string{"error": "name required"})
			return
		}

		existing, err := r.GetSubject(workgroup, sr.Name)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		if existing == nil {
			active := true // default active on create
			if sr.Active != nil {
				active = *sr.Active
			}
			subject := &ESSSubject{
				Workgroup:   workgroup,
				Name:        sr.Name,
				DisplayName: sr.DisplayName,
				Species:     sr.Species,
				Active:      active,
				Description: sr.Description,
				RegistryURL: sr.RegistryURL,
			}
			id, err := r.CreateSubject(subject)
			if err != nil {
				writeJSON(w, 500, map[string]string{"error": err.Error()})
				return
			}
			subject.ID = id
			writeJSON(w, 201, map[string]interface{}{"success": true, "id": id, "created": true, "subject": subject})
			return
		}

		existing.DisplayName = sr.DisplayName
		existing.Species = sr.Species
		existing.Description = sr.Description
		if sr.Active != nil {
			existing.Active = *sr.Active
		}
		if sr.RegistryURL != "" {
			existing.RegistryURL = sr.RegistryURL
		}
		if err := r.UpdateSubject(existing); err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, 200, map[string]interface{}{"success": true, "id": existing.ID, "created": false, "subject": existing})

	default:
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
	}
}

// handleSeedSubjects: POST — register a subject for each distinct subject
// referenced by the workgroup's configs that isn't already registered.
func (r *ESSRegistry) handleSeedSubjects(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	workgroup := req.URL.Query().Get("workgroup")
	if workgroup == "" {
		http.Error(w, "workgroup parameter required", http.StatusBadRequest)
		return
	}
	added, err := r.SeedSubjectsFromConfigs(workgroup)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	if added == nil {
		added = []string{}
	}
	writeJSON(w, 200, map[string]interface{}{"success": true, "added": added, "count": len(added)})
}

// handleSubject: GET / PUT / DELETE a single subject at /{workgroup}/{name}.
func (r *ESSRegistry) handleSubject(w http.ResponseWriter, req *http.Request) {
	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/subject/")
	parts := strings.Split(path, "/")
	if len(parts) < 2 || parts[0] == "" || parts[1] == "" {
		http.Error(w, "Invalid path: need workgroup/name", http.StatusBadRequest)
		return
	}
	workgroup := parts[0]
	name := parts[1]

	switch req.Method {
	case http.MethodGet:
		subject, err := r.GetSubject(workgroup, name)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		if subject == nil {
			writeJSON(w, 404, map[string]string{"error": "Subject not found"})
			return
		}
		writeJSON(w, 200, subject)

	case http.MethodPut:
		existing, err := r.GetSubject(workgroup, name)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		if existing == nil {
			writeJSON(w, 404, map[string]string{"error": "Subject not found"})
			return
		}
		var sr SubjectRequest
		if err := json.NewDecoder(req.Body).Decode(&sr); err != nil {
			writeJSON(w, 400, map[string]string{"error": "Invalid JSON"})
			return
		}
		existing.DisplayName = sr.DisplayName
		existing.Species = sr.Species
		existing.Description = sr.Description
		if sr.Active != nil {
			existing.Active = *sr.Active
		}
		if sr.RegistryURL != "" {
			existing.RegistryURL = sr.RegistryURL
		}
		if err := r.UpdateSubject(existing); err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, 200, map[string]interface{}{"success": true, "subject": existing})

	case http.MethodDelete:
		if err := r.DeleteSubject(workgroup, name); err != nil {
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
