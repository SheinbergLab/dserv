// firmware.go -- the extio firmware shelf (server/registry mode).
//
// The agent on dserv.net houses versioned firmware images for the extio box
// (wiznet-io/) so a fleet of bench tools (tools/extio-setup) and, later, the
// on-box A/B updater (wiznet-io/OTA.md) all pull the SAME artifact from one
// place instead of hand-copying .uf2 files around. This file is the shelf:
// the manifest contract, on-disk layout, read endpoints, and an
// authenticated publish endpoint.
//
// Design mirrors releases.go (managed, allow-listed, read side open so a
// fresh box needs no token) and handleUpload/handleFiles (multipart in, path
// traversal guarded).
//
// On-disk layout (rooted at cfg.FirmwareDir):
//
//	<root>/extio/<channel>/<version>/<file>.uf2   # the image(s)
//	<root>/extio/<channel>/<version>/<file>.bin   # optional flat slot image
//	<root>/extio/<channel>/<version>/manifest.json
//	<root>/extio/<channel>/latest.json            # {"version": "..."} pointer
//
// One version can carry several target images (usb, dual, eth) -- publish is
// called once per target uf2 and merges its entry into the version manifest.
//
// Channels: "dev" is mutable (build.sh --push overwrites) and may be built
// from a dirty tree; every other channel (e.g. "stable", "extio-fw-vN") is
// immutable -- a version's target can be published once, and a -dirty build
// is refused. This encodes the publish discipline that a dirty demo image
// once shipped to a box under.
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
)

// fwFamily is the only artifact family the shelf serves today. The path space
// is /<family>/... so a second family (a different board) can be added later
// without reshaping the API.
const fwFamily = "extio"

// manifestName / latestName are the reserved filenames within a channel; they
// can never be a version dir or an uploaded artifact.
const (
	manifestName = "manifest.json"
	latestName   = "latest.json"
)

// Name validators: guard every path segment before it is joined onto the
// firmware root, so a crafted channel/version/target/file can't escape it.
var (
	fwChannelRe = regexp.MustCompile(`^[a-z0-9][a-z0-9._-]*$`)
	fwVersionRe = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]*$`)
	fwBuildRe   = regexp.MustCompile(`^[a-z0-9][a-z0-9_-]*$`) // build.sh target name (unique key)
	fwIdentRe   = regexp.MustCompile(`^[a-z0-9][a-z0-9_-]*$`) // board / variant tokens
	fwFileRe    = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]*\.(uf2|bin)$`)
)

// FirmwareImage is one build's artifact within a version. sha256 is the
// contract the puller verifies before flashing; bin is the optional flat
// per-slot image the on-box updater fetches.
//
// Three identity fields, because the box's build matrix has two independent
// axes plus a compatibility key (see dserv-agent/README.md):
//
//   - Build   = the build.sh target name -- the UNIQUE machine key. The
//     on-box updater fetches the image whose Build == its own baked build,
//     so it never strays onto another variant.
//   - Board   = PICO_BOARD -- the HARD compatibility filter. Bench flashing an
//     unknown board, and the on-box updater, both refuse a Board mismatch
//     (e.g. a pimoroni image must never land on a sparkfun board).
//   - Variant = BOX_TARGET -- the descriptive role (usb|dual|w6300|pico2w).
//     Not unique on its own (the three WiFi builds all share pico2w), which is
//     exactly why Build is the key.
type FirmwareImage struct {
	Build      string `json:"build"`             // build.sh target: usb|dual|w6300|pico2w|picoplus2w|thingplus
	Board      string `json:"board,omitempty"`   // PICO_BOARD compatibility key: pico2|pico2_w|pimoroni_pico_plus2_w_rp2350|…
	Variant    string `json:"variant,omitempty"` // BOX_TARGET role: usb|dual|w6300|pico2w
	OtaCapable bool   `json:"otaCapable"`         // image carries a working on-box A/B updater (WiFi builds: false today)
	File       string `json:"file"`              // .uf2 filename within the version dir
	Size       int64  `json:"size"`
	SHA256     string `json:"sha256"`
	Bin        string `json:"bin,omitempty"` // optional flat .bin filename (on-box updater)
	BinSize    int64  `json:"binSize,omitempty"`
	BinSHA256  string `json:"binSha256,omitempty"`
}

// FirmwareManifest describes one published version and all its target images.
// version = `git describe --match 'extio-fw-*'` on the box source tree.
type FirmwareManifest struct {
	Family      string          `json:"family"`  // always "extio" today
	Channel     string          `json:"channel"` // dev | stable | extio-fw-vN
	Version     string          `json:"version"`
	Dirty       bool            `json:"dirty,omitempty"` // built from a dirty tree (dev only)
	Notes       string          `json:"notes,omitempty"`
	PublishedAt string          `json:"publishedAt"` // RFC3339
	Images      []FirmwareImage `json:"images"`
}

// firmwareRoot is the extio family directory; "" if no firmware dir is set.
func (a *Agent) firmwareRoot() string {
	if a.cfg.FirmwareDir == "" {
		return ""
	}
	return filepath.Join(a.cfg.FirmwareDir, fwFamily)
}

// registerFirmwareHandlers wires the shelf into server mode. Read side is
// open (fresh boxes / bench tools need bare access, same as /api/releases);
// publish is behind the agent's auth token.
func (a *Agent) registerFirmwareHandlers(mux *http.ServeMux) {
	mux.HandleFunc("/api/firmware/", a.handleFirmwareAPI)
	mux.HandleFunc("/firmware/", a.handleFirmwareArtifact)
}

// ---- read side ----

// handleFirmwareAPI serves the manifest surface:
//
//	GET  /api/firmware/extio                     -> every channel + its versions
//	GET  /api/firmware/extio/<channel>           -> one channel (latest + versions)
//	GET  /api/firmware/extio/<channel>/<version> -> one version manifest
//	POST /api/firmware/extio/<channel>           -> publish (authenticated)
func (a *Agent) handleFirmwareAPI(w http.ResponseWriter, r *http.Request) {
	root := a.firmwareRoot()
	if root == "" {
		writeJSON(w, 503, map[string]string{"error": "firmware shelf not configured (--firmware-dir)"})
		return
	}
	rest := strings.TrimPrefix(r.URL.Path, "/api/firmware/")
	parts := splitPath(rest)
	if len(parts) == 0 || parts[0] != fwFamily {
		writeJSON(w, 404, map[string]string{"error": "unknown firmware family"})
		return
	}
	parts = parts[1:] // drop family

	if r.Method == http.MethodPost {
		if len(parts) != 1 {
			writeJSON(w, 400, map[string]string{"error": "publish to /api/firmware/extio/<channel>"})
			return
		}
		a.auth(func(w http.ResponseWriter, r *http.Request) {
			a.handleFirmwarePublish(w, r, parts[0])
		})(w, r)
		return
	}
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", 405)
		return
	}

	switch len(parts) {
	case 0:
		a.firmwareListAll(w)
	case 1:
		a.firmwareListChannel(w, parts[0])
	case 2:
		a.firmwareGetVersion(w, parts[0], parts[1])
	default:
		writeJSON(w, 404, map[string]string{"error": "not found"})
	}
}

func (a *Agent) firmwareListAll(w http.ResponseWriter) {
	root := a.firmwareRoot()
	entries, _ := os.ReadDir(root)
	channels := map[string]any{}
	for _, e := range entries {
		if !e.IsDir() || !fwChannelRe.MatchString(e.Name()) {
			continue
		}
		latest, versions := a.readChannel(e.Name())
		channels[e.Name()] = map[string]any{"latest": latest, "versions": versions}
	}
	writeJSON(w, 200, map[string]any{"family": fwFamily, "channels": channels})
}

func (a *Agent) firmwareListChannel(w http.ResponseWriter, channel string) {
	if !fwChannelRe.MatchString(channel) {
		writeJSON(w, 400, map[string]string{"error": "invalid channel"})
		return
	}
	if _, err := os.Stat(filepath.Join(a.firmwareRoot(), channel)); err != nil {
		writeJSON(w, 404, map[string]string{"error": "unknown channel"})
		return
	}
	latest, versions := a.readChannel(channel)
	writeJSON(w, 200, map[string]any{"family": fwFamily, "channel": channel,
		"latest": latest, "versions": versions})
}

func (a *Agent) firmwareGetVersion(w http.ResponseWriter, channel, version string) {
	if !fwChannelRe.MatchString(channel) || !fwVersionRe.MatchString(version) {
		writeJSON(w, 400, map[string]string{"error": "invalid channel or version"})
		return
	}
	m, err := a.readManifest(channel, version)
	if err != nil {
		writeJSON(w, 404, map[string]string{"error": "version not found"})
		return
	}
	writeJSON(w, 200, m)
}

// readChannel returns the channel's latest-version pointer and every version
// manifest it holds, newest publish first.
func (a *Agent) readChannel(channel string) (string, []FirmwareManifest) {
	dir := filepath.Join(a.firmwareRoot(), channel)
	entries, _ := os.ReadDir(dir)
	var versions []FirmwareManifest
	for _, e := range entries {
		if !e.IsDir() || !fwVersionRe.MatchString(e.Name()) {
			continue
		}
		if m, err := a.readManifest(channel, e.Name()); err == nil {
			versions = append(versions, *m)
		}
	}
	sort.Slice(versions, func(i, j int) bool {
		return versions[i].PublishedAt > versions[j].PublishedAt
	})
	return a.readLatest(channel), versions
}

func (a *Agent) readManifest(channel, version string) (*FirmwareManifest, error) {
	path := filepath.Join(a.firmwareRoot(), channel, version, manifestName)
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var m FirmwareManifest
	if err := json.Unmarshal(b, &m); err != nil {
		return nil, err
	}
	return &m, nil
}

func (a *Agent) readLatest(channel string) string {
	b, err := os.ReadFile(filepath.Join(a.firmwareRoot(), channel, latestName))
	if err != nil {
		return ""
	}
	var p struct {
		Version string `json:"version"`
	}
	if json.Unmarshal(b, &p) != nil {
		return ""
	}
	return p.Version
}

// handleFirmwareArtifact serves the raw .uf2/.bin:
//
//	GET /firmware/extio/<channel>/<version>/<file>
//
// Every segment is validated, then the resolved path is confirmed to still
// live under the firmware root before it is served.
func (a *Agent) handleFirmwareArtifact(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", 405)
		return
	}
	root := a.firmwareRoot()
	if root == "" {
		http.Error(w, "firmware shelf not configured", 503)
		return
	}
	parts := splitPath(strings.TrimPrefix(r.URL.Path, "/firmware/"))
	if len(parts) != 4 || parts[0] != fwFamily ||
		!fwChannelRe.MatchString(parts[1]) || !fwVersionRe.MatchString(parts[2]) ||
		!fwFileRe.MatchString(parts[3]) {
		http.Error(w, "Not found", 404)
		return
	}
	path := filepath.Join(root, parts[1], parts[2], parts[3])
	// Defense in depth: the regexes already forbid "..", but confirm the
	// cleaned path is still inside the root before opening it.
	if rel, err := filepath.Rel(root, path); err != nil || strings.HasPrefix(rel, "..") {
		http.Error(w, "Not found", 404)
		return
	}
	if _, err := os.Stat(path); err != nil {
		http.Error(w, "Not found", 404)
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Disposition", "attachment; filename="+parts[3])
	http.ServeFile(w, r, path)
}

// ---- publish side ----

// handleFirmwarePublish stores one target's artifact into a version and
// upserts its manifest entry. Multipart form:
//
//	fields: version (req), target (req), board, dirty (bool), notes
//	files:  uf2 (req), bin (optional flat slot image)
//
// Called once per target by the publisher (build.sh --push / dservctl fw
// push). sha256 is computed here, not trusted from the client.
func (a *Agent) handleFirmwarePublish(w http.ResponseWriter, r *http.Request, channel string) {
	if !fwChannelRe.MatchString(channel) {
		writeJSON(w, 400, map[string]string{"error": "invalid channel"})
		return
	}
	if err := r.ParseMultipartForm(64 << 20); err != nil {
		writeJSON(w, 400, map[string]string{"error": "multipart form required: " + err.Error()})
		return
	}
	version := strings.TrimSpace(r.FormValue("version"))
	build := strings.TrimSpace(r.FormValue("build"))
	board := strings.TrimSpace(r.FormValue("board"))
	variant := strings.TrimSpace(r.FormValue("variant"))
	notes := strings.TrimSpace(r.FormValue("notes"))
	dirty := boolForm(r.FormValue("dirty"))
	ota := boolForm(r.FormValue("ota"))

	if !fwVersionRe.MatchString(version) {
		writeJSON(w, 400, map[string]string{"error": "invalid or missing version"})
		return
	}
	if !fwBuildRe.MatchString(build) {
		writeJSON(w, 400, map[string]string{"error": "invalid or missing build (build.sh target name)"})
		return
	}
	// board/variant are optional but recommended -- validate if present.
	if board != "" && !fwIdentRe.MatchString(board) {
		writeJSON(w, 400, map[string]string{"error": "invalid board"})
		return
	}
	if variant != "" && !fwIdentRe.MatchString(variant) {
		writeJSON(w, 400, map[string]string{"error": "invalid variant"})
		return
	}
	// Publish discipline: only the mutable dev channel accepts dirty builds
	// or overwrites; everything else is immutable.
	mutable := channel == "dev"
	if dirty && !mutable {
		writeJSON(w, 400, map[string]string{"error": "refusing -dirty build on immutable channel " + channel})
		return
	}

	versionDir := filepath.Join(a.firmwareRoot(), channel, version)
	existing, _ := a.readManifest(channel, version)
	if existing != nil && !mutable {
		for _, img := range existing.Images {
			if img.Build == build {
				writeJSON(w, 409, map[string]string{
					"error": fmt.Sprintf("%s/%s build %s already published (immutable)", channel, version, build)})
				return
			}
		}
	}
	if err := os.MkdirAll(versionDir, 0755); err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}

	// Required uf2.
	uf2Name, uf2Size, uf2Sum, err := saveFirmwareUpload(r, "uf2", versionDir, "uf2")
	if err != nil {
		writeJSON(w, 400, map[string]string{"error": "uf2: " + err.Error()})
		return
	}
	img := FirmwareImage{Build: build, Board: board, Variant: variant, OtaCapable: ota,
		File: uf2Name, Size: uf2Size, SHA256: uf2Sum}

	// Optional flat .bin (on-box updater fetches this; absent today is fine).
	if binName, binSize, binSum, berr := saveFirmwareUpload(r, "bin", versionDir, "bin"); berr == nil {
		img.Bin, img.BinSize, img.BinSHA256 = binName, binSize, binSum
	} else if berr != errNoUpload {
		writeJSON(w, 400, map[string]string{"error": "bin: " + berr.Error()})
		return
	}

	// Upsert this target into the version manifest.
	m := existing
	if m == nil {
		m = &FirmwareManifest{Family: fwFamily, Channel: channel, Version: version}
	}
	m.Dirty = dirty
	if notes != "" {
		m.Notes = notes
	}
	m.PublishedAt = time.Now().UTC().Format(time.RFC3339)
	replaced := false
	for i := range m.Images {
		if m.Images[i].Build == build {
			m.Images[i] = img
			replaced = true
			break
		}
	}
	if !replaced {
		m.Images = append(m.Images, img)
	}
	if err := a.writeManifest(m); err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	a.writeLatest(channel, version)

	writeJSON(w, 200, map[string]any{"ok": true, "channel": channel, "version": version,
		"build": build, "sha256": uf2Sum, "manifest": m})
}

func (a *Agent) writeManifest(m *FirmwareManifest) error {
	b, err := json.MarshalIndent(m, "", "  ")
	if err != nil {
		return err
	}
	path := filepath.Join(a.firmwareRoot(), m.Channel, m.Version, manifestName)
	return os.WriteFile(path, b, 0644)
}

func (a *Agent) writeLatest(channel, version string) {
	b, _ := json.Marshal(map[string]string{"version": version})
	os.WriteFile(filepath.Join(a.firmwareRoot(), channel, latestName), b, 0644)
}

// errNoUpload distinguishes "no file was sent for this field" (fine for the
// optional bin) from a real save failure.
var errNoUpload = fmt.Errorf("no upload")

// saveFirmwareUpload streams one multipart file field to versionDir, computing
// its sha256 on the way. ext is the required extension (uf2|bin); the stored
// filename is the client's basename, validated. Returns errNoUpload if the
// field is absent.
func saveFirmwareUpload(r *http.Request, field, versionDir, ext string) (name string, size int64, sum string, err error) {
	file, header, ferr := r.FormFile(field)
	if ferr != nil {
		return "", 0, "", errNoUpload
	}
	defer file.Close()

	base := filepath.Base(header.Filename)
	if !fwFileRe.MatchString(base) || !strings.HasSuffix(base, "."+ext) {
		return "", 0, "", fmt.Errorf("bad filename %q (want *.%s)", header.Filename, ext)
	}
	dst, err := os.Create(filepath.Join(versionDir, base))
	if err != nil {
		return "", 0, "", err
	}
	defer dst.Close()

	h := sha256.New()
	n, err := io.Copy(io.MultiWriter(dst, h), file)
	if err != nil {
		return "", 0, "", err
	}
	return base, n, hex.EncodeToString(h.Sum(nil)), nil
}

// boolForm reads a truthy multipart field ("1"/"true", case-insensitive).
func boolForm(v string) bool {
	return v == "1" || strings.EqualFold(v, "true")
}

// splitPath splits a cleaned URL path into non-empty segments.
func splitPath(p string) []string {
	var out []string
	for _, s := range strings.Split(p, "/") {
		if s != "" {
			out = append(out, s)
		}
	}
	return out
}
