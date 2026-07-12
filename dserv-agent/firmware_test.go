package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"testing"
)

// newFwAgent returns an agent whose firmware shelf is rooted in a temp dir.
func newFwAgent(t *testing.T) *Agent {
	t.Helper()
	return &Agent{cfg: Config{FirmwareDir: t.TempDir(), ServerMode: true}}
}

// publish drives one multipart publish through the real handler and returns
// the recorded response. `build` is the build.sh target name (the unique key).
func publish(t *testing.T, a *Agent, channel, version, build string, dirty bool, uf2 []byte) *httptest.ResponseRecorder {
	t.Helper()
	return publishAuth(t, a, channel, version, build, dirty, uf2, "")
}

// publishAuth is publish with an optional Bearer token on the request.
func publishAuth(t *testing.T, a *Agent, channel, version, build string, dirty bool, uf2 []byte, token string) *httptest.ResponseRecorder {
	t.Helper()
	var body bytes.Buffer
	mw := multipart.NewWriter(&body)
	_ = mw.WriteField("version", version)
	_ = mw.WriteField("build", build)
	_ = mw.WriteField("board", "pico2")
	_ = mw.WriteField("variant", build)
	if dirty {
		_ = mw.WriteField("dirty", "1")
	}
	fw, err := mw.CreateFormFile("uf2", "wizchip_dserv_config_"+build+".uf2")
	if err != nil {
		t.Fatal(err)
	}
	fw.Write(uf2)
	mw.Close()

	req := httptest.NewRequest(http.MethodPost, "/api/firmware/extio/"+channel, &body)
	req.Header.Set("Content-Type", mw.FormDataContentType())
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	rr := httptest.NewRecorder()
	a.handleFirmwareAPI(rr, req)
	return rr
}

func TestFirmwarePublishAndRead(t *testing.T) {
	a := newFwAgent(t)
	uf2 := []byte("fake uf2 image bytes")
	want := sha256.Sum256(uf2)
	wantHex := hex.EncodeToString(want[:])

	rr := publish(t, a, "dev", "0.48.0-1-gabc", "dual", false, uf2)
	if rr.Code != 200 {
		t.Fatalf("publish: got %d, body %s", rr.Code, rr.Body.String())
	}

	// The version manifest must carry the server-computed sha256.
	m, err := a.readManifest("dev", "0.48.0-1-gabc")
	if err != nil {
		t.Fatalf("readManifest: %v", err)
	}
	if len(m.Images) != 1 || m.Images[0].Build != "dual" {
		t.Fatalf("images = %+v", m.Images)
	}
	if m.Images[0].SHA256 != wantHex {
		t.Fatalf("sha256 = %s, want %s", m.Images[0].SHA256, wantHex)
	}
	if a.readLatest("dev") != "0.48.0-1-gabc" {
		t.Fatalf("latest = %q", a.readLatest("dev"))
	}

	// A second target merges into the same version manifest.
	if rr := publish(t, a, "dev", "0.48.0-1-gabc", "usb", false, []byte("usb image")); rr.Code != 200 {
		t.Fatalf("second target: %d %s", rr.Code, rr.Body.String())
	}
	m, _ = a.readManifest("dev", "0.48.0-1-gabc")
	if len(m.Images) != 2 {
		t.Fatalf("expected 2 images, got %d", len(m.Images))
	}

	// The artifact is downloadable and byte-identical.
	dl := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodGet, "/firmware/extio/dev/0.48.0-1-gabc/wizchip_dserv_config_dual.uf2", nil)
	a.handleFirmwareArtifact(dl, req)
	if dl.Code != 200 || !bytes.Equal(dl.Body.Bytes(), uf2) {
		t.Fatalf("artifact download: code %d, len %d", dl.Code, dl.Body.Len())
	}
}

func TestFirmwareImmutabilityAndDirty(t *testing.T) {
	a := newFwAgent(t)

	// Dev channel accepts a dirty build and allows overwriting a target.
	if rr := publish(t, a, "dev", "v1", "usb", true, []byte("a")); rr.Code != 200 {
		t.Fatalf("dev dirty: %d %s", rr.Code, rr.Body.String())
	}
	if rr := publish(t, a, "dev", "v1", "usb", false, []byte("b")); rr.Code != 200 {
		t.Fatalf("dev overwrite: %d %s", rr.Code, rr.Body.String())
	}

	// Stable rejects a dirty build outright...
	if rr := publish(t, a, "stable", "extio-fw-v1", "usb", true, []byte("x")); rr.Code != 400 {
		t.Fatalf("stable dirty should 400, got %d", rr.Code)
	}
	// ...accepts a clean one once...
	if rr := publish(t, a, "stable", "extio-fw-v1", "usb", false, []byte("x")); rr.Code != 200 {
		t.Fatalf("stable clean: %d %s", rr.Code, rr.Body.String())
	}
	// ...and refuses to overwrite the same target (immutable).
	if rr := publish(t, a, "stable", "extio-fw-v1", "usb", false, []byte("y")); rr.Code != 409 {
		t.Fatalf("stable re-publish should 409, got %d %s", rr.Code, rr.Body.String())
	}
}

func TestFirmwarePathTraversalGuard(t *testing.T) {
	a := newFwAgent(t)
	// A crafted version segment must not escape the root.
	req := httptest.NewRequest(http.MethodGet, "/firmware/extio/dev/..%2f..%2fetc/passwd", nil)
	rr := httptest.NewRecorder()
	a.handleFirmwareArtifact(rr, req)
	if rr.Code != 404 {
		t.Fatalf("traversal should 404, got %d", rr.Code)
	}
}

func TestFirmwarePublishToken(t *testing.T) {
	a := newFwAgent(t)
	a.cfg.FirmwareToken = "s3cret"
	img := []byte("img")

	// Read side must stay open even with a publish token set.
	rr := httptest.NewRecorder()
	a.handleFirmwareAPI(rr, httptest.NewRequest(http.MethodGet, "/api/firmware/extio", nil))
	if rr.Code != 200 {
		t.Fatalf("read should stay open, got %d", rr.Code)
	}

	// No token / wrong token -> 401; correct token -> 200.
	if rr := publishAuth(t, a, "dev", "v1", "usb", false, img, ""); rr.Code != 401 {
		t.Fatalf("no token should 401, got %d", rr.Code)
	}
	if rr := publishAuth(t, a, "dev", "v1", "usb", false, img, "wrong"); rr.Code != 401 {
		t.Fatalf("wrong token should 401, got %d", rr.Code)
	}
	if rr := publishAuth(t, a, "dev", "v1", "usb", false, img, "s3cret"); rr.Code != 200 {
		t.Fatalf("correct token should 200, got %d %s", rr.Code, rr.Body.String())
	}
}

func TestFirmwareListAll(t *testing.T) {
	a := newFwAgent(t)
	publish(t, a, "dev", "v1", "usb", false, []byte("a"))
	publish(t, a, "stable", "extio-fw-v1", "usb", false, []byte("b"))

	rr := httptest.NewRecorder()
	a.handleFirmwareAPI(rr, httptest.NewRequest(http.MethodGet, "/api/firmware/extio", nil))
	if rr.Code != 200 {
		t.Fatalf("list all: %d", rr.Code)
	}
	var resp struct {
		Family   string                 `json:"family"`
		Channels map[string]any `json:"channels"`
	}
	if err := json.Unmarshal(rr.Body.Bytes(), &resp); err != nil {
		t.Fatal(err)
	}
	if resp.Family != "extio" || len(resp.Channels) != 2 {
		t.Fatalf("channels = %+v", resp.Channels)
	}
}
