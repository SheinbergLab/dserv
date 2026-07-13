package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
)

// fakeShelf serves the two shelf endpoints fetchVerifiedImage uses: the
// version manifest and the raw artifact. `manifestSHA` is what the manifest
// advertises; `body` is what the file endpoint actually serves -- pass a
// mismatch to exercise the tamper/corruption guard.
func fakeShelf(t *testing.T, channel, version, file string, body []byte, manifestSHA string) *httptest.Server {
	t.Helper()
	mux := http.NewServeMux()
	mux.HandleFunc("/api/firmware/extio/"+channel+"/"+version, func(w http.ResponseWriter, r *http.Request) {
		json.NewEncoder(w).Encode(map[string]any{
			"family": "extio", "channel": channel, "version": version,
			"images": []map[string]any{{"build": "dual", "file": file, "sha256": manifestSHA}},
		})
	})
	mux.HandleFunc("/firmware/extio/"+channel+"/"+version+"/"+file, func(w http.ResponseWriter, r *http.Request) {
		w.Write(body)
	})
	return httptest.NewServer(mux)
}

func TestFetchVerifiedImageOK(t *testing.T) {
	body := []byte("a genuine uf2 payload")
	sum := sha256.Sum256(body)
	ts := fakeShelf(t, "dev", "v1", "wizchip_dserv_config_dual.uf2", body, hex.EncodeToString(sum[:]))
	defer ts.Close()
	s := &server{shelfURL: ts.URL}

	path, err := s.fetchVerifiedImage("dev", "v1", "wizchip_dserv_config_dual.uf2")
	if err != nil {
		t.Fatalf("fetchVerifiedImage: %v", err)
	}
	defer os.Remove(path)
	got, _ := os.ReadFile(path)
	if string(got) != string(body) {
		t.Fatalf("downloaded bytes differ")
	}
}

func TestFetchVerifiedImageRejectsMismatch(t *testing.T) {
	// Manifest advertises the sha of "good", but the file endpoint serves
	// "TAMPERED" -- fetchVerifiedImage must refuse and leave nothing behind.
	goodSum := sha256.Sum256([]byte("good"))
	ts := fakeShelf(t, "dev", "v1", "img.uf2", []byte("TAMPERED"), hex.EncodeToString(goodSum[:]))
	defer ts.Close()
	s := &server{shelfURL: ts.URL}

	path, err := s.fetchVerifiedImage("dev", "v1", "img.uf2")
	if err == nil {
		os.Remove(path)
		t.Fatal("expected sha mismatch error, got nil")
	}
	if !strings.Contains(err.Error(), "mismatch") {
		t.Fatalf("expected mismatch error, got %v", err)
	}
	if path != "" {
		t.Fatalf("no file should be returned on mismatch, got %q", path)
	}
}

func TestShelfNameGuards(t *testing.T) {
	bad := []string{"", "../etc", "a/b", "a b", "x?y", "has..dots"}
	for _, b := range bad {
		if shelfNameOK(b) {
			t.Errorf("shelfNameOK(%q) should be false", b)
		}
	}
	for _, g := range []string{"dev", "extio-fw-v3", "0.47.15-8-g8098ece"} {
		if !shelfNameOK(g) {
			t.Errorf("shelfNameOK(%q) should be true", g)
		}
	}
	if shelfFileOK("evil.sh") {
		t.Error("shelfFileOK must reject non-uf2/bin")
	}
	if !shelfFileOK("wizchip_dserv_config_dual.uf2") {
		t.Error("shelfFileOK should accept a .uf2")
	}
}

// TestFetchVerifiedImageProxyErrors: a 404 from the shelf surfaces as an error,
// never a silent empty file.
func TestFetchVerifiedImageManifestMissing(t *testing.T) {
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Error(w, "nope", 404)
	}))
	defer ts.Close()
	s := &server{shelfURL: ts.URL}
	if _, err := s.fetchVerifiedImage("dev", "v1", "img.uf2"); err == nil {
		t.Fatal(fmt.Sprintf("expected error on 404 manifest"))
	}
}
