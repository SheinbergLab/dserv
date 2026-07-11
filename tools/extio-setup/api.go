// api.go -- the HTTP surface the embedded UI talks to.
package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

type server struct {
	mu    sync.Mutex
	link  *Link
	data  *DataLink // live event stream from the box's data CDC (may be nil)
	fwDir string
}

func newServer(fwDir string) *server { return &server{fwDir: fwDir} }

// shutdown closes both serial links cleanly (releases the OS port claims).
func (s *server) shutdown() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.link != nil {
		s.link.Close()
		s.link = nil
	}
	if s.data != nil {
		s.data.Close()
		s.data = nil
	}
}

func (s *server) routes(mux *http.ServeMux) {
	mux.HandleFunc("GET /api/status", s.handleStatus)
	mux.HandleFunc("GET /api/ports", s.handlePorts)
	mux.HandleFunc("POST /api/connect", s.handleConnect)
	mux.HandleFunc("POST /api/disconnect", s.handleDisconnect)
	mux.HandleFunc("POST /api/exec", s.handleExec)
	mux.HandleFunc("GET /api/dump", s.handleDump)
	mux.HandleFunc("POST /api/apply", s.handleApply)
	mux.HandleFunc("GET /api/console", s.handleConsole)
	mux.HandleFunc("POST /api/consolewrite", s.handleConsoleWrite)
	mux.HandleFunc("GET /api/events", s.handleEvents)
	mux.HandleFunc("GET /api/firmware", s.handleFirmware)
	mux.HandleFunc("POST /api/flash", s.handleFlash)
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(v)
}

func httpErr(w http.ResponseWriter, code int, format string, a ...any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(map[string]string{"error": fmt.Sprintf(format, a...)})
}

// current returns the live link, clearing it first if the reader tore it
// down (unplug) so status flips to disconnected on its own.
func (s *server) current() *Link {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.link != nil && s.link.isClosed() {
		s.link = nil
		if s.data != nil {
			s.data.Close()
			s.data = nil
		}
	}
	return s.link
}

func (s *server) currentData() *DataLink {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.data != nil && s.data.isClosed() {
		s.data = nil
	}
	return s.data
}

func (s *server) handleStatus(w http.ResponseWriter, r *http.Request) {
	l := s.current()
	d := s.currentData()
	st := map[string]any{"version": version, "connected": l != nil, "firmwareDir": s.fwDir,
		"data": d != nil}
	if l != nil {
		st["port"] = l.name
	}
	if d != nil {
		frames, bad, drops, bytes, skip := d.Stats()
		st["dataStats"] = map[string]uint64{"frames": frames, "bad": bad, "drops": drops,
			"bytes": bytes, "skip": skip}
	}
	writeJSON(w, st)
}

func (s *server) handlePorts(w http.ResponseWriter, r *http.Request) {
	ports, err := listBoxPorts()
	if err != nil {
		httpErr(w, 500, "enumerate: %v", err)
		return
	}
	if ports == nil {
		ports = []BoxPort{}
	}
	writeJSON(w, ports)
}

func (s *server) handleConnect(w http.ResponseWriter, r *http.Request) {
	var req struct{ Port string }
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Port == "" {
		httpErr(w, 400, "need {\"port\": ...}")
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.link != nil && !s.link.isClosed() {
		s.link.Close()
	}
	if s.data != nil {
		s.data.Close()
		s.data = nil
	}
	l, err := openLink(req.Port)
	if err != nil {
		s.link = nil
		httpErr(w, 500, "open %s: %v", req.Port, err)
		return
	}
	s.link = l
	// Best-effort: open the data sibling for the live event stream. One
	// retry covers transient claims; a persistent failure is REPORTED, not
	// swallowed -- a missing event stream must be visible, not mysterious.
	var dataErr string
	if ports, perr := listBoxPorts(); perr == nil {
		for _, bp := range ports {
			if bp.Console == req.Port && bp.Data != "" {
				d, derr := openDataLink(bp.Data)
				if derr != nil {
					time.Sleep(500 * time.Millisecond)
					d, derr = openDataLink(bp.Data)
				}
				if derr == nil {
					s.data = d
				} else {
					dataErr = fmt.Sprintf("%s: %v (live events unavailable; box reboot or replug clears a stale claim)", bp.Data, derr)
				}
				break
			}
		}
	}
	resp := map[string]any{"connected": true, "port": req.Port, "data": s.data != nil}
	if dataErr != "" {
		resp["dataError"] = dataErr
	}
	writeJSON(w, resp)
}

func (s *server) handleDisconnect(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	if s.link != nil {
		s.link.Close()
		s.link = nil
	}
	if s.data != nil {
		s.data.Close()
		s.data = nil
	}
	s.mu.Unlock()
	writeJSON(w, map[string]any{"connected": false})
}

func (s *server) handleExec(w http.ResponseWriter, r *http.Request) {
	var req struct{ Cmd string }
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || strings.TrimSpace(req.Cmd) == "" {
		httpErr(w, 400, "need {\"cmd\": ...}")
		return
	}
	l := s.current()
	if l == nil {
		httpErr(w, 409, "not connected")
		return
	}
	lines, err := l.Exec(strings.TrimSpace(req.Cmd), 300*time.Millisecond, 3*time.Second)
	if err != nil {
		httpErr(w, 502, "%v", err)
		return
	}
	writeJSON(w, map[string]any{"lines": lines})
}

func (s *server) handleDump(w http.ResponseWriter, r *http.Request) {
	l := s.current()
	if l == nil {
		httpErr(w, 409, "not connected")
		return
	}
	// dump is multi-line with no OK terminator; rely on the quiet window.
	lines, err := l.Exec("dump", 400*time.Millisecond, 6*time.Second)
	if err != nil {
		httpErr(w, 502, "%v", err)
		return
	}
	writeJSON(w, map[string]any{"lines": lines})
}

// handleApply streams a profile (dump-format lines) to the box one command
// at a time, checking each response. `newName` overrides any `name` line so
// cloning box A's profile onto box B doesn't duplicate its identity.
func (s *server) handleApply(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Lines   []string
		NewName string
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || len(req.Lines) == 0 {
		httpErr(w, 400, "need {\"lines\": [...]}")
		return
	}
	l := s.current()
	if l == nil {
		httpErr(w, 409, "not connected")
		return
	}
	type result struct {
		Cmd  string `json:"cmd"`
		Resp string `json:"resp"`
		OK   bool   `json:"ok"`
	}
	var results []result
	sawName := false
	for _, raw := range req.Lines {
		cmd := strings.TrimSpace(raw)
		if cmd == "" || strings.HasPrefix(cmd, "#") {
			continue
		}
		if strings.HasPrefix(cmd, "name ") && req.NewName != "" {
			cmd = "name " + req.NewName
			sawName = true
		}
		lines, err := l.Exec(cmd, 300*time.Millisecond, 3*time.Second)
		resp := strings.Join(lines, " / ")
		ok := err == nil && !strings.HasPrefix(resp, "ERR")
		if err != nil {
			resp = err.Error()
		}
		results = append(results, result{Cmd: cmd, Resp: resp, OK: ok})
		if !ok {
			break // stop at the first failure: a partial profile is visible, not silent
		}
	}
	if req.NewName != "" && !sawName {
		lines, err := l.Exec("name "+req.NewName, 300*time.Millisecond, 3*time.Second)
		results = append(results, result{Cmd: "name " + req.NewName,
			Resp: strings.Join(lines, " / "), OK: err == nil})
	}
	writeJSON(w, map[string]any{"results": results})
}

// handleConsole streams console lines as server-sent events, starting with
// a replay of the recent ring so a fresh tab shows context.
func (s *server) handleConsole(w http.ResponseWriter, r *http.Request) {
	l := s.current()
	if l == nil {
		httpErr(w, 409, "not connected")
		return
	}
	fl, ok := w.(http.Flusher)
	if !ok {
		httpErr(w, 500, "streaming unsupported")
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	ch, replay := l.Subscribe()
	defer l.Unsubscribe(ch)
	for _, s := range replay {
		fmt.Fprintf(w, "data: %s\n\n", s)
	}
	fl.Flush()
	for {
		select {
		case line, open := <-ch:
			if !open {
				return // link closed (disconnect or unplug)
			}
			fmt.Fprintf(w, "data: %s\n\n", line)
			fl.Flush()
		case <-r.Context().Done():
			return
		}
	}
}

// handleEvents streams decoded data-CDC frames (DI edges, DO readbacks,
// groups, heartbeat) as JSON server-sent events.
func (s *server) handleEvents(w http.ResponseWriter, r *http.Request) {
	d := s.currentData()
	if d == nil {
		httpErr(w, 409, "no data interface open")
		return
	}
	fl, ok := w.(http.Flusher)
	if !ok {
		httpErr(w, 500, "streaming unsupported")
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	ch, snap := d.Subscribe()
	defer d.Unsubscribe(ch)
	for _, ev := range snap { // repaint current pin state before live events
		if b, err := json.Marshal(ev); err == nil {
			fmt.Fprintf(w, "data: %s\n\n", b)
		}
	}
	fl.Flush()
	for {
		select {
		case ev, open := <-ch:
			if !open {
				return
			}
			b, err := json.Marshal(ev)
			if err != nil {
				continue
			}
			fmt.Fprintf(w, "data: %s\n\n", b)
			fl.Flush()
		case <-r.Context().Done():
			return
		}
	}
}

func (s *server) handleConsoleWrite(w http.ResponseWriter, r *http.Request) {
	var req struct{ Data string }
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		httpErr(w, 400, "need {\"data\": ...}")
		return
	}
	l := s.current()
	if l == nil {
		httpErr(w, 409, "not connected")
		return
	}
	if err := l.WriteRaw([]byte(req.Data)); err != nil {
		httpErr(w, 502, "%v", err)
		return
	}
	writeJSON(w, map[string]any{"ok": true})
}

func (s *server) handleFirmware(w http.ResponseWriter, r *http.Request) {
	type fwFile struct {
		Name  string    `json:"name"`
		Size  int64     `json:"size"`
		Mtime time.Time `json:"mtime"`
	}
	files := []fwFile{}
	if s.fwDir != "" {
		matches, _ := filepath.Glob(filepath.Join(s.fwDir, "*.uf2"))
		for _, m := range matches {
			if fi, err := os.Stat(m); err == nil {
				files = append(files, fwFile{Name: filepath.Base(m), Size: fi.Size(), Mtime: fi.ModTime()})
			}
		}
	}
	writeJSON(w, map[string]any{"dir": s.fwDir, "files": files})
}

func (s *server) handleFlash(w http.ResponseWriter, r *http.Request) {
	var req struct{ File string }
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.File == "" {
		httpErr(w, 400, "need {\"file\": ...}")
		return
	}
	if s.fwDir == "" {
		httpErr(w, 409, "no firmware directory configured (-fw)")
		return
	}
	// Basename only: the UI picks from our own listing, nothing else.
	path := filepath.Join(s.fwDir, filepath.Base(req.File))
	if _, err := os.Stat(path); err != nil {
		httpErr(w, 404, "%s: %v", req.File, err)
		return
	}
	steps, err := s.flash(path)
	if err != nil {
		writeJSON(w, map[string]any{"ok": false, "steps": steps, "error": err.Error()})
		return
	}
	writeJSON(w, map[string]any{"ok": true, "steps": steps})
}
