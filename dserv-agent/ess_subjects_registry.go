// ess_subjects_registry.go - Database operations for workgroup-scoped subjects.
//
// Subjects are managed independently of the configs that reference them
// (ess_configs.subject). The ess side reads the active names into the
// ess/subject_ids datapoint at startup; see ess-2.0.tm.

package main

import (
	"database/sql"
	"fmt"
	"strings"
	"time"
)

// ListSubjects returns a workgroup's subjects ordered by name. When
// includeInactive is false, only active subjects are returned (the picker list).
func (r *ESSRegistry) ListSubjects(workgroup string, includeInactive bool) ([]*ESSSubject, error) {
	q := `SELECT id, workgroup, name, display_name, species, active, description,
	             registry_url, last_sync_at, created_at, updated_at
	      FROM ess_subjects WHERE workgroup = ?`
	if !includeInactive {
		q += " AND active = 1"
	}
	q += " ORDER BY name"

	rows, err := r.db.Query(q, workgroup)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var subjects []*ESSSubject
	for rows.Next() {
		s, err := scanSubject(rows)
		if err != nil {
			return nil, err
		}
		subjects = append(subjects, s)
	}
	return subjects, rows.Err()
}

// GetSubject returns a single subject, or (nil, nil) if it does not exist.
func (r *ESSRegistry) GetSubject(workgroup, name string) (*ESSSubject, error) {
	row := r.db.QueryRow(`
		SELECT id, workgroup, name, display_name, species, active, description,
		       registry_url, last_sync_at, created_at, updated_at
		FROM ess_subjects WHERE workgroup = ? AND name = ?
	`, workgroup, strings.ToLower(name))
	s, err := scanSubject(row)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	return s, err
}

// CreateSubject inserts a subject. Name is lowercased to match set_subject on the
// ess side. Returns a UNIQUE error if (workgroup, name) already exists.
func (r *ESSRegistry) CreateSubject(s *ESSSubject) (int64, error) {
	now := time.Now()
	s.CreatedAt = now
	s.UpdatedAt = now
	s.Name = strings.ToLower(s.Name)

	active := 0
	if s.Active {
		active = 1
	}
	var lastSync interface{}
	if s.LastSyncAt != nil {
		lastSync = s.LastSyncAt.Unix()
	}

	result, err := r.db.Exec(`
		INSERT INTO ess_subjects
		    (workgroup, name, display_name, species, active, description,
		     registry_url, last_sync_at, created_at, updated_at)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
	`, s.Workgroup, s.Name, s.DisplayName, s.Species, active, s.Description,
		s.RegistryURL, lastSync, now.Unix(), now.Unix())
	if err != nil {
		return 0, err
	}
	return result.LastInsertId()
}

// UpdateSubject updates the mutable fields of an existing subject (by ID).
func (r *ESSRegistry) UpdateSubject(s *ESSSubject) error {
	now := time.Now()
	s.UpdatedAt = now

	active := 0
	if s.Active {
		active = 1
	}
	var lastSync interface{}
	if s.LastSyncAt != nil {
		lastSync = s.LastSyncAt.Unix()
	}

	_, err := r.db.Exec(`
		UPDATE ess_subjects
		SET display_name = ?, species = ?, active = ?, description = ?,
		    registry_url = ?, last_sync_at = ?, updated_at = ?
		WHERE id = ?
	`, s.DisplayName, s.Species, active, s.Description,
		s.RegistryURL, lastSync, now.Unix(), s.ID)
	return err
}

// DeleteSubject removes a subject. Prefer deactivating (active=0) to preserve
// datafile history; hard delete is provided for cleanup.
func (r *ESSRegistry) DeleteSubject(workgroup, name string) error {
	result, err := r.db.Exec(
		"DELETE FROM ess_subjects WHERE workgroup = ? AND name = ?",
		workgroup, strings.ToLower(name))
	if err != nil {
		return err
	}
	rows, _ := result.RowsAffected()
	if rows == 0 {
		return fmt.Errorf("subject not found: %s/%s", workgroup, name)
	}
	return nil
}

// SeedSubjectsFromConfigs registers a subject for each distinct non-empty
// subject referenced by the workgroup's (non-archived) configs that is not
// already registered. Idempotent; returns the names it actually added.
func (r *ESSRegistry) SeedSubjectsFromConfigs(workgroup string) ([]string, error) {
	rows, err := r.db.Query(`
		SELECT DISTINCT c.subject
		FROM ess_configs c
		JOIN ess_project_defs p ON c.project_id = p.id
		WHERE p.workgroup = ? AND c.subject != '' AND c.archived = 0
		ORDER BY c.subject
	`, workgroup)
	if err != nil {
		return nil, err
	}
	var candidates []string
	for rows.Next() {
		var s string
		if err := rows.Scan(&s); err != nil {
			rows.Close()
			return nil, err
		}
		candidates = append(candidates, s)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}

	var added []string
	for _, name := range candidates {
		// GetSubject/CreateSubject lowercase, so mixed-case duplicates
		// (e.g. "Riker" and "riker") collapse to one.
		existing, err := r.GetSubject(workgroup, name)
		if err != nil {
			return added, err
		}
		if existing != nil {
			continue
		}
		if _, err := r.CreateSubject(&ESSSubject{Workgroup: workgroup, Name: name, Active: true}); err != nil {
			continue // skip on a race/unique collision, keep going
		}
		added = append(added, strings.ToLower(name))
	}
	return added, nil
}

// scanSubject reads one row into an ESSSubject. Works with *sql.Row and *sql.Rows.
func scanSubject(row interface{ Scan(...interface{}) error }) (*ESSSubject, error) {
	s := &ESSSubject{}
	var active int
	var lastSyncAt sql.NullInt64
	var createdAt, updatedAt int64

	err := row.Scan(&s.ID, &s.Workgroup, &s.Name, &s.DisplayName, &s.Species,
		&active, &s.Description, &s.RegistryURL, &lastSyncAt, &createdAt, &updatedAt)
	if err != nil {
		return nil, err
	}

	s.Active = active != 0
	s.CreatedAt = time.Unix(createdAt, 0)
	s.UpdatedAt = time.Unix(updatedAt, 0)
	if lastSyncAt.Valid {
		t := time.Unix(lastSyncAt.Int64, 0)
		s.LastSyncAt = &t
	}
	return s, nil
}
