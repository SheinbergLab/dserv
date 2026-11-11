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

// /setup/plan and the landing page's configurator: the plan must be what
// /setup itself resolves, and the page must carry the profiles /setup knows.

func getPlan(t *testing.T, a *Agent, query string) (int, setupPlan, map[string]string) {
	t.Helper()
	rec := httptest.NewRecorder()
	a.handleSetupPlan(rec, httptest.NewRequest("GET", "/setup/plan?"+query, nil))
	var plan setupPlan
	var e map[string]string
	if rec.Code == 200 {
		if err := json.Unmarshal(rec.Body.Bytes(), &plan); err != nil {
			t.Fatalf("plan JSON: %v\n%s", err, rec.Body)
		}
	} else {
		json.Unmarshal(rec.Body.Bytes(), &e)
	}
	return rec.Code, plan, e
}

func planIDs(cs []setupPlanComponent) []string {
	var out []string
	for _, c := range cs {
		out = append(out, c.ID)
	}
	return out
}

func TestSetupPlanMatchesResolution(t *testing.T) {
	a := embeddedAgent(t)
	for _, p := range a.getProfiles() {
		code, plan, e := getPlan(t, a, "profile="+p.Name)
		if code != 200 {
			t.Fatalf("%s: status %d %v", p.Name, code, e)
		}
		res, err := a.resolveBootstrap(map[string][]string{"profile": {p.Name}})
		if err != nil {
			t.Fatal(err)
		}
		// Install + addons is exactly the roster the script carries, and
		// nothing optional is installed without being asked for.
		var all []string
		all = append(all, planIDs(plan.Install)...)
		all = append(all, planIDs(plan.Addons)...)
		if len(all) != len(res.Components) {
			t.Errorf("%s: plan covers %v, script carries %v", p.Name, all, ids(res.Components))
		}
		for _, c := range plan.Install {
			for _, rc := range res.Components {
				if rc.ID == c.ID && rc.Optional {
					t.Errorf("%s: optional %s listed as installed without ?with=", p.Name, c.ID)
				}
			}
		}
		if !has(planIDs(plan.Install), agentComponentID) {
			t.Errorf("%s: plan does not install the agent", p.Name)
		}
		if plan.StimMode != p.StimMode {
			t.Errorf("%s: stimMode %q, want %q", p.Name, plan.StimMode, p.StimMode)
		}
		if plan.Retire == nil || plan.Addons == nil || plan.Install == nil {
			t.Errorf("%s: lists must be [] not null for the page", p.Name)
		}
	}
}

func TestSetupPlanAddonsAndRetire(t *testing.T) {
	a := embeddedAgent(t)

	// server offers stim2-render; ?with= moves it into the install list.
	_, plan, _ := getPlan(t, a, "profile=server")
	if !has(planIDs(plan.Addons), "stim2-render") || has(planIDs(plan.Install), "stim2-render") {
		t.Errorf("server: addons %v install %v", planIDs(plan.Addons), planIDs(plan.Install))
	}
	_, plan, _ = getPlan(t, a, "profile=server&with=stim2-render")
	if !has(planIDs(plan.Install), "stim2-render") {
		t.Errorf("server+with: install %v", planIDs(plan.Install))
	}
	for _, c := range plan.Addons {
		if c.ID == "stim2-render" && !c.Selected {
			t.Error("server+with: stim2-render not marked selected")
		}
	}

	// A display box has no dserv: no add-on that needs one, and dserv's
	// unit is what a retype to it turns off.
	_, plan, _ = getPlan(t, a, "profile=stim")
	if has(planIDs(plan.Addons), "stim2-render") || has(planIDs(plan.Addons), "dserv-camera") {
		t.Errorf("stim: offers %v", planIDs(plan.Addons))
	}
	if !has(plan.Retire, "dserv") {
		t.Errorf("stim: retire %v, want dserv", plan.Retire)
	}
}

func TestSetupPlanRefusesWhatSetupRefuses(t *testing.T) {
	a := embeddedAgent(t)
	for _, q := range []string{
		"profile=mirror&mirror=x%3Breboot",
		"time_role=client%20%24(id)",
		"with=nope",
		"workgroup=%24(id)",
		"workgroup=a%20b",
	} {
		code, _, e := getPlan(t, a, q)
		if code != 400 || e["error"] == "" {
			t.Errorf("%s: status %d %v, want 400 with an error", q, code, e)
		}
		rec := httptest.NewRecorder()
		a.handleBootstrap(rec, httptest.NewRequest("GET", "/setup?"+q, nil))
		if rec.Code != 400 {
			t.Errorf("/setup?%s: status %d, want 400", q, rec.Code)
		}
	}
	if code, plan, _ := getPlan(t, a, "workgroup=brown-sheinberg"); code != 200 || plan.Workgroup != "brown-sheinberg" {
		t.Errorf("a plain workgroup: %d %+v", code, plan)
	}
}

// The sudo re-fetch carries the URL's ?workgroup=. It used to fetch only
// ?profile=, so the root run got the registry's default workgroup.
func TestRefetchKeepsURLWorkgroup(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("check_root does not re-fetch as root")
	}
	bash, err := exec.LookPath("bash")
	if err != nil {
		t.Skip("bash not available")
	}
	// Rendered from the resolution, as handleBootstrap does, without its
	// release lookups.
	a := embeddedAgent(t)
	res, err := a.resolveBootstrap(map[string][]string{"profile": {"stim"}, "workgroup": {"mylab"}})
	if err != nil {
		t.Fatal(err)
	}
	var b strings.Builder
	if err := bootstrapTmpl.Execute(&b, BootstrapConfig{
		ServerURL: "https://dserv.net", DefaultWG: res.Workgroup, Version: "test",
		ProfileName: res.Profile, StimMode: res.StimMode,
		ComponentsJSON:   shellQuote(`{"components":[]}`),
		AgentReleaseJSON: shellQuote(`{"tag":"0.0.0","assets":[]}`),
	}); err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	script := filepath.Join(dir, "setup.sh")
	os.WriteFile(script, []byte(b.String()), 0o644)

	// Fake curl and sudo on PATH: check_root ends in `exec sudo ...`, which
	// a shell function cannot intercept.
	bin := filepath.Join(dir, "bin")
	os.Mkdir(bin, 0o755)
	os.WriteFile(filepath.Join(bin, "curl"), []byte("#!/bin/sh\necho \"FETCH $*\" >&2\nprintf '#!/bin/bash\\n'\n"), 0o755)
	os.WriteFile(filepath.Join(bin, "sudo"), []byte("#!/bin/sh\necho SUDO\n"), 0o755)

	extract := func(fn string) string {
		out, err := exec.Command("sed", "-n", "/^"+fn+"() {/,/^}/p", script).Output()
		if err != nil || len(out) == 0 {
			t.Fatalf("could not extract %s", fn)
		}
		return string(out)
	}
	vars, _ := exec.Command("sed", "-En",
		`/^(REGISTRY_URL|DEFAULT_WORKGROUP|PROFILE|COMPONENTS_ARG|WITH_ADDONS)=/p`, script).Output()
	prog := `set -u
info() { :; }; fail() { echo "fail: $*"; exit 1; }
` + string(vars) + `
ROLE=""; TIME_ROLE=""; MIRROR_RIG=""; WORKGROUP="$DEFAULT_WORKGROUP"; WORKGROUP_EXPLICIT=false
SYSTEMS_PATH_EXPLICIT=false; ESS_USER_ARG=""; DRY_RUN=false; SKIP_AGENT=false
SCRIPTS_ONLY=false; SYNC_SCRIPTS=false; REINSTALL=false
` + extract("check_root") + "check_root\n"
	cmd := exec.Command(bash, "-c", prog)
	cmd.Env = append(os.Environ(), "PATH="+bin+":"+os.Getenv("PATH"))
	out, _ := cmd.CombinedOutput()
	if !strings.Contains(string(out), "FETCH -fsSL https://dserv.net/setup?profile=stim&workgroup=mylab") {
		t.Errorf("re-fetch should carry the workgroup:\n%s", out)
	}
	if !strings.Contains(string(out), "SUDO") {
		t.Errorf("re-fetch did not go on to sudo:\n%s", out)
	}
}

func TestLandingPageRender(t *testing.T) {
	a := embeddedAgent(t)
	page := a.renderLandingPage("https://dserv.net")
	for _, ph := range []string{"__BASE_JSON__", "__PROFILES_JSON__", "__BASE__", "__VERSION__"} {
		if strings.Contains(page, ph) {
			t.Errorf("placeholder %s left in the page", ph)
		}
	}
	if !strings.Contains(page, `var BASE = "https://dserv.net";`) {
		t.Error("BASE not set from the request")
	}
	// Every profile /setup knows, by name and title.
	for _, p := range a.getProfiles() {
		if !strings.Contains(page, `"name":"`+p.Name+`"`) || p.Title == "" {
			t.Errorf("profile %s missing from the page, or untitled", p.Name)
		}
	}
	// A hostile Host header cannot break out of the script.
	evil := a.renderLandingPage(`http://x</script><script>alert(1)</script>`)
	if strings.Contains(evil, "<script>alert(1)") {
		t.Error("base URL is not escaped in the page")
	}

	rec := httptest.NewRecorder()
	a.handleLandingPage(rec, httptest.NewRequest("GET", "/elsewhere", nil))
	if rec.Code != 404 {
		t.Errorf("non-root path: %d, want 404", rec.Code)
	}
}
