// ess_viewer.go - Viewer plugin endpoint for DG Viewer
//
// Serves experiment-specific viewer JS plugins. The loading cascade:
//   1. Local ESS registry (if this agent has --ess-registry)
//   2. Remote registry (proxied + cached locally)
//
// Endpoints:
//   GET /api/v1/ess/viewer/{system}              — system-level viewer
//   GET /api/v1/ess/viewer/{system}/{protocol}   — protocol-level viewer
//
// Protocol-level viewers overlay on top of system viewers. The client
// is responsible for loading the system viewer first, then optionally
// the protocol viewer.

package main

import (
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// viewerCache provides in-memory + filesystem caching for viewer plugins
// fetched from remote registries.
type viewerCache struct {
	mu       sync.RWMutex
	dir      string            // filesystem cache directory
	entries  map[string]*viewerCacheEntry
}

type viewerCacheEntry struct {
	content   string
	checksum  string
	fetchedAt time.Time
}

const viewerCacheTTL = 5 * time.Minute

func newViewerCache(cacheDir string) *viewerCache {
	if cacheDir != "" {
		os.MkdirAll(cacheDir, 0755)
	}
	return &viewerCache{
		dir:     cacheDir,
		entries: make(map[string]*viewerCacheEntry),
	}
}

// handleViewerPlugin serves viewer JS plugins for experiments.
// URL patterns:
//   /api/v1/ess/viewer/{system}
//   /api/v1/ess/viewer/{system}/{protocol}
func (a *Agent) handleViewerPlugin(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	// Parse path: /api/v1/ess/viewer/{system}[/{protocol}]
	path := strings.TrimPrefix(r.URL.Path, "/api/v1/ess/viewer/")
	path = strings.TrimSuffix(path, "/")
	if path == "" {
		http.Error(w, "system name required", http.StatusBadRequest)
		return
	}

	parts := strings.SplitN(path, "/", 2)
	system := parts[0]
	protocol := ""
	if len(parts) > 1 {
		protocol = parts[1]
	}

	// Determine workgroup from agent config
	workgroup := a.cfg.Workgroup

	// 1. Try local ESS registry
	if a.essRegistry != nil {
		content, err := a.viewerFromLocalRegistry(workgroup, system, protocol)
		if err == nil && content != "" {
			w.Header().Set("Content-Type", "application/javascript; charset=utf-8")
			w.Header().Set("Cache-Control", "no-cache")
			w.Write([]byte(content))
			return
		}
	}

	// 2. Try remote registry (with caching)
	if len(a.cfg.RegistryURLs) > 0 && workgroup != "" {
		content, err := a.viewerFromRemoteRegistry(workgroup, system, protocol)
		if err == nil && content != "" {
			w.Header().Set("Content-Type", "application/javascript; charset=utf-8")
			w.Header().Set("Cache-Control", "no-cache")
			w.Write([]byte(content))
			return
		}
		if err != nil {
			log.Printf("viewer: remote fetch failed for %s/%s: %v", system, protocol, err)
		}
	}

	// 3. Try filesystem cache (works offline after first fetch)
	if a.vCache != nil {
		content := a.vCache.getFromDisk(system, protocol)
		if content != "" {
			w.Header().Set("Content-Type", "application/javascript; charset=utf-8")
			w.Header().Set("Cache-Control", "no-cache")
			w.Header().Set("X-Viewer-Source", "cache")
			w.Write([]byte(content))
			return
		}
	}

	http.Error(w, "no viewer found", http.StatusNotFound)
}

// viewerFromLocalRegistry fetches a viewer script from the local ESS database.
func (a *Agent) viewerFromLocalRegistry(workgroup, system, protocol string) (string, error) {
	sys, err := a.essRegistry.GetSystem(workgroup, system, "")
	if err != nil || sys == nil {
		return "", fmt.Errorf("system not found: %s/%s", workgroup, system)
	}

	script, err := a.essRegistry.GetScript(sys.ID, protocol, ScriptTypeViewer)
	if err != nil {
		return "", err
	}
	return script.Content, nil
}

// viewerFromRemoteRegistry fetches a viewer script from a remote registry,
// using an in-memory + disk cache to minimize network calls.
func (a *Agent) viewerFromRemoteRegistry(workgroup, system, protocol string) (string, error) {
	cacheKey := system + "/" + protocol

	// Check in-memory cache
	if a.vCache != nil {
		a.vCache.mu.RLock()
		entry, ok := a.vCache.entries[cacheKey]
		a.vCache.mu.RUnlock()
		if ok && time.Since(entry.fetchedAt) < viewerCacheTTL {
			return entry.content, nil
		}
	}

	// Fetch from remote registry
	// The protocol path segment: use "_" for system-level (empty protocol)
	protoPath := protocol
	if protoPath == "" {
		protoPath = "_"
	}

	var content string
	var checksum string
	var lastErr error

	for _, registryURL := range a.cfg.RegistryURLs {
		base := strings.TrimSuffix(registryURL, "/")
		url := fmt.Sprintf("%s/api/v1/ess/script/%s/%s/%s/%s",
			base, workgroup, system, protoPath, ScriptTypeViewer)

		resp, err := a.http.Get(url)
		if err != nil {
			lastErr = err
			continue
		}
		defer resp.Body.Close()

		if resp.StatusCode == http.StatusNotFound {
			return "", nil // Not an error, just no viewer
		}
		if resp.StatusCode != http.StatusOK {
			body, _ := io.ReadAll(resp.Body)
			lastErr = fmt.Errorf("registry returned %d: %s", resp.StatusCode, string(body))
			continue
		}

		// Parse the ESSScript JSON response
		var script struct {
			Content  string `json:"content"`
			Checksum string `json:"checksum"`
		}
		if err := json.NewDecoder(resp.Body).Decode(&script); err != nil {
			lastErr = fmt.Errorf("decode error: %w", err)
			continue
		}
		content = script.Content
		checksum = script.Checksum
		break
	}

	if content == "" {
		return "", lastErr
	}

	// Update cache
	if a.vCache != nil {
		a.vCache.mu.Lock()
		a.vCache.entries[cacheKey] = &viewerCacheEntry{
			content:   content,
			checksum:  checksum,
			fetchedAt: time.Now(),
		}
		a.vCache.mu.Unlock()

		// Write to disk cache for offline use
		a.vCache.writeToDisk(system, protocol, content)
	}

	return content, nil
}

// viewerCache disk methods

func (c *viewerCache) cacheFilePath(system, protocol string) string {
	if c.dir == "" {
		return ""
	}
	name := system
	if protocol != "" {
		name = system + "_" + protocol
	}
	// Sanitize
	name = strings.Map(func(r rune) rune {
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') || r == '_' || r == '-' {
			return r
		}
		return '_'
	}, name)
	return filepath.Join(c.dir, name+".js")
}

func (c *viewerCache) writeToDisk(system, protocol, content string) {
	path := c.cacheFilePath(system, protocol)
	if path == "" {
		return
	}
	// Write atomically via temp file
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, []byte(content), 0644); err != nil {
		log.Printf("viewer cache: write error: %v", err)
		return
	}
	os.Rename(tmp, path)
}

func (c *viewerCache) getFromDisk(system, protocol string) string {
	path := c.cacheFilePath(system, protocol)
	if path == "" {
		return ""
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return string(data)
}

func viewerChecksum(content string) string {
	h := sha256.Sum256([]byte(content))
	return fmt.Sprintf("%x", h[:8])
}
