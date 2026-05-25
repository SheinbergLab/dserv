// releases.go - Shared GitHub release lookups
//
// A registry-mode agent caches "latest release" metadata so a fleet of
// deployed agents and bootstrap clients can share one set of api.github.com
// calls instead of each hitting (and exhausting) the unauthenticated rate
// limit. Deployed agents prefer their registry; GitHub stays as the fallback.

package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"regexp"
	"strings"
	"sync"
	"time"
)

const releaseCacheTTL = 1 * time.Minute

// agentRepo is the dserv-agent source repo, installed by the bootstrap
// script and allowed through the /api/releases endpoint.
const agentRepo = "SheinbergLab/dserv-agent"

// repoPattern validates an "owner/repo" string before it is interpolated
// into a GitHub API URL — guards the unauthenticated /api/releases endpoint
// against path traversal and SSRF.
var repoPattern = regexp.MustCompile(`^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$`)

type releaseCacheEntry struct {
	release *ReleaseInfo
	fetched time.Time
}

// ReleaseCache memoizes GitHub release lookups. Safe for concurrent use.
type ReleaseCache struct {
	mu      sync.Mutex
	entries map[string]*releaseCacheEntry
}

func NewReleaseCache() *ReleaseCache {
	return &ReleaseCache{entries: make(map[string]*releaseCacheEntry)}
}

// getOrFetch returns a cached release if it is younger than releaseCacheTTL,
// otherwise calls fetch. If fetch fails it falls back to the stale entry
// (when present) so a GitHub/registry outage doesn't break callers. A double
// fetch on simultaneous expiry is possible but harmless at this scale.
func (c *ReleaseCache) getOrFetch(repo string, fetch func(string) (*ReleaseInfo, error)) *ReleaseInfo {
	c.mu.Lock()
	entry := c.entries[repo]
	c.mu.Unlock()

	if entry != nil && time.Since(entry.fetched) < releaseCacheTTL {
		return entry.release
	}

	release, err := fetch(repo)
	if err != nil {
		if entry != nil {
			log.Printf("release cache: %s fetch failed (%v), serving stale", repo, err)
			return entry.release
		}
		log.Printf("release cache: %s fetch failed: %v", repo, err)
		return nil
	}

	c.mu.Lock()
	c.entries[repo] = &releaseCacheEntry{release: release, fetched: time.Now()}
	c.mu.Unlock()
	return release
}

// fetchReleaseFromGitHub queries the GitHub API directly. Uses GITHUB_TOKEN
// when set (5000 req/hr authenticated vs 60 unauthenticated).
func (a *Agent) fetchReleaseFromGitHub(repo string) (*ReleaseInfo, error) {
	url := fmt.Sprintf("%s/%s/releases/latest", githubAPI, repo)
	req, err := http.NewRequest(http.MethodGet, url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Accept", "application/vnd.github+json")
	if a.cfg.GitHubToken != "" {
		req.Header.Set("Authorization", "Bearer "+a.cfg.GitHubToken)
	}
	resp, err := a.http.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("github API %s: HTTP %d", repo, resp.StatusCode)
	}
	var release ReleaseInfo
	if err := json.NewDecoder(resp.Body).Decode(&release); err != nil {
		return nil, err
	}
	return &release, nil
}

// fetchReleaseFromRegistry asks a configured registry for cached release
// info, so deployed agents don't each hit api.github.com.
func (a *Agent) fetchReleaseFromRegistry(repo string) (*ReleaseInfo, error) {
	var lastErr error
	for _, registryURL := range a.cfg.RegistryURLs {
		base := strings.TrimSuffix(normalizeURL(registryURL), "/")
		resp, err := a.http.Get(base + "/api/releases/" + repo)
		if err != nil {
			lastErr = err
			continue
		}
		if resp.StatusCode != http.StatusOK {
			resp.Body.Close()
			lastErr = fmt.Errorf("registry %s: HTTP %d", base, resp.StatusCode)
			continue
		}
		var release ReleaseInfo
		err = json.NewDecoder(resp.Body).Decode(&release)
		resp.Body.Close()
		if err != nil {
			lastErr = err
			continue
		}
		return &release, nil
	}
	if lastErr == nil {
		lastErr = fmt.Errorf("no registries configured")
	}
	return nil, lastErr
}

// fetchRelease picks the best source: a configured registry first, with
// GitHub as the fallback.
func (a *Agent) fetchRelease(repo string) (*ReleaseInfo, error) {
	if len(a.cfg.RegistryURLs) > 0 {
		if release, err := a.fetchReleaseFromRegistry(repo); err == nil {
			return release, nil
		} else if a.cfg.Verbose {
			log.Printf("registry release lookup for %s failed: %v, falling back to GitHub", repo, err)
		}
	}
	return a.fetchReleaseFromGitHub(repo)
}

// releaseRepoAllowed restricts the unauthenticated /api/releases endpoint to
// repos this registry actually manages, so it can't be used as an open
// GitHub API proxy.
func (a *Agent) releaseRepoAllowed(repo string) bool {
	if repo == agentRepo {
		return true
	}
	for _, c := range a.components {
		if c.Repo == repo {
			return true
		}
	}
	return false
}

// handleReleases serves cached GitHub release metadata so deployed agents and
// bootstrap clients don't each hit api.github.com. Unauthenticated (fresh
// boxes need bare access); restricted to managed repos.
//
// GET /api/releases/<owner>/<repo>
func (a *Agent) handleReleases(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	repo := strings.TrimPrefix(r.URL.Path, "/api/releases/")
	if !repoPattern.MatchString(repo) {
		http.Error(w, "Invalid repo", http.StatusBadRequest)
		return
	}
	if !a.releaseRepoAllowed(repo) {
		http.Error(w, "Unknown repo", http.StatusNotFound)
		return
	}
	release := a.getLatestRelease(repo)
	if release == nil {
		http.Error(w, "Release unavailable", http.StatusBadGateway)
		return
	}
	writeJSON(w, http.StatusOK, release)
}
