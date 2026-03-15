package main

import (
	"encoding/json"
	"fmt"
	"net/url"
)

// Registry API helper methods on AgentClient.
// All registry endpoints live under /api/v1/ess/.

const registryBase = "/api/v1/ess"

// ListSystems returns systems for a workgroup.
func (c *AgentClient) ListSystems(workgroup string) ([]map[string]interface{}, error) {
	result, err := c.Get(registryBase + "/systems?workgroup=" + url.QueryEscape(workgroup))
	if err != nil {
		return nil, err
	}
	return extractList(result, "systems"), nil
}

// GetSystem returns a system with its scripts.
func (c *AgentClient) GetSystem(workgroup, system, version string) (map[string]interface{}, error) {
	path := fmt.Sprintf("%s/system/%s/%s", registryBase, url.PathEscape(workgroup), url.PathEscape(system))
	if version != "" {
		path += "/" + url.PathEscape(version)
	}
	return c.Get(path)
}

// GetScripts returns scripts grouped by protocol.
func (c *AgentClient) GetScripts(workgroup, system, version string) (map[string]interface{}, error) {
	path := fmt.Sprintf("%s/scripts/%s/%s", registryBase, url.PathEscape(workgroup), url.PathEscape(system))
	if version != "" {
		path += "?version=" + url.QueryEscape(version)
	}
	return c.Get(path)
}

// GetScript retrieves a single script.
func (c *AgentClient) GetScript(workgroup, system, protocol, scriptType, version string) (map[string]interface{}, error) {
	if protocol == "" || protocol == "_system" || protocol == "_" {
		protocol = "_"
	}
	path := fmt.Sprintf("%s/script/%s/%s/%s/%s",
		registryBase,
		url.PathEscape(workgroup),
		url.PathEscape(system),
		url.PathEscape(protocol),
		url.PathEscape(scriptType))
	if version != "" {
		path += "?version=" + url.QueryEscape(version)
	}
	return c.Get(path)
}

// SaveScript commits a script to the registry.
func (c *AgentClient) SaveScript(workgroup, system, protocol, scriptType, version string, req map[string]interface{}) (map[string]interface{}, error) {
	if protocol == "" || protocol == "_system" || protocol == "_" {
		protocol = "_"
	}
	path := fmt.Sprintf("%s/script/%s/%s/%s/%s",
		registryBase,
		url.PathEscape(workgroup),
		url.PathEscape(system),
		url.PathEscape(protocol),
		url.PathEscape(scriptType))
	if version != "" {
		path += "?version=" + url.QueryEscape(version)
	}
	return c.Do("PUT", path, req)
}

// DeleteScript deletes a script.
func (c *AgentClient) DeleteScript(workgroup, system, protocol, scriptType string) (map[string]interface{}, error) {
	if protocol == "" || protocol == "_system" || protocol == "_" {
		protocol = "_"
	}
	path := fmt.Sprintf("%s/script/%s/%s/%s/%s",
		registryBase,
		url.PathEscape(workgroup),
		url.PathEscape(system),
		url.PathEscape(protocol),
		url.PathEscape(scriptType))
	return c.Do("DELETE", path, nil)
}

// GetHistory returns version history for a script.
func (c *AgentClient) GetHistory(workgroup, system, protocol, scriptType string) (map[string]interface{}, error) {
	if protocol == "" || protocol == "_system" || protocol == "_" {
		protocol = "_"
	}
	path := fmt.Sprintf("%s/history/%s/%s/%s/%s",
		registryBase,
		url.PathEscape(workgroup),
		url.PathEscape(system),
		url.PathEscape(protocol),
		url.PathEscape(scriptType))
	return c.Get(path)
}

// ListTemplates returns the template zoo.
func (c *AgentClient) ListTemplates() ([]map[string]interface{}, error) {
	result, err := c.Get(registryBase + "/templates")
	if err != nil {
		return nil, err
	}
	return extractList(result, "systems"), nil
}

// AddToWorkgroup forks a template into a workgroup.
func (c *AgentClient) AddToWorkgroup(templateSystem, targetWorkgroup, addedBy string) (map[string]interface{}, error) {
	return c.Post(registryBase+"/add-to-workgroup", map[string]string{
		"templateSystem":  templateSystem,
		"templateVersion": "latest",
		"targetWorkgroup": targetWorkgroup,
		"addedBy":         addedBy,
	})
}

// GetManifest returns checksums for a system (no content).
func (c *AgentClient) GetManifest(workgroup, system, version string) (map[string]interface{}, error) {
	path := fmt.Sprintf("%s/manifest/%s/%s", registryBase, url.PathEscape(workgroup), url.PathEscape(system))
	if version != "" {
		path += "?version=" + url.QueryEscape(version)
	}
	return c.Get(path)
}

// SyncCheck posts local checksums and gets stale/extra scripts.
func (c *AgentClient) SyncCheck(workgroup, system string, checksums map[string]string, version string) (map[string]interface{}, error) {
	path := fmt.Sprintf("%s/sync/%s/%s", registryBase, url.PathEscape(workgroup), url.PathEscape(system))
	return c.Post(path, map[string]interface{}{
		"checksums": checksums,
		"version":   version,
	})
}

// CreateSandbox creates a new sandbox version.
func (c *AgentClient) CreateSandbox(workgroup, system, fromVersion, toVersion, username, comment string) (map[string]interface{}, error) {
	path := fmt.Sprintf("%s/sandbox/%s/%s/create", registryBase, url.PathEscape(workgroup), url.PathEscape(system))
	return c.Post(path, map[string]string{
		"fromVersion": fromVersion,
		"toVersion":   toVersion,
		"username":    username,
		"comment":     comment,
	})
}

// PromoteSandbox promotes a sandbox to a target version.
func (c *AgentClient) PromoteSandbox(workgroup, system, fromVersion, toVersion, username, comment string) (map[string]interface{}, error) {
	path := fmt.Sprintf("%s/sandbox/%s/%s/promote", registryBase, url.PathEscape(workgroup), url.PathEscape(system))
	return c.Post(path, map[string]string{
		"fromVersion": fromVersion,
		"toVersion":   toVersion,
		"username":    username,
		"comment":     comment,
	})
}

// SyncSandbox pulls changes from a source version into a sandbox.
func (c *AgentClient) SyncSandbox(workgroup, system, fromVersion, toVersion, username string) (map[string]interface{}, error) {
	path := fmt.Sprintf("%s/sandbox/%s/%s/sync", registryBase, url.PathEscape(workgroup), url.PathEscape(system))
	return c.Post(path, map[string]string{
		"fromVersion": fromVersion,
		"toVersion":   toVersion,
		"username":    username,
		"conflicts":   "overwrite",
	})
}

// DeleteSandbox removes a sandbox version.
func (c *AgentClient) DeleteSandbox(workgroup, system, version string) (map[string]interface{}, error) {
	path := fmt.Sprintf("%s/sandbox/%s/%s/%s",
		registryBase,
		url.PathEscape(workgroup),
		url.PathEscape(system),
		url.PathEscape(version))
	return c.Do("DELETE", path, nil)
}

// ListVersions returns all versions of a system.
func (c *AgentClient) ListVersions(workgroup, system string) (map[string]interface{}, error) {
	path := fmt.Sprintf("%s/sandbox/%s/%s/versions", registryBase, url.PathEscape(workgroup), url.PathEscape(system))
	return c.Get(path)
}

// SeedTemplates imports systems from a filesystem path into the _templates workgroup.
func (c *AgentClient) SeedTemplates(sourcePath string, systems []string) (map[string]interface{}, error) {
	return c.Post(registryBase+"/admin/seed-templates", map[string]interface{}{
		"sourcePath": sourcePath,
		"systems":    systems,
	})
}

// DeleteSystem removes a system and all its scripts.
func (c *AgentClient) DeleteSystem(workgroup, system string) (map[string]interface{}, error) {
	return c.Do("DELETE", registryBase+"/scaffold/system", map[string]string{
		"workgroup": workgroup,
		"system":    system,
	})
}

// ExportZip downloads a workgroup or system as ZIP.
func (c *AgentClient) ExportZip(workgroup, system string) ([]byte, error) {
	path := fmt.Sprintf("%s/export/%s", registryBase, url.PathEscape(workgroup))
	if system != "" {
		path += "/" + url.PathEscape(system)
	}
	path += "?format=zip"
	data, _, err := c.GetRaw(path)
	return data, err
}

// --- lib API ---

// ListLibs returns shared libraries for a workgroup with checksums.
func (c *AgentClient) ListLibs(workgroup string) ([]map[string]interface{}, error) {
	result, err := c.Get(registryBase + "/libs?workgroup=" + url.QueryEscape(workgroup))
	if err != nil {
		return nil, err
	}
	return extractList(result, "libs"), nil
}

// GetLib retrieves a single library file.
func (c *AgentClient) GetLib(workgroup, name, version string) (map[string]interface{}, error) {
	path := fmt.Sprintf("%s/lib/%s/%s/%s",
		registryBase,
		url.PathEscape(workgroup),
		url.PathEscape(name),
		url.PathEscape(version))
	return c.Get(path)
}

// SaveLib commits a library file to the registry.
func (c *AgentClient) SaveLib(workgroup, name, version string, req map[string]interface{}) (map[string]interface{}, error) {
	path := fmt.Sprintf("%s/lib/%s/%s/%s",
		registryBase,
		url.PathEscape(workgroup),
		url.PathEscape(name),
		url.PathEscape(version))
	return c.Do("PUT", path, req)
}

// --- helpers ---

func extractList(result map[string]interface{}, key string) []map[string]interface{} {
	if items, ok := result[key].([]interface{}); ok {
		return toMapSlice(items)
	}
	// Try "items" (from array wrapper)
	if items, ok := result["items"].([]interface{}); ok {
		return toMapSlice(items)
	}
	return nil
}

func mustJSON(v interface{}) string {
	b, _ := json.MarshalIndent(v, "", "  ")
	return string(b)
}
