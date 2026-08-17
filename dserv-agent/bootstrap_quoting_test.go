package main

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The served bootstrap embeds the component list as a shell variable. Component
// fields are free text: dserv-camera's detectCmd carries a grep pattern quoted
// with single quotes, and when the template supplied the surrounding quotes
// itself that inner quote closed the assignment mid-JSON. Every /setup?profile=
// script then died at the assignment line with "syntax error near unexpected
// token `('" -- which meant no box could be provisioned or retyped at all,
// from any profile, while the endpoint still returned a plausible-looking 200.
//
// Render the real template with a quote-bearing payload and let bash judge it.
func TestBootstrapScriptParsesWithQuotesInComponents(t *testing.T) {
	bash, err := exec.LookPath("bash")
	if err != nil {
		t.Skip("bash not available")
	}

	// The exact shape that broke it, plus a quote in a description and an
	// apostrophe in prose -- the other free-text fields a human edits.
	comps, err := json.Marshal(map[string]any{"components": []map[string]any{{
		"id":          "dserv-camera",
		"description": "CSI camera capture subsystem (the box's own sensor)",
		"detectCmd":   []string{"sh", "-c", "cat /sys/bus/i2c/devices/*/name 2>/dev/null | grep -qiE '^(imx|ov|arducam)'"},
	}}})
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}

	var b strings.Builder
	cfg := BootstrapConfig{
		ServerURL:        "https://dserv.net",
		DefaultWG:        "test-wg",
		Version:          "test",
		ProfileName:      "incage",
		ComponentsJSON:   shellQuote(string(comps)),
		AgentReleaseJSON: shellQuote(`{"tag":"0.0.0","assets":[]}`),
	}
	if err := bootstrapTmpl.Execute(&b, cfg); err != nil {
		t.Fatalf("template execute: %v", err)
	}

	path := filepath.Join(t.TempDir(), "setup.sh")
	if err := os.WriteFile(path, []byte(b.String()), 0o644); err != nil {
		t.Fatalf("write: %v", err)
	}
	if out, err := exec.Command(bash, "-n", path).CombinedOutput(); err != nil {
		t.Fatalf("served bootstrap is not valid bash: %v\n%s", err, out)
	}

	// Parsing is necessary but not sufficient: a mangled quote could still
	// leave COMPONENTS_JSON holding something that is no longer the JSON.
	// Source the script's variable block and compare it back.
	extract := `set -e; eval "$(sed -n '/^COMPONENTS_JSON=/p' ` + path + `)"; printf %s "$COMPONENTS_JSON"`
	out, err := exec.Command(bash, "-c", extract).Output()
	if err != nil {
		t.Fatalf("could not read COMPONENTS_JSON back: %v", err)
	}
	if string(out) != string(comps) {
		t.Errorf("COMPONENTS_JSON round-trip mismatch\n got: %s\nwant: %s", out, comps)
	}
}
