// dserv-agent: Management agent for dserv systems
// Provides HTTP/WebSocket APIs for remote management, updates, and monitoring.
// Runs as an independent systemd service so it can manage dserv even when dserv is down.
//
// Uses only Go standard library - no external dependencies.

package main

import (
	"archive/zip"
	"bufio"
	"context"
	"crypto/sha1"
	"crypto/tls"
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

const (
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

var (
	version   = "0.3.0"
	startTime = time.Now()
)

// MeshPeer represents a discovered dserv instance
type MeshPeer struct {
	ApplianceID   string            `json:"applianceId"`
	Name          string            `json:"name"`
	Status        string            `json:"status"`
	IPAddress     string            `json:"ipAddress"`
	WebPort       int               `json:"webPort"`
	SSL           bool              `json:"ssl"`
	LastHeartbeat time.Time         `json:"lastHeartbeat"`
	IsLocal       bool              `json:"isLocal"`
	CustomFields  map[string]string `json:"customFields,omitempty"`
}

// MeshHeartbeat is the UDP broadcast format from dserv
type MeshHeartbeat struct {
	Type        string `json:"type"`
	ApplianceID string `json:"applianceId"`
	Timestamp   int64  `json:"timestamp"`
	Data        struct {
		Name    string `json:"name"`
		Status  string `json:"status"`
		WebPort int    `json:"webPort"`
		SSL     bool   `json:"ssl"`
		// Custom fields captured separately
	} `json:"data"`
}

// Component represents an installable software component
type Component struct {
	ID           string   `json:"id"`
	Name         string   `json:"name"`
	Description  string   `json:"description,omitempty"`
	Type         string   `json:"type"` // "github-deb", "github-zip"
	Repo         string   `json:"repo,omitempty"`
	Package      string   `json:"package,omitempty"`
	Service      string   `json:"service,omitempty"`
	InstallPath  string   `json:"installPath,omitempty"`
	VersionCmd   []string `json:"versionCmd,omitempty"`
	VersionFile  string   `json:"versionFile,omitempty"`
	PostInstall  []string `json:"postInstall,omitempty"`
}

// ComponentStatus represents the current state of a component
type ComponentStatus struct {
	Component      Component `json:"component"`
	Installed      bool      `json:"installed"`
	CurrentVersion string    `json:"currentVersion,omitempty"`
	LatestVersion  string    `json:"latestVersion,omitempty"`
	UpdateAvail    bool      `json:"updateAvailable"`
	Assets         []string  `json:"assets,omitempty"`
}

// Config holds agent configuration
type Config struct {
	ListenAddr     string
	AuthToken      string
	DservService   string
	AllowReboot    bool
	UploadDir      string
	Timeout        time.Duration
	Verbose        bool
	ComponentsFile string
	// Mesh discovery
	MeshEnabled bool
	MeshPort    int  // UDP port for discovery (default 12346)
	MeshTimeout int  // Seconds before peer considered offline (default 6)
	MeshSSL     bool // Use HTTPS for mesh API (default false, auto-detect)
}

// Agent is the main management agent
type Agent struct {
	cfg        Config
	clients    map[*WSConn]bool
	mu         sync.RWMutex
	http       *http.Client
	components []Component
	// Mesh discovery
	meshPeers  map[string]*MeshPeer
	meshMu     sync.RWMutex
}

// WSConn is a minimal WebSocket connection
type WSConn struct {
	conn   net.Conn
	br     *bufio.Reader
	mu     sync.Mutex
	closed bool
}

// Data structures
type WSMessage struct {
	Type      string          `json:"type"`
	ID        string          `json:"id,omitempty"`
	Action    string          `json:"action,omitempty"`
	Component string          `json:"component,omitempty"`
	Asset     string          `json:"asset,omitempty"`
	Payload   json.RawMessage `json:"payload,omitempty"`
}

type WSResponse struct {
	Type    string      `json:"type"`
	ID      string      `json:"id,omitempty"`
	Success bool        `json:"success"`
	Data    interface{} `json:"data,omitempty"`
	Error   string      `json:"error,omitempty"`
}

type StatusInfo struct {
	Agent  AgentInfo   `json:"agent"`
	Dserv  ServiceInfo `json:"dserv"`
	System SystemInfo  `json:"system"`
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

func main() {
	cfg := Config{DservService: "dserv"}

	flag.StringVar(&cfg.ListenAddr, "listen", ":80", "HTTP listen address")
	flag.StringVar(&cfg.AuthToken, "token", "", "Bearer token for authentication")
	flag.StringVar(&cfg.DservService, "service", "dserv", "systemd service name")
	flag.BoolVar(&cfg.AllowReboot, "allow-reboot", false, "Allow system reboot")
	flag.StringVar(&cfg.UploadDir, "upload-dir", "/tmp/dserv-uploads", "Upload directory")
	flag.DurationVar(&cfg.Timeout, "timeout", 5*time.Minute, "HTTP timeout")
	flag.BoolVar(&cfg.Verbose, "v", false, "Verbose output")
	flag.StringVar(&cfg.ComponentsFile, "components", "/etc/dserv-agent/components.json", "Components config file")
	flag.BoolVar(&cfg.MeshEnabled, "mesh", true, "Enable mesh discovery")
	flag.IntVar(&cfg.MeshPort, "mesh-port", 12346, "UDP port for mesh discovery")
	flag.IntVar(&cfg.MeshTimeout, "mesh-timeout", 6, "Seconds before peer considered offline")
	flag.BoolVar(&cfg.MeshSSL, "mesh-ssl", false, "Use HTTPS for mesh API")
	flag.Parse()

	if cfg.AuthToken == "" {
		cfg.AuthToken = os.Getenv("DSERV_AGENT_TOKEN")
	}

	agent := &Agent{
		cfg:       cfg,
		clients:   make(map[*WSConn]bool),
		http:      &http.Client{Timeout: cfg.Timeout},
		meshPeers: make(map[string]*MeshPeer),
	}

	agent.loadComponents()

	// Start mesh discovery if enabled
	if cfg.MeshEnabled {
		go agent.meshDiscoveryLoop()
		go agent.meshCleanupLoop()
	}

	mux := http.NewServeMux()

	mux.HandleFunc("/api/status", agent.auth(agent.handleStatus))
	mux.HandleFunc("/api/dserv/", agent.auth(agent.handleDserv))
	mux.HandleFunc("/api/logs", agent.auth(agent.handleLogs))
	mux.HandleFunc("/api/system/reboot", agent.auth(agent.handleReboot))
	mux.HandleFunc("/api/upload", agent.auth(agent.handleUpload))
	mux.HandleFunc("/api/files/", agent.auth(agent.handleFiles))
	mux.HandleFunc("/api/components", agent.auth(agent.handleComponents))
	mux.HandleFunc("/api/components/", agent.auth(agent.handleComponentAction))
	mux.HandleFunc("/api/mesh/peers", agent.auth(agent.handleMeshPeers))
	mux.HandleFunc("/ws", agent.handleWebSocket)

	webFS, _ := fs.Sub(webContent, "web")
	mux.Handle("/", http.FileServer(http.FS(webFS)))

	server := &http.Server{Addr: cfg.ListenAddr, Handler: mux}

	go func() {
		c := make(chan os.Signal, 1)
		signal.Notify(c, syscall.SIGINT, syscall.SIGTERM)
		<-c
		log.Println("Shutting down...")
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		server.Shutdown(ctx)
	}()

	log.Printf("dserv-agent %s on %s", version, cfg.ListenAddr)
	if cfg.AuthToken == "" {
		log.Println("WARNING: No auth token set")
	}
	log.Printf("Loaded %d components", len(agent.components))
	if cfg.MeshEnabled {
		log.Printf("Mesh discovery enabled on UDP port %d", cfg.MeshPort)
	}

	if err := server.ListenAndServe(); err != http.ErrServerClosed {
		log.Fatal(err)
	}
}

func (a *Agent) loadComponents() {
	data, err := os.ReadFile(a.cfg.ComponentsFile)
	if err == nil {
		var cfg struct {
			Components []Component `json:"components"`
		}
		if json.Unmarshal(data, &cfg) == nil {
			a.components = cfg.Components
			return
		}
	}

	// Default component
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

func (a *Agent) log(format string, args ...interface{}) {
	if a.cfg.Verbose {
		log.Printf(format, args...)
	}
}

func writeJSON(w http.ResponseWriter, code int, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(v)
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
		Dserv:  a.getDservStatus(),
		System: a.getSystemInfo(),
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

// GET /api/logs?service=dserv&lines=50
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

	// Use journalctl to get logs
	cmd := exec.Command("journalctl", "-u", service, "-n", lines, "--no-pager", "-o", "short-iso")
	out, err := cmd.CombinedOutput()
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error(), "output": string(out)})
		return
	}

	writeJSON(w, 200, map[string]interface{}{
		"service": service,
		"lines":   lines,
		"logs":    string(out),
	})
}

// ============ Component Management ============

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
		if asset == "" {
			var body struct {
				Asset string `json:"asset"`
			}
			json.NewDecoder(r.Body).Decode(&body)
			asset = body.Asset
		}
		go a.installComponent(*comp, asset)
		writeJSON(w, 202, map[string]string{"status": "started", "component": comp.ID})

	default:
		http.Error(w, "Unknown action", 400)
	}
}

func (a *Agent) getComponentStatus(comp Component) ComponentStatus {
	status := ComponentStatus{Component: comp}
	status.CurrentVersion = a.getInstalledVersion(comp)
	status.Installed = status.CurrentVersion != ""

	if strings.HasPrefix(comp.Type, "github-") && comp.Repo != "" {
		release := a.getLatestRelease(comp.Repo)
		if release != nil {
			status.LatestVersion = release.TagName
			arch := runtime.GOARCH
			if arch == "arm" {
				arch = "armhf"
			}

			// Collect matching assets, scoring them by relevance
			type scoredAsset struct {
				name  string
				score int
			}
			var scored []scoredAsset

			for _, asset := range release.Assets {
				name := asset.Name
				nameLower := strings.ToLower(name)

				// Only include debs or zips
				isDeb := strings.HasSuffix(nameLower, ".deb")
				isZip := strings.HasSuffix(nameLower, ".zip")
				if !isDeb && !isZip {
					continue
				}

				// Check architecture - be strict about filtering
				hasAmd64 := strings.Contains(nameLower, "amd64") || strings.Contains(nameLower, "x86_64")
				hasArm64 := strings.Contains(nameLower, "arm64") || strings.Contains(nameLower, "aarch64")
				hasArmhf := strings.Contains(nameLower, "armhf") || strings.Contains(nameLower, "armv7")
				hasArch := hasAmd64 || hasArm64 || hasArmhf

				// If asset specifies an arch, it must match ours
				if hasArch {
					matchesArch := false
					switch arch {
					case "amd64":
						matchesArch = hasAmd64
					case "arm64":
						matchesArch = hasArm64
					case "armhf":
						matchesArch = hasArmhf
					}
					if !matchesArch {
						continue // Wrong architecture, skip
					}
				}

				score := 0
				compID := strings.ToLower(comp.ID)
				compPkg := strings.ToLower(comp.Package)

				// For deb packages, strongly prefer matching package name
				if isDeb && compPkg != "" {
					// Exact match: dserv_0.36.1_arm64.deb for package "dserv"
					if strings.HasPrefix(nameLower, compPkg+"_") {
						score += 200
					} else if strings.HasPrefix(nameLower, compPkg+"-") && !strings.Contains(nameLower, compPkg+"-camera") {
						score += 150
					} else if strings.Contains(nameLower, compPkg) {
						// Contains but doesn't start with - likely a sub-package
						score -= 50
					}
				}

				// Match component ID
				if strings.HasPrefix(nameLower, compID+"_") {
					score += 100
				} else if strings.HasPrefix(nameLower, compID+"-") {
					score += 50
				}

				// Penalize things that are clearly different packages
				if compID == "dserv" || compPkg == "dserv" {
					if strings.Contains(nameLower, "camera") {
						score -= 100
					}
					if strings.Contains(nameLower, "essgui") {
						score -= 100
					}
					if strings.Contains(nameLower, "dlsh") {
						score -= 100
					}
				}

				// Prefer .deb for deb components, .zip for zip components
				if strings.HasSuffix(comp.Type, "-deb") && isDeb {
					score += 50
				} else if strings.HasSuffix(comp.Type, "-zip") && isZip {
					score += 50
				} else if strings.HasSuffix(comp.Type, "-deb") && isZip {
					score -= 200 // Wrong type entirely
				} else if strings.HasSuffix(comp.Type, "-zip") && isDeb {
					score -= 200
				}

				scored = append(scored, scoredAsset{name, score})
			}

			// Sort by score descending
			for i := 0; i < len(scored)-1; i++ {
				for j := i + 1; j < len(scored); j++ {
					if scored[j].score > scored[i].score {
						scored[i], scored[j] = scored[j], scored[i]
					}
				}
			}

			// Extract sorted names
			for _, sa := range scored {
				status.Assets = append(status.Assets, sa.name)
			}

			status.UpdateAvail = status.Installed && status.CurrentVersion != status.LatestVersion
		}
	}
	return status
}

func (a *Agent) getInstalledVersion(comp Component) string {
	// Try version command first (e.g., essctrl for running service)
	if len(comp.VersionCmd) > 0 {
		out, err := exec.Command(comp.VersionCmd[0], comp.VersionCmd[1:]...).Output()
		if err == nil && len(strings.TrimSpace(string(out))) > 0 {
			return strings.TrimSpace(string(out))
		}
		// Command failed - service might be down, fall through to other methods
	}

	// Try dpkg for deb packages (works even if service is down)
	if comp.Package != "" {
		out, err := exec.Command("dpkg-query", "-W", "-f=${Version}", comp.Package).Output()
		if err == nil && len(strings.TrimSpace(string(out))) > 0 {
			return strings.TrimSpace(string(out))
		}
	}

	// Try version file (for zip installs)
	if comp.VersionFile != "" {
		data, err := os.ReadFile(comp.VersionFile)
		if err == nil && len(strings.TrimSpace(string(data))) > 0 {
			return strings.TrimSpace(string(data))
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
	if json.NewDecoder(resp.Body).Decode(&release) != nil {
		return nil
	}
	return &release
}

func (a *Agent) installComponent(comp Component, selectedAsset string) {
	a.broadcast(WSResponse{
		Type: "install_progress",
		Data: map[string]string{"component": comp.ID, "stage": "checking"},
	})

	release := a.getLatestRelease(comp.Repo)
	if release == nil {
		a.broadcast(WSResponse{Type: "install_error", Error: "Failed to get release info"})
		return
	}

	a.broadcast(WSResponse{
		Type: "install_progress",
		Data: map[string]string{"component": comp.ID, "stage": "found", "version": release.TagName},
	})

	var assetURL, assetName string
	for _, asset := range release.Assets {
		if selectedAsset != "" && asset.Name == selectedAsset {
			assetURL = asset.DownloadURL
			assetName = asset.Name
			break
		}
	}

	if assetURL == "" {
		a.broadcast(WSResponse{Type: "install_error", Error: "Asset not found: " + selectedAsset})
		return
	}

	a.broadcast(WSResponse{
		Type: "install_progress",
		Data: map[string]string{"component": comp.ID, "stage": "downloading", "asset": assetName},
	})

	resp, err := a.http.Get(assetURL)
	if err != nil {
		a.broadcast(WSResponse{Type: "install_error", Error: err.Error()})
		return
	}
	defer resp.Body.Close()

	tmpPath := filepath.Join(os.TempDir(), assetName)
	f, err := os.Create(tmpPath)
	if err != nil {
		a.broadcast(WSResponse{Type: "install_error", Error: err.Error()})
		return
	}
	io.Copy(f, resp.Body)
	f.Close()

	if comp.Service != "" {
		a.broadcast(WSResponse{
			Type: "install_progress",
			Data: map[string]string{"component": comp.ID, "stage": "stopping"},
		})
		exec.Command("sudo", "systemctl", "stop", comp.Service).Run()
		time.Sleep(2 * time.Second)
	}

	a.broadcast(WSResponse{
		Type: "install_progress",
		Data: map[string]string{"component": comp.ID, "stage": "installing"},
	})

	var installErr error
	if strings.HasSuffix(assetName, ".deb") {
		installErr = a.installDeb(tmpPath)
	} else if strings.HasSuffix(assetName, ".zip") {
		installErr = a.installZip(tmpPath, comp.InstallPath)
	} else {
		installErr = fmt.Errorf("unknown file type: %s", assetName)
	}

	if installErr != nil {
		a.broadcast(WSResponse{Type: "install_error", Error: installErr.Error()})
		if comp.Service != "" {
			exec.Command("sudo", "systemctl", "start", comp.Service).Run()
		}
		return
	}

	for _, cmd := range comp.PostInstall {
		exec.Command("sudo", "sh", "-c", cmd).Run()
	}

	if comp.Service != "" {
		a.broadcast(WSResponse{
			Type: "install_progress",
			Data: map[string]string{"component": comp.ID, "stage": "starting"},
		})
		exec.Command("sudo", "systemctl", "start", comp.Service).Run()
		time.Sleep(2 * time.Second)
	}

	os.Remove(tmpPath)

	newVersion := a.getInstalledVersion(comp)
	a.broadcast(WSResponse{
		Type:    "install_complete",
		Success: true,
		Data:    map[string]interface{}{"component": comp.ID, "version": newVersion},
	})
}

func (a *Agent) installDeb(path string) error {
	out, err := exec.Command("sudo", "dpkg", "-i", path).CombinedOutput()
	if err != nil {
		fixOut, fixErr := exec.Command("sudo", "apt-get", "install", "-f", "-y").CombinedOutput()
		if fixErr != nil {
			return fmt.Errorf("dpkg: %s\napt fix: %s", out, fixOut)
		}
	}
	return nil
}

func (a *Agent) installZip(zipPath, destPath string) error {
	if destPath == "" {
		return fmt.Errorf("no install path for zip")
	}
	os.MkdirAll(destPath, 0755)

	r, err := zip.OpenReader(zipPath)
	if err != nil {
		return err
	}
	defer r.Close()

	for _, f := range r.File {
		fpath := filepath.Join(destPath, f.Name)
		if !strings.HasPrefix(fpath, filepath.Clean(destPath)+string(os.PathSeparator)) {
			continue
		}
		if f.FileInfo().IsDir() {
			os.MkdirAll(fpath, 0755)
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
		_, err = io.Copy(outFile, rc)
		outFile.Close()
		rc.Close()
		if err != nil {
			return err
		}
	}
	return nil
}

func (a *Agent) handleReboot(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		http.Error(w, "Method not allowed", 405)
		return
	}
	if !a.cfg.AllowReboot {
		writeJSON(w, 403, map[string]string{"error": "Reboot not allowed"})
		return
	}
	delay := 5
	fmt.Sscanf(r.URL.Query().Get("delay"), "%d", &delay)
	writeJSON(w, 202, map[string]interface{}{"status": "scheduled", "delay": delay})
	go func() {
		time.Sleep(time.Duration(delay) * time.Second)
		exec.Command("sudo", "reboot").Run()
	}()
}

func (a *Agent) handleUpload(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		http.Error(w, "Method not allowed", 405)
		return
	}
	os.MkdirAll(a.cfg.UploadDir, 0755)
	r.ParseMultipartForm(32 << 20)
	file, header, err := r.FormFile("file")
	if err != nil {
		writeJSON(w, 400, map[string]string{"error": "No file"})
		return
	}
	defer file.Close()
	dest := filepath.Join(a.cfg.UploadDir, header.Filename)
	if p := r.FormValue("path"); p != "" {
		dest = p
	}
	out, err := os.Create(dest)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	n, _ := io.Copy(out, file)
	out.Close()
	writeJSON(w, 200, map[string]interface{}{"path": dest, "size": n})
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
		go a.installComponent(*comp, msg.Asset)
		resp.Success = true
		resp.Data = map[string]string{"status": "started", "component": comp.ID}

	case "mesh_peers":
		resp.Success = true
		resp.Data = a.getMeshPeers()

	case "logs":
		service := msg.Action // reuse action field for service name
		if service == "" {
			service = a.cfg.DservService
		}
		lines := "100"
		cmd := exec.Command("journalctl", "-u", service, "-n", lines, "--no-pager", "-o", "short-iso")
		out, err := cmd.CombinedOutput()
		if err != nil {
			resp.Error = err.Error()
		} else {
			resp.Success = true
			resp.Data = map[string]interface{}{
				"service": service,
				"logs":    string(out),
			}
		}

	default:
		resp.Error = "Unknown type"
	}
	return resp
}

func (a *Agent) broadcast(v interface{}) {
	a.mu.RLock()
	defer a.mu.RUnlock()
	for ws := range a.clients {
		ws.WriteJSON(v)
	}
}

// ============ WebSocket frame handling ============

func (ws *WSConn) ReadMessage() ([]byte, error) {
	header := make([]byte, 2)
	if _, err := io.ReadFull(ws.br, header); err != nil {
		return nil, err
	}
	fin := header[0]&0x80 != 0
	opcode := header[0] & 0x0f
	masked := header[1]&0x80 != 0
	length := int64(header[1] & 0x7f)

	if opcode == wsOpClose {
		return nil, io.EOF
	}
	if opcode == wsOpPing {
		if length > 0 {
			data := make([]byte, length)
			io.ReadFull(ws.br, data)
		}
		ws.writeFrame(wsOpPong, nil)
		return ws.ReadMessage()
	}
	if length == 126 {
		ext := make([]byte, 2)
		io.ReadFull(ws.br, ext)
		length = int64(binary.BigEndian.Uint16(ext))
	} else if length == 127 {
		ext := make([]byte, 8)
		io.ReadFull(ws.br, ext)
		length = int64(binary.BigEndian.Uint64(ext))
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
	if !fin {
		next, err := ws.ReadMessage()
		if err != nil {
			return nil, err
		}
		return append(data, next...), nil
	}
	return data, nil
}

func (ws *WSConn) WriteJSON(v interface{}) error {
	data, err := json.Marshal(v)
	if err != nil {
		return err
	}
	return ws.writeFrame(wsOpText, data)
}

func (ws *WSConn) writeFrame(opcode byte, data []byte) error {
	ws.mu.Lock()
	defer ws.mu.Unlock()
	if ws.closed {
		return io.EOF
	}
	length := len(data)
	header := make([]byte, 2, 10)
	header[0] = 0x80 | opcode
	if length < 126 {
		header[1] = byte(length)
	} else if length < 65536 {
		header[1] = 126
		header = append(header, byte(length>>8), byte(length))
	} else {
		header[1] = 127
		header = append(header, make([]byte, 8)...)
		binary.BigEndian.PutUint64(header[2:], uint64(length))
	}
	if _, err := ws.conn.Write(header); err != nil {
		return err
	}
	if len(data) > 0 {
		if _, err := ws.conn.Write(data); err != nil {
			return err
		}
	}
	return nil
}

func (ws *WSConn) Close() error {
	ws.mu.Lock()
	defer ws.mu.Unlock()
	if ws.closed {
		return nil
	}
	ws.closed = true
	ws.writeFrame(wsOpClose, nil)
	return ws.conn.Close()
}

// ============ Mesh Discovery ============

func (a *Agent) meshDiscoveryLoop() {
	// Poll local dserv's mesh endpoint for peer data
	// This avoids UDP port conflict since dserv owns the mesh discovery
	ticker := time.NewTicker(2 * time.Second)
	defer ticker.Stop()

	// dserv mesh server on port 2569 - try HTTP first, fallback to HTTPS
	// Once we find one that works, stick with it
	meshURL := "http://localhost:2569/api/peers"
	if a.cfg.MeshSSL {
		meshURL = "https://localhost:2569/api/peers"
	}
	triedHTTPS := a.cfg.MeshSSL

	for range ticker.C {
		ok := a.fetchMeshPeers(meshURL)
		// If HTTP failed and we haven't tried HTTPS yet, switch to HTTPS
		if !ok && !triedHTTPS {
			meshURL = "https://localhost:2569/api/peers"
			triedHTTPS = true
			if a.cfg.Verbose {
				log.Println("Mesh: trying HTTPS")
			}
		}
	}
}

func (a *Agent) fetchMeshPeers(url string) bool {
	// Create client that accepts self-signed certs for local mesh API
	tr := &http.Transport{
		TLSClientConfig: &tls.Config{InsecureSkipVerify: true},
	}
	client := &http.Client{Timeout: 2 * time.Second, Transport: tr}
	resp, err := client.Get(url)
	if err != nil {
		// dserv not running or mesh not available - clear peers
		a.meshMu.Lock()
		if len(a.meshPeers) > 0 {
			a.meshPeers = make(map[string]*MeshPeer)
			a.meshMu.Unlock()
			a.broadcastMeshUpdate()
		} else {
			a.meshMu.Unlock()
		}
		return false
	}
	defer resp.Body.Close()

	if resp.StatusCode != 200 {
		return false
	}

	// dserv returns {"appliances": [...]}
	var wrapper struct {
		Appliances []struct {
			ApplianceID string `json:"applianceId"`
			Name        string `json:"name"`
			Status      string `json:"status"`
			IPAddress   string `json:"ipAddress"`
			WebPort     int    `json:"webPort"`
			SSL         bool   `json:"ssl"`
			IsLocal     bool   `json:"isLocal"`
			// Custom fields come as additional JSON fields
		} `json:"appliances"`
	}
	
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return false
	}
	
	if err := json.Unmarshal(body, &wrapper); err != nil {
		if a.cfg.Verbose {
			log.Printf("Mesh: failed to parse peers: %v", err)
		}
		return false
	}

	// Also parse raw to get custom fields
	var raw struct {
		Appliances []map[string]interface{} `json:"appliances"`
	}
	json.Unmarshal(body, &raw)

	// Update peer map
	a.meshMu.Lock()
	newPeers := make(map[string]*MeshPeer)
	
	for i, app := range wrapper.Appliances {
		p := &MeshPeer{
			ApplianceID:   app.ApplianceID,
			Name:          app.Name,
			Status:        app.Status,
			IPAddress:     app.IPAddress,
			WebPort:       app.WebPort,
			SSL:           app.SSL,
			IsLocal:       app.IsLocal,
			LastHeartbeat: time.Now(),
			CustomFields:  make(map[string]string),
		}
		
		// Extract custom fields from raw data
		if i < len(raw.Appliances) {
			for k, v := range raw.Appliances[i] {
				switch k {
				case "applianceId", "name", "status", "ipAddress", "webPort", "ssl", "isLocal":
					continue
				default:
					if str, ok := v.(string); ok {
						p.CustomFields[k] = str
					}
				}
			}
		}
		
		newPeers[app.ApplianceID] = p
	}
	
	changed := len(newPeers) != len(a.meshPeers)
	if !changed {
		for id, p := range newPeers {
			if existing, ok := a.meshPeers[id]; !ok || existing.Status != p.Status {
				changed = true
				break
			}
		}
	}
	
	a.meshPeers = newPeers
	a.meshMu.Unlock()

	if changed {
		if a.cfg.Verbose {
			log.Printf("Mesh: updated, %d peers", len(newPeers))
		}
		a.broadcastMeshUpdate()
	}
	return true
}

func (a *Agent) meshCleanupLoop() {
	// No longer needed - dserv handles peer timeouts
	// Keeping function stub for compatibility
}

func (a *Agent) getMeshPeers() []MeshPeer {
	a.meshMu.RLock()
	defer a.meshMu.RUnlock()

	peers := make([]MeshPeer, 0, len(a.meshPeers))
	for _, p := range a.meshPeers {
		peers = append(peers, *p)
	}

	// Sort by name for consistent ordering
	for i := 0; i < len(peers)-1; i++ {
		for j := i + 1; j < len(peers); j++ {
			if peers[i].Name > peers[j].Name {
				peers[i], peers[j] = peers[j], peers[i]
			}
		}
	}

	return peers
}

func (a *Agent) broadcastMeshUpdate() {
	peers := a.getMeshPeers()
	a.broadcast(WSResponse{
		Type:    "mesh_update",
		Success: true,
		Data:    peers,
	})
}

// GET /api/mesh/peers
func (a *Agent) handleMeshPeers(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" {
		http.Error(w, "Method not allowed", 405)
		return
	}
	writeJSON(w, 200, a.getMeshPeers())
}