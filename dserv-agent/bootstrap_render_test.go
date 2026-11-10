package main

import (
	"encoding/json"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"regexp"
	"strings"
	"testing"
)

// The stim2-render add-on: offered (not installed) wherever dserv, stim2 and
// the dlsh interpreter are, installed by ?with=stim2-render or the panel,
// and never enabled by the setup script itself (its package does that).

func embeddedAgent(t *testing.T) *Agent {
	t.Helper()
	var cfg struct {
		Components []Component `json:"components"`
	}
	if err := json.Unmarshal(defaultComponentsJSON, &cfg); err != nil {
		t.Fatalf("embedded components.json: %v", err)
	}
	return &Agent{components: cfg.Components}
}

func ids(cs []Component) []string {
	var out []string
	for _, c := range cs {
		out = append(out, c.ID)
	}
	return out
}

func has(list []string, id string) bool {
	for _, x := range list {
		if x == id {
			return true
		}
	}
	return false
}

func TestRenderComponent(t *testing.T) {
	a := embeddedAgent(t)
	var r *Component
	for i := range a.components {
		if a.components[i].ID == "stim2-render" {
			r = &a.components[i]
		}
	}
	if r == nil {
		t.Fatal("no stim2-render component")
	}
	if !r.Optional || r.Service != "stim2-render" || r.Package != "stim2-render" {
		t.Errorf("stim2-render = %+v", r)
	}
	for _, d := range []string{"dserv", "stim2", "dlsh-interp"} {
		if !has(r.Depends, d) {
			t.Errorf("stim2-render should depend on %s", d)
		}
	}
	// The release asset, and not the stim2 package's per-distro debs.
	re := mustCompile(t, r.AssetPattern)
	if !re.MatchString("stim2-render_0.28.22_all.deb") {
		t.Error("pattern misses stim2-render_0.28.22_all.deb")
	}
	if re.MatchString("stim2_0.28.22_amd64_bookworm.deb") {
		t.Error("pattern matches the stim2 package")
	}
	// ... nor does stim2's own pattern take the render package.
	for _, c := range a.components {
		if c.ID == "stim2" && mustCompile(t, c.AssetPattern).MatchString("stim2-render_0.28.22_all.deb") {
			t.Error("stim2's pattern matches the render package")
		}
	}
}

func TestServerProfileHeadless(t *testing.T) {
	a := embeddedAgent(t)
	if got := a.profileStimMode("server"); got != "headless" {
		t.Errorf("server StimMode = %q, want headless", got)
	}
	got := ids(a.filterComponents("server", ""))
	for _, want := range []string{"dserv", "dlsh", "stim2", "dlsh-interp"} {
		if !has(got, want) {
			t.Errorf("server profile lacks %s: %v", want, got)
		}
	}
	if has(got, "stim2-render") {
		t.Errorf("server profile installs render without ?with=: %v", got)
	}
}

func TestParseWith(t *testing.T) {
	a := embeddedAgent(t)
	got, err := a.parseWith(" stim2-render,,stim2-render ")
	if err != nil || !reflect.DeepEqual(got, []string{"stim2-render"}) {
		t.Errorf("parseWith = %v, %v", got, err)
	}
	if got, err := a.parseWith(""); err != nil || len(got) != 0 {
		t.Errorf("empty with = %v, %v", got, err)
	}
	for _, bad := range []string{"nosuch", "stim2-render;reboot", "$(id)"} {
		if _, err := a.parseWith(bad); err == nil {
			t.Errorf("parseWith(%q) should fail", bad)
		}
	}
}

func TestApplyWith(t *testing.T) {
	a := embeddedAgent(t)

	// A minimal box opting in pulls what render needs.
	comps, addons := a.applyWith(a.filterComponents("minimal", ""), []string{"stim2-render"}, false)
	for _, want := range []string{"stim2-render", "stim2", "dlsh-interp", "dserv"} {
		if !has(ids(comps), want) {
			t.Errorf("minimal+render lacks %s: %v", want, ids(comps))
		}
	}
	if !reflect.DeepEqual(addons, []string{"stim2-render"}) {
		t.Errorf("addons = %v", addons)
	}

	// A profile alone opts into nothing, even with add-ons in its roster.
	_, addons = a.applyWith(a.filterComponents("incage", ""), nil, false)
	if len(addons) != 0 {
		t.Errorf("incage without with opted into %v", addons)
	}

	// Naming an optional component in ?components= is the opt-in.
	_, addons = a.applyWith(a.filterComponents("", "dserv,dserv-camera"), nil, true)
	if !has(addons, "dserv-camera") {
		t.Errorf("explicit camera not opted in: %v", addons)
	}
}

func TestSetupRefusesUnknownWith(t *testing.T) {
	a := embeddedAgent(t)
	rec := httptest.NewRecorder()
	a.handleBootstrap(rec, httptest.NewRequest("GET", "/setup?profile=server&with=x%3Breboot", nil))
	if rec.Code != 400 {
		t.Errorf("status %d, want 400", rec.Code)
	}
}

func TestMergeEmbeddedAddons(t *testing.T) {
	a := embeddedAgent(t)
	pick := func(want ...string) []Component {
		var out []Component
		for _, c := range a.components {
			if has(want, c.ID) {
				out = append(out, c)
			}
		}
		return out
	}

	// A box provisioned before the add-on existed gets it offered.
	got := ids(mergeEmbeddedAddons(pick("dlsh", "dlsh-interp", "dserv", "stim2", "dserv-agent"), a.components))
	if !has(got, "stim2-render") {
		t.Errorf("incage-like roster not offered render: %v", got)
	}
	// Nothing else joins: the camera needs only dserv, so it does too, but
	// no non-optional component is added.
	for _, id := range got {
		if !has([]string{"dlsh", "dlsh-interp", "dserv", "stim2", "dserv-agent", "stim2-render", "dserv-camera"}, id) {
			t.Errorf("merge added %s", id)
		}
	}

	// A display box (no dserv) and a box without the interpreter are not.
	if got := ids(mergeEmbeddedAddons(pick("dlsh", "stim2", "dserv-agent"), a.components)); has(got, "stim2-render") {
		t.Errorf("stim box offered render: %v", got)
	}
	if got := ids(mergeEmbeddedAddons(pick("dlsh", "dserv", "stim2", "dserv-agent"), a.components)); has(got, "stim2-render") {
		t.Errorf("box without dlsh-interp offered render: %v", got)
	}

	// The roster's own entry wins over the embedded one.
	own := pick("dlsh", "dlsh-interp", "dserv", "stim2", "dserv-agent", "stim2-render")
	own[len(own)-1].Description = "site's own"
	merged := mergeEmbeddedAddons(own, a.components)
	n := 0
	for _, c := range merged {
		if c.ID == "stim2-render" {
			n++
			if c.Description != "site's own" {
				t.Error("embedded entry replaced the roster's")
			}
		}
	}
	if n != 1 {
		t.Errorf("stim2-render appears %d times", n)
	}
}

// The served script for a server box with render in its roster.
func TestRenderScript(t *testing.T) {
	bash, err := exec.LookPath("bash")
	if err != nil {
		t.Skip("bash not available")
	}
	if _, err := exec.LookPath("jq"); err != nil {
		t.Skip("jq not available")
	}
	a := embeddedAgent(t)
	comps, addons := a.applyWith(a.filterComponents("server", ""), []string{"stim2-render"}, false)
	compJSON, _ := json.Marshal(map[string]any{"components": comps})

	render := func(mode, with string) string {
		var b strings.Builder
		cfg := BootstrapConfig{
			ServerURL:        "https://dserv.net",
			DefaultWG:        "test-wg",
			Version:          "test",
			ProfileName:      "server",
			StimMode:         mode,
			With:             with,
			ComponentsJSON:   shellQuote(string(compJSON)),
			AgentReleaseJSON: shellQuote(`{"tag":"0.0.0","assets":[]}`),
		}
		if err := bootstrapTmpl.Execute(&b, cfg); err != nil {
			t.Fatalf("template execute: %v", err)
		}
		path := filepath.Join(t.TempDir(), "setup.sh")
		os.WriteFile(path, []byte(b.String()), 0o644)
		if out, err := exec.Command(bash, "-n", path).CombinedOutput(); err != nil {
			t.Fatalf("served script is not valid bash: %v\n%s", err, out)
		}
		return path
	}
	run := func(script, prelude, body string) string {
		vars, _ := exec.Command("sed", "-En",
			`/^(COMPONENTS_JSON|STIM_MODE|WITH_ADDONS|STIM_COMPONENT_ID|AGENT_COMPONENT_ID|MIRROR_[A-Z_]*)=/p`, script).Output()
		var fns string
		for _, fn := range []string{"profile_services", "skip_addon"} {
			out, _ := exec.Command("sed", "-n", "/^"+fn+"() {/,/^}/p", script).Output()
			if len(out) == 0 {
				t.Fatalf("could not extract %s", fn)
			}
			fns += string(out)
		}
		prog := "info() { echo \"info: $*\"; }\n" + string(vars) + fns + prelude + "\n" + body
		out, err := exec.Command(bash, "-c", prog).CombinedOutput()
		if err != nil {
			t.Fatalf("%v\n%s", err, out)
		}
		return string(out)
	}

	script := render("headless", strings.Join(addons, " "))
	if !strings.Contains(mustRead(t, script), `WITH_ADDONS="stim2-render"`) {
		t.Error("WITH_ADDONS not in the served script")
	}

	// A server enables dserv, and neither stim2's display nor the add-on.
	got := strings.Fields(run(script, "", "profile_services"))
	if !has(got, "dserv") || has(got, "stim2") || has(got, "stim2-render") {
		t.Errorf("headless profile_services = %v", got)
	}
	// incage: stim2 yes, the add-on still no.
	got = strings.Fields(run(render("", ""), "", "profile_services"))
	if !has(got, "stim2") || has(got, "stim2-render") {
		t.Errorf("incage profile_services = %v", got)
	}

	renderJSON := `'{"id":"stim2-render","optional":true}'`
	plainJSON := `'{"id":"stim2"}'`
	decide := func(script, installed, comp string) string {
		prelude := `installed_version() { echo "` + installed + `"; }`
		return strings.TrimSpace(run(script, prelude,
			`if skip_addon `+comp+` >/dev/null; then echo skip; else echo install; fi`))
	}
	if d := decide(script, "", renderJSON); d != "install" {
		t.Errorf("?with=stim2-render: render %s", d)
	}
	noWith := render("headless", "")
	if d := decide(noWith, "", renderJSON); d != "skip" {
		t.Errorf("no ?with=, not installed: render %s", d)
	}
	if d := decide(noWith, "0.28.22", renderJSON); d != "install" {
		t.Errorf("no ?with=, installed (a re-run keeps it): render %s", d)
	}
	if d := decide(noWith, "", plainJSON); d != "install" {
		t.Errorf("a non-optional component was skipped")
	}
}

func mustRead(t *testing.T, path string) string {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

func mustCompile(t *testing.T, pattern string) *regexp.Regexp {
	t.Helper()
	re, err := regexp.Compile(pattern)
	if err != nil {
		t.Fatalf("pattern %q: %v", pattern, err)
	}
	return re
}
