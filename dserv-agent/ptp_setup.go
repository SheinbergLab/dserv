// ptp_setup.go - serve the PTP role tooling as a one-liner installer.
//
// scripts/dserv-ptp-setup and its unit templates ship in the dserv .deb --
// which is the one package a display-only box deliberately does not have. So
// a stim box that should be a PTP client had no way to get the tooling short
// of hand-copying files. The registry already solves this shape of problem
// twice (/setup for boxes, /extio/setup for boards); this is the same move
// for time roles:
//
//	curl -sSL https://registry/ptp/setup | sudo bash                     # tooling only, inert
//	curl -sSL https://registry/ptp/setup | sudo bash -s -- client eth0   # tooling + role
//
// The installer only ever INSTALLS by default: unit templates are copied
// inert, exactly as dserv's postinst does, and assigning a role stays an
// explicit argument -- preserving dserv-ptp-setup's own "once, deliberately"
// philosophy along with all its preflights (a client role is still refused
// when no sane grandmaster is visible on the segment).
//
// The embedded copies are mirrored from ../scripts and ../systemd by the
// Makefile (go:embed cannot reach a parent directory); embed_sync_test.go
// fails the build's tests if they drift from the canonical sources.

package main

import (
	"embed"
	"fmt"
	"net/http"
	"strings"
)

//go:embed all:ptp
var ptpFS embed.FS

// ptpInstallFile maps an embedded file to where it lands on the target host.
type ptpInstallFile struct {
	embedName string // name under ptp/ in the embedded FS
	dest      string // absolute install path on the target
	mode      string // chmod mode
}

// Order matters only for readability of the generated script. Destinations
// mirror the dserv .deb layout exactly, so a box that later DOES install
// dserv converges on the same files instead of a second copy.
var ptpInstallFiles = []ptpInstallFile{
	{"dserv-ptp-setup", "/usr/local/dserv/scripts/dserv-ptp-setup", "0755"},
	{"dserv-ptp-select-phc", "/usr/local/dserv/scripts/dserv-ptp-select-phc", "0755"},
	{"dserv-ptp4l@.service", "/etc/systemd/system/dserv-ptp4l@.service", "0644"},
	{"dserv-phc2sys@.service", "/etc/systemd/system/dserv-phc2sys@.service", "0644"},
	{"dserv-ptp4l-client@.service", "/etc/systemd/system/dserv-ptp4l-client@.service", "0644"},
	{"dserv-phc2sys-client@.service", "/etc/systemd/system/dserv-phc2sys-client@.service", "0644"},
	{"chrony-grandmaster.conf", "/usr/local/dserv/systemd/chrony-grandmaster.conf", "0644"},
}

// ptpHeredocEOF delimits each embedded file in the generated script. Quoted
// at the `cat`, so nothing inside the payload expands. embed_sync_test.go
// asserts no payload contains this string -- the one way heredoc embedding
// can silently truncate.
const ptpHeredocEOF = "__DSERV_PTP_EMBED_EOF__"

// GET /ptp/setup - a self-contained installer for the PTP tooling.
// Registered without auth, like /setup: fresh boxes need bare curl access.
func (a *Agent) handlePTPSetup(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	scheme := "http"
	if r.TLS != nil || r.Header.Get("X-Forwarded-Proto") == "https" {
		scheme = "https"
	}
	serverURL := fmt.Sprintf("%s://%s", scheme, r.Host)

	var b strings.Builder
	fmt.Fprintf(&b, `#!/usr/bin/env bash
#
# dserv PTP tooling installer -- served by %[1]s/ptp/setup
#
# Installs dserv-ptp-setup, its PHC selector, and the four systemd unit
# templates (inert until a role is assigned), then forwards any arguments to
# dserv-ptp-setup:
#
#   curl -sSL %[1]s/ptp/setup | sudo bash                      # tooling only
#   curl -sSL %[1]s/ptp/setup | sudo bash -s -- candidates
#   curl -sSL %[1]s/ptp/setup | sudo bash -s -- client IFACE
#   curl -sSL %[1]s/ptp/setup | sudo bash -s -- grandmaster IFACE
#   curl -sSL %[1]s/ptp/setup | sudo bash -s -- ntp-client SERVER   # no PTP NIC needed
#
# DSERV_PTP_DESTDIR=<dir> writes the files under <dir> and touches nothing
# else (no apt, no systemctl, no role) -- a dry run you can diff.
set -euo pipefail

DESTDIR="${DSERV_PTP_DESTDIR:-}"

if [[ -z "$DESTDIR" && $EUID -ne 0 ]]; then
    if command -v sudo &>/dev/null; then
        echo "[info] Re-running with sudo..."
        # Re-fetch rather than substituting a possibly-empty download straight
        # into bash -c -- same reasoning as the box bootstrap's check_root.
        script=$(curl -fsSL "%[1]s/ptp/setup") || script=""
        if [[ -z "$script" || "${script:0:2}" != '#!' ]]; then
            echo "Could not re-fetch the installer from %[1]s to run as root -- re-run this command under sudo yourself" >&2
            exit 1
        fi
        exec sudo bash -c "$script" ptp-setup "$@"
    fi
    echo "This installer must run as root" >&2
    exit 1
fi

if [[ -z "$DESTDIR" ]]; then
    # linuxptp is what the units exec; ethtool is how the timestamping clock
    # is checked and selected on multi-PHC NICs. Skipped when both present.
    if [[ ! -x /usr/sbin/ptp4l || ! -x /usr/sbin/phc2sys ]] || ! command -v ethtool &>/dev/null; then
        echo "[info] Installing linuxptp + ethtool..."
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq || true
        apt-get install -y linuxptp ethtool
    fi
fi

install -d "${DESTDIR}/usr/local/dserv/scripts" "${DESTDIR}/usr/local/dserv/systemd" "${DESTDIR}/etc/systemd/system"

`, serverURL)

	for _, f := range ptpInstallFiles {
		content, err := ptpFS.ReadFile("ptp/" + f.embedName)
		if err != nil {
			http.Error(w, "embedded file missing: "+f.embedName, 500)
			return
		}
		payload := string(content)
		if !strings.HasSuffix(payload, "\n") {
			payload += "\n"
		}
		fmt.Fprintf(&b, "cat > \"${DESTDIR}%s\" <<'%s'\n%s%s\nchmod %s \"${DESTDIR}%s\"\n\n",
			f.dest, ptpHeredocEOF, payload, ptpHeredocEOF, f.mode, f.dest)
	}

	b.WriteString(`echo "[ok] PTP tooling installed"

if [[ -n "$DESTDIR" ]]; then
    echo "[info] DSERV_PTP_DESTDIR set -- skipped apt, daemon-reload, and role assignment"
    exit 0
fi

systemctl daemon-reload

if [[ $# -gt 0 ]]; then
    exec /usr/local/dserv/scripts/dserv-ptp-setup "$@"
fi

echo ""
echo "No role assigned -- that stays a deliberate step:"
echo "  dserv-ptp-setup candidates          # interfaces that can actually do PTP"
echo "  dserv-ptp-setup client IFACE        # follow the site's grandmaster"
echo "  dserv-ptp-setup grandmaster IFACE   # define the site's time (ONE host per site)"
echo "  dserv-ptp-setup ntp-client SERVER   # follow site time over NTP (no PTP NIC needed)"
echo ""
/usr/local/dserv/scripts/dserv-ptp-setup status || true
`)

	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.Header().Set("Content-Disposition", "inline; filename=\"dserv-ptp-setup.sh\"")
	w.Write([]byte(b.String()))
}
