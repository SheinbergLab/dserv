// shelf.go -- the "pull from dserv.net" consumer.
//
// The dserv-agent firmware shelf (dserv-agent/firmware.go) houses versioned
// .uf2 images per channel. This driver lets the tool list that shelf and flash
// a chosen image: fetch by URL, VERIFY the sha256 against the shelf's manifest
// (never flash an image whose bytes don't match what was published), stage it
// to a temp file, then hand it to the existing BOOTSEL flash path (flash.go).
//
// The browser never talks to the shelf directly -- these endpoints proxy it,
// so there's no cross-origin dance and the sha check happens server-side where
// it can't be skipped.
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"time"
)

// shelfImage / shelfManifest mirror the agent's manifest contract
// (dserv-agent/firmware.go). Only the fields the consumer needs.
type shelfImage struct {
	Build      string `json:"build"`
	Board      string `json:"board"`
	Variant    string `json:"variant"`
	OtaCapable bool   `json:"otaCapable"`
	File       string `json:"file"`
	Size       int64  `json:"size"`
	SHA256     string `json:"sha256"`
}

type shelfManifest struct {
	Family      string       `json:"family"`
	Channel     string       `json:"channel"`
	Version     string       `json:"version"`
	Dirty       bool         `json:"dirty"`
	PublishedAt string       `json:"publishedAt"`
	Images      []shelfImage `json:"images"`
}

// shelfClient is a bounded HTTP client for shelf calls; a hung dserv.net must
// not wedge the tool.
var shelfClient = &http.Client{Timeout: 30 * time.Second}

// handleShelf proxies the shelf listing so the UI can render available images.
//
//	GET /api/shelf            -> all channels ({family, channels})
//	GET /api/shelf?channel=dev-> one channel  ({family, channel, latest, versions})
func (s *server) handleShelf(w http.ResponseWriter, r *http.Request) {
	if s.shelfURL == "" {
		httpErr(w, 409, "no firmware shelf configured (-shelf)")
		return
	}
	path := "/api/firmware/extio"
	if ch := strings.TrimSpace(r.URL.Query().Get("channel")); ch != "" {
		if !shelfNameOK(ch) {
			httpErr(w, 400, "bad channel")
			return
		}
		path += "/" + ch
	}
	resp, err := shelfClient.Get(s.shelfURL + path)
	if err != nil {
		httpErr(w, 502, "shelf unreachable: %v", err)
		return
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 4<<20))
	if resp.StatusCode != 200 {
		httpErr(w, resp.StatusCode, "shelf: %s", strings.TrimSpace(string(body)))
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Write(body)
}

// handleShelfFlash downloads a chosen image, verifies its sha256 against the
// shelf manifest, and flashes it. Body: {channel, version, file}.
func (s *server) handleShelfFlash(w http.ResponseWriter, r *http.Request) {
	if s.shelfURL == "" {
		httpErr(w, 409, "no firmware shelf configured (-shelf)")
		return
	}
	var req struct{ Channel, Version, File string }
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil ||
		req.Channel == "" || req.Version == "" || req.File == "" {
		httpErr(w, 400, "need {channel, version, file}")
		return
	}
	if !shelfNameOK(req.Channel) || !shelfNameOK(req.Version) || !shelfFileOK(req.File) {
		httpErr(w, 400, "bad channel/version/file")
		return
	}

	tmp, err := s.fetchVerifiedImage(req.Channel, req.Version, req.File)
	if err != nil {
		httpErr(w, 502, "%v", err)
		return
	}
	defer os.Remove(tmp)

	steps := []string{fmt.Sprintf("fetched %s/%s/%s (sha256 ok)", req.Channel, req.Version, req.File)}
	flashSteps, ferr := s.flash(tmp)
	steps = append(steps, flashSteps...)
	if ferr != nil {
		writeJSON(w, map[string]any{"ok": false, "steps": steps, "error": ferr.Error()})
		return
	}
	writeJSON(w, map[string]any{"ok": true, "steps": steps})
}

// fetchVerifiedImage downloads an image and confirms its bytes match the
// sha256 the shelf manifest published. On ANY failure -- unreachable, HTTP
// error, or sha mismatch -- it returns an error and no usable file, so a
// corrupted or tampered image is never handed to the flasher. The caller owns
// removing the returned temp file.
func (s *server) fetchVerifiedImage(channel, version, file string) (string, error) {
	// Authoritative sha comes from the manifest, not the client.
	wantSHA, err := s.shelfImageSHA(channel, version, file)
	if err != nil {
		return "", err
	}
	tmp, gotSHA, err := s.downloadShelfImage(channel, version, file)
	if err != nil {
		return "", fmt.Errorf("download: %w", err)
	}
	if !strings.EqualFold(gotSHA, wantSHA) {
		os.Remove(tmp)
		return "", fmt.Errorf("sha256 mismatch (got %s, manifest %s) -- NOT flashing", gotSHA, wantSHA)
	}
	return tmp, nil
}

// shelfImageSHA fetches the version manifest and returns the published sha256
// for the named file.
func (s *server) shelfImageSHA(channel, version, file string) (string, error) {
	url := fmt.Sprintf("%s/api/firmware/extio/%s/%s", s.shelfURL, channel, version)
	resp, err := shelfClient.Get(url)
	if err != nil {
		return "", fmt.Errorf("shelf unreachable: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return "", fmt.Errorf("shelf manifest %s/%s: HTTP %d", channel, version, resp.StatusCode)
	}
	var m shelfManifest
	if err := json.NewDecoder(resp.Body).Decode(&m); err != nil {
		return "", fmt.Errorf("manifest decode: %w", err)
	}
	for _, img := range m.Images {
		if img.File == file {
			if img.SHA256 == "" {
				return "", fmt.Errorf("manifest has no sha256 for %s", file)
			}
			return img.SHA256, nil
		}
	}
	return "", fmt.Errorf("%s not in %s/%s manifest", file, channel, version)
}

// downloadShelfImage streams the artifact to a temp file, returning its path
// and computed sha256.
func (s *server) downloadShelfImage(channel, version, file string) (path, sum string, err error) {
	url := fmt.Sprintf("%s/firmware/extio/%s/%s/%s", s.shelfURL, channel, version, file)
	resp, err := shelfClient.Get(url)
	if err != nil {
		return "", "", err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return "", "", fmt.Errorf("HTTP %d fetching %s", resp.StatusCode, file)
	}
	f, err := os.CreateTemp("", "extio-*.uf2")
	if err != nil {
		return "", "", err
	}
	h := sha256.New()
	if _, err := io.Copy(io.MultiWriter(f, h), resp.Body); err != nil {
		f.Close()
		os.Remove(f.Name())
		return "", "", err
	}
	f.Close()
	return f.Name(), hex.EncodeToString(h.Sum(nil)), nil
}

// shelfNameOK / shelfFileOK guard the path segments we interpolate into shelf
// URLs (channel, version, file) against traversal and injection.
func shelfNameOK(s string) bool {
	if s == "" || strings.ContainsAny(s, "/\\ ?#") || strings.Contains(s, "..") {
		return false
	}
	return true
}

func shelfFileOK(s string) bool {
	return shelfNameOK(s) && (strings.HasSuffix(s, ".uf2") || strings.HasSuffix(s, ".bin"))
}
