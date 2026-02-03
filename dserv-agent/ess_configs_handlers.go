// ess_configs_handlers.go - HTTP handlers for Projects, Configs, and Queues API
//
// RESTful API for managing experiment configurations:
//   /api/v1/ess/projectdefs - Project definitions (list, create)
//   /api/v1/ess/projectdef/{workgroup}/{name} - Single project (get, update, delete)
//   /api/v1/ess/configs/{workgroup}/{project} - Configs in a project
//   /api/v1/ess/config/{workgroup}/{project}/{name} - Single config
//   /api/v1/ess/queues/{workgroup}/{project} - Queues in a project
//   /api/v1/ess/queue/{workgroup}/{project}/{name} - Single queue
//   /api/v1/ess/queue-items/{workgroup}/{project}/{queue} - Queue items
//   /api/v1/ess/bundle/{workgroup}/{project} - Push/pull complete project bundles

package main

import (
	"encoding/json"
	"net/http"
	"strconv"
	"strings"
)

// RegisterConfigsHandlers registers all config-related HTTP endpoints
func (r *ESSRegistry) RegisterConfigsHandlers(mux *http.ServeMux, authMiddleware func(http.HandlerFunc) http.HandlerFunc) {
	// Ensure tables exist
	r.migrateConfigsTables()

	// Project definitions
	mux.HandleFunc("/api/v1/ess/projectdefs", authMiddleware(r.handleListProjectDefs))
	mux.HandleFunc("/api/v1/ess/projectdef/", authMiddleware(r.handleProjectDef))

	// Configs
	mux.HandleFunc("/api/v1/ess/configs/", authMiddleware(r.handleConfigs))
	mux.HandleFunc("/api/v1/ess/config/", authMiddleware(r.handleConfig))

	// Queues
	mux.HandleFunc("/api/v1/ess/queues/", authMiddleware(r.handleQueues))
	mux.HandleFunc("/api/v1/ess/queue/", authMiddleware(r.handleQueue))

	// Queue items
	mux.HandleFunc("/api/v1/ess/queue-items/", authMiddleware(r.handleQueueItems))

	// Bundle operations (push/pull)
	mux.HandleFunc("/api/v1/ess/bundle/", authMiddleware(r.handleBundle))
}

// ============ Project Definition Handlers ============

// GET /api/v1/ess/projectdefs?workgroup=xxx
func (r *ESSRegistry) handleListProjectDefs(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	workgroup := req.URL.Query().Get("workgroup")
	if workgroup == "" {
		http.Error(w, "workgroup parameter required", http.StatusBadRequest)
		return
	}

	projects, err := r.ListProjectDefs(workgroup)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	writeJSON(w, 200, map[string]interface{}{
		"workgroup": workgroup,
		"projects":  projects,
	})
}

// GET/POST/PUT/DELETE /api/v1/ess/projectdef/{workgroup}/{name}
func (r *ESSRegistry) handleProjectDef(w http.ResponseWriter, req *http.Request) {
	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/projectdef/")
	parts := strings.Split(path, "/")

	if len(parts) < 1 {
		http.Error(w, "Invalid path: need workgroup[/name]", http.StatusBadRequest)
		return
	}

	workgroup := parts[0]
	name := ""
	if len(parts) > 1 {
		name = parts[1]
	}

	switch req.Method {
	case http.MethodGet:
		if name == "" {
			http.Error(w, "Project name required for GET", http.StatusBadRequest)
			return
		}
		project, err := r.GetProjectDef(workgroup, name)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		if project == nil {
			writeJSON(w, 404, map[string]string{"error": "Project not found"})
			return
		}
		writeJSON(w, 200, project)

	case http.MethodPost:
		var projReq ProjectDefRequest
		if err := json.NewDecoder(req.Body).Decode(&projReq); err != nil {
			writeJSON(w, 400, map[string]string{"error": "Invalid JSON"})
			return
		}
		if projReq.Name == "" {
			writeJSON(w, 400, map[string]string{"error": "name required"})
			return
		}

		project := &ESSProjectDef{
			Workgroup:   workgroup,
			Name:        projReq.Name,
			Description: projReq.Description,
			Systems:     projReq.Systems,
			RegistryURL: projReq.RegistryURL,
		}

		id, err := r.CreateProjectDef(project)
		if err != nil {
			if strings.Contains(err.Error(), "UNIQUE") {
				writeJSON(w, 409, map[string]string{"error": "Project already exists"})
			} else {
				writeJSON(w, 500, map[string]string{"error": err.Error()})
			}
			return
		}
		project.ID = id
		writeJSON(w, 201, map[string]interface{}{"success": true, "id": id, "project": project})

	case http.MethodPut:
		if name == "" {
			http.Error(w, "Project name required for PUT", http.StatusBadRequest)
			return
		}

		existing, err := r.GetProjectDef(workgroup, name)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		if existing == nil {
			writeJSON(w, 404, map[string]string{"error": "Project not found"})
			return
		}

		var projReq ProjectDefRequest
		if err := json.NewDecoder(req.Body).Decode(&projReq); err != nil {
			writeJSON(w, 400, map[string]string{"error": "Invalid JSON"})
			return
		}

		existing.Description = projReq.Description
		if projReq.Systems != nil {
			existing.Systems = projReq.Systems
		}
		if projReq.RegistryURL != "" {
			existing.RegistryURL = projReq.RegistryURL
		}

		if err := r.UpdateProjectDef(existing); err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, 200, map[string]interface{}{"success": true, "project": existing})

	case http.MethodDelete:
		if name == "" {
			http.Error(w, "Project name required for DELETE", http.StatusBadRequest)
			return
		}
		if err := r.DeleteProjectDef(workgroup, name); err != nil {
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

// ============ Config Handlers ============

// GET /api/v1/ess/configs/{workgroup}/{project}
func (r *ESSRegistry) handleConfigs(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/configs/")
	parts := strings.Split(path, "/")

	if len(parts) < 2 {
		http.Error(w, "Invalid path: need workgroup/project", http.StatusBadRequest)
		return
	}

	workgroup := parts[0]
	projectName := parts[1]
	includeArchived := req.URL.Query().Get("archived") == "true"

	project, err := r.GetProjectDef(workgroup, projectName)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	if project == nil {
		writeJSON(w, 404, map[string]string{"error": "Project not found"})
		return
	}

	configs, err := r.ListConfigs(project.ID, includeArchived)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	writeJSON(w, 200, map[string]interface{}{
		"workgroup": workgroup,
		"project":   projectName,
		"configs":   configs,
	})
}

// GET/POST/PUT/DELETE /api/v1/ess/config/{workgroup}/{project}/{name}
func (r *ESSRegistry) handleConfig(w http.ResponseWriter, req *http.Request) {
	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/config/")
	parts := strings.Split(path, "/")

	if len(parts) < 2 {
		http.Error(w, "Invalid path: need workgroup/project[/name]", http.StatusBadRequest)
		return
	}

	workgroup := parts[0]
	projectName := parts[1]
	configName := ""
	if len(parts) > 2 {
		configName = parts[2]
	}

	project, err := r.GetProjectDef(workgroup, projectName)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	if project == nil {
		writeJSON(w, 404, map[string]string{"error": "Project not found"})
		return
	}

	switch req.Method {
	case http.MethodGet:
		if configName == "" {
			http.Error(w, "Config name required for GET", http.StatusBadRequest)
			return
		}
		config, err := r.GetConfig(project.ID, configName)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		if config == nil {
			writeJSON(w, 404, map[string]string{"error": "Config not found"})
			return
		}
		writeJSON(w, 200, config)

	case http.MethodPost:
		var cfgReq ConfigRequest
		if err := json.NewDecoder(req.Body).Decode(&cfgReq); err != nil {
			writeJSON(w, 400, map[string]string{"error": "Invalid JSON"})
			return
		}
		if cfgReq.Name == "" || cfgReq.System == "" || cfgReq.Protocol == "" || cfgReq.Variant == "" {
			writeJSON(w, 400, map[string]string{"error": "name, system, protocol, and variant required"})
			return
		}

		config := &ESSConfig{
			ProjectID:    project.ID,
			Name:         cfgReq.Name,
			Description:  cfgReq.Description,
			ScriptSource: cfgReq.ScriptSource,
			System:       cfgReq.System,
			Protocol:     cfgReq.Protocol,
			Variant:      cfgReq.Variant,
			Subject:      cfgReq.Subject,
			VariantArgs:  cfgReq.VariantArgs,
			Params:       cfgReq.Params,
			FileTemplate: cfgReq.FileTemplate,
			Tags:         cfgReq.Tags,
			CreatedBy:    cfgReq.CreatedBy,
		}

		id, err := r.CreateConfig(config)
		if err != nil {
			if strings.Contains(err.Error(), "UNIQUE") {
				writeJSON(w, 409, map[string]string{"error": "Config already exists"})
			} else {
				writeJSON(w, 500, map[string]string{"error": err.Error()})
			}
			return
		}
		config.ID = id
		writeJSON(w, 201, map[string]interface{}{"success": true, "id": id, "config": config})

	case http.MethodPut:
		if configName == "" {
			http.Error(w, "Config name required for PUT", http.StatusBadRequest)
			return
		}

		existing, err := r.GetConfig(project.ID, configName)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		if existing == nil {
			writeJSON(w, 404, map[string]string{"error": "Config not found"})
			return
		}

		var cfgReq ConfigRequest
		if err := json.NewDecoder(req.Body).Decode(&cfgReq); err != nil {
			writeJSON(w, 400, map[string]string{"error": "Invalid JSON"})
			return
		}

		// Update fields
		if cfgReq.Name != "" {
			existing.Name = cfgReq.Name
		}
		existing.Description = cfgReq.Description
		if cfgReq.ScriptSource != "" {
			existing.ScriptSource = cfgReq.ScriptSource
		}
		if cfgReq.System != "" {
			existing.System = cfgReq.System
		}
		if cfgReq.Protocol != "" {
			existing.Protocol = cfgReq.Protocol
		}
		if cfgReq.Variant != "" {
			existing.Variant = cfgReq.Variant
		}
		existing.Subject = cfgReq.Subject
		if cfgReq.VariantArgs != nil {
			existing.VariantArgs = cfgReq.VariantArgs
		}
		if cfgReq.Params != nil {
			existing.Params = cfgReq.Params
		}
		existing.FileTemplate = cfgReq.FileTemplate
		if cfgReq.Tags != nil {
			existing.Tags = cfgReq.Tags
		}

		if err := r.UpdateConfig(existing); err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, 200, map[string]interface{}{"success": true, "config": existing})

	case http.MethodDelete:
		if configName == "" {
			http.Error(w, "Config name required for DELETE", http.StatusBadRequest)
			return
		}

		existing, err := r.GetConfig(project.ID, configName)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		if existing == nil {
			writeJSON(w, 404, map[string]string{"error": "Config not found"})
			return
		}

		// Check for archive=true parameter
		if req.URL.Query().Get("archive") == "true" {
			if err := r.ArchiveConfig(existing.ID); err != nil {
				writeJSON(w, 400, map[string]string{"error": err.Error()})
				return
			}
			writeJSON(w, 200, map[string]interface{}{"success": true, "archived": true})
			return
		}

		if err := r.DeleteConfig(existing.ID); err != nil {
			writeJSON(w, 400, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, 200, map[string]interface{}{"success": true})

	default:
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
	}
}

// ============ Queue Handlers ============

// GET /api/v1/ess/queues/{workgroup}/{project}
func (r *ESSRegistry) handleQueues(w http.ResponseWriter, req *http.Request) {
	if req.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/queues/")
	parts := strings.Split(path, "/")

	if len(parts) < 2 {
		http.Error(w, "Invalid path: need workgroup/project", http.StatusBadRequest)
		return
	}

	workgroup := parts[0]
	projectName := parts[1]

	project, err := r.GetProjectDef(workgroup, projectName)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	if project == nil {
		writeJSON(w, 404, map[string]string{"error": "Project not found"})
		return
	}

	queues, err := r.ListQueues(project.ID)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	writeJSON(w, 200, map[string]interface{}{
		"workgroup": workgroup,
		"project":   projectName,
		"queues":    queues,
	})
}

// GET/POST/PUT/DELETE /api/v1/ess/queue/{workgroup}/{project}/{name}
func (r *ESSRegistry) handleQueue(w http.ResponseWriter, req *http.Request) {
	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/queue/")
	parts := strings.Split(path, "/")

	if len(parts) < 2 {
		http.Error(w, "Invalid path: need workgroup/project[/name]", http.StatusBadRequest)
		return
	}

	workgroup := parts[0]
	projectName := parts[1]
	queueName := ""
	if len(parts) > 2 {
		queueName = parts[2]
	}

	project, err := r.GetProjectDef(workgroup, projectName)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	if project == nil {
		writeJSON(w, 404, map[string]string{"error": "Project not found"})
		return
	}

	switch req.Method {
	case http.MethodGet:
		if queueName == "" {
			http.Error(w, "Queue name required for GET", http.StatusBadRequest)
			return
		}
		queue, err := r.GetQueue(project.ID, queueName)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		if queue == nil {
			writeJSON(w, 404, map[string]string{"error": "Queue not found"})
			return
		}
		writeJSON(w, 200, queue)

	case http.MethodPost:
		var queueReq QueueRequest
		if err := json.NewDecoder(req.Body).Decode(&queueReq); err != nil {
			writeJSON(w, 400, map[string]string{"error": "Invalid JSON"})
			return
		}
		if queueReq.Name == "" {
			writeJSON(w, 400, map[string]string{"error": "name required"})
			return
		}

		queue := &ESSQueue{
			ProjectID:    project.ID,
			Name:         queueReq.Name,
			Description:  queueReq.Description,
			AutoStart:    true,
			AutoAdvance:  true,
			AutoDatafile: true,
			CreatedBy:    queueReq.CreatedBy,
		}
		if queueReq.AutoStart != nil {
			queue.AutoStart = *queueReq.AutoStart
		}
		if queueReq.AutoAdvance != nil {
			queue.AutoAdvance = *queueReq.AutoAdvance
		}
		if queueReq.AutoDatafile != nil {
			queue.AutoDatafile = *queueReq.AutoDatafile
		}

		id, err := r.CreateQueue(queue)
		if err != nil {
			if strings.Contains(err.Error(), "UNIQUE") {
				writeJSON(w, 409, map[string]string{"error": "Queue already exists"})
			} else {
				writeJSON(w, 500, map[string]string{"error": err.Error()})
			}
			return
		}
		queue.ID = id
		writeJSON(w, 201, map[string]interface{}{"success": true, "id": id, "queue": queue})

	case http.MethodPut:
		if queueName == "" {
			http.Error(w, "Queue name required for PUT", http.StatusBadRequest)
			return
		}

		existing, err := r.GetQueue(project.ID, queueName)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		if existing == nil {
			writeJSON(w, 404, map[string]string{"error": "Queue not found"})
			return
		}

		var queueReq QueueRequest
		if err := json.NewDecoder(req.Body).Decode(&queueReq); err != nil {
			writeJSON(w, 400, map[string]string{"error": "Invalid JSON"})
			return
		}

		if queueReq.Name != "" {
			existing.Name = queueReq.Name
		}
		existing.Description = queueReq.Description
		if queueReq.AutoStart != nil {
			existing.AutoStart = *queueReq.AutoStart
		}
		if queueReq.AutoAdvance != nil {
			existing.AutoAdvance = *queueReq.AutoAdvance
		}
		if queueReq.AutoDatafile != nil {
			existing.AutoDatafile = *queueReq.AutoDatafile
		}

		if err := r.UpdateQueue(existing); err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, 200, map[string]interface{}{"success": true, "queue": existing})

	case http.MethodDelete:
		if queueName == "" {
			http.Error(w, "Queue name required for DELETE", http.StatusBadRequest)
			return
		}

		existing, err := r.GetQueue(project.ID, queueName)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		if existing == nil {
			writeJSON(w, 404, map[string]string{"error": "Queue not found"})
			return
		}

		if err := r.DeleteQueue(existing.ID); err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, 200, map[string]interface{}{"success": true})

	default:
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
	}
}

// ============ Queue Item Handlers ============

// GET/POST/PUT/DELETE /api/v1/ess/queue-items/{workgroup}/{project}/{queue}[/{position}]
func (r *ESSRegistry) handleQueueItems(w http.ResponseWriter, req *http.Request) {
	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/queue-items/")
	parts := strings.Split(path, "/")

	if len(parts) < 3 {
		http.Error(w, "Invalid path: need workgroup/project/queue", http.StatusBadRequest)
		return
	}

	workgroup := parts[0]
	projectName := parts[1]
	queueName := parts[2]
	itemPosition := -1
	if len(parts) > 3 {
		if pos, err := strconv.Atoi(parts[3]); err == nil {
			itemPosition = pos
		}
	}

	project, err := r.GetProjectDef(workgroup, projectName)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	if project == nil {
		writeJSON(w, 404, map[string]string{"error": "Project not found"})
		return
	}

	queue, err := r.GetQueue(project.ID, queueName)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	if queue == nil {
		writeJSON(w, 404, map[string]string{"error": "Queue not found"})
		return
	}

	switch req.Method {
	case http.MethodGet:
		items, err := r.GetQueueItems(queue.ID)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, 200, map[string]interface{}{
			"queue": queueName,
			"items": items,
		})

	case http.MethodPost:
		var itemReq QueueItemRequest
		if err := json.NewDecoder(req.Body).Decode(&itemReq); err != nil {
			writeJSON(w, 400, map[string]string{"error": "Invalid JSON"})
			return
		}

		// Resolve config ID from name if needed
		configID := itemReq.ConfigID
		if configID == 0 && itemReq.ConfigName != "" {
			config, err := r.GetConfig(project.ID, itemReq.ConfigName)
			if err != nil || config == nil {
				writeJSON(w, 400, map[string]string{"error": "Config not found: " + itemReq.ConfigName})
				return
			}
			configID = config.ID
		}
		if configID == 0 {
			writeJSON(w, 400, map[string]string{"error": "configId or configName required"})
			return
		}

		item := &ESSQueueItem{
			QueueID:     queue.ID,
			ConfigID:    configID,
			Position:    -1, // Append
			RepeatCount: itemReq.RepeatCount,
			PauseAfter:  itemReq.PauseAfter,
			Notes:       itemReq.Notes,
		}
		if itemReq.Position != nil {
			item.Position = *itemReq.Position
		}
		if item.RepeatCount < 1 {
			item.RepeatCount = 1
		}

		id, err := r.AddQueueItem(queue.ID, item)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		item.ID = id
		writeJSON(w, 201, map[string]interface{}{"success": true, "id": id, "item": item})

	case http.MethodPut:
		// Reorder items if body contains "order" array
		var body map[string]interface{}
		if err := json.NewDecoder(req.Body).Decode(&body); err != nil {
			writeJSON(w, 400, map[string]string{"error": "Invalid JSON"})
			return
		}

		if orderRaw, ok := body["order"]; ok {
			// Reorder operation
			orderSlice, ok := orderRaw.([]interface{})
			if !ok {
				writeJSON(w, 400, map[string]string{"error": "order must be array of item IDs"})
				return
			}
			var itemIDs []int64
			for _, v := range orderSlice {
				switch id := v.(type) {
				case float64:
					itemIDs = append(itemIDs, int64(id))
				case int64:
					itemIDs = append(itemIDs, id)
				}
			}
			if err := r.ReorderQueueItems(queue.ID, itemIDs); err != nil {
				writeJSON(w, 500, map[string]string{"error": err.Error()})
				return
			}
			writeJSON(w, 200, map[string]interface{}{"success": true, "reordered": len(itemIDs)})
			return
		}

		// Update single item by position
		if itemPosition < 0 {
			writeJSON(w, 400, map[string]string{"error": "Item position required for update"})
			return
		}

		// Find item by position
		items, _ := r.GetQueueItems(queue.ID)
		var targetItem *ESSQueueItem
		for i := range items {
			if items[i].Position == itemPosition {
				targetItem = &items[i]
				break
			}
		}
		if targetItem == nil {
			writeJSON(w, 404, map[string]string{"error": "Item not found at position"})
			return
		}

		var itemReq QueueItemRequest
		jsonBytes, _ := json.Marshal(body)
		json.Unmarshal(jsonBytes, &itemReq)

		if itemReq.RepeatCount > 0 {
			targetItem.RepeatCount = itemReq.RepeatCount
		}
		targetItem.PauseAfter = itemReq.PauseAfter
		targetItem.Notes = itemReq.Notes

		if err := r.UpdateQueueItem(targetItem); err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, 200, map[string]interface{}{"success": true, "item": targetItem})

	case http.MethodDelete:
		if itemPosition < 0 {
			writeJSON(w, 400, map[string]string{"error": "Item position required for DELETE"})
			return
		}

		// Find item by position
		items, _ := r.GetQueueItems(queue.ID)
		var targetID int64
		for _, item := range items {
			if item.Position == itemPosition {
				targetID = item.ID
				break
			}
		}
		if targetID == 0 {
			writeJSON(w, 404, map[string]string{"error": "Item not found at position"})
			return
		}

		if err := r.RemoveQueueItem(targetID); err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, 200, map[string]interface{}{"success": true})

	default:
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
	}
}

// ============ Bundle Handlers (Push/Pull) ============

// GET/POST /api/v1/ess/bundle/{workgroup}/{project}
func (r *ESSRegistry) handleBundle(w http.ResponseWriter, req *http.Request) {
	path := strings.TrimPrefix(req.URL.Path, "/api/v1/ess/bundle/")
	parts := strings.Split(path, "/")

	if len(parts) < 2 {
		http.Error(w, "Invalid path: need workgroup/project", http.StatusBadRequest)
		return
	}

	workgroup := parts[0]
	projectName := parts[1]

	switch req.Method {
	case http.MethodGet:
		// Export (pull from registry)
		exportedBy := req.URL.Query().Get("exportedBy")
		sourceRig := req.URL.Query().Get("sourceRig")

		bundle, err := r.ExportProjectBundle(workgroup, projectName, exportedBy, sourceRig)
		if err != nil {
			if strings.Contains(err.Error(), "not found") {
				writeJSON(w, 404, map[string]string{"error": err.Error()})
			} else {
				writeJSON(w, 500, map[string]string{"error": err.Error()})
			}
			return
		}

		writeJSON(w, 200, bundle)

	case http.MethodPost:
		// Import (push to registry)
		var bundle ESSProjectBundle
		if err := json.NewDecoder(req.Body).Decode(&bundle); err != nil {
			writeJSON(w, 400, map[string]string{"error": "Invalid JSON: " + err.Error()})
			return
		}

		result, err := r.ImportProjectBundle(workgroup, &bundle)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}

		writeJSON(w, 200, result)

	default:
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
	}
}
