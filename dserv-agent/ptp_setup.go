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
//	wget -qO- https://registry/ptp/setup | sudo bash                     # hosts without curl
//
// Re-running the same line is also how a box UPDATES: only changed files are
// rewritten, running daemons whose unit or PHC selector changed are restarted
// (a daemon keeps its old command line until it restarts), and what was
// installed is recorded for `dserv-ptp-setup status` to report. That is what
// makes this the maintenance path for hosts with no dserv package -- the
// tracker and console on a rig -- rather than hand-staged copies that drift.
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
	"crypto/sha256"
	"embed"
	"encoding/hex"
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

// ptpToolingInfo and ptpToolingSums are where an install records itself, next
// to the dserv tree it installs into. `dserv-ptp-setup status` reads both.
const (
	ptpToolingInfo = "/usr/local/dserv/ptp-tooling.info"
	ptpToolingSums = "/usr/local/dserv/ptp-tooling.sha256"
)

// ptpPayload is one embedded file as it will land on disk.
type ptpPayload struct {
	ptpInstallFile
	content string
}

// ptpPayloads loads the embedded files, normalised exactly as written (a
// trailing newline added where missing), so the hashes below describe the
// bytes on the target and not the bytes in the binary.
func ptpPayloads() ([]ptpPayload, error) {
	var out []ptpPayload
	for _, f := range ptpInstallFiles {
		b, err := ptpFS.ReadFile("ptp/" + f.embedName)
		if err != nil {
			return nil, fmt.Errorf("embedded file missing: %s", f.embedName)
		}
		c := string(b)
		if !strings.HasSuffix(c, "\n") {
			c += "\n"
		}
		out = append(out, ptpPayload{f, c})
	}
	return out, nil
}

// ptpToolingID names this exact set of files: the first 12 hex digits of a
// sha256 over every name and content. Same files, same id, whichever registry
// or agent build serves them -- so two boxes can be compared at a glance.
func ptpToolingID(files []ptpPayload) string {
	h := sha256.New()
	for _, f := range files {
		fmt.Fprintf(h, "%s\x00%d\x00", f.embedName, len(f.content))
		h.Write([]byte(f.content))
	}
	return hex.EncodeToString(h.Sum(nil))[:12]
}

// GET /ptp/setup - a self-contained installer (and updater) for the PTP tooling.
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

	files, err := ptpPayloads()
	if err != nil {
		http.Error(w, err.Error(), 500)
		return
	}
	script := renderPTPSetup(files, serverURL, version)
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.Header().Set("Content-Disposition", "inline; filename=\"dserv-ptp-setup.sh\"")
	w.Write([]byte(script))
}

// renderPTPSetup builds the installer script. Split from the handler so tests
// can run it without an HTTP round trip.
func renderPTPSetup(files []ptpPayload, serverURL, agentVersion string) string {
	id := ptpToolingID(files)
	var manifest strings.Builder
	for _, f := range files {
		sum := sha256.Sum256([]byte(f.content))
		fmt.Fprintf(&manifest, "%s  %s\n", hex.EncodeToString(sum[:]), f.dest)
	}

	var b strings.Builder
	b.WriteString(ptpSetupHead)
	b.WriteString("\n")
	for _, f := range files {
		fmt.Fprintf(&b, "cat > \"$STAGE/%s\" <<'%s'\n%s%s\nput %s %s %s\n\n",
			f.embedName, ptpHeredocEOF, f.content, ptpHeredocEOF, f.embedName, f.dest, f.mode)
	}
	fmt.Fprintf(&b, "cat > \"$STAGE/ptp-tooling.sha256\" <<'%s'\n%s%s\n",
		ptpHeredocEOF, manifest.String(), ptpHeredocEOF)
	b.WriteString(ptpSetupTail)

	r := strings.NewReplacer(
		"__BASE__", serverURL,
		"__TOOLING_ID__", id,
		"__AGENT__", agentVersion,
		"__INFO__", ptpToolingInfo,
		"__SUMS__", ptpToolingSums,
	)
	return r.Replace(b.String())
}

// The script around the payload. Placeholders (__BASE__ etc.) are substituted
// in renderPTPSetup; nothing inside a payload heredoc can collide with them
// because the payloads are dserv's own scripts, and embed_sync_test.go would
// catch a new one that tried.
const ptpSetupHead = `#!/usr/bin/env bash
#
# dserv PTP tooling installer -- served by __BASE__/ptp/setup
# tooling __TOOLING_ID__ (agent __AGENT__)
#
# Installs dserv-ptp-setup, its PHC selector, and the four systemd unit
# templates (inert until a role is assigned), then forwards any arguments to
# dserv-ptp-setup:
#
#   curl -sSL __BASE__/ptp/setup | sudo bash                      # install, or UPDATE
#   curl -sSL __BASE__/ptp/setup | sudo bash -s -- candidates
#   curl -sSL __BASE__/ptp/setup | sudo bash -s -- client IFACE
#   curl -sSL __BASE__/ptp/setup | sudo bash -s -- grandmaster IFACE
#   curl -sSL __BASE__/ptp/setup | sudo bash -s -- ntp-client SERVER   # no PTP NIC needed
#   wget -qO- __BASE__/ptp/setup | sudo bash                      # a host without curl
#
# RE-RUNNING IS HOW YOU UPDATE. Only files that differ are rewritten, and a
# RUNNING ptp4l/phc2sys whose unit or PHC selector changed is restarted: a
# daemon keeps its old command line until it restarts, so an update that only
# rewrote files would change nothing until the next reboot. The restart makes
# PTP clients re-lock -- on a grandmaster, the whole segment -- so run updates
# between sessions. DSERV_PTP_NO_RESTART=1 installs and prints the restarts
# instead of doing them.
#
# What was installed is recorded in __INFO__ (and a sha256 manifest beside
# it); ` + "`dserv-ptp-setup status`" + ` reports it, and any file changed since.
#
# DSERV_PTP_DESTDIR=<dir> writes the files under <dir> and touches nothing
# else (no apt, no systemctl, no role) -- a dry run you can diff.
set -euo pipefail

DESTDIR="${DSERV_PTP_DESTDIR:-}"

# curl or wget, whichever the host has: a minimal Debian ships wget and not
# curl (the psychophysics tracker), so the re-fetch below must not assume one.
fetch() {
    if command -v curl &>/dev/null; then curl -fsSL "$1"
    elif command -v wget &>/dev/null; then wget -qO- "$1"
    else return 1
    fi
}

if [[ -z "$DESTDIR" && $EUID -ne 0 ]]; then
    if command -v sudo &>/dev/null; then
        echo "[info] Re-running with sudo..."
        # Re-fetch rather than substituting a possibly-empty download straight
        # into bash -c -- same reasoning as the box bootstrap's check_root.
        script=$(fetch "__BASE__/ptp/setup") || script=""
        if [[ -z "$script" || "${script:0:2}" != '#!' ]]; then
            echo "Could not re-fetch the installer from __BASE__ to run as root -- re-run this command under sudo yourself" >&2
            exit 1
        fi
        # sudo resets the environment; carry the one knob through explicitly.
        exec sudo env DSERV_PTP_NO_RESTART="${DSERV_PTP_NO_RESTART:-}" bash -c "$script" ptp-setup "$@"
    fi
    echo "This installer must run as root" >&2
    exit 1
fi

# linuxptp is what the units exec; ethtool is how the timestamping clock is
# checked and selected on multi-PHC NICs. Skipped when both are present, and
# for the ntp-client tier, which is chrony and needs neither (a console whose
# NIC cannot timestamp should not grow a PTP stack to follow NTP).
if [[ -z "$DESTDIR" && "${1:-}" != ntp-client ]]; then
    if [[ ! -x /usr/sbin/ptp4l || ! -x /usr/sbin/phc2sys ]] || ! command -v ethtool &>/dev/null; then
        echo "[info] Installing linuxptp + ethtool..."
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq || true
        # Wait for the dpkg lock instead of failing on it -- a bootstrap-driven
        # role apply overlaps the deferred dserv-agent migration's apt run.
        apt-get install -y -o DPkg::Lock::Timeout=300 linuxptp ethtool
    fi
fi

install -d "${DESTDIR}/usr/local/dserv/scripts" "${DESTDIR}/usr/local/dserv/systemd" "${DESTDIR}/etc/systemd/system"

# Every file is staged first and installed only if it differs, so a re-run on
# a current box rewrites nothing and restarts nothing.
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
CHANGED=" "
put() {
    local name=$1 dest=$2 mode=$3
    local target="${DESTDIR}${dest}"
    if [[ -f "$target" ]] && cmp -s "$STAGE/$name" "$target"; then
        chmod "$mode" "$target"
        echo "  unchanged  $dest"
    else
        if [[ -e "$target" ]]; then echo "  updated    $dest"; else echo "  installed  $dest"; fi
        install -m "$mode" "$STAGE/$name" "$target"
        CHANGED+="$name "
    fi
}
changed() { [[ "$CHANGED" == *" $1 "* ]]; }

echo "[info] PTP tooling __TOOLING_ID__ from __BASE__"
`

const ptpSetupTail = `
# Record what is installed: an id for the set, and a manifest that lets
# ` + "`dserv-ptp-setup status`" + ` notice a file changed since (a dserv package
# upgrade, or a hand edit) instead of vouching for bytes it never checked.
install -m 0644 "$STAGE/ptp-tooling.sha256" "${DESTDIR}__SUMS__"
{
    echo "id=__TOOLING_ID__"
    echo "source=__BASE__/ptp/setup"
    echo "agent=__AGENT__"
    echo "applied=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "${DESTDIR}__INFO__"
chmod 0644 "${DESTDIR}__INFO__"

if [[ "$CHANGED" == " " ]]; then
    echo "[ok] PTP tooling __TOOLING_ID__ already current -- nothing rewritten"
else
    echo "[ok] PTP tooling __TOOLING_ID__ installed"
fi
if [[ -n "$DESTDIR" ]]; then
    echo "[info] DSERV_PTP_DESTDIR set -- skipped apt, daemon-reload, restarts, and role assignment"
    exit 0
fi
systemctl daemon-reload

# A drop-in that replaces ExecStart= freezes the whole command line, so the
# unit's own per-host choices (hwts_filter, the PTPv2.0 wire pin) never reach
# ptp4l -- the interim hwts-normal.conf / minor-version.conf fixes on the
# psychophysics rig were exactly this. Warn; removing one is the operator's call.
for d in /etc/systemd/system/dserv-ptp4l*@*.service.d/*.conf          /etc/systemd/system/dserv-phc2sys*@*.service.d/*.conf; do
    [[ -f "$d" ]] || continue
    if grep -q '^[[:space:]]*ExecStart=' "$d"; then
        echo "[warn] $d replaces ExecStart= and bypasses the unit's own options."
        echo "       If it predates this tooling:  rm $d && systemctl daemon-reload"
    fi
done

# Restart what runs an old version. ptp4l instances pick up a new unit OR a new
# PHC selector (its ExecStartPre); phc2sys only its own unit.
# Never fails: an error here must not end the script under set -e/pipefail.
active_instances() {
    { systemctl list-units --state=active --no-legend --plain "$1" 2>/dev/null || true; } | awk '{print $1}'
}
RESTART=""
if changed dserv-ptp-select-phc || changed dserv-ptp4l@.service; then
    RESTART+=" $(active_instances 'dserv-ptp4l@*.service')"
fi
if changed dserv-ptp-select-phc || changed dserv-ptp4l-client@.service; then
    RESTART+=" $(active_instances 'dserv-ptp4l-client@*.service')"
fi
if changed dserv-phc2sys@.service; then
    RESTART+=" $(active_instances 'dserv-phc2sys@*.service')"
fi
if changed dserv-phc2sys-client@.service; then
    RESTART+=" $(active_instances 'dserv-phc2sys-client@*.service')"
fi
# RESTART is a space-separated unit list; word-splitting it is the point.
# shellcheck disable=SC2086,SC2116
RESTART=$(echo $RESTART)
if [[ -n "$RESTART" ]]; then
    if [[ -n "${DSERV_PTP_NO_RESTART:-}" ]]; then
        echo "[info] DSERV_PTP_NO_RESTART set -- these still run the OLD version until restarted:"
        echo "       systemctl restart $RESTART"
    else
        echo "[info] Restarting so the update takes effect (PTP clients re-lock): $RESTART"
        # shellcheck disable=SC2086
        systemctl restart $RESTART
    fi
fi
if changed chrony-grandmaster.conf && [[ -n "$(active_instances 'dserv-ptp4l@*.service')" ]]; then
    echo "[info] chrony-grandmaster.conf changed; the live copy is only refreshed by re-running"
    echo "       dserv-ptp-setup grandmaster IFACE"
fi

if [[ $# -gt 0 ]]; then
    exec /usr/local/dserv/scripts/dserv-ptp-setup "$@"
fi
echo ""
# Not every box has a box.conf -- the hosts this installer exists for (a
# tracker, a console) usually do not -- and under pipefail a missing file
# fails the pipeline and set -e ends the script right here, silently.
declared=""
if [[ -f /etc/dserv-agent/box.conf ]]; then
    declared=$(sed -n 's/^time_role=//p' /etc/dserv-agent/box.conf | head -1)
fi
if [[ -n "$declared" || -n "$(systemctl list-units --all --no-legend --plain 'dserv-ptp4l*@*.service' 2>/dev/null || true)" ]]; then
    echo "Existing time role left as it is${declared:+ (declared: $declared)}."
else
    echo "No role assigned -- that stays a deliberate step:"
    echo "  dserv-ptp-setup candidates          # interfaces that can actually do PTP"
    echo "  dserv-ptp-setup client IFACE        # follow the site's grandmaster"
    echo "  dserv-ptp-setup grandmaster IFACE   # define the site's time (ONE host per site)"
    echo "  dserv-ptp-setup ntp-client SERVER   # follow site time over NTP (no PTP NIC needed)"
fi
echo ""
/usr/local/dserv/scripts/dserv-ptp-setup status || true
`
