package main

import (
	"encoding/json"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The mirror profile: ?mirror=HOST lands unquoted in the served script, the
// box's services become the mirror's two units in stim2.service's place, and
// a re-run without a rig keeps the one the box already follows.

func TestMirrorRigValidation(t *testing.T) {
	for _, ok := range []string{"192.168.88.40", "officepi.local", "rig-3", "10.0.0.5:2570"} {
		if !mirrorRigRe.MatchString(ok) {
			t.Errorf("%q should be accepted", ok)
		}
	}
	for _, bad := range []string{"", "-rf", "a b", "x;reboot", "$(id)", "`id`", "a\"b", "a'b", ".hidden"} {
		if mirrorRigRe.MatchString(bad) {
			t.Errorf("%q should be refused", bad)
		}
	}
}

func TestSetupRefusesBadMirror(t *testing.T) {
	a := &Agent{}
	for _, bad := range []string{"x%3Breboot", "%24(id)", "a%20b"} {
		rec := httptest.NewRecorder()
		a.handleBootstrap(rec, httptest.NewRequest("GET", "/setup?profile=mirror&mirror="+bad, nil))
		if rec.Code != 400 {
			t.Errorf("?mirror=%s: status %d, want 400", bad, rec.Code)
		}
	}
}

func TestBoxIdentityMirror(t *testing.T) {
	path := filepath.Join(t.TempDir(), "box.conf")
	os.WriteFile(path, []byte("profile=mirror\nstim_mode=mirror\nmirror=192.168.88.40\n"), 0o644)
	id := readBoxIdentity(path)
	if id == nil || id.StimMode != "mirror" || id.Mirror != "192.168.88.40" {
		t.Errorf("readBoxIdentity = %+v", id)
	}
}

func TestMirrorProfile(t *testing.T) {
	var p *BootstrapProfile
	for i := range defaultProfiles {
		if defaultProfiles[i].Name == "mirror" {
			p = &defaultProfiles[i]
		}
	}
	if p == nil {
		t.Fatal("no mirror profile")
	}
	if p.StimMode != "mirror" {
		t.Errorf("mirror profile StimMode = %q", p.StimMode)
	}
	want := map[string]bool{"stim2": true, "dlsh": true, "dlsh-interp": true}
	for _, c := range p.Components {
		delete(want, c)
	}
	if len(want) != 0 {
		t.Errorf("mirror profile lacks %v", want)
	}
}

// renderMirrorScript renders the real template as /setup?profile=mirror would.
func renderMirrorScript(t *testing.T, rig string) string {
	t.Helper()
	comps, _ := json.Marshal(map[string]any{"components": []map[string]any{
		{"id": "dlsh"},
		{"id": "dlsh-interp", "package": "dlsh-interp"},
		{"id": "stim2", "package": "stim2", "service": "stim2"},
		{"id": "dserv-agent", "package": "dserv-agent"},
	}})
	var b strings.Builder
	cfg := BootstrapConfig{
		ServerURL:        "https://dserv.net",
		DefaultWG:        "test-wg",
		Version:          "test",
		ProfileName:      "mirror",
		StimMode:         "mirror",
		MirrorRig:        rig,
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
	return path
}

// runMirrorFunctions runs the script's own variable block and mirror
// functions (not main) under bash, with dpkg answering arch.
func runMirrorFunctions(t *testing.T, script, arch, env, body string) (string, error) {
	t.Helper()
	bash, err := exec.LookPath("bash")
	if err != nil {
		t.Skip("bash not available")
	}
	extract := func(fn string) string {
		out, err := exec.Command("sed", "-n", "/^"+fn+"() {/,/^}/p", script).Output()
		if err != nil || len(out) == 0 {
			t.Fatalf("could not extract %s from the script", fn)
		}
		return string(out)
	}
	vars, _ := exec.Command("sed", "-En",
		`/^(COMPONENTS_JSON|STIM_MODE|STIM_COMPONENT_ID|AGENT_COMPONENT_ID|MIRROR_[A-Z_]*)=/p`, script).Output()
	if !strings.Contains(string(vars), "STIM_MODE=") || !strings.Contains(string(vars), "MIRROR_DEFAULTS=") {
		t.Fatalf("could not extract the script's variables:\n%s", vars)
	}
	prog := `
info() { echo "info: $*"; }; ok() { echo "ok: $*"; }; warn() { echo "warn: $*"; }
fail() { echo "fail: $*"; exit 1; }
dpkg() { echo ` + arch + `; }
` + env + "\n" + string(vars) + extract("profile_services") + extract("check_mirror") + body
	out, err := exec.Command(bash, "-c", prog).CombinedOutput()
	return string(out), err
}

func TestMirrorScript(t *testing.T) {
	if _, err := exec.LookPath("jq"); err != nil {
		t.Skip("jq not available")
	}
	script := renderMirrorScript(t, "192.168.88.40")
	if out, err := exec.Command("bash", "-n", script).CombinedOutput(); err != nil {
		t.Fatalf("served mirror bootstrap is not valid bash: %v\n%s", err, out)
	}

	// The services: the mirror's two units, not stim2.service.
	out, err := runMirrorFunctions(t, script, "amd64", "", `profile_services`)
	if err != nil {
		t.Fatalf("profile_services: %v\n%s", err, out)
	}
	if got := strings.Fields(out); strings.Join(got, " ") != "stim2-mirror-display stim2-mirror" {
		t.Errorf("profile_services = %q, want the mirror's two units only", got)
	}

	// The rig from the URL.
	out, err = runMirrorFunctions(t, script, "amd64", "", `check_mirror; echo "rig=$MIRROR_RIG"`)
	if err != nil || !strings.Contains(out, "rig=192.168.88.40") {
		t.Errorf("check_mirror with ?mirror=: %v\n%s", err, out)
	}

	// The display unit follows the arch: X11 on amd64, cage on arm64, both
	// installed as stim2-mirror-display.service.
	noMali := `MIRROR_MALI_PATHS=` + filepath.Join(t.TempDir(), "no-mali") + `; `
	for arch, want := range map[string]string{
		"amd64": "stim2-mirror-display.service",
		"arm64": "stim2-mirror-display-cage.service",
	} {
		out, err = runMirrorFunctions(t, script, arch, "", noMali+`check_mirror; echo "src=$MIRROR_DISPLAY_SRC"`)
		if err != nil || !strings.Contains(out, "src="+want) {
			t.Errorf("check_mirror on %s: want src=%s: %v\n%s", arch, want, err, out)
		}
	}

	// arm64 on a vendor Mali blob runs stim2 under Weston: no mirror unit yet.
	mali := filepath.Join(t.TempDir(), "mali0")
	os.WriteFile(mali, nil, 0o644)
	out, err = runMirrorFunctions(t, script, "arm64", "", `MIRROR_MALI_PATHS=`+mali+`; check_mirror`)
	if err == nil || !strings.Contains(out, "Weston") {
		t.Errorf("check_mirror on a Mali board should fail: %v\n%s", err, out)
	}

	// Any other arch: refused before anything is installed.
	out, err = runMirrorFunctions(t, script, "armhf", "", noMali+`check_mirror`)
	if err == nil || !strings.Contains(out, "armhf") {
		t.Errorf("check_mirror on armhf should fail: %v\n%s", err, out)
	}

	// A re-run with no rig keeps the box's own, from box.conf ...
	bare := renderMirrorScript(t, "")
	dir := t.TempDir()
	conf := filepath.Join(dir, "box.conf")
	os.WriteFile(conf, []byte("profile=mirror\nstim_mode=mirror\nmirror=officepi.local\n"), 0o644)
	out, err = runMirrorFunctions(t, bare, "amd64", "DSERV_BOX_CONF="+conf, `check_mirror; echo "rig=$MIRROR_RIG"`)
	if err != nil || !strings.Contains(out, "rig=officepi.local") {
		t.Errorf("check_mirror should keep box.conf's rig: %v\n%s", err, out)
	}

	// ... else from the mirror's settings file (a box set up by hand with
	// install-mirror.sh) ...
	defaults := filepath.Join(dir, "stim2-mirror")
	os.WriteFile(defaults, []byte("MIRROR_DSERV=10.0.0.9\nMIRROR_ARGS=\"-report 30\"\n"), 0o644)
	out, err = runMirrorFunctions(t, bare, "amd64",
		"DSERV_BOX_CONF="+filepath.Join(dir, "none.conf"),
		`MIRROR_DEFAULTS=`+defaults+`; check_mirror; echo "rig=$MIRROR_RIG"`)
	if err != nil || !strings.Contains(out, "rig=10.0.0.9") {
		t.Errorf("check_mirror should keep the settings file's rig: %v\n%s", err, out)
	}

	// ... and with neither it says what it needs.
	out, err = runMirrorFunctions(t, bare, "amd64",
		"DSERV_BOX_CONF="+filepath.Join(dir, "none.conf"),
		`MIRROR_DEFAULTS=`+filepath.Join(dir, "none")+`; check_mirror`)
	if err == nil || !strings.Contains(out, "?mirror=HOST") {
		t.Errorf("check_mirror with no rig should fail and say how: %v\n%s", err, out)
	}
}
