// retype.go - reprovision a box to a different profile, from the panel or curl.
//
// A retype IS the bootstrap: the agent fetches the registry's own
// /setup?profile=X and runs it flagless (script sync is opt-in there, so
// local ESS scripts are kept), and the panel button and the curl a human
// would paste share one source of truth -- profile semantics live only in
// the served script. The bootstrap skips components already at
// their target version, so a retype that only changes class (incage -> dev)
// is a few seconds of convergence work, not a reinstall.
//
// The run is handed to a transient systemd unit for the same reason
// selfUpdate does it: step_start_services restarts dserv-agent partway
// through, and work in this process's cgroup would die with it. The unit also
// re-fetches the script itself -- the agent's PrivateTmp means a file written
// here is invisible to any other unit, the exact trap the first agent
// self-update fell into.

package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"net/url"
	"os/exec"
	"regexp"
	"strings"
)

// retypeUnit is the transient unit a retype runs under. Fixed name: it doubles
// as the concurrency guard (a second retype is refused while one is active)
// and as the journal address for "view log" (journalctl -u dserv-retype).
const retypeUnit = "dserv-retype"

// Belt-and-braces under the registry list check: the name reaches a URL and a
// shell line, so reject anything that is not a plain profile word outright.
var profileNameRe = regexp.MustCompile(`^[a-z0-9][a-z0-9_-]*$`)

// registryBase is the registry this box answers to: the -registry flag when
// set, else the one the bootstrap recorded in box.conf. Boxes provisioned
// before either exist cannot retype from the panel -- the error says to use
// the curl instead.
func (a *Agent) registryBase() string {
	if len(a.cfg.RegistryURLs) > 0 && a.cfg.RegistryURLs[0] != "" {
		return strings.TrimRight(a.cfg.RegistryURLs[0], "/")
	}
	if id := readBoxIdentity(boxConfPath); id != nil && id.Registry != "" {
		return strings.TrimRight(id.Registry, "/")
	}
	return ""
}

// fetchRegistryProfiles asks the registry what profiles it serves. Live, not
// cached: the list is tiny, retypes are rare, and a stale answer here would
// offer a profile the registry no longer resolves.
func (a *Agent) fetchRegistryProfiles(registry string) ([]BootstrapProfile, error) {
	resp, err := a.http.Get(registry + "/setup/profiles")
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("HTTP %d from %s/setup/profiles", resp.StatusCode, registry)
	}
	var body struct {
		Profiles []BootstrapProfile `json:"profiles"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		return nil, err
	}
	return body.Profiles, nil
}

// startRetype validates the request and hands the provisioning run to the
// transient unit. Returns once systemd has accepted the unit -- nothing waits
// for the bootstrap itself, whose endgame restarts this process.
// timeRole is optional: empty means "no change", which is what a retype has
// always meant -- step_record_identity preserves the existing declaration.
// A value re-declares it, "none" clears it.
func (a *Agent) startRetype(profile, timeRole string) error {
	registry := a.registryBase()
	if registry == "" {
		return fmt.Errorf("no registry configured (-registry flag or %s) — retype from a shell instead: curl -sSL <registry>/setup?profile=%s | bash", boxConfPath, profile)
	}
	if !profileNameRe.MatchString(profile) {
		return fmt.Errorf("invalid profile name %q", profile)
	}
	if timeRole != "" && !timeRoleDeclRe.MatchString(timeRole) {
		return fmt.Errorf("invalid time role %q (want \"grandmaster IFACE\", \"client IFACE\", \"ntp-client SERVER\", or \"none\")", timeRole)
	}

	// The registry must actually know this profile. filterComponents falls
	// back to the FULL component set for names it does not recognize, so
	// without this check a typo would not fail -- it would quietly retype
	// the box to everything-incage.
	profiles, err := a.fetchRegistryProfiles(registry)
	if err != nil {
		return fmt.Errorf("could not list profiles from %s: %v", registry, err)
	}
	known := false
	for _, p := range profiles {
		if strings.EqualFold(p.Name, profile) {
			known = true
			break
		}
	}
	if !known {
		return fmt.Errorf("registry %s has no profile %q", registry, profile)
	}

	if unitActive(retypeUnit) {
		return fmt.Errorf("a retype is already running (journalctl -u %s)", retypeUnit)
	}

	// Clear a previous run's failure before claiming the name. Deliberately
	// NOT --collect below: a transient unit vanishes the moment it goes
	// inactive, which erased the evidence exactly when it mattered -- a
	// bootstrap that died in its first second looked identical, to the panel,
	// to one still grinding away, for the four minutes retypeWatch waits.
	// Without --collect a success still self-collects and a failure lingers in
	// `failed`, which is the state retypeStatus reports. The cost is that the
	// name stays taken until this reset.
	exec.Command("sudo", "systemctl", "reset-failed", retypeUnit).Run()

	// No --scripts, ever: a box being retyped is a box in service, and the
	// script sync unzips the registry's copy OVER ~/systems/ess. Scripts are
	// opt-in in the bootstrap now, so the flagless default IS the safe one
	// (the old --skip-scripts is accepted but redundant). Deliberately NOT
	// --skip-agent -- converging the agent package is part of converging
	// the box. A box running a hand-built agent should retype from a shell
	// with --skip-agent instead.
	setupURL := registry + "/setup?profile=" + profile
	if timeRole != "" {
		setupURL += "&time_role=" + url.QueryEscape(timeRole)
	}
	script := fmt.Sprintf(`set -e
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
echo "retype: fetching %s"
curl -fsSL -o "$d/setup.sh" %s
case "$(head -c 2 "$d/setup.sh")" in
  '#!') ;;
  *) echo "retype: registry did not return a script"; exit 1 ;;
esac
echo "retype: running bootstrap (profile %s; local scripts kept)"
bash "$d/setup.sh"
echo "retype: complete"`,
		setupURL, shellQuote(setupURL), profile)

	cmd := exec.Command("sudo", "systemd-run",
		"--unit", retypeUnit,
		"--description", "dserv box retype to profile "+profile,
		"--setenv=DEBIAN_FRONTEND=noninteractive",
		"/bin/sh", "-c", script)
	if out, err := cmd.CombinedOutput(); err != nil {
		msg := strings.TrimSpace(string(out))
		if msg == "" {
			msg = err.Error()
		}
		return fmt.Errorf("could not hand off retype to systemd: %s", msg)
	}

	log.Printf("retype to %q handed off to %s.service (registry %s)", profile, retypeUnit, registry)
	return nil
}

// RetypeState is how a retype run looks from outside: rides along on every
// status payload so the panel's existing poll sees a failure on its next tick
// rather than waiting out its give-up timer.
type RetypeState struct {
	Active bool   `json:"active"`
	Failed bool   `json:"failed"`
	Result string `json:"result,omitempty"` // systemd's Result, e.g. "exit-code"
	Code   string `json:"code,omitempty"`   // the script's exit status, when nonzero
}

// retypeStatus reports the transient unit, or nil when there is nothing to
// say -- no run in flight and no failure held. nil is the common case by far:
// a unit that succeeded collected itself, and most boxes never retype at all.
func retypeStatus() *RetypeState {
	out, err := exec.Command("systemctl", "show", retypeUnit,
		"--property=LoadState,ActiveState,Result,ExecMainStatus").Output()
	if err != nil {
		return nil
	}
	f := make(map[string]string, 4)
	for _, line := range strings.Split(string(out), "\n") {
		if k, v, ok := strings.Cut(strings.TrimSpace(line), "="); ok {
			f[k] = v
		}
	}
	// An unloaded unit is the steady state, not an error: systemd garbage
	// collects a transient unit as soon as it succeeds.
	if ls := f["LoadState"]; ls == "" || ls == "not-found" {
		return nil
	}
	st := &RetypeState{}
	switch f["ActiveState"] {
	case "activating", "active", "deactivating", "reloading":
		st.Active = true
	case "failed":
		st.Failed = true
	default:
		// inactive-but-loaded: on its way out, and it did not fail.
		return nil
	}
	if st.Failed {
		st.Result = f["Result"]
		if c := f["ExecMainStatus"]; c != "" && c != "0" {
			st.Code = c
		}
	}
	return st
}

// GET /api/retype/profiles - what the registry offers, plus where this box
// stands. The panel builds its selector from this; curl users get the same.
func (a *Agent) handleRetypeProfiles(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" {
		http.Error(w, "Method not allowed", 405)
		return
	}
	registry := a.registryBase()
	if registry == "" {
		writeJSON(w, 400, map[string]string{"error": "no registry configured"})
		return
	}
	profiles, err := a.fetchRegistryProfiles(registry)
	if err != nil {
		writeJSON(w, 502, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, 200, map[string]interface{}{
		"registry": registry,
		"box":      readBoxIdentity(boxConfPath),
		"profiles": profiles,
	})
}

// POST /api/retype?profile=X (or JSON {"profile": "X"}) - curl-scriptable
// twin of the panel button, e.g. for sweeping a batch of old dev Pis.
func (a *Agent) handleRetype(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		http.Error(w, "Method not allowed", 405)
		return
	}
	profile := r.URL.Query().Get("profile")
	timeRole := strings.TrimSpace(r.URL.Query().Get("time_role"))
	if profile == "" {
		var body struct {
			Profile  string `json:"profile"`
			TimeRole string `json:"timeRole"`
		}
		json.NewDecoder(r.Body).Decode(&body)
		profile = body.Profile
		if timeRole == "" {
			timeRole = strings.TrimSpace(body.TimeRole)
		}
	}
	if profile == "" {
		writeJSON(w, 400, map[string]string{"error": "profile required"})
		return
	}
	if err := a.startRetype(profile, timeRole); err != nil {
		writeJSON(w, 400, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, 202, map[string]string{"status": "started", "profile": profile, "unit": retypeUnit})
}
