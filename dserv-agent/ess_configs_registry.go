// ess_configs_registry.go - Database operations for Projects, Configs, and Queues
//
// Extends ESSRegistry with methods for managing experiment configurations.
// These are stored separately from the ESS script registry but use the same
// database and workgroup model.

package main

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"time"
)

// ============ Schema Migration ============

// migrateConfigsTables adds the configs-related tables to the database
func (r *ESSRegistry) migrateConfigsTables() error {
	schema := `
	-- Project definitions (containers for configs and queues)
	CREATE TABLE IF NOT EXISTS ess_project_defs (
		id INTEGER PRIMARY KEY AUTOINCREMENT,
		workgroup TEXT NOT NULL,
		name TEXT NOT NULL,
		description TEXT DEFAULT '',
		systems TEXT DEFAULT '[]',
		registry_url TEXT DEFAULT '',
		last_sync_at INTEGER,
		created_at INTEGER NOT NULL,
		updated_at INTEGER NOT NULL,
		UNIQUE(workgroup, name)
	);

	-- Configs (experiment configuration snapshots)
	CREATE TABLE IF NOT EXISTS ess_configs (
		id INTEGER PRIMARY KEY AUTOINCREMENT,
		project_id INTEGER NOT NULL REFERENCES ess_project_defs(id) ON DELETE CASCADE,
		
		name TEXT NOT NULL,
		description TEXT DEFAULT '',
		
		script_source TEXT DEFAULT '',
		system TEXT NOT NULL,
		protocol TEXT NOT NULL,
		variant TEXT NOT NULL,
		subject TEXT DEFAULT '',
		variant_args TEXT DEFAULT '{}',
		params TEXT DEFAULT '{}',
		
		file_template TEXT DEFAULT '',
		
		tags TEXT DEFAULT '[]',
		created_by TEXT DEFAULT '',
		created_at INTEGER NOT NULL,
		updated_at INTEGER NOT NULL,
		last_used_at INTEGER,
		use_count INTEGER DEFAULT 0,
		archived INTEGER DEFAULT 0,
		
		UNIQUE(project_id, name)
	);

	-- Queues (ordered sequences of configs)
	CREATE TABLE IF NOT EXISTS ess_queues (
		id INTEGER PRIMARY KEY AUTOINCREMENT,
		project_id INTEGER NOT NULL REFERENCES ess_project_defs(id) ON DELETE CASCADE,
		
		name TEXT NOT NULL,
		description TEXT DEFAULT '',
		
		auto_start INTEGER DEFAULT 1,
		auto_advance INTEGER DEFAULT 1,
		auto_datafile INTEGER DEFAULT 1,
		
		created_at INTEGER NOT NULL,
		created_by TEXT DEFAULT '',
		updated_at INTEGER NOT NULL,
		
		UNIQUE(project_id, name)
	);

	-- Queue items (config references within a queue)
	CREATE TABLE IF NOT EXISTS ess_queue_items (
		id INTEGER PRIMARY KEY AUTOINCREMENT,
		queue_id INTEGER NOT NULL REFERENCES ess_queues(id) ON DELETE CASCADE,
		config_id INTEGER NOT NULL REFERENCES ess_configs(id),
		
		position INTEGER NOT NULL,
		repeat_count INTEGER DEFAULT 1,
		pause_after INTEGER DEFAULT 0,
		notes TEXT DEFAULT '',
		
		UNIQUE(queue_id, position)
	);

	-- Indexes
	CREATE INDEX IF NOT EXISTS idx_ess_project_defs_workgroup ON ess_project_defs(workgroup);
	CREATE INDEX IF NOT EXISTS idx_ess_configs_project ON ess_configs(project_id);
	CREATE INDEX IF NOT EXISTS idx_ess_configs_system ON ess_configs(system, protocol, variant);
	CREATE INDEX IF NOT EXISTS idx_ess_configs_archived ON ess_configs(archived);
	CREATE INDEX IF NOT EXISTS idx_ess_queues_project ON ess_queues(project_id);
	CREATE INDEX IF NOT EXISTS idx_ess_queue_items_queue ON ess_queue_items(queue_id, position);
	CREATE INDEX IF NOT EXISTS idx_ess_queue_items_config ON ess_queue_items(config_id);

	-- Bundle push history (audit log for recovery)
	CREATE TABLE IF NOT EXISTS ess_bundle_history (
		id INTEGER PRIMARY KEY AUTOINCREMENT,
		workgroup TEXT NOT NULL,
		project_name TEXT NOT NULL,
		bundle_json TEXT NOT NULL,
		pushed_by TEXT DEFAULT '',
		source_rig TEXT DEFAULT '',
		pushed_at INTEGER NOT NULL
	);
	CREATE INDEX IF NOT EXISTS idx_ess_bundle_history_project ON ess_bundle_history(workgroup, project_name);
	`
	_, err := r.db.Exec(schema)
	return err
}

// ============ Project Definition Operations ============

func (r *ESSRegistry) ListProjectDefs(workgroup string) ([]*ESSProjectDef, error) {
	rows, err := r.db.Query(`
		SELECT id, workgroup, name, description, systems, registry_url, 
		       last_sync_at, created_at, updated_at
		FROM ess_project_defs WHERE workgroup = ? ORDER BY name
	`, workgroup)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var projects []*ESSProjectDef
	for rows.Next() {
		p := &ESSProjectDef{}
		var systemsJSON string
		var lastSyncAt sql.NullInt64
		var createdAt, updatedAt int64
		
		err := rows.Scan(&p.ID, &p.Workgroup, &p.Name, &p.Description, &systemsJSON,
			&p.RegistryURL, &lastSyncAt, &createdAt, &updatedAt)
		if err != nil {
			return nil, err
		}
		
		p.CreatedAt = time.Unix(createdAt, 0)
		p.UpdatedAt = time.Unix(updatedAt, 0)
		if lastSyncAt.Valid {
			t := time.Unix(lastSyncAt.Int64, 0)
			p.LastSyncAt = &t
		}
		json.Unmarshal([]byte(systemsJSON), &p.Systems)
		
		// Get counts
		r.db.QueryRow("SELECT COUNT(*) FROM ess_configs WHERE project_id = ? AND archived = 0", p.ID).Scan(&p.ConfigCount)
		r.db.QueryRow("SELECT COUNT(*) FROM ess_queues WHERE project_id = ?", p.ID).Scan(&p.QueueCount)
		
		projects = append(projects, p)
	}
	return projects, nil
}

func (r *ESSRegistry) GetProjectDef(workgroup, name string) (*ESSProjectDef, error) {
	p := &ESSProjectDef{}
	var systemsJSON string
	var lastSyncAt sql.NullInt64
	var createdAt, updatedAt int64
	
	err := r.db.QueryRow(`
		SELECT id, workgroup, name, description, systems, registry_url,
		       last_sync_at, created_at, updated_at
		FROM ess_project_defs WHERE workgroup = ? AND name = ?
	`, workgroup, name).Scan(&p.ID, &p.Workgroup, &p.Name, &p.Description, &systemsJSON,
		&p.RegistryURL, &lastSyncAt, &createdAt, &updatedAt)
	
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	
	p.CreatedAt = time.Unix(createdAt, 0)
	p.UpdatedAt = time.Unix(updatedAt, 0)
	if lastSyncAt.Valid {
		t := time.Unix(lastSyncAt.Int64, 0)
		p.LastSyncAt = &t
	}
	json.Unmarshal([]byte(systemsJSON), &p.Systems)
	
	r.db.QueryRow("SELECT COUNT(*) FROM ess_configs WHERE project_id = ? AND archived = 0", p.ID).Scan(&p.ConfigCount)
	r.db.QueryRow("SELECT COUNT(*) FROM ess_queues WHERE project_id = ?", p.ID).Scan(&p.QueueCount)
	
	return p, nil
}

func (r *ESSRegistry) CreateProjectDef(p *ESSProjectDef) (int64, error) {
	now := time.Now()
	p.CreatedAt = now
	p.UpdatedAt = now
	
	systemsJSON, _ := json.Marshal(p.Systems)
	if p.Systems == nil {
		systemsJSON = []byte("[]")
	}
	
	result, err := r.db.Exec(`
		INSERT INTO ess_project_defs (workgroup, name, description, systems, registry_url, created_at, updated_at)
		VALUES (?, ?, ?, ?, ?, ?, ?)
	`, p.Workgroup, p.Name, p.Description, string(systemsJSON), p.RegistryURL, now.Unix(), now.Unix())
	if err != nil {
		return 0, err
	}
	return result.LastInsertId()
}

func (r *ESSRegistry) UpdateProjectDef(p *ESSProjectDef) error {
	now := time.Now()
	p.UpdatedAt = now
	
	systemsJSON, _ := json.Marshal(p.Systems)
	if p.Systems == nil {
		systemsJSON = []byte("[]")
	}
	
	_, err := r.db.Exec(`
		UPDATE ess_project_defs SET description = ?, systems = ?, registry_url = ?, updated_at = ?
		WHERE id = ?
	`, p.Description, string(systemsJSON), p.RegistryURL, now.Unix(), p.ID)
	return err
}

func (r *ESSRegistry) DeleteProjectDef(workgroup, name string) error {
	result, err := r.db.Exec("DELETE FROM ess_project_defs WHERE workgroup = ? AND name = ?", workgroup, name)
	if err != nil {
		return err
	}
	rows, _ := result.RowsAffected()
	if rows == 0 {
		return fmt.Errorf("project not found: %s/%s", workgroup, name)
	}
	return nil
}

// ============ Config Operations ============

func (r *ESSRegistry) ListConfigs(projectID int64, includeArchived bool) ([]*ESSConfig, error) {
	query := `
		SELECT c.id, c.project_id, c.name, c.description, c.script_source,
		       c.system, c.protocol, c.variant, c.subject, c.variant_args, c.params,
		       c.file_template, c.tags, c.created_by, c.created_at, c.updated_at,
		       c.last_used_at, c.use_count, c.archived,
		       p.name as project_name
		FROM ess_configs c
		JOIN ess_project_defs p ON p.id = c.project_id
		WHERE c.project_id = ?`
	
	if !includeArchived {
		query += " AND c.archived = 0"
	}
	query += " ORDER BY c.name"
	
	rows, err := r.db.Query(query, projectID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	
	return r.scanConfigs(rows)
}

func (r *ESSRegistry) GetConfig(projectID int64, name string) (*ESSConfig, error) {
	rows, err := r.db.Query(`
		SELECT c.id, c.project_id, c.name, c.description, c.script_source,
		       c.system, c.protocol, c.variant, c.subject, c.variant_args, c.params,
		       c.file_template, c.tags, c.created_by, c.created_at, c.updated_at,
		       c.last_used_at, c.use_count, c.archived,
		       p.name as project_name
		FROM ess_configs c
		JOIN ess_project_defs p ON p.id = c.project_id
		WHERE c.project_id = ? AND c.name = ?
	`, projectID, name)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	
	configs, err := r.scanConfigs(rows)
	if err != nil {
		return nil, err
	}
	if len(configs) == 0 {
		return nil, nil
	}
	return configs[0], nil
}

func (r *ESSRegistry) GetConfigByID(id int64) (*ESSConfig, error) {
	rows, err := r.db.Query(`
		SELECT c.id, c.project_id, c.name, c.description, c.script_source,
		       c.system, c.protocol, c.variant, c.subject, c.variant_args, c.params,
		       c.file_template, c.tags, c.created_by, c.created_at, c.updated_at,
		       c.last_used_at, c.use_count, c.archived,
		       p.name as project_name
		FROM ess_configs c
		JOIN ess_project_defs p ON p.id = c.project_id
		WHERE c.id = ?
	`, id)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	
	configs, err := r.scanConfigs(rows)
	if err != nil {
		return nil, err
	}
	if len(configs) == 0 {
		return nil, nil
	}
	return configs[0], nil
}

func (r *ESSRegistry) scanConfigs(rows *sql.Rows) ([]*ESSConfig, error) {
	var configs []*ESSConfig
	for rows.Next() {
		c := &ESSConfig{}
		var variantArgsJSON, paramsJSON, tagsJSON string
		var createdAt, updatedAt int64
		var lastUsedAt sql.NullInt64
		var archived int
		
		err := rows.Scan(&c.ID, &c.ProjectID, &c.Name, &c.Description, &c.ScriptSource,
			&c.System, &c.Protocol, &c.Variant, &c.Subject, &variantArgsJSON, &paramsJSON,
			&c.FileTemplate, &tagsJSON, &c.CreatedBy, &createdAt, &updatedAt,
			&lastUsedAt, &c.UseCount, &archived, &c.ProjectName)
		if err != nil {
			return nil, err
		}
		
		c.CreatedAt = time.Unix(createdAt, 0)
		c.UpdatedAt = time.Unix(updatedAt, 0)
		if lastUsedAt.Valid {
			t := time.Unix(lastUsedAt.Int64, 0)
			c.LastUsedAt = &t
		}
		c.Archived = archived != 0
		
		json.Unmarshal([]byte(variantArgsJSON), &c.VariantArgs)
		json.Unmarshal([]byte(paramsJSON), &c.Params)
		json.Unmarshal([]byte(tagsJSON), &c.Tags)
		
		configs = append(configs, c)
	}
	return configs, nil
}

func (r *ESSRegistry) CreateConfig(c *ESSConfig) (int64, error) {
	now := time.Now()
	c.CreatedAt = now
	c.UpdatedAt = now
	
	variantArgsJSON, _ := json.Marshal(c.VariantArgs)
	if c.VariantArgs == nil {
		variantArgsJSON = []byte("{}")
	}
	paramsJSON, _ := json.Marshal(c.Params)
	if c.Params == nil {
		paramsJSON = []byte("{}")
	}
	tagsJSON, _ := json.Marshal(c.Tags)
	if c.Tags == nil {
		tagsJSON = []byte("[]")
	}
	
	result, err := r.db.Exec(`
		INSERT INTO ess_configs (project_id, name, description, script_source, system, protocol,
		                         variant, subject, variant_args, params, file_template, tags,
		                         created_by, created_at, updated_at, use_count, archived)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 0)
	`, c.ProjectID, c.Name, c.Description, c.ScriptSource, c.System, c.Protocol,
		c.Variant, c.Subject, string(variantArgsJSON), string(paramsJSON), c.FileTemplate,
		string(tagsJSON), c.CreatedBy, now.Unix(), now.Unix())
	if err != nil {
		return 0, err
	}
	return result.LastInsertId()
}

func (r *ESSRegistry) UpdateConfig(c *ESSConfig) error {
	now := time.Now()
	c.UpdatedAt = now
	
	variantArgsJSON, _ := json.Marshal(c.VariantArgs)
	paramsJSON, _ := json.Marshal(c.Params)
	tagsJSON, _ := json.Marshal(c.Tags)
	
	_, err := r.db.Exec(`
		UPDATE ess_configs SET name = ?, description = ?, script_source = ?, system = ?,
		       protocol = ?, variant = ?, subject = ?, variant_args = ?, params = ?,
		       file_template = ?, tags = ?, updated_at = ?
		WHERE id = ?
	`, c.Name, c.Description, c.ScriptSource, c.System, c.Protocol, c.Variant,
		c.Subject, string(variantArgsJSON), string(paramsJSON), c.FileTemplate,
		string(tagsJSON), now.Unix(), c.ID)
	return err
}

func (r *ESSRegistry) DeleteConfig(id int64) error {
	// Check if used in any queue
	var count int
	r.db.QueryRow("SELECT COUNT(*) FROM ess_queue_items WHERE config_id = ?", id).Scan(&count)
	if count > 0 {
		return fmt.Errorf("config is used in %d queue(s)", count)
	}
	
	_, err := r.db.Exec("DELETE FROM ess_configs WHERE id = ?", id)
	return err
}

func (r *ESSRegistry) ArchiveConfig(id int64) error {
	// Check if used in any queue
	var count int
	r.db.QueryRow("SELECT COUNT(*) FROM ess_queue_items WHERE config_id = ?", id).Scan(&count)
	if count > 0 {
		return fmt.Errorf("config is used in %d queue(s)", count)
	}
	
	now := time.Now()
	_, err := r.db.Exec(`
		UPDATE ess_configs SET archived = 1, 
		       name = name || '_archived_' || ?,
		       updated_at = ?
		WHERE id = ?
	`, now.Unix(), now.Unix(), id)
	return err
}

// ============ Queue Operations ============

func (r *ESSRegistry) ListQueues(projectID int64) ([]*ESSQueue, error) {
	rows, err := r.db.Query(`
		SELECT q.id, q.project_id, q.name, q.description, q.auto_start, q.auto_advance,
		       q.auto_datafile, q.created_at, q.created_by, q.updated_at,
		       p.name as project_name
		FROM ess_queues q
		JOIN ess_project_defs p ON p.id = q.project_id
		WHERE q.project_id = ? ORDER BY q.name
	`, projectID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	
	var queues []*ESSQueue
	for rows.Next() {
		q := &ESSQueue{}
		var autoStart, autoAdvance, autoDatafile int
		var createdAt, updatedAt int64
		
		err := rows.Scan(&q.ID, &q.ProjectID, &q.Name, &q.Description, &autoStart, &autoAdvance,
			&autoDatafile, &createdAt, &q.CreatedBy, &updatedAt, &q.ProjectName)
		if err != nil {
			return nil, err
		}
		
		q.AutoStart = autoStart != 0
		q.AutoAdvance = autoAdvance != 0
		q.AutoDatafile = autoDatafile != 0
		q.CreatedAt = time.Unix(createdAt, 0)
		q.UpdatedAt = time.Unix(updatedAt, 0)
		
		r.db.QueryRow("SELECT COUNT(*) FROM ess_queue_items WHERE queue_id = ?", q.ID).Scan(&q.ItemCount)
		
		queues = append(queues, q)
	}
	return queues, nil
}

func (r *ESSRegistry) GetQueue(projectID int64, name string) (*ESSQueue, error) {
	q := &ESSQueue{}
	var autoStart, autoAdvance, autoDatafile int
	var createdAt, updatedAt int64
	
	err := r.db.QueryRow(`
		SELECT q.id, q.project_id, q.name, q.description, q.auto_start, q.auto_advance,
		       q.auto_datafile, q.created_at, q.created_by, q.updated_at,
		       p.name as project_name
		FROM ess_queues q
		JOIN ess_project_defs p ON p.id = q.project_id
		WHERE q.project_id = ? AND q.name = ?
	`, projectID, name).Scan(&q.ID, &q.ProjectID, &q.Name, &q.Description, &autoStart, &autoAdvance,
		&autoDatafile, &createdAt, &q.CreatedBy, &updatedAt, &q.ProjectName)
	
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	
	q.AutoStart = autoStart != 0
	q.AutoAdvance = autoAdvance != 0
	q.AutoDatafile = autoDatafile != 0
	q.CreatedAt = time.Unix(createdAt, 0)
	q.UpdatedAt = time.Unix(updatedAt, 0)
	
	// Get items
	q.Items, _ = r.GetQueueItems(q.ID)
	q.ItemCount = len(q.Items)
	
	return q, nil
}

func (r *ESSRegistry) GetQueueByID(id int64) (*ESSQueue, error) {
	q := &ESSQueue{}
	var autoStart, autoAdvance, autoDatafile int
	var createdAt, updatedAt int64
	
	err := r.db.QueryRow(`
		SELECT q.id, q.project_id, q.name, q.description, q.auto_start, q.auto_advance,
		       q.auto_datafile, q.created_at, q.created_by, q.updated_at,
		       p.name as project_name
		FROM ess_queues q
		JOIN ess_project_defs p ON p.id = q.project_id
		WHERE q.id = ?
	`, id).Scan(&q.ID, &q.ProjectID, &q.Name, &q.Description, &autoStart, &autoAdvance,
		&autoDatafile, &createdAt, &q.CreatedBy, &updatedAt, &q.ProjectName)
	
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	
	q.AutoStart = autoStart != 0
	q.AutoAdvance = autoAdvance != 0
	q.AutoDatafile = autoDatafile != 0
	q.CreatedAt = time.Unix(createdAt, 0)
	q.UpdatedAt = time.Unix(updatedAt, 0)
	
	q.Items, _ = r.GetQueueItems(q.ID)
	q.ItemCount = len(q.Items)
	
	return q, nil
}

func (r *ESSRegistry) CreateQueue(q *ESSQueue) (int64, error) {
	now := time.Now()
	q.CreatedAt = now
	q.UpdatedAt = now
	
	autoStart := 0
	if q.AutoStart {
		autoStart = 1
	}
	autoAdvance := 0
	if q.AutoAdvance {
		autoAdvance = 1
	}
	autoDatafile := 0
	if q.AutoDatafile {
		autoDatafile = 1
	}
	
	result, err := r.db.Exec(`
		INSERT INTO ess_queues (project_id, name, description, auto_start, auto_advance,
		                        auto_datafile, created_at, created_by, updated_at)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
	`, q.ProjectID, q.Name, q.Description, autoStart, autoAdvance, autoDatafile,
		now.Unix(), q.CreatedBy, now.Unix())
	if err != nil {
		return 0, err
	}
	return result.LastInsertId()
}

func (r *ESSRegistry) UpdateQueue(q *ESSQueue) error {
	now := time.Now()
	q.UpdatedAt = now
	
	autoStart := 0
	if q.AutoStart {
		autoStart = 1
	}
	autoAdvance := 0
	if q.AutoAdvance {
		autoAdvance = 1
	}
	autoDatafile := 0
	if q.AutoDatafile {
		autoDatafile = 1
	}
	
	_, err := r.db.Exec(`
		UPDATE ess_queues SET name = ?, description = ?, auto_start = ?, auto_advance = ?,
		       auto_datafile = ?, updated_at = ?
		WHERE id = ?
	`, q.Name, q.Description, autoStart, autoAdvance, autoDatafile, now.Unix(), q.ID)
	return err
}

func (r *ESSRegistry) DeleteQueue(id int64) error {
	// Items deleted via CASCADE
	_, err := r.db.Exec("DELETE FROM ess_queues WHERE id = ?", id)
	return err
}

// ============ Queue Item Operations ============

func (r *ESSRegistry) GetQueueItems(queueID int64) ([]ESSQueueItem, error) {
	rows, err := r.db.Query(`
		SELECT qi.id, qi.queue_id, qi.config_id, qi.position, qi.repeat_count,
		       qi.pause_after, qi.notes, c.name as config_name
		FROM ess_queue_items qi
		JOIN ess_configs c ON c.id = qi.config_id
		WHERE qi.queue_id = ?
		ORDER BY qi.position
	`, queueID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	
	var items []ESSQueueItem
	for rows.Next() {
		item := ESSQueueItem{}
		err := rows.Scan(&item.ID, &item.QueueID, &item.ConfigID, &item.Position,
			&item.RepeatCount, &item.PauseAfter, &item.Notes, &item.ConfigName)
		if err != nil {
			return nil, err
		}
		items = append(items, item)
	}
	return items, nil
}

func (r *ESSRegistry) AddQueueItem(queueID int64, item *ESSQueueItem) (int64, error) {
	// Get next position if not specified
	if item.Position < 0 {
		var maxPos sql.NullInt64
		r.db.QueryRow("SELECT MAX(position) FROM ess_queue_items WHERE queue_id = ?", queueID).Scan(&maxPos)
		if maxPos.Valid {
			item.Position = int(maxPos.Int64) + 1
		} else {
			item.Position = 0
		}
	}
	
	if item.RepeatCount < 1 {
		item.RepeatCount = 1
	}
	
	result, err := r.db.Exec(`
		INSERT INTO ess_queue_items (queue_id, config_id, position, repeat_count, pause_after, notes)
		VALUES (?, ?, ?, ?, ?, ?)
	`, queueID, item.ConfigID, item.Position, item.RepeatCount, item.PauseAfter, item.Notes)
	if err != nil {
		return 0, err
	}
	
	// Update queue timestamp
	r.db.Exec("UPDATE ess_queues SET updated_at = ? WHERE id = ?", time.Now().Unix(), queueID)
	
	return result.LastInsertId()
}

func (r *ESSRegistry) UpdateQueueItem(item *ESSQueueItem) error {
	_, err := r.db.Exec(`
		UPDATE ess_queue_items SET config_id = ?, position = ?, repeat_count = ?,
		       pause_after = ?, notes = ?
		WHERE id = ?
	`, item.ConfigID, item.Position, item.RepeatCount, item.PauseAfter, item.Notes, item.ID)
	
	if err == nil {
		r.db.Exec("UPDATE ess_queues SET updated_at = ? WHERE id = ?", time.Now().Unix(), item.QueueID)
	}
	return err
}

func (r *ESSRegistry) RemoveQueueItem(itemID int64) error {
	// Get queue_id first for timestamp update
	var queueID int64
	r.db.QueryRow("SELECT queue_id FROM ess_queue_items WHERE id = ?", itemID).Scan(&queueID)
	
	_, err := r.db.Exec("DELETE FROM ess_queue_items WHERE id = ?", itemID)
	if err != nil {
		return err
	}
	
	// Renumber remaining items
	r.renumberQueueItems(queueID)
	r.db.Exec("UPDATE ess_queues SET updated_at = ? WHERE id = ?", time.Now().Unix(), queueID)
	
	return nil
}

func (r *ESSRegistry) renumberQueueItems(queueID int64) {
	rows, _ := r.db.Query("SELECT id FROM ess_queue_items WHERE queue_id = ? ORDER BY position", queueID)
	if rows == nil {
		return
	}
	defer rows.Close()
	
	pos := 0
	for rows.Next() {
		var id int64
		rows.Scan(&id)
		r.db.Exec("UPDATE ess_queue_items SET position = ? WHERE id = ?", pos, id)
		pos++
	}
}

func (r *ESSRegistry) ReorderQueueItems(queueID int64, itemIDs []int64) error {
	tx, err := r.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	
	for pos, id := range itemIDs {
		_, err := tx.Exec("UPDATE ess_queue_items SET position = ? WHERE id = ? AND queue_id = ?",
			pos, id, queueID)
		if err != nil {
			return err
		}
	}
	
	tx.Exec("UPDATE ess_queues SET updated_at = ? WHERE id = ?", time.Now().Unix(), queueID)
	return tx.Commit()
}

// ============ Bundle Operations (Push/Pull) ============

// ExportProjectBundle creates a complete bundle of a project for sync
func (r *ESSRegistry) ExportProjectBundle(workgroup, projectName, exportedBy, sourceRig string) (*ESSProjectBundle, error) {
	project, err := r.GetProjectDef(workgroup, projectName)
	if err != nil {
		return nil, err
	}
	if project == nil {
		return nil, fmt.Errorf("project not found: %s/%s", workgroup, projectName)
	}
	
	configs, err := r.ListConfigs(project.ID, false) // Don't include archived
	if err != nil {
		return nil, err
	}
	
	queues, err := r.ListQueues(project.ID)
	if err != nil {
		return nil, err
	}
	
	// Populate queue items
	for i := range queues {
		queues[i].Items, _ = r.GetQueueItems(queues[i].ID)
	}
	
	bundle := &ESSProjectBundle{
		Project:    *project,
		ExportedAt: time.Now(),
		ExportedBy: exportedBy,
		SourceRig:  sourceRig,
	}
	
	// Convert pointers to values for JSON
	for _, c := range configs {
		bundle.Configs = append(bundle.Configs, *c)
	}
	for _, q := range queues {
		bundle.Queues = append(bundle.Queues, *q)
	}
	
	return bundle, nil
}

// ImportProjectBundle imports a bundle into a workgroup, creating/updating as needed
func (r *ESSRegistry) ImportProjectBundle(workgroup string, bundle *ESSProjectBundle, replace bool) (*SyncResult, error) {
	result := &SyncResult{
		Success:     true,
		ProjectName: bundle.Project.Name,
	}
	
	// Start transaction
	tx, err := r.db.Begin()
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	
	// Save bundle snapshot for audit/recovery
	if bundleJSON, err := json.Marshal(bundle); err == nil {
		tx.Exec(`
			INSERT INTO ess_bundle_history (workgroup, project_name, bundle_json, pushed_by, source_rig, pushed_at)
			VALUES (?, ?, ?, ?, ?, ?)
		`, workgroup, bundle.Project.Name, string(bundleJSON), bundle.ExportedBy, bundle.SourceRig, time.Now().Unix())
	}
	
	// Create or update project
	existing, _ := r.GetProjectDef(workgroup, bundle.Project.Name)
	var projectID int64
	
	if existing != nil {
		projectID = existing.ID
		// Update existing project
		systemsJSON, _ := json.Marshal(bundle.Project.Systems)
		now := time.Now()
		_, err = tx.Exec(`
			UPDATE ess_project_defs SET description = ?, systems = ?, last_sync_at = ?, updated_at = ?
			WHERE id = ?
		`, bundle.Project.Description, string(systemsJSON), now.Unix(), now.Unix(), projectID)
		if err != nil {
			result.Errors = append(result.Errors, fmt.Sprintf("update project: %v", err))
		} else {
			result.Updated = append(result.Updated, "project:"+bundle.Project.Name)
		}
	} else {
		// Create new project
		systemsJSON, _ := json.Marshal(bundle.Project.Systems)
		now := time.Now()
		res, err := tx.Exec(`
			INSERT INTO ess_project_defs (workgroup, name, description, systems, last_sync_at, created_at, updated_at)
			VALUES (?, ?, ?, ?, ?, ?, ?)
		`, workgroup, bundle.Project.Name, bundle.Project.Description, string(systemsJSON),
			now.Unix(), now.Unix(), now.Unix())
		if err != nil {
			return nil, fmt.Errorf("create project: %w", err)
		}
		projectID, _ = res.LastInsertId()
		result.Created = append(result.Created, "project:"+bundle.Project.Name)
	}
	
	// Build a map of old config IDs to new config IDs (for queue item references)
	configIDMap := make(map[int64]int64)
	
	// Import configs
	for _, c := range bundle.Configs {
		oldID := c.ID
		c.ProjectID = projectID
		
		// Check if config exists
		var existingID int64
		err := tx.QueryRow("SELECT id FROM ess_configs WHERE project_id = ? AND name = ?",
			projectID, c.Name).Scan(&existingID)
		
		variantArgsJSON, _ := json.Marshal(c.VariantArgs)
		paramsJSON, _ := json.Marshal(c.Params)
		tagsJSON, _ := json.Marshal(c.Tags)
		now := time.Now()
		
		if err == sql.ErrNoRows {
			// Create new config
			res, err := tx.Exec(`
				INSERT INTO ess_configs (project_id, name, description, script_source, system, protocol,
				                         variant, subject, variant_args, params, file_template, tags,
				                         created_by, created_at, updated_at, use_count, archived)
				VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 0)
			`, projectID, c.Name, c.Description, c.ScriptSource, c.System, c.Protocol, c.Variant,
				c.Subject, string(variantArgsJSON), string(paramsJSON), c.FileTemplate,
				string(tagsJSON), c.CreatedBy, now.Unix(), now.Unix())
			if err != nil {
				result.Errors = append(result.Errors, fmt.Sprintf("create config %s: %v", c.Name, err))
				continue
			}
			newID, _ := res.LastInsertId()
			configIDMap[oldID] = newID
			result.Created = append(result.Created, "config:"+c.Name)
		} else if err == nil {
			// Update existing config
			_, err := tx.Exec(`
				UPDATE ess_configs SET description = ?, script_source = ?, system = ?, protocol = ?,
				       variant = ?, subject = ?, variant_args = ?, params = ?, file_template = ?,
				       tags = ?, updated_at = ?
				WHERE id = ?
			`, c.Description, c.ScriptSource, c.System, c.Protocol, c.Variant, c.Subject,
				string(variantArgsJSON), string(paramsJSON), c.FileTemplate, string(tagsJSON),
				now.Unix(), existingID)
			if err != nil {
				result.Errors = append(result.Errors, fmt.Sprintf("update config %s: %v", c.Name, err))
			} else {
				configIDMap[oldID] = existingID
				result.Updated = append(result.Updated, "config:"+c.Name)
			}
		}
	}
	
	// Import queues
	for _, q := range bundle.Queues {
		q.ProjectID = projectID
		
		// Check if queue exists
		var existingQueueID int64
		err := tx.QueryRow("SELECT id FROM ess_queues WHERE project_id = ? AND name = ?",
			projectID, q.Name).Scan(&existingQueueID)
		
		autoStart, autoAdvance, autoDatafile := 0, 0, 0
		if q.AutoStart {
			autoStart = 1
		}
		if q.AutoAdvance {
			autoAdvance = 1
		}
		if q.AutoDatafile {
			autoDatafile = 1
		}
		now := time.Now()
		
		var queueID int64
		if err == sql.ErrNoRows {
			// Create new queue
			res, err := tx.Exec(`
				INSERT INTO ess_queues (project_id, name, description, auto_start, auto_advance,
				                        auto_datafile, created_at, created_by, updated_at)
				VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
			`, projectID, q.Name, q.Description, autoStart, autoAdvance, autoDatafile,
				now.Unix(), q.CreatedBy, now.Unix())
			if err != nil {
				result.Errors = append(result.Errors, fmt.Sprintf("create queue %s: %v", q.Name, err))
				continue
			}
			queueID, _ = res.LastInsertId()
			result.Created = append(result.Created, "queue:"+q.Name)
		} else if err == nil {
			queueID = existingQueueID
			// Update existing queue
			_, err := tx.Exec(`
				UPDATE ess_queues SET description = ?, auto_start = ?, auto_advance = ?,
				       auto_datafile = ?, updated_at = ?
				WHERE id = ?
			`, q.Description, autoStart, autoAdvance, autoDatafile, now.Unix(), queueID)
			if err != nil {
				result.Errors = append(result.Errors, fmt.Sprintf("update queue %s: %v", q.Name, err))
				continue
			}
			// Delete existing items (will recreate)
			tx.Exec("DELETE FROM ess_queue_items WHERE queue_id = ?", queueID)
			result.Updated = append(result.Updated, "queue:"+q.Name)
		}
		
		// Add queue items with mapped config IDs
		for _, item := range q.Items {
			newConfigID, ok := configIDMap[item.ConfigID]
			if !ok {
				result.Errors = append(result.Errors, fmt.Sprintf("queue %s item %d: config ID %d not found",
					q.Name, item.Position, item.ConfigID))
				continue
			}
			
			_, err := tx.Exec(`
				INSERT INTO ess_queue_items (queue_id, config_id, position, repeat_count, pause_after, notes)
				VALUES (?, ?, ?, ?, ?, ?)
			`, queueID, newConfigID, item.Position, item.RepeatCount, item.PauseAfter, item.Notes)
			if err != nil {
				result.Errors = append(result.Errors, fmt.Sprintf("queue %s item %d: %v",
					q.Name, item.Position, err))
			}
		}
	}
	
	// If replace mode, remove configs and queues not in the bundle
	if replace {
		// Build sets of names that were in the bundle
		bundleConfigNames := make(map[string]bool)
		for _, c := range bundle.Configs {
			bundleConfigNames[c.Name] = true
		}
		bundleQueueNames := make(map[string]bool)
		for _, q := range bundle.Queues {
			bundleQueueNames[q.Name] = true
		}
		
		// Delete queues not in bundle (must delete queues first since queue_items reference configs)
		rows, err := tx.Query("SELECT id, name FROM ess_queues WHERE project_id = ?", projectID)
		if err == nil {
			var staleQueues []struct{ id int64; name string }
			for rows.Next() {
				var id int64
				var name string
				if rows.Scan(&id, &name) == nil && !bundleQueueNames[name] {
					staleQueues = append(staleQueues, struct{ id int64; name string }{id, name})
				}
			}
			rows.Close()
			for _, sq := range staleQueues {
				// Items deleted via CASCADE
				tx.Exec("DELETE FROM ess_queues WHERE id = ?", sq.id)
				result.Deleted = append(result.Deleted, "queue:"+sq.name)
			}
		}
		
		// Delete configs not in bundle (now safe since stale queues are gone)
		rows, err = tx.Query("SELECT id, name FROM ess_configs WHERE project_id = ? AND archived = 0", projectID)
		if err == nil {
			var staleConfigs []struct{ id int64; name string }
			for rows.Next() {
				var id int64
				var name string
				if rows.Scan(&id, &name) == nil && !bundleConfigNames[name] {
					staleConfigs = append(staleConfigs, struct{ id int64; name string }{id, name})
				}
			}
			rows.Close()
			for _, sc := range staleConfigs {
				// Check if still referenced by any remaining queue
				var refCount int
				tx.QueryRow("SELECT COUNT(*) FROM ess_queue_items WHERE config_id = ?", sc.id).Scan(&refCount)
				if refCount == 0 {
					tx.Exec("DELETE FROM ess_configs WHERE id = ?", sc.id)
					result.Deleted = append(result.Deleted, "config:"+sc.name)
				} else {
					result.Errors = append(result.Errors, fmt.Sprintf("config %s still referenced by queue items, not deleted", sc.name))
				}
			}
		}
	}
	
	if err := tx.Commit(); err != nil {
		return nil, err
	}
	
	// Auto-prune old history entries (keep last 50 per project)
	r.PruneBundleHistory(workgroup, bundle.Project.Name, 50)
	
	result.ConfigsCount = len(bundle.Configs)
	result.QueuesCount = len(bundle.Queues)
	result.Success = len(result.Errors) == 0
	
	return result, nil
}

// ============ Bundle History Operations ============

// BundleHistoryEntry represents a saved bundle snapshot
type BundleHistoryEntry struct {
	ID          int64  `json:"id"`
	Workgroup   string `json:"workgroup"`
	ProjectName string `json:"projectName"`
	PushedBy    string `json:"pushedBy"`
	SourceRig   string `json:"sourceRig"`
	PushedAt    int64  `json:"pushedAt"`
}

// ListBundleHistory returns recent push history for a project (without the full JSON)
func (r *ESSRegistry) ListBundleHistory(workgroup, projectName string, limit int) ([]BundleHistoryEntry, error) {
	if limit <= 0 {
		limit = 20
	}
	rows, err := r.db.Query(`
		SELECT id, workgroup, project_name, pushed_by, source_rig, pushed_at
		FROM ess_bundle_history
		WHERE workgroup = ? AND project_name = ?
		ORDER BY pushed_at DESC
		LIMIT ?
	`, workgroup, projectName, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var entries []BundleHistoryEntry
	for rows.Next() {
		var e BundleHistoryEntry
		if err := rows.Scan(&e.ID, &e.Workgroup, &e.ProjectName, &e.PushedBy, &e.SourceRig, &e.PushedAt); err != nil {
			continue
		}
		entries = append(entries, e)
	}
	return entries, nil
}

// GetBundleSnapshot retrieves the full bundle JSON for a history entry
func (r *ESSRegistry) GetBundleSnapshot(id int64) (*ESSProjectBundle, error) {
	var bundleJSON string
	err := r.db.QueryRow("SELECT bundle_json FROM ess_bundle_history WHERE id = ?", id).Scan(&bundleJSON)
	if err != nil {
		return nil, fmt.Errorf("bundle snapshot not found: %w", err)
	}

	var bundle ESSProjectBundle
	if err := json.Unmarshal([]byte(bundleJSON), &bundle); err != nil {
		return nil, fmt.Errorf("failed to parse bundle snapshot: %w", err)
	}
	return &bundle, nil
}

// PruneBundleHistory removes old entries, keeping the most recent N per project
func (r *ESSRegistry) PruneBundleHistory(workgroup, projectName string, keep int) (int64, error) {
	if keep <= 0 {
		keep = 50
	}
	res, err := r.db.Exec(`
		DELETE FROM ess_bundle_history
		WHERE workgroup = ? AND project_name = ? AND id NOT IN (
			SELECT id FROM ess_bundle_history
			WHERE workgroup = ? AND project_name = ?
			ORDER BY pushed_at DESC
			LIMIT ?
		)
	`, workgroup, projectName, workgroup, projectName, keep)
	if err != nil {
		return 0, err
	}
	return res.RowsAffected()
}
