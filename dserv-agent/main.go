// dserv-agent: Management agent for dserv systems
//
// Two operational modes:
//   --server    Registry mode: receives heartbeats from dserv instances, serves mesh state
//   (default)   Client mode: component management, service control, web UI
//
// In client mode, mesh state comes from local dserv's mesh/peers datapoint (set by Tcl subprocess).
// Uses only Go standard library - no external dependencies.

package main

import (
	"archive/zip"
	"bufio"
	"context"
	"crypto/sha1"
	"embed"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"syscall"
	"time"
)

//go:embed web/*
var webContent embed.FS

//go:embed components.json
var defaultComponentsJSON []byte

const (
	version    = "0.6.0"
	githubAPI  = "https://api.github.com/repos"
	maxMsgSize = 512 * 1024
	wsGUID     = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
)

// WebSocket opcodes
const (
	wsOpContinue = 0
	wsOpText     = 1
	wsOpBinary   = 2
	wsOpClose    = 8
	wsOpPing     = 9
	wsOpPong     = 10
)

// Node state thresholds (in seconds)
const (
	staleThresholdSecs  = 10  // Node is "stale" after 10s without heartbeat
	unrespThresholdSecs = 60  // Node is "unresponsive" after 60s
	goneThresholdSecs   = 180 // Node is removed after 180s
)

var startTime = time.Now()

// ============ Data Types ============

// MeshNode represents a dserv instance in the mesh
type MeshNode struct {
	Hostname     string                 `json:"hostname"`
	IP           string                 `json:"ip"`
	Port         int                    `json:"port"`
	SSL          bool                   `json:"ssl"`
	Workgroup    string                 `json:"workgroup,omitempty"`
	Status       string                 `json:"status"`            // running, idle, stopped
	State        string                 `json:"state"`             // active, stale, unresponsive (computed)
	LastSeen     time.Time              `json:"lastSeen"`          // Last heartbeat time
	LastSeenAgo  int                    `json:"lastSeenAgo"`       // Seconds since last heartbeat (computed)
	IsLocal      bool                   `json:"isLocal,omitempty"` // True if this is the local node
	Services     []string               `json:"services,omitempty"`
	CustomFields map[string]interface{} `json:"customFields,omitempty"`
}

// HeartbeatRequest is sent by dserv Tcl subprocess to the registry
type HeartbeatRequest struct {
	Hostname     string                 `json:"hostname"`
	IP           string                 `json:"ip"`
	Port         int                    `json:"port"`
	SSL          bool                   `json:"ssl"`
	Workgroup    string                 `json:"workgroup"`
	Status       string                 `json:"status"`
	Services     []string               `json:"services,omitempty"`
	CustomFields map[string]interface{} `json:"customFields,omitempty"`
}

// HeartbeatResponse returns current mesh state to the sender
type HeartbeatResponse struct {
	OK    bool        `json:"ok"`
	Mesh  []*MeshNode `json:"mesh"`
	Error string      `json:"error,omitempty"`
}

// MeshResponse is returned by GET /api/v1/mesh
type MeshResponse struct {
	Workgroup string      `json:"workgroup"`
	Nodes     []*MeshNode `json:"nodes"`
	Updated   time.Time   `json:"updated"`
}

// Component represents an installable software component
type Component struct {
	ID           string   `json:"id"`
	Name         string   `json:"name"`
	Description  string   `json:"description,omitempty"`
	Type         string   `json:"type"`
	Repo         string   `json:"repo,omitempty"`
	Package      string   `json:"package,omitempty"`
	Service      string   `json:"service,omitempty"`
	InstallPath  string   `json:"installPath,omitempty"`
	VersionCmd   []string `json:"versionCmd,omitempty"`
	VersionFile  string   `json:"versionFile,omitempty"`
	PostInstall  []string `json:"postInstall,omitempty"`
	InstallCmd   string   `json:"installCmd,omitempty"`
	AssetPattern string   `json:"assetPattern,omitempty"`
	Depends      []string `json:"depends,omitempty"`
}

type ComponentStatus struct {
	Component      Component `json:"component"`
	Installed      bool      `json:"installed"`
	CurrentVersion string    `json:"currentVersion,omitempty"`
	LatestVersion  string    `json:"latestVersion,omitempty"`
	UpdateAvail    bool      `json:"updateAvailable"`
	Assets         []string  `json:"assets,omitempty"`
}

type StatusInfo struct {
	Agent    AgentInfo         `json:"agent"`
	Dserv    ServiceInfo       `json:"dserv"`
	System   SystemInfo        `json:"system"`
	Services map[string]string `json:"services,omitempty"`
}

type AgentInfo struct {
	Version   string `json:"version"`
	Uptime    string `json:"uptime"`
	StartTime string `json:"startTime"`
}

type ServiceInfo struct {
	Status  string `json:"status"`
	Version string `json:"version,omitempty"`
	PID     int    `json:"pid,omitempty"`
}

type SystemInfo struct {
	Hostname string `json:"hostname"`
	OS       string `json:"os"`
	Arch     string `json:"arch"`
	Uptime   string `json:"uptime,omitempty"`
	LoadAvg  string `json:"loadAvg,omitempty"`
}

type ReleaseInfo struct {
	TagName string `json:"tag_name"`
	Assets  []struct {
		Name        string `json:"name"`
		DownloadURL string `json:"browser_download_url"`
		Size        int64  `json:"size"`
	} `json:"assets"`
}

// Config holds agent configuration
type Config struct {
	// Common
	ListenAddr string
	Verbose    bool

	// TLS
	TLSCert string
	TLSKey  string

	// Server mode
	ServerMode bool

	// Client mode
	AuthToken      string
	DservService   string
	AllowReboot    bool
	UploadDir      string
	Timeout        time.Duration
	ComponentsFile string

	// Mesh registry (client mode)
	RegistryURLs []string // Multiple registries for redundancy
	Workgroup    string
}

// registryList is a custom flag type for multiple -registry flags
type registryList []string

func (r *registryList) String() string {
	return strings.Join(*r, ",")
}

func (r *registryList) Set(value string) error {
	*r = append(*r, value)
	return nil
}

// Registry holds mesh state (server mode only)
type Registry struct {
	workgroups map[string]*Workgroup
	mu         sync.RWMutex
}

type Workgroup struct {
	nodes   map[string]*MeshNode
	updated time.Time
}

// Agent is the main application
type Agent struct {
	cfg        Config
	registry   *Registry // server mode only
	clients    map[*WSConn]bool
	mu         sync.RWMutex
	http       *http.Client
	components []Component
	localID    string

	// Mesh cache (client mode) - populated by polling registry
	meshCache   []*MeshNode
	meshUpdated time.Time
	meshMu      sync.RWMutex
}

// WebSocket types
type WSConn struct {
	conn   net.Conn
	br     *bufio.Reader
	mu     sync.Mutex
	closed bool
}

type WSMessage struct {
	Type         string          `json:"type"`
	ID           string          `json:"id,omitempty"`
	Action       string          `json:"action,omitempty"`
	Component    string          `json:"component,omitempty"`
	Asset        string          `json:"asset,omitempty"`
	Service      string          `json:"service,omitempty"`
	StopServices []string        `json:"stopServices,omitempty"`
	Payload      json.RawMessage `json:"payload,omitempty"`
}

type WSResponse struct {
	Type    string      `json:"type"`
	ID      string      `json:"id,omitempty"`
	Success bool        `json:"success"`
	Data    interface{} `json:"data,omitempty"`
	Error   string      `json:"error,omitempty"`
	Service string      `json:"service,omitempty"`
	Action  string      `json:"action,omitempty"`
}

// ============ Main ============

func main() {
	cfg := Config{DservService: "dserv"}
	var registries registryList

	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, `dserv-agent - Management agent for dserv systems

Usage: dserv-agent [options]

The agent provides a web interface for managing dserv installations,
viewing mesh status, and updating components. It can also act as a
mesh registry server to coordinate multiple dserv instances.

Options:
`)
		flag.PrintDefaults()
		fmt.Fprintf(os.Stderr, `
Examples:
  # Basic client mode (web UI only, no mesh)
  dserv-agent

  # Client with mesh (polls registry for mesh status)
  dserv-agent --registry https://dserv.io --workgroup mylab

  # Registry server (receives heartbeats, serves mesh state)
  dserv-agent --server --listen :443

  # Combined: registry + local management + polls itself
  dserv-agent --server --registry http://localhost:80 --workgroup mylab

  # Multiple registries for redundancy
  dserv-agent --registry https://dserv.io --registry http://backup.local --workgroup mylab

  # Enable HTTPS with TLS certificates
  dserv-agent --tls-cert /usr/local/dserv/ssl/cert.pem --tls-key /usr/local/dserv/ssl/key.pem

Environment:
  DSERV_AGENT_TOKEN  Bearer token for API authentication (or use --token)
`)
	}

	// Common flags
	flag.StringVar(&cfg.ListenAddr, "listen", "0.0.0.0:80", "HTTP listen address (host:port)")
	flag.BoolVar(&cfg.Verbose, "v", false, "Verbose logging")

	// TLS flags
	flag.StringVar(&cfg.TLSCert, "tls-cert", "", "TLS certificate file (enables HTTPS)")
	flag.StringVar(&cfg.TLSKey, "tls-key", "", "TLS private key file")

	// Mode selection
	flag.BoolVar(&cfg.ServerMode, "server", false, "Enable registry server mode (receive heartbeats)")

	// Client mode flags
	flag.StringVar(&cfg.AuthToken, "token", "", "Bearer token for API authentication")
	flag.StringVar(&cfg.DservService, "service", "dserv", "systemd service name to manage")
	flag.BoolVar(&cfg.AllowReboot, "allow-reboot", false, "Allow system reboot via API")
	flag.StringVar(&cfg.UploadDir, "upload-dir", "/tmp/dserv-uploads", "Directory for file uploads")
	flag.DurationVar(&cfg.Timeout, "timeout", 5*time.Minute, "HTTP client timeout")
	flag.StringVar(&cfg.ComponentsFile, "components", "/etc/dserv-agent/components.json", "Components configuration file")

	// Mesh registry (client mode) - can specify multiple for redundancy
	flag.Var(&registries, "registry", "Mesh registry URL (can be specified multiple times)")
	flag.StringVar(&cfg.Workgroup, "workgroup", "", "Mesh workgroup name")

	flag.Parse()

	cfg.RegistryURLs = registries

	// Auto-detect dserv SSL certificates if not specified
	if cfg.TLSCert == "" && cfg.TLSKey == "" {
		defaultCert := "/usr/local/dserv/ssl/cert.pem"
		defaultKey := "/usr/local/dserv/ssl/key.pem"
		if _, err := os.Stat(defaultCert); err == nil {
			if _, err := os.Stat(defaultKey); err == nil {
				cfg.TLSCert = defaultCert
				cfg.TLSKey = defaultKey
			}
		}
	}

	if cfg.AuthToken == "" {
		cfg.AuthToken = os.Getenv("DSERV_AGENT_TOKEN")
	}

	localID, _ := os.Hostname()
	localID = strings.ToLower(strings.TrimSuffix(localID, ".local"))

	agent := &Agent{
		cfg:     cfg,
		clients: make(map[*WSConn]bool),
		http:    &http.Client{Timeout: cfg.Timeout},
		localID: localID,
	}

	mux := http.NewServeMux()

	if cfg.ServerMode {
		// ===== SERVER MODE =====
		agent.registry = &Registry{
			workgroups: make(map[string]*Workgroup),
		}
		log.Printf("dserv-agent %s starting on %s", version, cfg.ListenAddr)
		log.Printf("  Registry mode: enabled")

		// Registry endpoints
		mux.HandleFunc("/api/v1/heartbeat", agent.handleHeartbeat)
		mux.HandleFunc("/api/v1/mesh", agent.handleMeshQuery)
		mux.HandleFunc("/api/registry/status", agent.handleServerStatus)

		// Start cleanup goroutine
		go agent.registryCleanupLoop()

	} else {
		log.Printf("dserv-agent %s starting on %s", version, cfg.ListenAddr)
	}

	// ===== CLIENT MODE (always enabled) =====
	agent.loadComponents()
	log.Printf("  Loaded %d components", len(agent.components))

	if len(cfg.RegistryURLs) > 0 && cfg.Workgroup != "" {
		log.Printf("  Mesh workgroup: %s", cfg.Workgroup)
		for _, reg := range cfg.RegistryURLs {
			log.Printf("  Mesh registry: %s", reg)
		}
		// Start mesh polling loop
		go agent.meshPollingLoop()
	} else if !cfg.ServerMode {
		log.Printf("  Mesh: not configured (use --registry and --workgroup)")
	}

	// Standard agent endpoints
	mux.HandleFunc("/api/status", agent.auth(agent.handleStatus))
	mux.HandleFunc("/api/dserv/", agent.auth(agent.handleDserv))
	mux.HandleFunc("/api/service/", agent.auth(agent.handleService))
	mux.HandleFunc("/api/agent/restart", agent.auth(agent.handleAgentRestart))
	mux.HandleFunc("/api/logs", agent.auth(agent.handleLogs))
	mux.HandleFunc("/api/system/reboot", agent.auth(agent.handleReboot))
	mux.HandleFunc("/api/upload", agent.auth(agent.handleUpload))
	mux.HandleFunc("/api/files/", agent.auth(agent.handleFiles))
	mux.HandleFunc("/api/components", agent.auth(agent.handleComponents))
	mux.HandleFunc("/api/components/", agent.auth(agent.handleComponentAction))

	// Mesh endpoints (reads from cache)
	mux.HandleFunc("/api/mesh/peers", agent.auth(agent.handleMeshPeers))
	if !cfg.ServerMode {
		// Only add local mesh handler if not in server mode
		// (server mode already handles /api/v1/mesh from registry)
		mux.HandleFunc("/api/v1/mesh", agent.auth(agent.handleLocalMesh))
	}

	// WebSocket
	mux.HandleFunc("/ws", agent.handleWebSocket)

	// Serve embedded web UI
	webFS, _ := fs.Sub(webContent, "web")
	mux.Handle("/", http.FileServer(http.FS(webFS)))

	server := &http.Server{Addr: cfg.ListenAddr, Handler: mux}

	// Graceful shutdown
	go func() {
		c := make(chan os.Signal, 1)
		signal.Notify(c, syscall.SIGINT, syscall.SIGTERM)
		<-c
		log.Println("Shutting down...")
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		server.Shutdown(ctx)
	}()

	if cfg.AuthToken == "" {
		log.Println("WARNING: No auth token set")
	}

	// Start server with or without TLS
	if cfg.TLSCert != "" && cfg.TLSKey != "" {
		log.Printf("  TLS: enabled (cert: %s)", cfg.TLSCert)
		if err := server.ListenAndServeTLS(cfg.TLSCert, cfg.TLSKey); err != http.ErrServerClosed {
			log.Fatal(err)
		}
	} else {
		log.Printf("  TLS: disabled")
		if err := server.ListenAndServe(); err != http.ErrServerClosed {
			log.Fatal(err)
		}
	}
}

// ============ Server Mode: Registry Handlers ============

// POST /api/v1/heartbeat - receive heartbeat from dserv, return mesh state
func (a *Agent) handleHeartbeat(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req HeartbeatRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, HeartbeatResponse{
			OK:    false,
			Error: "Invalid JSON: " + err.Error(),
		})
		return
	}

	if req.Workgroup == "" {
		writeJSON(w, http.StatusBadRequest, HeartbeatResponse{
			OK:    false,
			Error: "Workgroup is required",
		})
		return
	}

	if req.Hostname == "" {
		writeJSON(w, http.StatusBadRequest, HeartbeatResponse{
			OK:    false,
			Error: "Hostname is required",
		})
		return
	}

	// Update registry
	node := &MeshNode{
		Hostname:     req.Hostname,
		IP:           req.IP,
		Port:         req.Port,
		SSL:          req.SSL,
		Workgroup:    req.Workgroup,
		Status:       req.Status,
		LastSeen:     time.Now(),
		Services:     req.Services,
		CustomFields: req.CustomFields,
	}

	a.registry.upsert(req.Workgroup, node)

	if a.cfg.Verbose {
		log.Printf("Heartbeat from %s/%s (%s)", req.Workgroup, req.Hostname, req.IP)
	}

	// Return current mesh state for this workgroup
	nodes := a.registry.getNodes(req.Workgroup)
	writeJSON(w, http.StatusOK, HeartbeatResponse{
		OK:   true,
		Mesh: nodes,
	})
}

// GET /api/v1/mesh?workgroup=xxx - query mesh state
func (a *Agent) handleMeshQuery(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	workgroup := r.URL.Query().Get("workgroup")
	if workgroup == "" {
		http.Error(w, "workgroup parameter required", http.StatusBadRequest)
		return
	}

	nodes := a.registry.getNodes(workgroup)
	writeJSON(w, http.StatusOK, MeshResponse{
		Workgroup: workgroup,
		Nodes:     nodes,
		Updated:   time.Now(),
	})
}

// GET /api/status - server status
func (a *Agent) handleServerStatus(w http.ResponseWriter, r *http.Request) {
	a.registry.mu.RLock()
	numWorkgroups := len(a.registry.workgroups)
	totalNodes := 0
	for _, wg := range a.registry.workgroups {
		totalNodes += len(wg.nodes)
	}
	a.registry.mu.RUnlock()

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"mode":       "server",
		"version":    version,
		"workgroups": numWorkgroups,
		"nodes":      totalNodes,
		"uptime":     time.Since(startTime).Round(time.Second).String(),
		"system": map[string]string{
			"os":   runtime.GOOS,
			"arch": runtime.GOARCH,
		},
	})
}

// Registry methods
func (r *Registry) upsert(workgroup string, node *MeshNode) {
	r.mu.Lock()
	defer r.mu.Unlock()

	wg, exists := r.workgroups[workgroup]
	if !exists {
		wg = &Workgroup{
			nodes: make(map[string]*MeshNode),
		}
		r.workgroups[workgroup] = wg
	}

	wg.nodes[node.Hostname] = node
	wg.updated = time.Now()
}

func (r *Registry) getNodes(workgroup string) []*MeshNode {
	r.mu.RLock()
	defer r.mu.RUnlock()

	wg, exists := r.workgroups[workgroup]
	if !exists {
		return []*MeshNode{}
	}

	now := time.Now()
	nodes := make([]*MeshNode, 0, len(wg.nodes))
	for _, n := range wg.nodes {
		node := *n // copy
		node.LastSeenAgo = int(now.Sub(n.LastSeen).Seconds())
		node.State = computeState(node.LastSeenAgo)
		nodes = append(nodes, &node)
	}
	return nodes
}

func (r *Registry) cleanup() {
	r.mu.Lock()
	defer r.mu.Unlock()

	now := time.Now()
	for wgName, wg := range r.workgroups {
		for hostname, node := range wg.nodes {
			age := int(now.Sub(node.LastSeen).Seconds())
			if age > goneThresholdSecs {
				delete(wg.nodes, hostname)
				log.Printf("Removed stale node %s/%s (last seen %ds ago)", wgName, hostname, age)
			}
		}
		if len(wg.nodes) == 0 {
			delete(r.workgroups, wgName)
		}
	}
}

func (a *Agent) registryCleanupLoop() {
	ticker := time.NewTicker(5 * time.Second)
	defer ticker.Stop()
	for range ticker.C {
		a.registry.cleanup()
	}
}

func computeState(lastSeenAgo int) string {
	switch {
	case lastSeenAgo <= staleThresholdSecs:
		return "active"
	case lastSeenAgo <= unrespThresholdSecs:
		return "stale"
	default:
		return "unresponsive"
	}
}

// ============ Client Mode: Mesh Polling ============

// normalizeURL ensures URL has scheme and port
// "localhost" -> "http://localhost:80"
// "http://example.com" -> "http://example.com:80"
// "https://example.com" -> "https://example.com:443"
func normalizeURL(rawURL string) string {
	// Add scheme if missing
	if !strings.HasPrefix(rawURL, "http://") && !strings.HasPrefix(rawURL, "https://") {
		rawURL = "http://" + rawURL
	}

	// Parse to check for port
	u, err := url.Parse(rawURL)
	if err != nil {
		return rawURL
	}

	// Add default port if missing
	if u.Port() == "" {
		if u.Scheme == "https" {
			u.Host = u.Host + ":443"
		} else {
			u.Host = u.Host + ":80"
		}
	}

	return u.String()
}

// meshPollingLoop fetches mesh state from registry every 5 seconds
func (a *Agent) meshPollingLoop() {
	ticker := time.NewTicker(5 * time.Second)
	defer ticker.Stop()

	// Initial fetch
	a.fetchMeshFromRegistry()

	for range ticker.C {
		a.fetchMeshFromRegistry()
	}
}

// fetchMeshFromRegistry polls the registry for current mesh state
// Tries each configured registry in order until one succeeds
func (a *Agent) fetchMeshFromRegistry() {
	var meshResp MeshResponse
	var lastErr error

	for _, registryURL := range a.cfg.RegistryURLs {
		baseURL := normalizeURL(registryURL)
		fullURL := fmt.Sprintf("%s/api/v1/mesh?workgroup=%s",
			strings.TrimSuffix(baseURL, "/"),
			a.cfg.Workgroup)

		resp, err := a.http.Get(fullURL)
		if err != nil {
			lastErr = err
			if a.cfg.Verbose {
				log.Printf("Mesh fetch from %s failed: %v", baseURL, err)
			}
			continue
		}

		if resp.StatusCode != http.StatusOK {
			resp.Body.Close()
			lastErr = fmt.Errorf("HTTP %d", resp.StatusCode)
			if a.cfg.Verbose {
				log.Printf("Mesh fetch from %s error: %d", baseURL, resp.StatusCode)
			}
			continue
		}

		err = json.NewDecoder(resp.Body).Decode(&meshResp)
		resp.Body.Close()
		if err != nil {
			lastErr = err
			if a.cfg.Verbose {
				log.Printf("Mesh decode from %s error: %v", baseURL, err)
			}
			continue
		}

		// Success - break out of loop
		lastErr = nil
		break
	}

	if lastErr != nil {
		// All registries failed
		if a.cfg.Verbose {
			log.Printf("All mesh registries failed, last error: %v", lastErr)
		}
		return
	}

	// Mark local node and update cache
	for _, n := range meshResp.Nodes {
		n.IsLocal = strings.EqualFold(n.Hostname, a.localID)
	}

	a.meshMu.Lock()
	a.meshCache = meshResp.Nodes
	a.meshUpdated = time.Now()
	a.meshMu.Unlock()

	if a.cfg.Verbose {
		log.Printf("Mesh updated: %d nodes", len(meshResp.Nodes))
	}

	// Broadcast to any connected WebSocket clients
	a.broadcast(WSResponse{
		Type:    "mesh_update",
		Success: true,
		Data:    meshResp.Nodes,
	})
}

// getMeshCache returns the current cached mesh state
func (a *Agent) getMeshCache() []*MeshNode {
	a.meshMu.RLock()
	defer a.meshMu.RUnlock()

	// Return a copy
	nodes := make([]*MeshNode, len(a.meshCache))
	copy(nodes, a.meshCache)
	return nodes
}

// GET /api/mesh/peers - return cached mesh peers
func (a *Agent) handleMeshPeers(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" {
		http.Error(w, "Method not allowed", 405)
		return
	}
	writeJSON(w, 200, a.getMeshCache())
}

// GET /api/v1/mesh - return mesh in standard format
func (a *Agent) handleLocalMesh(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" {
		http.Error(w, "Method not allowed", 405)
		return
	}

	a.meshMu.RLock()
	nodes := a.meshCache
	updated := a.meshUpdated
	a.meshMu.RUnlock()

	writeJSON(w, http.StatusOK, MeshResponse{
		Workgroup: a.cfg.Workgroup,
		Nodes:     nodes,
		Updated:   updated,
	})
}

// ============ Client Mode: Standard Agent Handlers ============

func (a *Agent) auth(h http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if a.cfg.AuthToken != "" {
			auth := r.Header.Get("Authorization")
			token := r.URL.Query().Get("token")
			if auth != "Bearer "+a.cfg.AuthToken && token != a.cfg.AuthToken {
				http.Error(w, "Unauthorized", 401)
				return
			}
		}
		h(w, r)
	}
}

func (a *Agent) handleStatus(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" {
		http.Error(w, "Method not allowed", 405)
		return
	}
	writeJSON(w, 200, a.getStatus())
}

func (a *Agent) getStatus() StatusInfo {
	return StatusInfo{
		Agent: AgentInfo{
			Version:   version,
			Uptime:    time.Since(startTime).Round(time.Second).String(),
			StartTime: startTime.Format(time.RFC3339),
		},
		Dserv:    a.getDservStatus(),
		System:   a.getSystemInfo(),
		Services: a.getAllServiceStatuses(),
	}
}

func (a *Agent) getDservStatus() ServiceInfo {
	info := ServiceInfo{Status: "unknown"}
	out, err := exec.Command("systemctl", "is-active", a.cfg.DservService).Output()
	if err == nil {
		info.Status = strings.TrimSpace(string(out))
	}
	if info.Status == "active" {
		if out, err := exec.Command("essctrl", "-c", "dservVersion").Output(); err == nil {
			info.Version = strings.TrimSpace(string(out))
		}
		out, _ := exec.Command("systemctl", "show", "-p", "MainPID", "--value", a.cfg.DservService).Output()
		fmt.Sscanf(strings.TrimSpace(string(out)), "%d", &info.PID)
	}
	return info
}

func (a *Agent) getSystemInfo() SystemInfo {
	info := SystemInfo{OS: runtime.GOOS, Arch: runtime.GOARCH}
	info.Hostname, _ = os.Hostname()
	if runtime.GOOS == "linux" {
		if data, err := os.ReadFile("/proc/uptime"); err == nil {
			var secs float64
			fmt.Sscanf(string(data), "%f", &secs)
			info.Uptime = (time.Duration(secs) * time.Second).Round(time.Second).String()
		}
		if data, err := os.ReadFile("/proc/loadavg"); err == nil {
			parts := strings.Fields(string(data))
			if len(parts) >= 3 {
				info.LoadAvg = strings.Join(parts[:3], " ")
			}
		}
	}
	return info
}

func (a *Agent) getAllServiceStatuses() map[string]string {
	services := make(map[string]string)
	for _, comp := range a.components {
		if comp.Service != "" {
			services[comp.Service] = a.getServiceStatus(comp.Service)
		}
	}
	return services
}

func (a *Agent) getServiceStatus(service string) string {
	out, err := exec.Command("systemctl", "is-active", service).Output()
	if err != nil {
		return "inactive"
	}
	status := strings.TrimSpace(string(out))
	if status == "active" {
		return "active"
	}
	return "inactive"
}

func (a *Agent) handleDserv(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		http.Error(w, "Method not allowed", 405)
		return
	}
	action := strings.TrimPrefix(r.URL.Path, "/api/dserv/")
	var cmd *exec.Cmd
	switch action {
	case "start":
		cmd = exec.Command("sudo", "systemctl", "start", a.cfg.DservService)
	case "stop":
		cmd = exec.Command("sudo", "systemctl", "stop", a.cfg.DservService)
	case "restart":
		cmd = exec.Command("sudo", "systemctl", "restart", a.cfg.DservService)
	case "status":
		writeJSON(w, 200, a.getDservStatus())
		return
	default:
		http.Error(w, "Unknown action", 400)
		return
	}
	out, err := cmd.CombinedOutput()
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": string(out)})
		return
	}
	time.Sleep(500 * time.Millisecond)
	writeJSON(w, 200, map[string]interface{}{"success": true, "action": action, "status": a.getDservStatus()})
}

func (a *Agent) handleService(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		http.Error(w, "Method not allowed", 405)
		return
	}

	path := strings.TrimPrefix(r.URL.Path, "/api/service/")
	parts := strings.SplitN(path, "/", 2)
	if len(parts) != 2 {
		http.Error(w, "Invalid path", 400)
		return
	}

	service := parts[0]
	action := parts[1]

	found := false
	for _, comp := range a.components {
		if comp.Service == service {
			found = true
			break
		}
	}
	if !found {
		http.Error(w, "Unknown service: "+service, 404)
		return
	}

	var cmd *exec.Cmd
	switch action {
	case "start":
		cmd = exec.Command("sudo", "systemctl", "start", service)
	case "stop":
		cmd = exec.Command("sudo", "systemctl", "stop", service)
	case "restart":
		cmd = exec.Command("sudo", "systemctl", "restart", service)
	case "status":
		writeJSON(w, 200, map[string]string{"service": service, "status": a.getServiceStatus(service)})
		return
	default:
		http.Error(w, "Unknown action: "+action, 400)
		return
	}

	out, err := cmd.CombinedOutput()
	if err != nil {
		writeJSON(w, 500, map[string]interface{}{"success": false, "service": service, "error": string(out)})
		return
	}
	time.Sleep(500 * time.Millisecond)
	writeJSON(w, 200, map[string]interface{}{"success": true, "service": service, "status": a.getServiceStatus(service)})
}

func (a *Agent) handleAgentRestart(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		http.Error(w, "Method not allowed", 405)
		return
	}
	writeJSON(w, 202, map[string]string{"status": "restarting"})
	if f, ok := w.(http.Flusher); ok {
		f.Flush()
	}
	go func() {
		time.Sleep(500 * time.Millisecond)
		exec.Command("sudo", "systemctl", "restart", "dserv-agent").Run()
	}()
}

func (a *Agent) handleLogs(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" {
		http.Error(w, "Method not allowed", 405)
		return
	}

	service := r.URL.Query().Get("service")
	if service == "" {
		service = a.cfg.DservService
	}

	lines := r.URL.Query().Get("lines")
	if lines == "" {
		lines = "50"
	}

	cmd := exec.Command("journalctl", "-u", service, "-n", lines, "--no-pager", "-o", "short-iso")
	out, err := cmd.CombinedOutput()
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error(), "output": string(out)})
		return
	}

	writeJSON(w, 200, map[string]interface{}{"service": service, "lines": lines, "logs": string(out)})
}

func (a *Agent) handleReboot(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		http.Error(w, "Method not allowed", 405)
		return
	}
	if !a.cfg.AllowReboot {
		http.Error(w, "Reboot not allowed", 403)
		return
	}
	writeJSON(w, 202, map[string]string{"status": "rebooting"})
	go func() {
		time.Sleep(1 * time.Second)
		exec.Command("sudo", "reboot").Run()
	}()
}

func (a *Agent) handleUpload(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		http.Error(w, "Method not allowed", 405)
		return
	}
	os.MkdirAll(a.cfg.UploadDir, 0755)
	r.ParseMultipartForm(100 << 20)
	file, handler, err := r.FormFile("file")
	if err != nil {
		http.Error(w, "File required", 400)
		return
	}
	defer file.Close()

	dst, err := os.Create(filepath.Join(a.cfg.UploadDir, handler.Filename))
	if err != nil {
		http.Error(w, err.Error(), 500)
		return
	}
	defer dst.Close()
	io.Copy(dst, file)
	writeJSON(w, 200, map[string]string{"filename": handler.Filename, "path": dst.Name()})
}

func (a *Agent) handleFiles(w http.ResponseWriter, r *http.Request) {
	path := strings.TrimPrefix(r.URL.Path, "/api/files")
	if path == "" {
		path = a.cfg.UploadDir
	}
	switch r.Method {
	case "GET":
		info, err := os.Stat(path)
		if err != nil {
			http.Error(w, "Not found", 404)
			return
		}
		if info.IsDir() {
			entries, _ := os.ReadDir(path)
			var files []map[string]interface{}
			for _, e := range entries {
				i, _ := e.Info()
				files = append(files, map[string]interface{}{"name": e.Name(), "isDir": e.IsDir(), "size": i.Size()})
			}
			writeJSON(w, 200, files)
		} else {
			http.ServeFile(w, r, path)
		}
	case "DELETE":
		if err := os.Remove(path); err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, 200, map[string]bool{"deleted": true})
	default:
		http.Error(w, "Method not allowed", 405)
	}
}

// ============ Component Management ============

func (a *Agent) loadComponents() {
	var cfg struct {
		Components []Component `json:"components"`
	}

	if err := json.Unmarshal(defaultComponentsJSON, &cfg); err == nil {
		a.components = cfg.Components
	}

	data, err := os.ReadFile(a.cfg.ComponentsFile)
	if err == nil {
		var fileCfg struct {
			Components []Component `json:"components"`
		}
		if json.Unmarshal(data, &fileCfg) == nil {
			a.components = fileCfg.Components
			log.Printf("Loaded components from %s", a.cfg.ComponentsFile)
		}
	}

	if len(a.components) == 0 {
		a.components = []Component{
			{
				ID:          "dserv",
				Name:        "dserv",
				Description: "Data acquisition server",
				Type:        "github-deb",
				Repo:        "SheinbergLab/dserv",
				Package:     "dserv",
				Service:     "dserv",
				VersionCmd:  []string{"essctrl", "-c", "dservVersion"},
			},
		}
	}
}

func (a *Agent) handleComponents(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" {
		http.Error(w, "Method not allowed", 405)
		return
	}
	statuses := make([]ComponentStatus, 0, len(a.components))
	for _, comp := range a.components {
		statuses = append(statuses, a.getComponentStatus(comp))
	}
	writeJSON(w, 200, statuses)
}

func (a *Agent) handleComponentAction(w http.ResponseWriter, r *http.Request) {
	path := strings.TrimPrefix(r.URL.Path, "/api/components/")
	parts := strings.SplitN(path, "/", 2)
	compID := parts[0]
	action := ""
	if len(parts) > 1 {
		action = parts[1]
	}

	var comp *Component
	for i := range a.components {
		if a.components[i].ID == compID {
			comp = &a.components[i]
			break
		}
	}
	if comp == nil {
		http.Error(w, "Component not found", 404)
		return
	}

	switch action {
	case "", "status":
		if r.Method != "GET" {
			http.Error(w, "Method not allowed", 405)
			return
		}
		writeJSON(w, 200, a.getComponentStatus(*comp))

	case "install":
		if r.Method != "POST" {
			http.Error(w, "Method not allowed", 405)
			return
		}
		asset := r.URL.Query().Get("asset")
		var stopServices []string
		if asset == "" {
			var body struct {
				Asset        string   `json:"asset"`
				StopServices []string `json:"stopServices"`
			}
			json.NewDecoder(r.Body).Decode(&body)
			asset = body.Asset
			stopServices = body.StopServices
		}
		go a.installComponent(*comp, asset, stopServices)
		writeJSON(w, 202, map[string]string{"status": "started", "component": comp.ID})

	default:
		http.Error(w, "Unknown action", 400)
	}
}

func (a *Agent) getComponentStatus(comp Component) ComponentStatus {
	status := ComponentStatus{Component: comp}
	status.CurrentVersion = a.getInstalledVersion(comp)
	status.Installed = status.CurrentVersion != ""

	if comp.Repo != "" {
		release := a.getLatestRelease(comp.Repo)
		if release != nil {
			status.LatestVersion = release.TagName
			status.Assets = a.filterAssets(release, comp)
			status.UpdateAvail = status.Installed && status.CurrentVersion != status.LatestVersion
		}
	}
	return status
}

func (a *Agent) getInstalledVersion(comp Component) string {
	if len(comp.VersionCmd) > 0 {
		out, err := exec.Command(comp.VersionCmd[0], comp.VersionCmd[1:]...).Output()
		if err == nil {
			return strings.TrimSpace(string(out))
		}
	}
	if comp.VersionFile != "" {
		data, err := os.ReadFile(comp.VersionFile)
		if err == nil {
			return strings.TrimSpace(string(data))
		}
	}
	if comp.Package != "" {
		out, err := exec.Command("dpkg-query", "-W", "-f=${Version}", comp.Package).Output()
		if err == nil {
			return strings.TrimSpace(string(out))
		}
	}
	return ""
}

func (a *Agent) getLatestRelease(repo string) *ReleaseInfo {
	url := fmt.Sprintf("%s/%s/releases/latest", githubAPI, repo)
	resp, err := a.http.Get(url)
	if err != nil {
		return nil
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return nil
	}
	var release ReleaseInfo
	if err := json.NewDecoder(resp.Body).Decode(&release); err != nil {
		return nil
	}
	return &release
}

func (a *Agent) filterAssets(release *ReleaseInfo, comp Component) []string {
	arch := runtime.GOARCH
	if arch == "arm" {
		arch = "armhf"
	}

	var assets []string
	for _, asset := range release.Assets {
		name := strings.ToLower(asset.Name)

		// Basic filtering
		if !strings.HasSuffix(name, ".deb") && !strings.HasSuffix(name, ".zip") {
			continue
		}

		// Architecture filtering
		if strings.Contains(name, "amd64") && arch != "amd64" {
			continue
		}
		if strings.Contains(name, "arm64") && arch != "arm64" {
			continue
		}
		if strings.Contains(name, "armhf") && arch != "armhf" {
			continue
		}

		assets = append(assets, asset.Name)
	}
	return assets
}

func (a *Agent) installComponent(comp Component, assetName string, stopServices []string) {
	a.broadcast(WSResponse{Type: "install_progress", Data: map[string]string{"stage": "checking", "component": comp.ID}})

	release := a.getLatestRelease(comp.Repo)
	if release == nil {
		a.broadcast(WSResponse{Type: "install_error", Error: "Failed to get release info"})
		return
	}

	var downloadURL string
	for _, asset := range release.Assets {
		if asset.Name == assetName {
			downloadURL = asset.DownloadURL
			break
		}
	}
	if downloadURL == "" {
		a.broadcast(WSResponse{Type: "install_error", Error: "Asset not found: " + assetName})
		return
	}

	a.broadcast(WSResponse{Type: "install_progress", Data: map[string]string{"stage": "downloading", "version": release.TagName}})

	// Download
	os.MkdirAll(a.cfg.UploadDir, 0755)
	localPath := filepath.Join(a.cfg.UploadDir, assetName)

	resp, err := a.http.Get(downloadURL)
	if err != nil {
		a.broadcast(WSResponse{Type: "install_error", Error: "Download failed: " + err.Error()})
		return
	}
	defer resp.Body.Close()

	out, err := os.Create(localPath)
	if err != nil {
		a.broadcast(WSResponse{Type: "install_error", Error: "Create file failed: " + err.Error()})
		return
	}
	io.Copy(out, resp.Body)
	out.Close()

	// Stop services if requested
	if len(stopServices) > 0 {
		a.broadcast(WSResponse{Type: "install_progress", Data: map[string]string{"stage": "stopping"}})
		for _, svc := range stopServices {
			exec.Command("sudo", "systemctl", "stop", svc).Run()
		}
	}

	// Install
	a.broadcast(WSResponse{Type: "install_progress", Data: map[string]string{"stage": "installing"}})

	if strings.HasSuffix(assetName, ".deb") {
		cmd := exec.Command("sudo", "dpkg", "-i", localPath)
		if output, err := cmd.CombinedOutput(); err != nil {
			a.broadcast(WSResponse{Type: "install_error", Error: "Install failed: " + string(output)})
			return
		}
	} else if strings.HasSuffix(assetName, ".zip") && comp.InstallPath != "" {
		if err := a.extractZip(localPath, comp.InstallPath); err != nil {
			a.broadcast(WSResponse{Type: "install_error", Error: "Extract failed: " + err.Error()})
			return
		}
	}

	// Start services
	if len(stopServices) > 0 {
		a.broadcast(WSResponse{Type: "install_progress", Data: map[string]string{"stage": "starting"}})
		for _, svc := range stopServices {
			exec.Command("sudo", "systemctl", "start", svc).Run()
		}
	}

	os.Remove(localPath)
	a.broadcast(WSResponse{Type: "install_complete", Success: true, Data: map[string]string{"component": comp.ID, "version": release.TagName}})
}

func (a *Agent) extractZip(zipPath, destPath string) error {
	r, err := zip.OpenReader(zipPath)
	if err != nil {
		return err
	}
	defer r.Close()

	os.MkdirAll(destPath, 0755)

	for _, f := range r.File {
		fpath := filepath.Join(destPath, f.Name)
		if f.FileInfo().IsDir() {
			os.MkdirAll(fpath, f.Mode())
			continue
		}
		os.MkdirAll(filepath.Dir(fpath), 0755)
		outFile, err := os.OpenFile(fpath, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, f.Mode())
		if err != nil {
			return err
		}
		rc, err := f.Open()
		if err != nil {
			outFile.Close()
			return err
		}
		io.Copy(outFile, rc)
		outFile.Close()
		rc.Close()
	}
	return nil
}

// ============ WebSocket ============

func (a *Agent) handleWebSocket(w http.ResponseWriter, r *http.Request) {
	if a.cfg.AuthToken != "" {
		if r.URL.Query().Get("token") != a.cfg.AuthToken {
			http.Error(w, "Unauthorized", 401)
			return
		}
	}
	key := r.Header.Get("Sec-WebSocket-Key")
	if key == "" {
		http.Error(w, "Bad request", 400)
		return
	}
	h := sha1.New()
	h.Write([]byte(key + wsGUID))
	accept := base64.StdEncoding.EncodeToString(h.Sum(nil))

	hj, ok := w.(http.Hijacker)
	if !ok {
		http.Error(w, "Hijack not supported", 500)
		return
	}
	conn, rw, err := hj.Hijack()
	if err != nil {
		http.Error(w, err.Error(), 500)
		return
	}

	rw.WriteString("HTTP/1.1 101 Switching Protocols\r\n")
	rw.WriteString("Upgrade: websocket\r\n")
	rw.WriteString("Connection: Upgrade\r\n")
	rw.WriteString("Sec-WebSocket-Accept: " + accept + "\r\n")
	rw.WriteString("Access-Control-Allow-Origin: *\r\n\r\n")
	rw.Flush()

	ws := &WSConn{conn: conn, br: rw.Reader}
	a.mu.Lock()
	a.clients[ws] = true
	a.mu.Unlock()

	ws.WriteJSON(WSResponse{Type: "status", Success: true, Data: a.getStatus()})
	go a.wsReadLoop(ws)
}

func (a *Agent) wsReadLoop(ws *WSConn) {
	defer func() {
		a.mu.Lock()
		delete(a.clients, ws)
		a.mu.Unlock()
		ws.Close()
	}()
	for {
		data, err := ws.ReadMessage()
		if err != nil {
			return
		}
		var msg WSMessage
		if json.Unmarshal(data, &msg) != nil {
			continue
		}
		resp := a.handleWSMessage(msg)
		if resp.Type != "" {
			ws.WriteJSON(resp)
		}
	}
}

func (a *Agent) handleWSMessage(msg WSMessage) WSResponse {
	resp := WSResponse{Type: msg.Type + "_response", ID: msg.ID}

	switch msg.Type {
	case "status":
		resp.Success = true
		resp.Data = a.getStatus()

	case "dserv":
		var cmd *exec.Cmd
		switch msg.Action {
		case "start":
			cmd = exec.Command("sudo", "systemctl", "start", a.cfg.DservService)
		case "stop":
			cmd = exec.Command("sudo", "systemctl", "stop", a.cfg.DservService)
		case "restart":
			cmd = exec.Command("sudo", "systemctl", "restart", a.cfg.DservService)
		case "status":
			resp.Success = true
			resp.Data = a.getDservStatus()
			return resp
		default:
			resp.Error = "Unknown action"
			return resp
		}
		if out, err := cmd.CombinedOutput(); err != nil {
			resp.Error = string(out)
		} else {
			time.Sleep(500 * time.Millisecond)
			resp.Success = true
			resp.Data = a.getDservStatus()
		}

	case "service":
		service := msg.Service
		action := msg.Action
		if service == "" {
			resp.Type = "service_response"
			resp.Error = "No service specified"
			return resp
		}
		found := false
		for _, comp := range a.components {
			if comp.Service == service {
				found = true
				break
			}
		}
		if !found {
			resp.Type = "service_response"
			resp.Error = "Unknown service: " + service
			return resp
		}
		var cmd *exec.Cmd
		switch action {
		case "start":
			cmd = exec.Command("sudo", "systemctl", "start", service)
		case "stop":
			cmd = exec.Command("sudo", "systemctl", "stop", service)
		case "restart":
			cmd = exec.Command("sudo", "systemctl", "restart", service)
		case "status":
			resp.Type = "service_response"
			resp.Success = true
			resp.Service = service
			resp.Data = map[string]string{"status": a.getServiceStatus(service)}
			return resp
		default:
			resp.Type = "service_response"
			resp.Error = "Unknown action"
			return resp
		}
		resp.Type = "service_response"
		resp.Service = service
		resp.Action = action
		if out, err := cmd.CombinedOutput(); err != nil {
			resp.Error = string(out)
		} else {
			time.Sleep(500 * time.Millisecond)
			resp.Success = true
			resp.Data = map[string]string{"status": a.getServiceStatus(service)}
		}

	case "components":
		statuses := make([]ComponentStatus, 0, len(a.components))
		for _, comp := range a.components {
			statuses = append(statuses, a.getComponentStatus(comp))
		}
		resp.Success = true
		resp.Data = statuses

	case "install":
		var comp *Component
		for i := range a.components {
			if a.components[i].ID == msg.Component {
				comp = &a.components[i]
				break
			}
		}
		if comp == nil {
			resp.Error = "Component not found: " + msg.Component
			return resp
		}
		go a.installComponent(*comp, msg.Asset, msg.StopServices)
		resp.Success = true
		resp.Data = map[string]string{"status": "started", "component": comp.ID}

	case "mesh_peers":
		resp.Success = true
		resp.Data = a.getMeshCache()

	case "logs":
		service := msg.Action
		if service == "" {
			service = a.cfg.DservService
		}
		cmd := exec.Command("journalctl", "-u", service, "-n", "100", "--no-pager", "-o", "short-iso")
		out, err := cmd.CombinedOutput()
		if err != nil {
			resp.Error = err.Error()
		} else {
			resp.Success = true
			resp.Data = map[string]interface{}{"service": service, "logs": string(out)}
		}

	case "agent_restart":
		resp.Success = true
		resp.Data = map[string]string{"status": "restarting"}
		go func() {
			time.Sleep(500 * time.Millisecond)
			exec.Command("sudo", "systemctl", "restart", "dserv-agent").Run()
		}()
	}

	return resp
}

func (a *Agent) broadcast(msg WSResponse) {
	a.mu.RLock()
	clients := make([]*WSConn, 0, len(a.clients))
	for c := range a.clients {
		clients = append(clients, c)
	}
	a.mu.RUnlock()

	for _, c := range clients {
		c.WriteJSON(msg)
	}
}

// WebSocket connection methods
func (ws *WSConn) WriteJSON(v interface{}) error {
	ws.mu.Lock()
	defer ws.mu.Unlock()
	if ws.closed {
		return fmt.Errorf("connection closed")
	}
	data, err := json.Marshal(v)
	if err != nil {
		return err
	}
	return ws.writeFrame(wsOpText, data)
}

func (ws *WSConn) writeFrame(opcode byte, data []byte) error {
	var header []byte
	length := len(data)

	if length < 126 {
		header = []byte{0x80 | opcode, byte(length)}
	} else if length < 65536 {
		header = []byte{0x80 | opcode, 126, byte(length >> 8), byte(length)}
	} else {
		header = make([]byte, 10)
		header[0] = 0x80 | opcode
		header[1] = 127
		binary.BigEndian.PutUint64(header[2:], uint64(length))
	}

	if _, err := ws.conn.Write(header); err != nil {
		return err
	}
	_, err := ws.conn.Write(data)
	return err
}

func (ws *WSConn) ReadMessage() ([]byte, error) {
	header := make([]byte, 2)
	if _, err := io.ReadFull(ws.br, header); err != nil {
		return nil, err
	}

	opcode := header[0] & 0x0f
	masked := header[1]&0x80 != 0
	length := uint64(header[1] & 0x7f)

	if opcode == wsOpClose {
		return nil, fmt.Errorf("connection closed")
	}
	if opcode == wsOpPing {
		ws.writeFrame(wsOpPong, nil)
		return ws.ReadMessage()
	}

	if length == 126 {
		ext := make([]byte, 2)
		io.ReadFull(ws.br, ext)
		length = uint64(binary.BigEndian.Uint16(ext))
	} else if length == 127 {
		ext := make([]byte, 8)
		io.ReadFull(ws.br, ext)
		length = binary.BigEndian.Uint64(ext)
	}

	if length > maxMsgSize {
		return nil, fmt.Errorf("message too large")
	}

	var mask []byte
	if masked {
		mask = make([]byte, 4)
		io.ReadFull(ws.br, mask)
	}

	data := make([]byte, length)
	if _, err := io.ReadFull(ws.br, data); err != nil {
		return nil, err
	}

	if masked {
		for i := range data {
			data[i] ^= mask[i%4]
		}
	}

	return data, nil
}

func (ws *WSConn) Close() {
	ws.mu.Lock()
	defer ws.mu.Unlock()
	if !ws.closed {
		ws.closed = true
		ws.conn.Close()
	}
}

// ============ Helpers ============

func writeJSON(w http.ResponseWriter, code int, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(v)
}
