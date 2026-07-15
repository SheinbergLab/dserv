// extio_setup.go - Bootstrap endpoint for provisioning new extio I/O boxes.
//
// Serves the extio provisioning script at GET /extio/setup, the board-side
// sibling of /setup (which bootstraps a Linux acquisition box). The one-liner:
//
//   curl -fsSL https://dserv.net/extio/setup | bash                # dev / latest / dual
//   curl -fsSL https://dserv.net/extio/setup?build=pico2 | bash    # pick the board
//   curl -fsSL https://dserv.net/extio/setup?channel=stable | bash
//
// Prereqs the script enforces itself (die with a clear message): picotool on
// PATH, and an RP2350 in BOOTSEL.
//
// The body is wiznet-io/provision.sh (embedded), streamed behind a small
// preamble that makes shelf-pull the default and points FW_SHELF_URL at THIS
// server -- so a lab-internal agent provisions boxes from its own firmware
// shelf, exactly as /setup derives its serverURL. `bash -s -- <flags>` still
// overrides everything.

package main

import (
	_ "embed"
	"fmt"
	"net/http"
)

// provision.sh and pt.json are kept in sync from wiznet-io/ by the Makefile
// (go:embed can't reach outside this package dir). Edit the canonical copies in
// wiznet-io/, not these.
//
//go:embed provision.sh
var provisionScript string

// pt.json is the RP2350 A/B partition spec provision.sh feeds to picotool. The
// curl|bash flow has no script directory to read it from, so we ship it inline.
//
//go:embed pt.json
var partitionSpec string

// extioTokenOK allows only characters that can appear in a channel / build /
// version token, so a query param can never break out of the shell preamble.
func extioTokenOK(s string) bool {
	if s == "" || len(s) > 64 {
		return false
	}
	for _, c := range s {
		switch {
		case c >= 'a' && c <= 'z', c >= 'A' && c <= 'Z', c >= '0' && c <= '9':
		case c == '.' || c == '-' || c == '_':
		default:
			return false
		}
	}
	return true
}

// handleExtioSetup serves the extio provisioning script.
func (a *Agent) handleExtioSetup(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	scheme := "http"
	if r.TLS != nil || r.Header.Get("X-Forwarded-Proto") == "https" {
		scheme = "https"
	}
	serverURL := fmt.Sprintf("%s://%s", scheme, r.Host)

	// Map ?channel/?build/?version to the script's env knobs. Reject anything
	// that isn't a bare token rather than risk injecting into the preamble.
	q := r.URL.Query()
	env := map[string]string{}
	for param, envVar := range map[string]string{
		"channel": "PROVISION_CHANNEL",
		"build":   "PROVISION_BUILD",
		"version": "PROVISION_VERSION",
	} {
		if v := q.Get(param); v != "" {
			if !extioTokenOK(v) {
				http.Error(w, "invalid "+param+" (allowed: letters, digits, . - _)", http.StatusBadRequest)
				return
			}
			env[envVar] = v
		}
	}

	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.Header().Set("Content-Disposition", "inline; filename=\"extio-setup.sh\"")

	// Preamble: shelf-pull by default, from this server. Runs before the
	// embedded script's own `set -eu`, so plain exports are fine. %q quotes the
	// values (serverURL is server-derived; the env values are token-validated).
	fmt.Fprintf(w, "#!/usr/bin/env bash\n")
	fmt.Fprintf(w, "# extio box provisioning -- served by dserv-agent %s\n", version)
	fmt.Fprintf(w, "export FW_SHELF_URL=%q\n", serverURL)
	fmt.Fprintf(w, "export PROVISION_FROM_SHELF=1\n")
	for _, k := range []string{"PROVISION_CHANNEL", "PROVISION_BUILD", "PROVISION_VERSION"} {
		if v, ok := env[k]; ok {
			fmt.Fprintf(w, "export %s=%q\n", k, v)
		}
	}

	// The curl|bash flow has no script directory, so materialize the partition
	// spec to a temp file and hand it to provision.sh via PT_JSON. It cleans up
	// PROVISION_PT_CLEANUP in its own EXIT trap.
	// picotool infers the input format from the filename extension, so the temp
	// MUST end in .json or `partition create` fails ("does not have a recognized
	// file type"). mktemp gives no extension; rename it. (portable: mv, not
	// GNU-only `mktemp --suffix`.)
	fmt.Fprintf(w, "__EXTIO_PT=$(mktemp) || { echo \"!! mktemp failed\" >&2; exit 1; }\n")
	fmt.Fprintf(w, "mv \"$__EXTIO_PT\" \"$__EXTIO_PT.json\" || { echo \"!! could not name temp .json\" >&2; exit 1; }\n")
	fmt.Fprintf(w, "__EXTIO_PT=\"$__EXTIO_PT.json\"\n")
	fmt.Fprintf(w, "cat > \"$__EXTIO_PT\" <<'EXTIO_PT_JSON'\n")
	fmt.Fprint(w, partitionSpec)
	fmt.Fprintf(w, "\nEXTIO_PT_JSON\n")
	fmt.Fprintf(w, "export PT_JSON=\"$__EXTIO_PT\"\n")
	fmt.Fprintf(w, "export PROVISION_PT_CLEANUP=\"$__EXTIO_PT\"\n")

	fmt.Fprintf(w, "echo \">> extio: provisioning from %s (channel=${PROVISION_CHANNEL:-dev} build=${PROVISION_BUILD:-dual})\" >&2\n", serverURL)

	fmt.Fprint(w, provisionScript)
}
