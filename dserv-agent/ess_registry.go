// ess_registry.go - ESS Script Registry for dserv-agent
//
// Provides storage and management for ESS experiment scripts:
//   - Systems, protocols, variants, loaders, stim files
//   - Template library ("zoo") for bootstrapping workgroups
//   - Version history and rollback
//   - Advisory locking for concurrent editing
//   - Project definitions for scoping what's available on rigs
//
// Uses SQLite for storage (modernc.org/sqlite - pure Go, no CGO).
// Database files are standard SQLite format, compatible with sqlite3 CLI.

package main

import (
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io/ioutil"
	"log"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"

	_ "modernc.org/sqlite"
)

// ============ Registry ============

// ESSRegistry manages ESS scripts and metadata
type ESSRegistry struct {
	db     *sql.DB
	dbPath string
	mu     sync.RWMutex
}

// NewESSRegistry creates a new registry with the given database path
func NewESSRegistry(dbPath string) (*ESSRegistry, error) {
	// Ensure directory exists
	dir := filepath.Dir(dbPath)
	if err := os.MkdirAll(dir, 0755); err != nil {
		return nil, fmt.Errorf("failed to create database directory: %w", err)
	}

	db, err := sql.Open("sqlite", dbPath+"?_journal_mode=WAL&_busy_timeout=5000&_foreign_keys=ON")
	if err != nil {
		return nil, fmt.Errorf("failed to open database: %w", err)
	}

	// Test connection
	if err := db.Ping(); err != nil {
		return nil, fmt.Errorf("failed to connect to database: %w", err)
	}

	reg := &ESSRegistry{db: db, dbPath: dbPath}
	if err := reg.migrate(); err != nil {
		return nil, fmt.Errorf("failed to migrate database: %w", err)
	}

	// Start lock cleanup goroutine
	go reg.lockCleanupLoop()

	return reg, nil
}

// Close closes the database connection
func (r *ESSRegistry) Close() error {
	return r.db.Close()
}

// migrate creates or updates the database schema
func (r *ESSRegistry) migrate() error {
	schema := `
	CREATE TABLE IF NOT EXISTS ess_systems (
		id INTEGER PRIMARY KEY AUTOINCREMENT,
		workgroup TEXT NOT NULL,
		name TEXT NOT NULL,
		version TEXT NOT NULL,
		description TEXT DEFAULT '',
		author TEXT DEFAULT '',
		forked_from TEXT,
		forked_at INTEGER,
		created_at INTEGER NOT NULL,
		updated_at INTEGER NOT NULL,
		updated_by TEXT NOT NULL,
		UNIQUE(workgroup, name, version)
	);

	CREATE TABLE IF NOT EXISTS ess_scripts (
		id INTEGER PRIMARY KEY AUTOINCREMENT,
		system_id INTEGER NOT NULL REFERENCES ess_systems(id) ON DELETE CASCADE,
		protocol TEXT DEFAULT '',
		type TEXT NOT NULL,
		filename TEXT NOT NULL,
		content TEXT NOT NULL,
		checksum TEXT NOT NULL,
		updated_at INTEGER NOT NULL,
		updated_by TEXT NOT NULL,
		UNIQUE(system_id, protocol, type)
	);

	CREATE TABLE IF NOT EXISTS ess_libs (
		id INTEGER PRIMARY KEY AUTOINCREMENT,
		workgroup TEXT NOT NULL,
		name TEXT NOT NULL,
		version TEXT NOT NULL,
		filename TEXT NOT NULL,
		content TEXT NOT NULL,
		checksum TEXT NOT NULL,
		forked_from TEXT,
		created_at INTEGER NOT NULL,
		updated_at INTEGER NOT NULL,
		UNIQUE(workgroup, name, version)
	);

	CREATE TABLE IF NOT EXISTS ess_projects (
		id INTEGER PRIMARY KEY AUTOINCREMENT,
		workgroup TEXT NOT NULL,
		name TEXT NOT NULL,
		description TEXT DEFAULT '',
		config TEXT NOT NULL,
		created_at INTEGER NOT NULL,
		updated_at INTEGER NOT NULL,
		UNIQUE(workgroup, name)
	);

	CREATE TABLE IF NOT EXISTS ess_locks (
		key TEXT PRIMARY KEY,
		locked_by TEXT NOT NULL,
		locked_at INTEGER NOT NULL,
		expires_at INTEGER NOT NULL
	);

	CREATE TABLE IF NOT EXISTS ess_script_history (
		id INTEGER PRIMARY KEY AUTOINCREMENT,
		script_id INTEGER NOT NULL,
		content TEXT NOT NULL,
		checksum TEXT NOT NULL,
		saved_at INTEGER NOT NULL,
		saved_by TEXT NOT NULL,
		comment TEXT DEFAULT ''
	);

	CREATE TABLE IF NOT EXISTS ess_users (
		id INTEGER PRIMARY KEY AUTOINCREMENT,
		workgroup TEXT NOT NULL,
		username TEXT NOT NULL,
		full_name TEXT DEFAULT '',
		email TEXT DEFAULT '',
		role TEXT DEFAULT 'editor',
		created_at INTEGER NOT NULL,
		updated_at INTEGER NOT NULL,
		UNIQUE(workgroup, username)
	);

	CREATE INDEX IF NOT EXISTS idx_ess_systems_workgroup ON ess_systems(workgroup);
	CREATE INDEX IF NOT EXISTS idx_ess_systems_name ON ess_systems(workgroup, name);
	CREATE INDEX IF NOT EXISTS idx_ess_scripts_system ON ess_scripts(system_id);
	CREATE INDEX IF NOT EXISTS idx_ess_scripts_protocol ON ess_scripts(system_id, protocol);
	CREATE INDEX IF NOT EXISTS idx_ess_libs_workgroup ON ess_libs(workgroup);
	CREATE INDEX IF NOT EXISTS idx_ess_projects_workgroup ON ess_projects(workgroup);
	CREATE INDEX IF NOT EXISTS idx_ess_history_script ON ess_script_history(script_id);
	CREATE INDEX IF NOT EXISTS idx_ess_history_saved ON ess_script_history(script_id, saved_at DESC);
	`
	_, err := r.db.Exec(schema)
	return err
}

func (r *ESSRegistry) lockCleanupLoop() {
	ticker := time.NewTicker(1 * time.Minute)
	defer ticker.Stop()
	for range ticker.C {
		r.cleanupExpiredLocks()
	}
}

func (r *ESSRegistry) cleanupExpiredLocks() {
	now := time.Now().Unix()
	_, err := r.db.Exec("DELETE FROM ess_locks WHERE expires_at < ?", now)
	if err != nil {
		log.Printf("ESS Registry: failed to cleanup locks: %v", err)
	}
}

// ============ System Operations ============

func (r *ESSRegistry) ListSystems(workgroup string) ([]*ESSSystem, error) {
	rows, err := r.db.Query(`
		SELECT id, workgroup, name, version, description, author, 
		       forked_from, forked_at, created_at, updated_at, updated_by
		FROM ess_systems WHERE workgroup = ? ORDER BY name, version
	`, workgroup)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var systems []*ESSSystem
	for rows.Next() {
		sys := &ESSSystem{}
		var forkedFrom sql.NullString
		var forkedAt sql.NullInt64
		var createdAt, updatedAt int64
		err := rows.Scan(&sys.ID, &sys.Workgroup, &sys.Name, &sys.Version,
			&sys.Description, &sys.Author, &forkedFrom, &forkedAt,
			&createdAt, &updatedAt, &sys.UpdatedBy)
		if err != nil {
			return nil, err
		}
		sys.CreatedAt = time.Unix(createdAt, 0)
		sys.UpdatedAt = time.Unix(updatedAt, 0)
		if forkedFrom.Valid {
			sys.ForkedFrom = forkedFrom.String
		}
		if forkedAt.Valid {
			t := time.Unix(forkedAt.Int64, 0)
			sys.ForkedAt = &t
		}
		sys.Protocols, _ = r.getSystemProtocols(sys.ID)
		r.db.QueryRow("SELECT COUNT(*) FROM ess_scripts WHERE system_id = ?", sys.ID).Scan(&sys.Scripts)
		systems = append(systems, sys)
	}
	return systems, nil
}

func (r *ESSRegistry) GetSystem(workgroup, name, version string) (*ESSSystem, error) {
	query := `SELECT id, workgroup, name, version, description, author,
	                 forked_from, forked_at, created_at, updated_at, updated_by
	          FROM ess_systems WHERE workgroup = ? AND name = ?`
	args := []interface{}{workgroup, name}

	if version != "" && version != "latest" {
		query += " AND version = ?"
		args = append(args, version)
	} else {
		query += " ORDER BY created_at DESC LIMIT 1"
	}

	sys := &ESSSystem{}
	var forkedFrom sql.NullString
	var forkedAt sql.NullInt64
	var createdAt, updatedAt int64
	err := r.db.QueryRow(query, args...).Scan(&sys.ID, &sys.Workgroup, &sys.Name, &sys.Version,
		&sys.Description, &sys.Author, &forkedFrom, &forkedAt, &createdAt, &updatedAt, &sys.UpdatedBy)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	sys.CreatedAt = time.Unix(createdAt, 0)
	sys.UpdatedAt = time.Unix(updatedAt, 0)
	if forkedFrom.Valid {
		sys.ForkedFrom = forkedFrom.String
	}
	if forkedAt.Valid {
		t := time.Unix(forkedAt.Int64, 0)
		sys.ForkedAt = &t
	}
	sys.Protocols, _ = r.getSystemProtocols(sys.ID)
	return sys, nil
}

func (r *ESSRegistry) getSystemProtocols(systemID int64) ([]string, error) {
	rows, err := r.db.Query(`SELECT DISTINCT protocol FROM ess_scripts 
	                         WHERE system_id = ? AND protocol != '' ORDER BY protocol`, systemID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var protocols []string
	for rows.Next() {
		var p string
		if err := rows.Scan(&p); err != nil {
			return nil, err
		}
		protocols = append(protocols, p)
	}
	return protocols, nil
}

func (r *ESSRegistry) CreateSystem(sys *ESSSystem) (int64, error) {
	now := time.Now()
	sys.CreatedAt = now
	sys.UpdatedAt = now
	var forkedAt interface{}
	if sys.ForkedAt != nil {
		forkedAt = sys.ForkedAt.Unix()
	}
	result, err := r.db.Exec(`INSERT INTO ess_systems 
		(workgroup, name, version, description, author, forked_from, forked_at, created_at, updated_at, updated_by)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		sys.Workgroup, sys.Name, sys.Version, sys.Description, sys.Author,
		sys.ForkedFrom, forkedAt, now.Unix(), now.Unix(), sys.UpdatedBy)
	if err != nil {
		return 0, err
	}
	return result.LastInsertId()
}

// ============ Script Operations ============

func (r *ESSRegistry) GetScripts(systemID int64) ([]*ESSScript, error) {
	rows, err := r.db.Query(`SELECT id, system_id, protocol, type, filename, content, checksum, updated_at, updated_by
	                         FROM ess_scripts WHERE system_id = ? ORDER BY protocol, type`, systemID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var scripts []*ESSScript
	for rows.Next() {
		s := &ESSScript{}
		var updatedAt int64
		if err := rows.Scan(&s.ID, &s.SystemID, &s.Protocol, &s.Type, &s.Filename, &s.Content, &s.Checksum, &updatedAt, &s.UpdatedBy); err != nil {
			return nil, err
		}
		s.UpdatedAt = time.Unix(updatedAt, 0)
		scripts = append(scripts, s)
	}
	return scripts, nil
}

func (r *ESSRegistry) GetScript(systemID int64, protocol, scriptType string) (*ESSScript, error) {
	s := &ESSScript{}
	var updatedAt int64
	err := r.db.QueryRow(`SELECT id, system_id, protocol, type, filename, content, checksum, updated_at, updated_by
	                      FROM ess_scripts WHERE system_id = ? AND protocol = ? AND type = ?`,
		systemID, protocol, scriptType).Scan(&s.ID, &s.SystemID, &s.Protocol, &s.Type, &s.Filename, &s.Content, &s.Checksum, &updatedAt, &s.UpdatedBy)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	s.UpdatedAt = time.Unix(updatedAt, 0)
	return s, nil
}

func (r *ESSRegistry) SaveScript(script *ESSScript, expectedChecksum, comment string) error {
	r.mu.Lock()
	defer r.mu.Unlock()

	now := time.Now()
	script.Checksum = computeChecksum(script.Content)
	script.UpdatedAt = now

	existing, err := r.GetScript(script.SystemID, script.Protocol, script.Type)
	if err != nil {
		return err
	}

	tx, err := r.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()

	if existing != nil {
		if expectedChecksum != "" && existing.Checksum != expectedChecksum {
			return fmt.Errorf("conflict: script was modified (expected %s, got %s)", expectedChecksum[:8], existing.Checksum[:8])
		}
		// Save to history
		_, err = tx.Exec(`INSERT INTO ess_script_history (script_id, content, checksum, saved_at, saved_by, comment)
		                  VALUES (?, ?, ?, ?, ?, ?)`,
			existing.ID, existing.Content, existing.Checksum, now.Unix(), script.UpdatedBy, comment)
		if err != nil {
			return err
		}
		// Cleanup old history
		_, err = tx.Exec(`DELETE FROM ess_script_history WHERE script_id = ? AND id NOT IN 
		                  (SELECT id FROM ess_script_history WHERE script_id = ? ORDER BY saved_at DESC LIMIT ?)`,
			existing.ID, existing.ID, MaxHistoryVersions)
		if err != nil {
			return err
		}
		// Update script
		_, err = tx.Exec(`UPDATE ess_scripts SET content = ?, checksum = ?, updated_at = ?, updated_by = ? WHERE id = ?`,
			script.Content, script.Checksum, now.Unix(), script.UpdatedBy, existing.ID)
		if err != nil {
			return err
		}
		script.ID = existing.ID
	} else {
		result, err := tx.Exec(`INSERT INTO ess_scripts (system_id, protocol, type, filename, content, checksum, updated_at, updated_by)
		                        VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
			script.SystemID, script.Protocol, script.Type, script.Filename, script.Content, script.Checksum, now.Unix(), script.UpdatedBy)
		if err != nil {
			return err
		}
		script.ID, _ = result.LastInsertId()
	}

	_, err = tx.Exec(`UPDATE ess_systems SET updated_at = ?, updated_by = ? WHERE id = ?`, now.Unix(), script.UpdatedBy, script.SystemID)
	if err != nil {
		return err
	}
	return tx.Commit()
}

func (r *ESSRegistry) GetScriptHistory(scriptID int64, limit int) ([]*ESSScriptHistory, error) {
	if limit <= 0 {
		limit = MaxHistoryVersions
	}
	rows, err := r.db.Query(`SELECT id, script_id, content, checksum, saved_at, saved_by, comment
	                         FROM ess_script_history WHERE script_id = ? ORDER BY saved_at DESC LIMIT ?`, scriptID, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var history []*ESSScriptHistory
	for rows.Next() {
		h := &ESSScriptHistory{}
		var savedAt int64
		if err := rows.Scan(&h.ID, &h.ScriptID, &h.Content, &h.Checksum, &savedAt, &h.SavedBy, &h.Comment); err != nil {
			return nil, err
		}
		h.SavedAt = time.Unix(savedAt, 0)
		history = append(history, h)
	}
	return history, nil
}

// ============ Library Operations ============

func (r *ESSRegistry) ListLibs(workgroup string) ([]*ESSLib, error) {
	rows, err := r.db.Query(`SELECT id, workgroup, name, version, filename, checksum, forked_from, created_at, updated_at
	                         FROM ess_libs WHERE workgroup = ? ORDER BY name, version`, workgroup)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var libs []*ESSLib
	for rows.Next() {
		lib := &ESSLib{}
		var forkedFrom sql.NullString
		var createdAt, updatedAt int64
		if err := rows.Scan(&lib.ID, &lib.Workgroup, &lib.Name, &lib.Version, &lib.Filename, &lib.Checksum, &forkedFrom, &createdAt, &updatedAt); err != nil {
			return nil, err
		}
		lib.CreatedAt = time.Unix(createdAt, 0)
		lib.UpdatedAt = time.Unix(updatedAt, 0)
		if forkedFrom.Valid {
			lib.ForkedFrom = forkedFrom.String
		}
		libs = append(libs, lib)
	}
	return libs, nil
}

func (r *ESSRegistry) GetLib(workgroup, name, version string) (*ESSLib, error) {
	query := `SELECT id, workgroup, name, version, filename, content, checksum, forked_from, created_at, updated_at
	          FROM ess_libs WHERE workgroup = ? AND name = ?`
	args := []interface{}{workgroup, name}
	if version != "" && version != "latest" {
		query += " AND version = ?"
		args = append(args, version)
	} else {
		query += " ORDER BY created_at DESC LIMIT 1"
	}
	lib := &ESSLib{}
	var forkedFrom sql.NullString
	var createdAt, updatedAt int64
	err := r.db.QueryRow(query, args...).Scan(&lib.ID, &lib.Workgroup, &lib.Name, &lib.Version,
		&lib.Filename, &lib.Content, &lib.Checksum, &forkedFrom, &createdAt, &updatedAt)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	lib.CreatedAt = time.Unix(createdAt, 0)
	lib.UpdatedAt = time.Unix(updatedAt, 0)
	if forkedFrom.Valid {
		lib.ForkedFrom = forkedFrom.String
	}
	return lib, nil
}

func (r *ESSRegistry) SaveLib(lib *ESSLib) (int64, error) {
	now := time.Now()
	lib.Checksum = computeChecksum(lib.Content)
	lib.UpdatedAt = now

	existing, _ := r.GetLib(lib.Workgroup, lib.Name, lib.Version)
	if existing != nil {
		_, err := r.db.Exec(`UPDATE ess_libs SET content = ?, checksum = ?, updated_at = ? WHERE id = ?`,
			lib.Content, lib.Checksum, now.Unix(), existing.ID)
		return existing.ID, err
	}

	lib.CreatedAt = now
	result, err := r.db.Exec(`INSERT INTO ess_libs (workgroup, name, version, filename, content, checksum, forked_from, created_at, updated_at)
	                          VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		lib.Workgroup, lib.Name, lib.Version, lib.Filename, lib.Content, lib.Checksum, lib.ForkedFrom, now.Unix(), now.Unix())
	if err != nil {
		return 0, err
	}
	return result.LastInsertId()
}

// ============ Project Operations ============

func (r *ESSRegistry) ListProjects(workgroup string) ([]*ESSProject, error) {
	rows, err := r.db.Query(`SELECT id, workgroup, name, description, config, created_at, updated_at
	                         FROM ess_projects WHERE workgroup = ? ORDER BY name`, workgroup)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var projects []*ESSProject
	for rows.Next() {
		p := &ESSProject{}
		var configJSON string
		var createdAt, updatedAt int64
		if err := rows.Scan(&p.ID, &p.Workgroup, &p.Name, &p.Description, &configJSON, &createdAt, &updatedAt); err != nil {
			return nil, err
		}
		p.CreatedAt = time.Unix(createdAt, 0)
		p.UpdatedAt = time.Unix(updatedAt, 0)
		json.Unmarshal([]byte(configJSON), &p.Config)
		projects = append(projects, p)
	}
	return projects, nil
}

func (r *ESSRegistry) GetProject(workgroup, name string) (*ESSProject, error) {
	p := &ESSProject{}
	var configJSON string
	var createdAt, updatedAt int64
	err := r.db.QueryRow(`SELECT id, workgroup, name, description, config, created_at, updated_at
	                      FROM ess_projects WHERE workgroup = ? AND name = ?`, workgroup, name).
		Scan(&p.ID, &p.Workgroup, &p.Name, &p.Description, &configJSON, &createdAt, &updatedAt)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	p.CreatedAt = time.Unix(createdAt, 0)
	p.UpdatedAt = time.Unix(updatedAt, 0)
	json.Unmarshal([]byte(configJSON), &p.Config)
	return p, nil
}

func (r *ESSRegistry) SaveProject(project *ESSProject) (int64, error) {
	now := time.Now()
	project.UpdatedAt = now
	configJSON, err := json.Marshal(project.Config)
	if err != nil {
		return 0, err
	}

	existing, _ := r.GetProject(project.Workgroup, project.Name)
	if existing != nil {
		_, err := r.db.Exec(`UPDATE ess_projects SET description = ?, config = ?, updated_at = ? WHERE id = ?`,
			project.Description, string(configJSON), now.Unix(), existing.ID)
		return existing.ID, err
	}

	project.CreatedAt = now
	result, err := r.db.Exec(`INSERT INTO ess_projects (workgroup, name, description, config, created_at, updated_at)
	                          VALUES (?, ?, ?, ?, ?, ?)`,
		project.Workgroup, project.Name, project.Description, string(configJSON), now.Unix(), now.Unix())
	if err != nil {
		return 0, err
	}
	return result.LastInsertId()
}

func (r *ESSRegistry) DeleteProject(workgroup, name string) error {
	_, err := r.db.Exec("DELETE FROM ess_projects WHERE workgroup = ? AND name = ?", workgroup, name)
	return err
}

// ============ Lock Operations ============

func (r *ESSRegistry) AcquireLock(key, lockedBy string) (*ESSLock, error) {
	r.mu.Lock()
	defer r.mu.Unlock()

	now := time.Now()
	var existing ESSLock
	var lockedAt, expiresAt int64
	err := r.db.QueryRow(`SELECT key, locked_by, locked_at, expires_at FROM ess_locks WHERE key = ?`, key).
		Scan(&existing.Key, &existing.LockedBy, &lockedAt, &expiresAt)

	if err == nil {
		existing.LockedAt = time.Unix(lockedAt, 0)
		existing.ExpiresAt = time.Unix(expiresAt, 0)
		if existing.ExpiresAt.After(now) && existing.LockedBy != lockedBy {
			return &existing, fmt.Errorf("locked by %s until %s", existing.LockedBy, existing.ExpiresAt.Format(time.RFC3339))
		}
	}

	lock := &ESSLock{Key: key, LockedBy: lockedBy, LockedAt: now, ExpiresAt: now.Add(LockDuration)}
	_, err = r.db.Exec(`INSERT OR REPLACE INTO ess_locks (key, locked_by, locked_at, expires_at) VALUES (?, ?, ?, ?)`,
		lock.Key, lock.LockedBy, lock.LockedAt.Unix(), lock.ExpiresAt.Unix())
	if err != nil {
		return nil, err
	}
	return lock, nil
}

func (r *ESSRegistry) ReleaseLock(key, lockedBy string) error {
	result, err := r.db.Exec(`DELETE FROM ess_locks WHERE key = ? AND locked_by = ?`, key, lockedBy)
	if err != nil {
		return err
	}
	rows, _ := result.RowsAffected()
	if rows == 0 {
		return fmt.Errorf("lock not found or not owned by %s", lockedBy)
	}
	return nil
}

func (r *ESSRegistry) GetLock(key string) (*ESSLock, error) {
	var lock ESSLock
	var lockedAt, expiresAt int64
	err := r.db.QueryRow(`SELECT key, locked_by, locked_at, expires_at FROM ess_locks WHERE key = ?`, key).
		Scan(&lock.Key, &lock.LockedBy, &lockedAt, &expiresAt)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	lock.LockedAt = time.Unix(lockedAt, 0)
	lock.ExpiresAt = time.Unix(expiresAt, 0)
	if lock.ExpiresAt.Before(time.Now()) {
		return nil, nil
	}
	return &lock, nil
}

func (r *ESSRegistry) ListLocks() ([]*ESSLock, error) {
	now := time.Now().Unix()
	rows, err := r.db.Query(`SELECT key, locked_by, locked_at, expires_at FROM ess_locks WHERE expires_at > ? ORDER BY locked_at DESC`, now)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var locks []*ESSLock
	for rows.Next() {
		lock := &ESSLock{}
		var lockedAt, expiresAt int64
		if err := rows.Scan(&lock.Key, &lock.LockedBy, &lockedAt, &expiresAt); err != nil {
			return nil, err
		}
		lock.LockedAt = time.Unix(lockedAt, 0)
		lock.ExpiresAt = time.Unix(expiresAt, 0)
		locks = append(locks, lock)
	}
	return locks, nil
}

// ============ Template Operations ============

func (r *ESSRegistry) ListTemplates() ([]*ESSSystem, error) {
	return r.ListSystems(TemplatesWorkgroup)
}

func (r *ESSRegistry) AddToWorkgroup(req AddToWorkgroupRequest) (*ESSSystem, error) {
	r.mu.Lock()
	defer r.mu.Unlock()

	template, err := r.GetSystem(TemplatesWorkgroup, req.TemplateSystem, req.TemplateVersion)
	if err != nil {
		return nil, err
	}
	if template == nil {
		return nil, fmt.Errorf("template not found: %s", req.TemplateSystem)
	}

	existing, _ := r.GetSystem(req.TargetWorkgroup, req.TemplateSystem, "")
	if existing != nil {
		return nil, fmt.Errorf("system %s already exists in workgroup %s", req.TemplateSystem, req.TargetWorkgroup)
	}

	tx, err := r.db.Begin()
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()

	now := time.Now()
	forkedFrom := fmt.Sprintf("%s/%s@%s", TemplatesWorkgroup, template.Name, template.Version)

	result, err := tx.Exec(`INSERT INTO ess_systems 
		(workgroup, name, version, description, author, forked_from, forked_at, created_at, updated_at, updated_by)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		req.TargetWorkgroup, template.Name, template.Version, template.Description, template.Author,
		forkedFrom, now.Unix(), now.Unix(), now.Unix(), req.AddedBy)
	if err != nil {
		return nil, err
	}
	newSystemID, _ := result.LastInsertId()

	scripts, err := r.GetScripts(template.ID)
	if err != nil {
		return nil, err
	}
	for _, script := range scripts {
		_, err = tx.Exec(`INSERT INTO ess_scripts (system_id, protocol, type, filename, content, checksum, updated_at, updated_by)
		                  VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
			newSystemID, script.Protocol, script.Type, script.Filename, script.Content, script.Checksum, now.Unix(), req.AddedBy)
		if err != nil {
			return nil, err
		}
	}

	// Copy libs
	templateLibs, _ := r.ListLibs(TemplatesWorkgroup)
	for _, lib := range templateLibs {
		existingLib, _ := r.GetLib(req.TargetWorkgroup, lib.Name, lib.Version)
		if existingLib == nil {
			libForkedFrom := fmt.Sprintf("%s/%s@%s", TemplatesWorkgroup, lib.Name, lib.Version)
			_, err = tx.Exec(`INSERT INTO ess_libs (workgroup, name, version, filename, content, checksum, forked_from, created_at, updated_at)
			                  VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
				req.TargetWorkgroup, lib.Name, lib.Version, lib.Filename, lib.Content, lib.Checksum, libForkedFrom, now.Unix(), now.Unix())
			if err != nil && !strings.Contains(err.Error(), "UNIQUE constraint") {
				return nil, err
			}
		}
	}

	if err := tx.Commit(); err != nil {
		return nil, err
	}
	return r.GetSystem(req.TargetWorkgroup, template.Name, template.Version)
}

func (r *ESSRegistry) SeedFromFilesystem(sourcePath string, systemNames []string) ([]string, error) {
	entries, err := ioutil.ReadDir(sourcePath)
	if err != nil {
		return nil, fmt.Errorf("failed to read source path: %w", err)
	}

	var seeded []string
	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}
		sysName := entry.Name()

		if len(systemNames) > 0 {
			found := false
			for _, n := range systemNames {
				if n == sysName {
					found = true
					break
				}
			}
			if !found {
				continue
			}
		}

		if strings.HasPrefix(sysName, ".") || sysName == "lib" {
			continue
		}

		sysPath := filepath.Join(sourcePath, sysName)
		systemFile := filepath.Join(sysPath, sysName+".tcl")
		if _, err := os.Stat(systemFile); os.IsNotExist(err) {
			continue
		}

		if err := r.importSystem(sysPath, sysName); err != nil {
			log.Printf("ESS Registry: failed to import %s: %v", sysName, err)
			continue
		}
		seeded = append(seeded, sysName)
	}

	libPath := filepath.Join(sourcePath, "lib")
	if _, err := os.Stat(libPath); err == nil {
		r.importLibs(libPath)
	}

	return seeded, nil
}

func (r *ESSRegistry) importSystem(sysPath, sysName string) error {
	now := time.Now()

	existing, _ := r.GetSystem(TemplatesWorkgroup, sysName, "")
	if existing != nil {
		log.Printf("ESS Registry: template %s already exists, skipping", sysName)
		return nil
	}

	tx, err := r.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()

	result, err := tx.Exec(`INSERT INTO ess_systems 
		(workgroup, name, version, description, author, created_at, updated_at, updated_by)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
		TemplatesWorkgroup, sysName, "1.0.0", "", "imported", now.Unix(), now.Unix(), "seed")
	if err != nil {
		return err
	}
	systemID, _ := result.LastInsertId()

	// System-level scripts
	systemScripts := map[string]string{
		ScriptTypeSystem:  sysName + ".tcl",
		ScriptTypeExtract: sysName + "_extract.tcl",
	}
	for scriptType, filename := range systemScripts {
		filePath := filepath.Join(sysPath, filename)
		if content, err := ioutil.ReadFile(filePath); err == nil {
			checksum := computeChecksum(string(content))
			_, err = tx.Exec(`INSERT INTO ess_scripts (system_id, protocol, type, filename, content, checksum, updated_at, updated_by)
			                  VALUES (?, '', ?, ?, ?, ?, ?, ?)`,
				systemID, scriptType, filename, string(content), checksum, now.Unix(), "seed")
			if err != nil {
				return err
			}
		}
	}

	// Protocols
	entries, _ := ioutil.ReadDir(sysPath)
	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}
		protoName := entry.Name()
		protoPath := filepath.Join(sysPath, protoName)
		protoFile := filepath.Join(protoPath, protoName+".tcl")
		if _, err := os.Stat(protoFile); os.IsNotExist(err) {
			continue
		}

		protoScripts := map[string]string{
			ScriptTypeProtocol: protoName + ".tcl",
			ScriptTypeLoaders:  protoName + "_loaders.tcl",
			ScriptTypeVariants: protoName + "_variants.tcl",
			ScriptTypeStim:     protoName + "_stim.tcl",
			ScriptTypeExtract:  protoName + "_extract.tcl",
		}
		for scriptType, filename := range protoScripts {
			filePath := filepath.Join(protoPath, filename)
			if content, err := ioutil.ReadFile(filePath); err == nil {
				checksum := computeChecksum(string(content))
				_, err = tx.Exec(`INSERT INTO ess_scripts (system_id, protocol, type, filename, content, checksum, updated_at, updated_by)
				                  VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
					systemID, protoName, scriptType, filename, string(content), checksum, now.Unix(), "seed")
				if err != nil {
					return err
				}
			}
		}
	}

	return tx.Commit()
}

func (r *ESSRegistry) importLibs(libPath string) error {
	entries, err := ioutil.ReadDir(libPath)
	if err != nil {
		return err
	}

	tmPattern := regexp.MustCompile(`^(.+)-(\d+\.\d+)\.tm$`)
	now := time.Now()

	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		matches := tmPattern.FindStringSubmatch(entry.Name())
		if matches == nil {
			continue
		}
		name := matches[1]
		version := matches[2]
		filename := entry.Name()

		existing, _ := r.GetLib(TemplatesWorkgroup, name, version)
		if existing != nil {
			continue
		}

		content, err := ioutil.ReadFile(filepath.Join(libPath, filename))
		if err != nil {
			continue
		}
		checksum := computeChecksum(string(content))

		_, err = r.db.Exec(`INSERT INTO ess_libs (workgroup, name, version, filename, content, checksum, created_at, updated_at)
		                    VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
			TemplatesWorkgroup, name, version, filename, string(content), checksum, now.Unix(), now.Unix())
		if err != nil {
			log.Printf("ESS Registry: failed to import lib %s: %v", filename, err)
		}
	}
	return nil
}

// ============ Backup Operations ============

// BackupInfo represents a backup file
type BackupInfo struct {
	Filename  string    `json:"filename"`
	Path      string    `json:"path"`
	Size      int64     `json:"size"`
	CreatedAt time.Time `json:"createdAt"`
}

// Backup creates a backup of the database
func (r *ESSRegistry) Backup(destDir string) (*BackupInfo, error) {
	if err := os.MkdirAll(destDir, 0755); err != nil {
		return nil, fmt.Errorf("failed to create backup directory: %w", err)
	}

	timestamp := time.Now().Format("20060102-150405")
	filename := fmt.Sprintf("ess-registry-%s.db", timestamp)
	destPath := filepath.Join(destDir, filename)

	// Use VACUUM INTO for a consistent backup (SQLite 3.27+)
	_, err := r.db.Exec(fmt.Sprintf(`VACUUM INTO '%s'`, destPath))
	if err != nil {
		return nil, fmt.Errorf("backup failed: %w", err)
	}

	// Get file info
	info, err := os.Stat(destPath)
	if err != nil {
		return nil, err
	}

	return &BackupInfo{
		Filename:  filename,
		Path:      destPath,
		Size:      info.Size(),
		CreatedAt: time.Now(),
	}, nil
}

// ListBackups returns available backups in the directory
func (r *ESSRegistry) ListBackups(backupDir string) ([]*BackupInfo, error) {
	entries, err := ioutil.ReadDir(backupDir)
	if err != nil {
		if os.IsNotExist(err) {
			return []*BackupInfo{}, nil
		}
		return nil, err
	}

	var backups []*BackupInfo
	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		if !strings.HasPrefix(entry.Name(), "ess-registry-") || !strings.HasSuffix(entry.Name(), ".db") {
			continue
		}
		backups = append(backups, &BackupInfo{
			Filename:  entry.Name(),
			Path:      filepath.Join(backupDir, entry.Name()),
			Size:      entry.Size(),
			CreatedAt: entry.ModTime(),
		})
	}

	// Sort by date descending (newest first)
	sort.Slice(backups, func(i, j int) bool {
		return backups[i].CreatedAt.After(backups[j].CreatedAt)
	})

	return backups, nil
}

// CleanupOldBackups removes backups older than maxAge, keeping at least minKeep
func (r *ESSRegistry) CleanupOldBackups(backupDir string, maxAge time.Duration, minKeep int) (int, error) {
	backups, err := r.ListBackups(backupDir)
	if err != nil {
		return 0, err
	}

	if len(backups) <= minKeep {
		return 0, nil
	}

	cutoff := time.Now().Add(-maxAge)
	removed := 0

	for i, backup := range backups {
		// Always keep minKeep newest backups
		if i < minKeep {
			continue
		}
		// Remove if older than cutoff
		if backup.CreatedAt.Before(cutoff) {
			if err := os.Remove(backup.Path); err == nil {
				removed++
			}
		}
	}

	return removed, nil
}

// StartBackupScheduler runs automatic backups
func (r *ESSRegistry) StartBackupScheduler(backupDir string, interval time.Duration, maxAge time.Duration, minKeep int) {
	// Immediate backup on start
	if backup, err := r.Backup(backupDir); err != nil {
		log.Printf("ESS Registry: startup backup failed: %v", err)
	} else {
		log.Printf("ESS Registry: startup backup created: %s", backup.Filename)
	}

	go func() {
		ticker := time.NewTicker(interval)
		defer ticker.Stop()

		for range ticker.C {
			if backup, err := r.Backup(backupDir); err != nil {
				log.Printf("ESS Registry: scheduled backup failed: %v", err)
			} else {
				log.Printf("ESS Registry: backup created: %s", backup.Filename)
			}

			if removed, err := r.CleanupOldBackups(backupDir, maxAge, minKeep); err != nil {
				log.Printf("ESS Registry: backup cleanup failed: %v", err)
			} else if removed > 0 {
				log.Printf("ESS Registry: removed %d old backups", removed)
			}
		}
	}()
}

// ============ User Operations ============

func (r *ESSRegistry) ListUsers(workgroup string) ([]*ESSUser, error) {
	rows, err := r.db.Query(`SELECT id, workgroup, username, full_name, email, role, created_at, updated_at
	                         FROM ess_users WHERE workgroup = ? ORDER BY username`, workgroup)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var users []*ESSUser
	for rows.Next() {
		u := &ESSUser{}
		var createdAt, updatedAt int64
		if err := rows.Scan(&u.ID, &u.Workgroup, &u.Username, &u.FullName, &u.Email, &u.Role, &createdAt, &updatedAt); err != nil {
			return nil, err
		}
		u.CreatedAt = time.Unix(createdAt, 0)
		u.UpdatedAt = time.Unix(updatedAt, 0)
		users = append(users, u)
	}
	return users, nil
}

func (r *ESSRegistry) GetUser(workgroup, username string) (*ESSUser, error) {
	u := &ESSUser{}
	var createdAt, updatedAt int64
	err := r.db.QueryRow(`SELECT id, workgroup, username, full_name, email, role, created_at, updated_at
	                      FROM ess_users WHERE workgroup = ? AND username = ?`, workgroup, username).
		Scan(&u.ID, &u.Workgroup, &u.Username, &u.FullName, &u.Email, &u.Role, &createdAt, &updatedAt)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	u.CreatedAt = time.Unix(createdAt, 0)
	u.UpdatedAt = time.Unix(updatedAt, 0)
	return u, nil
}

func (r *ESSRegistry) SaveUser(user *ESSUser) (int64, error) {
	now := time.Now()
	user.UpdatedAt = now

	if user.Role == "" {
		user.Role = "editor"
	}

	existing, _ := r.GetUser(user.Workgroup, user.Username)
	if existing != nil {
		_, err := r.db.Exec(`UPDATE ess_users SET full_name = ?, email = ?, role = ?, updated_at = ? WHERE id = ?`,
			user.FullName, user.Email, user.Role, now.Unix(), existing.ID)
		return existing.ID, err
	}

	user.CreatedAt = now
	result, err := r.db.Exec(`INSERT INTO ess_users (workgroup, username, full_name, email, role, created_at, updated_at)
	                          VALUES (?, ?, ?, ?, ?, ?, ?)`,
		user.Workgroup, user.Username, user.FullName, user.Email, user.Role, now.Unix(), now.Unix())
	if err != nil {
		return 0, err
	}
	return result.LastInsertId()
}

func (r *ESSRegistry) DeleteUser(workgroup, username string) error {
	result, err := r.db.Exec("DELETE FROM ess_users WHERE workgroup = ? AND username = ?", workgroup, username)
	if err != nil {
		return err
	}
	rows, _ := result.RowsAffected()
	if rows == 0 {
		return fmt.Errorf("user not found: %s", username)
	}
	return nil
}

// ============ Utilities ============

func computeChecksum(content string) string {
	h := sha256.Sum256([]byte(content))
	return hex.EncodeToString(h[:])
}
