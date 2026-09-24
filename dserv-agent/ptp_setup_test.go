package main

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// runPTPSetupDry renders the installer and runs it with DSERV_PTP_DESTDIR, the
// dry-run mode that writes files and touches nothing else.
func runPTPSetupDry(t *testing.T, script, destdir string) string {
	t.Helper()
	bash, err := exec.LookPath("bash")
	if err != nil {
		t.Skip("bash not available")
	}
	path := filepath.Join(t.TempDir(), "ptp-setup.sh")
	if err := os.WriteFile(path, []byte(script), 0o644); err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(bash, path)
	cmd.Env = append(os.Environ(), "DSERV_PTP_DESTDIR="+destdir)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("installer failed: %v\n%s", err, out)
	}
	return string(out)
}

// The installer must land every file byte-identical to its canonical source
// (so nothing in the script's own placeholder substitution leaked into a
// payload), record an id and a manifest that verifies, and rewrite nothing on a
// second run -- the property the restart logic depends on.
func TestPTPSetupDryRunInstallsAndRecords(t *testing.T) {
	files, err := ptpPayloads()
	if err != nil {
		t.Fatal(err)
	}
	script := renderPTPSetup(files, "https://registry.example", "v9.9.9")
	for _, ph := range []string{"__BASE__", "__TOOLING_ID__", "__AGENT__", "__INFO__", "__SUMS__"} {
		if strings.Contains(script, ph) {
			t.Errorf("rendered script still contains placeholder %s", ph)
		}
	}

	dest := t.TempDir()
	out := runPTPSetupDry(t, script, dest)

	canonical := map[string]string{
		"dserv-ptp-setup":               "../scripts/dserv-ptp-setup",
		"dserv-ptp-select-phc":          "../scripts/dserv-ptp-select-phc",
		"dserv-ptp4l@.service":          "../systemd/dserv-ptp4l@.service",
		"dserv-phc2sys@.service":        "../systemd/dserv-phc2sys@.service",
		"dserv-ptp4l-client@.service":   "../systemd/dserv-ptp4l-client@.service",
		"dserv-phc2sys-client@.service": "../systemd/dserv-phc2sys-client@.service",
		"chrony-grandmaster.conf":       "../systemd/chrony-grandmaster.conf",
	}
	for _, f := range files {
		got, err := os.ReadFile(filepath.Join(dest, f.dest))
		if err != nil {
			t.Errorf("%s not installed: %v", f.dest, err)
			continue
		}
		if string(got) != f.content {
			t.Errorf("%s differs from the embedded payload", f.dest)
		}
		if src, err := os.ReadFile(canonical[f.embedName]); err == nil && string(src) != string(got) {
			t.Errorf("%s differs from canonical %s", f.dest, canonical[f.embedName])
		}
		st, _ := os.Stat(filepath.Join(dest, f.dest))
		if want := map[string]os.FileMode{"0755": 0o755, "0644": 0o644}[f.mode]; st != nil && st.Mode().Perm() != want {
			t.Errorf("%s mode %v, want %s", f.dest, st.Mode().Perm(), f.mode)
		}
	}

	id := ptpToolingID(files)
	info, err := os.ReadFile(filepath.Join(dest, ptpToolingInfo))
	if err != nil {
		t.Fatalf("no %s: %v", ptpToolingInfo, err)
	}
	for _, want := range []string{"id=" + id, "source=https://registry.example/ptp/setup", "agent=v9.9.9", "applied="} {
		if !strings.Contains(string(info), want) {
			t.Errorf("info missing %q:\n%s", want, info)
		}
	}

	// The manifest names the REAL target paths (not DESTDIR ones) with the
	// sha256 of what was written -- what `sha256sum -c` checks on the box.
	sums, err := os.ReadFile(filepath.Join(dest, ptpToolingSums))
	if err != nil {
		t.Fatalf("no %s: %v", ptpToolingSums, err)
	}
	lines := strings.Split(strings.TrimSpace(string(sums)), "\n")
	if len(lines) != len(files) {
		t.Fatalf("manifest has %d lines, want %d", len(lines), len(files))
	}
	for _, ln := range lines {
		parts := strings.SplitN(ln, "  ", 2)
		if len(parts) != 2 {
			t.Fatalf("bad manifest line %q", ln)
		}
		b, err := os.ReadFile(filepath.Join(dest, parts[1]))
		if err != nil {
			t.Errorf("manifest names %s, not installed", parts[1])
			continue
		}
		sum := sha256.Sum256(b)
		if hex.EncodeToString(sum[:]) != parts[0] {
			t.Errorf("manifest hash for %s does not match the installed file", parts[1])
		}
	}
	if !strings.Contains(out, "PTP tooling "+id+" installed") {
		t.Errorf("first run should report an install:\n%s", out)
	}

	// Second run over the same tree: nothing rewritten.
	out2 := runPTPSetupDry(t, script, dest)
	if strings.Contains(out2, "  updated ") || strings.Contains(out2, "  installed ") {
		t.Errorf("second run rewrote files:\n%s", out2)
	}
	if !strings.Contains(out2, "already current") {
		t.Errorf("second run should say already current:\n%s", out2)
	}

	// A changed file on disk is put back and reported as updated.
	sel := filepath.Join(dest, "/usr/local/dserv/scripts/dserv-ptp-select-phc")
	if err := os.WriteFile(sel, []byte("#!/bin/sh\nexit 0\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	out3 := runPTPSetupDry(t, script, dest)
	if !strings.Contains(out3, "  updated    /usr/local/dserv/scripts/dserv-ptp-select-phc") {
		t.Errorf("a drifted file should be reported updated:\n%s", out3)
	}
}

// Same files, same id; any byte changed, a different id.
func TestPTPToolingIDTracksContent(t *testing.T) {
	files, err := ptpPayloads()
	if err != nil {
		t.Fatal(err)
	}
	a := ptpToolingID(files)
	if b := ptpToolingID(files); a != b {
		t.Fatalf("id not stable: %s vs %s", a, b)
	}
	changed := append([]ptpPayload(nil), files...)
	changed[0].content += "#\n"
	if c := ptpToolingID(changed); c == a {
		t.Fatalf("id did not change with content")
	}
	if len(a) != 12 {
		t.Fatalf("id %q, want 12 hex digits", a)
	}
}
