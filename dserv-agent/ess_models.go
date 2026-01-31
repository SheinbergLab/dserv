// ess_models.go - Data models for ESS Script Registry

package main

import (
	"time"
)

// ============ Constants ============

const (
	TemplatesWorkgroup = "_templates" // Special workgroup for global templates
	LockDuration       = 30 * time.Minute
	MaxHistoryVersions = 20
)

// Script types
const (
	ScriptTypeSystem   = "system"
	ScriptTypeProtocol = "protocol"
	ScriptTypeLoaders  = "loaders"
	ScriptTypeVariants = "variants"
	ScriptTypeStim     = "stim"
	ScriptTypeExtract  = "extract"
)

// ============ Data Models ============

// ESSSystem represents a complete experiment system
type ESSSystem struct {
	ID          int64      `json:"id"`
	Workgroup   string     `json:"workgroup"`
	Name        string     `json:"name"`
	Version     string     `json:"version"`
	Description string     `json:"description,omitempty"`
	Author      string     `json:"author,omitempty"`
	ForkedFrom  string     `json:"forkedFrom,omitempty"`
	ForkedAt    *time.Time `json:"forkedAt,omitempty"`
	CreatedAt   time.Time  `json:"createdAt"`
	UpdatedAt   time.Time  `json:"updatedAt"`
	UpdatedBy   string     `json:"updatedBy"`

	// Populated on fetch, not stored directly
	Protocols []string `json:"protocols,omitempty"`
	Scripts   int      `json:"scriptCount,omitempty"`
}

// ESSScript represents a single script file
type ESSScript struct {
	ID        int64     `json:"id"`
	SystemID  int64     `json:"systemId"`
	Protocol  string    `json:"protocol,omitempty"` // Empty for system-level scripts
	Type      string    `json:"type"`               // system, protocol, loaders, variants, stim, extract
	Filename  string    `json:"filename"`
	Content   string    `json:"content"`
	Checksum  string    `json:"checksum"`
	UpdatedAt time.Time `json:"updatedAt"`
	UpdatedBy string    `json:"updatedBy"`
}

// ESSLib represents a shared Tcl module
type ESSLib struct {
	ID         int64     `json:"id"`
	Workgroup  string    `json:"workgroup"`
	Name       string    `json:"name"`    // e.g., "planko"
	Version    string    `json:"version"` // e.g., "3.2"
	Filename   string    `json:"filename"`
	Content    string    `json:"content"`
	Checksum   string    `json:"checksum"`
	ForkedFrom string    `json:"forkedFrom,omitempty"`
	CreatedAt  time.Time `json:"createdAt"`
	UpdatedAt  time.Time `json:"updatedAt"`
}

// ESSProject defines what systems/protocols are available in a scope
type ESSProject struct {
	ID          int64            `json:"id"`
	Workgroup   string           `json:"workgroup"`
	Name        string           `json:"name"`
	Description string           `json:"description,omitempty"`
	Config      ESSProjectConfig `json:"config"`
	CreatedAt   time.Time        `json:"createdAt"`
	UpdatedAt   time.Time        `json:"updatedAt"`
}

// ESSProjectConfig holds the project configuration
type ESSProjectConfig struct {
	Systems []ESSProjectSystem `json:"systems"`
	Access  []string           `json:"access,omitempty"` // Glob patterns for allowed rigs
}

// ESSProjectSystem defines a system reference in a project
type ESSProjectSystem struct {
	Name      string   `json:"name"`
	Version   string   `json:"version,omitempty"`   // Empty = latest, or pinned version
	Protocols []string `json:"protocols,omitempty"` // Empty = all protocols
}

// ESSLock represents an advisory edit lock
type ESSLock struct {
	Key       string    `json:"key"` // "workgroup/system" or "workgroup/system/protocol"
	LockedBy  string    `json:"lockedBy"`
	LockedAt  time.Time `json:"lockedAt"`
	ExpiresAt time.Time `json:"expiresAt"`
}

// ESSScriptHistory represents a historical version of a script
type ESSScriptHistory struct {
	ID       int64     `json:"id"`
	ScriptID int64     `json:"scriptId"`
	Content  string    `json:"content"`
	Checksum string    `json:"checksum"`
	SavedAt  time.Time `json:"savedAt"`
	SavedBy  string    `json:"savedBy"`
	Comment  string    `json:"comment,omitempty"`
}

// ============ Request/Response Types ============

// AddToWorkgroupRequest is the request to copy a template to a workgroup
type AddToWorkgroupRequest struct {
	TemplateSystem  string `json:"templateSystem"`
	TemplateVersion string `json:"templateVersion"` // "latest" or specific version
	TargetWorkgroup string `json:"targetWorkgroup"`
	AddedBy         string `json:"addedBy"`
}

// SeedTemplatesRequest is the request to seed templates from filesystem
type SeedTemplatesRequest struct {
	SourcePath string   `json:"sourcePath"`
	Systems    []string `json:"systems"` // Empty = all found systems
}

// SaveScriptRequest is the request to update a script
type SaveScriptRequest struct {
	Content          string `json:"content"`
	ExpectedChecksum string `json:"expectedChecksum,omitempty"` // For conflict detection
	UpdatedBy        string `json:"updatedBy"`
	Comment          string `json:"comment,omitempty"`
}

// LockRequest is the request to acquire or release a lock
type LockRequest struct {
	Key      string `json:"key"` // "workgroup/system" or "workgroup/system/protocol"
	LockedBy string `json:"lockedBy"`
}

// ESSUser represents a user in a workgroup
type ESSUser struct {
	ID        int64     `json:"id"`
	Workgroup string    `json:"workgroup"`
	Username  string    `json:"username"`
	FullName  string    `json:"fullName,omitempty"`
	Email     string    `json:"email,omitempty"`
	Role      string    `json:"role"` // admin, editor, viewer
	CreatedAt time.Time `json:"createdAt"`
	UpdatedAt time.Time `json:"updatedAt"`
}

// UserRequest is the request to add or update a user
type UserRequest struct {
	Username string `json:"username"`
	FullName string `json:"fullName,omitempty"`
	Email    string `json:"email,omitempty"`
	Role     string `json:"role,omitempty"` // defaults to "editor"
}
