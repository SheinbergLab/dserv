// ess_configs_models.go - Data models for ESS Projects, Configs, and Queues
//
// These models support pushing/pulling experiment configurations between
// dserv instances and the central registry. Projects contain configs and queues,
// enabling complete experiment setups to be shared across rigs.

package main

import (
	"time"
)

// ============ Project Models ============

// ESSProjectDef represents a project definition (container for configs and queues)
// Note: This is distinct from the existing ESSProject which defines system/protocol availability
type ESSProjectDef struct {
	ID          int64     `json:"id"`
	Workgroup   string    `json:"workgroup"`
	Name        string    `json:"name"`
	Description string    `json:"description,omitempty"`
	Systems     []string  `json:"systems,omitempty"`    // Which ESS systems this project uses
	RegistryURL string    `json:"registryUrl,omitempty"` // Source registry URL if synced
	LastSyncAt  *time.Time `json:"lastSyncAt,omitempty"`
	CreatedAt   time.Time `json:"createdAt"`
	UpdatedAt   time.Time `json:"updatedAt"`

	// Populated on fetch
	ConfigCount int `json:"configCount,omitempty"`
	QueueCount  int `json:"queueCount,omitempty"`
}

// ============ Config Models ============

// ESSConfig represents a saved experiment configuration snapshot
type ESSConfig struct {
	ID          int64  `json:"id"`
	ProjectID   int64  `json:"projectId"`
	ProjectName string `json:"projectName,omitempty"` // Populated on fetch

	Name        string `json:"name"`
	Description string `json:"description,omitempty"`

	// ESS setup fields
	ScriptSource string            `json:"scriptSource,omitempty"` // Registry source path
	System       string            `json:"system"`
	Protocol     string            `json:"protocol"`
	Variant      string            `json:"variant"`
	Subject      string            `json:"subject,omitempty"`
	VariantArgs  map[string]interface{} `json:"variantArgs,omitempty"`
	Params       map[string]interface{} `json:"params,omitempty"`

	// File naming
	FileTemplate string `json:"fileTemplate,omitempty"`

	// Metadata
	Tags       []string   `json:"tags,omitempty"`
	CreatedBy  string     `json:"createdBy,omitempty"`
	CreatedAt  time.Time  `json:"createdAt"`
	UpdatedAt  time.Time  `json:"updatedAt"`
	LastUsedAt *time.Time `json:"lastUsedAt,omitempty"`
	UseCount   int        `json:"useCount"`
	Archived   bool       `json:"archived"`
}

// ============ Queue Models ============

// ESSQueue represents an ordered sequence of configs for batch execution
type ESSQueue struct {
	ID          int64  `json:"id"`
	ProjectID   int64  `json:"projectId"`
	ProjectName string `json:"projectName,omitempty"` // Populated on fetch

	Name        string `json:"name"`
	Description string `json:"description,omitempty"`

	// Automation flags
	AutoStart    bool `json:"autoStart"`
	AutoAdvance  bool `json:"autoAdvance"`
	AutoDatafile bool `json:"autoDatafile"`

	CreatedAt time.Time `json:"createdAt"`
	CreatedBy string    `json:"createdBy,omitempty"`
	UpdatedAt time.Time `json:"updatedAt"`

	// Populated on fetch
	Items     []ESSQueueItem `json:"items,omitempty"`
	ItemCount int            `json:"itemCount,omitempty"`
}

// ESSQueueItem represents a config reference within a queue
type ESSQueueItem struct {
	ID         int64  `json:"id,omitempty"`
	QueueID    int64  `json:"queueId,omitempty"`
	ConfigID   int64  `json:"configId"`
	ConfigName string `json:"configName,omitempty"` // Populated on fetch

	Position    int    `json:"position"`
	RepeatCount int    `json:"repeatCount"`
	PauseAfter  int    `json:"pauseAfter"` // Seconds to pause after this item
	Notes       string `json:"notes,omitempty"`
}

// ============ Bundle Models (for sync operations) ============

// ESSProjectBundle contains a complete project with all its configs and queues
// Used for push/pull operations between dserv and registry
type ESSProjectBundle struct {
	Project ESSProjectDef `json:"project"`
	Configs []ESSConfig   `json:"configs"`
	Queues  []ESSQueue    `json:"queues"`
	
	// Metadata about the bundle
	ExportedAt time.Time `json:"exportedAt"`
	ExportedBy string    `json:"exportedBy,omitempty"`
	SourceRig  string    `json:"sourceRig,omitempty"`
}

// ============ Request/Response Types ============

// ProjectDefRequest is used to create or update a project definition
type ProjectDefRequest struct {
	Name        string   `json:"name"`
	Description string   `json:"description,omitempty"`
	Systems     []string `json:"systems,omitempty"`
	RegistryURL string   `json:"registryUrl,omitempty"`
}

// ConfigRequest is used to create or update a config
type ConfigRequest struct {
	Name         string                 `json:"name"`
	Description  string                 `json:"description,omitempty"`
	ScriptSource string                 `json:"scriptSource,omitempty"`
	System       string                 `json:"system"`
	Protocol     string                 `json:"protocol"`
	Variant      string                 `json:"variant"`
	Subject      string                 `json:"subject,omitempty"`
	VariantArgs  map[string]interface{} `json:"variantArgs,omitempty"`
	Params       map[string]interface{} `json:"params,omitempty"`
	FileTemplate string                 `json:"fileTemplate,omitempty"`
	Tags         []string               `json:"tags,omitempty"`
	CreatedBy    string                 `json:"createdBy,omitempty"`
}

// QueueRequest is used to create or update a queue
type QueueRequest struct {
	Name         string `json:"name"`
	Description  string `json:"description,omitempty"`
	AutoStart    *bool  `json:"autoStart,omitempty"`    // Pointer to distinguish false from unset
	AutoAdvance  *bool  `json:"autoAdvance,omitempty"`
	AutoDatafile *bool  `json:"autoDatafile,omitempty"`
	CreatedBy    string `json:"createdBy,omitempty"`
}

// QueueItemRequest is used to add or update a queue item
type QueueItemRequest struct {
	ConfigID    int64  `json:"configId,omitempty"`
	ConfigName  string `json:"configName,omitempty"` // Alternative to configId
	Position    *int   `json:"position,omitempty"`   // If nil, append to end
	RepeatCount int    `json:"repeatCount,omitempty"`
	PauseAfter  int    `json:"pauseAfter,omitempty"`
	Notes       string `json:"notes,omitempty"`
}

// PushProjectRequest is used to push a project to the registry
type PushProjectRequest struct {
	ProjectName string `json:"projectName"`
	PushedBy    string `json:"pushedBy,omitempty"`
	SourceRig   string `json:"sourceRig,omitempty"`
}

// PullProjectRequest is used to pull a project from the registry
type PullProjectRequest struct {
	Workgroup   string `json:"workgroup"`
	ProjectName string `json:"projectName"`
	Version     string `json:"version,omitempty"` // "latest" or specific version
}

// SyncResult contains the result of a push or pull operation
type SyncResult struct {
	Success      bool     `json:"success"`
	ProjectName  string   `json:"projectName"`
	ConfigsCount int      `json:"configsCount"`
	QueuesCount  int      `json:"queuesCount"`
	Created      []string `json:"created,omitempty"`
	Updated      []string `json:"updated,omitempty"`
	Deleted      []string `json:"deleted,omitempty"`
	Skipped      []string `json:"skipped,omitempty"`
	Errors       []string `json:"errors,omitempty"`
}
