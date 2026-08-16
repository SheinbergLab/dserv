// bootstrap.go - Bootstrap endpoint for provisioning new dserv boxes
//
// Serves a shell script at GET /setup that bootstraps a clean RPi (or similar)
// into a fully configured dserv data acquisition box.
//
// Usage from a fresh install:
//   curl -sSL http://server/setup | bash
//   curl -sSL http://server/setup?profile=incage | bash
//   curl -sSL http://server/setup?components=dserv,dlsh | bash
//   curl -sSL http://server/setup | bash -s -- --workgroup mylab
//
// Profiles define which components to install. The special "all" profile
// (and the default "incage" profile) installs everything.

package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"runtime"
	"strings"
	"text/template"
)

// BootstrapProfile defines a named set of components to install
type BootstrapProfile struct {
	Name        string   `json:"name"`
	Description string   `json:"description"`
	Components  []string `json:"components"` // component IDs; empty or ["*"] means all

	// StimMode is how the display program should RUN, as opposed to which
	// components get installed -- the first thing a profile has needed to say
	// that is not a component list. "windowed" means a desktop development box:
	// leave the cage unit (own X server on tty1) disabled and install a
	// launcher instead. Empty means the cage default, fullscreen via systemd.
	StimMode string `json:"stimMode,omitempty"`
}

// BootstrapConfig holds parameters for generating the bootstrap script
type BootstrapConfig struct {
	ServerURL        string
	DefaultWG        string
	Version          string
	ComponentsJSON   string // filtered component list as JSON, with resolved release assets
	AgentReleaseJSON string // dserv-agent release {tag, assets:[{name,url}]} as JSON
	ProfileName      string
	StimMode         string // "" = cage default (fullscreen service); "windowed" = dev

	// ExplicitComponents is the raw ?components= list, empty when provisioning
	// by profile. The script forwards it on the sudo re-fetch, which used to
	// carry only ?profile= and silently resolved to the full incage set.
	ExplicitComponents string

	// RetireServices is a space-separated list of units owned by components
	// this profile deliberately EXCLUDES. Provisioning by profile declares
	// what the box IS, so a unit left enabled by its previous class (stim2 on
	// a retyped dev Pi, dserv on a box demoted to a display) is drift the
	// script turns off. Empty for ?components= runs, which are additive by
	// intent and retire nothing.
	RetireServices string
}

// bootstrapAsset is a release asset pre-resolved to a direct download URL.
// The URL points at GitHub's CDN, which is not rate-limited like the API.
type bootstrapAsset struct {
	Name string `json:"name"`
	URL  string `json:"url"`
}

// bootstrapComponent is a Component plus its latest release, resolved
// server-side so the install script never has to call api.github.com.
type bootstrapComponent struct {
	Component
	LatestTag string           `json:"latestTag,omitempty"`
	Assets    []bootstrapAsset `json:"resolvedAssets,omitempty"`
}

// resolveReleaseAssets looks up the latest release for a repo (via the
// shared cache) and flattens its assets to {name, url}. Returns empty
// values on failure — the bootstrap script falls back to GitHub directly.
func (a *Agent) resolveReleaseAssets(repo string) (string, []bootstrapAsset) {
	if repo == "" {
		return "", nil
	}
	release := a.getLatestRelease(repo)
	if release == nil {
		return "", nil
	}
	assets := make([]bootstrapAsset, 0, len(release.Assets))
	for _, asset := range release.Assets {
		assets = append(assets, bootstrapAsset{Name: asset.Name, URL: asset.DownloadURL})
	}
	return release.TagName, assets
}

// Default profiles - can be overridden via profiles.json alongside components.json
var defaultProfiles = []BootstrapProfile{
	{
		Name:        "incage",
		Description: "Full in-cage data acquisition box (dserv + stim2 + dlsh)",
		Components:  []string{"*"},
	},
	{
		// Alias for incage: a development box wants all three components too.
		// Kept as its own entry rather than a name-matching special case so it
		// shows up in /setup/profiles and reads as a deliberate choice at the
		// call site (?profile=dev) instead of "the in-cage one, on my desk".
		Name:        "dev",
		Description: "Development box (dserv + stim2 + dlsh; stim2 windowed)",
		Components:  []string{"*"},
		StimMode:    "windowed",
	},
	{
		// The other half of a split rig: control on one box, display on
		// another. dlsh is listed explicitly even though resolveWithDeps
		// would pull it in via stim2's depends -- this profile's whole point
		// is being read at a glance by someone provisioning a display box.
		Name:        "stim",
		Description: "Display box only (stim2 + dlsh, no dserv)",
		Components:  []string{"stim2", "dlsh"},
	},
	{
		Name:        "server",
		Description: "Data server only (dserv + dlsh, no stimulus)",
		Components:  []string{"dserv", "dlsh"},
	},
	{
		Name:        "minimal",
		Description: "Minimal install (dserv + dlsh, agent only)",
		Components:  []string{"dserv", "dlsh"},
	},
}

// profileStimMode reports how this profile wants the display program to run.
// Unknown or unnamed profiles get the cage default, which is the safe answer:
// a box that should have been windowed merely runs fullscreen, whereas the
// reverse would leave a cage box with no display at all.
func (a *Agent) profileStimMode(profileName string) string {
	for _, p := range a.getProfiles() {
		if strings.EqualFold(p.Name, profileName) {
			return p.StimMode
		}
	}
	return ""
}

// getProfiles returns the active profile list.
// For now returns defaults; later can load from a profiles.json file.
func (a *Agent) getProfiles() []BootstrapProfile {
	return defaultProfiles
}

// filterComponents returns the subset of components matching a profile or
// explicit component list. Returns all components if profile is "all" or
// the profile's component list contains "*".
func (a *Agent) filterComponents(profileName string, explicitComponents string) []Component {
	// Explicit component list takes priority
	if explicitComponents != "" {
		wanted := make(map[string]bool)
		for _, id := range strings.Split(explicitComponents, ",") {
			id = strings.TrimSpace(id)
			if id != "" {
				wanted[id] = true
			}
		}
		// Always include dependencies of wanted components
		return a.resolveWithDeps(wanted)
	}

	// Profile lookup. Every all-components path filters to nonOptional:
	// profiles declare what a box IS, and optional add-ons (camera) are what
	// a box HAS -- they join only by explicit ?components= or panel install.
	if profileName == "" || profileName == "all" {
		return nonOptional(a.components)
	}

	for _, p := range a.getProfiles() {
		if strings.EqualFold(p.Name, profileName) {
			// "*" means all (non-optional)
			for _, c := range p.Components {
				if c == "*" {
					return nonOptional(a.components)
				}
			}
			wanted := make(map[string]bool)
			for _, id := range p.Components {
				wanted[id] = true
			}
			return a.resolveWithDeps(wanted)
		}
	}

	// Unknown profile - return all (non-optional) with a log
	return nonOptional(a.components)
}

// resolveWithDeps takes a set of wanted component IDs and adds any
// components they depend on (from the full component list).
//
// The agent is always added, whatever the profile asked for. step_install_agent
// puts one on every box by construction, so a profile that omitted it would be
// describing a box that does not exist -- and, worse, the filtered list is what
// lands in /etc/dserv-agent/components.json, so omitting it is precisely what
// would deny a display box the ability to update its own management plane.
func (a *Agent) resolveWithDeps(wanted map[string]bool) []Component {
	// By stable id, NOT isSelfComponent: that asks "is this the unit I am
	// running under", which is the right question when routing an install away
	// from this process, and the wrong one here -- a registry generates these
	// lists for other boxes, and its own unit name says nothing about theirs.
	wanted[agentComponentID] = true

	// Expand dependencies
	changed := true
	for changed {
		changed = false
		for _, comp := range a.components {
			if !wanted[comp.ID] {
				continue
			}
			for _, dep := range comp.Depends {
				if !wanted[dep] {
					wanted[dep] = true
					changed = true
				}
			}
		}
	}

	// Return in original order
	var result []Component
	for _, comp := range a.components {
		if wanted[comp.ID] {
			result = append(result, comp)
		}
	}
	return result
}

const bootstrapScriptTemplate = `#!/usr/bin/env bash
#
# dserv bootstrap - provision a data acquisition box
#
# Generated by dserv-agent {{.Version}} at {{.ServerURL}}
# Profile: {{.ProfileName}}
#
# Usage:
#   curl -sSL {{.ServerURL}}/setup | bash
#   curl -sSL {{.ServerURL}}/setup | bash -s -- --workgroup mylab
#
set -euo pipefail

# ============ Configuration ============

REGISTRY_URL="{{.ServerURL}}"
DEFAULT_WORKGROUP="{{.DefaultWG}}"
PROFILE="{{.ProfileName}}"
STIM_MODE="{{.StimMode}}"

# Non-empty when this invocation used an explicit ?components= list rather
# than a profile (PROFILE says "custom" then). Forwarded on the sudo re-fetch,
# which otherwise carries only ?profile= and loses the list.
COMPONENTS_ARG="{{.ExplicitComponents}}"

# Units owned by components this profile deliberately excludes, resolved by
# the registry. step_retire_services turns these OFF if it finds them; empty
# for ?components= runs. See that step for why.
RETIRE_SERVICES="{{.RetireServices}}"

DSERV_INSTALL_DIR="/usr/local/dserv"
LOG_FILE="/tmp/dserv-bootstrap-$(date +%Y%m%d-%H%M%S).log"

# Component definitions (filtered by profile, with release assets pre-resolved
# by the registry so installs hit GitHub's CDN, not the rate-limited API)
COMPONENTS_JSON='{{.ComponentsJSON}}'

# dserv-agent release, pre-resolved by the registry: {tag, assets:[{name,url}]}
AGENT_RELEASE_JSON='{{.AgentReleaseJSON}}'

# ============ Parse Arguments ============

ROLE=""
# Declared time role WITH its argument, e.g. "grandmaster eth0",
# "client eth0", "ntp-client 192.168.88.29". Recorded in box.conf and
# applied by step_time_role through the registry's /ptp/setup, so it works
# identically on a display box with no dserv package.
TIME_ROLE=""
WORKGROUP="${DEFAULT_WORKGROUP}"
DRY_RUN=false
SKIP_AGENT=false
SKIP_SCRIPTS=false
REINSTALL=false
ESS_USER_ARG=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --role)        ROLE="$2"; shift 2 ;;
        --time-role)   TIME_ROLE="$2"; shift 2 ;;
        --workgroup)   WORKGROUP="$2"; shift 2 ;;
        --user)        ESS_USER_ARG="$2"; shift 2 ;;
        --dry-run)     DRY_RUN=true; shift ;;
        --skip-agent)  SKIP_AGENT=true; shift ;;
        --skip-scripts) SKIP_SCRIPTS=true; shift ;;
        --reinstall)   REINSTALL=true; shift ;;
        --help|-h)
            echo "dserv bootstrap - provision a data acquisition box"
            echo ""
            echo "Usage: curl -sSL ${REGISTRY_URL}/setup | bash -s -- [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --role ROLE          Box role (e.g., eyetracker, stim, control)"
            echo "  --time-role \"R ARG\"  Declared time role, applied at the end:"
            echo "                       \"grandmaster IFACE\" | \"client IFACE\" |"
            echo "                       \"ntp-client SERVER\" (quote role+arg together;"
            echo "                       re-runs preserve it; \"none\" clears it)"
            echo "  --workgroup NAME     Workgroup name (default: ${DEFAULT_WORKGROUP})"
            echo "  --user USER          Local account owning the ESS systems tree"
            echo "                       (default: the user invoking sudo, else 'lab')"
            echo "  --dry-run            Show what would be done without making changes"
            echo "  --skip-agent         Skip dserv-agent install (components only)"
            echo "  --skip-scripts       Do not sync ESS scripts from the registry."
            echo "                       Use on any box with local work in"
            echo "                       ~/systems/ess: the sync unzips OVER that"
            echo "                       tree and the registry copy wins."
            echo "  --reinstall          Reinstall components even when already at the"
            echo "                       target version (repairs a broken install)"
            echo "  --help               Show this help"
            echo ""
            echo "Profiles (set via URL, e.g. /setup?profile=server):"
            echo "  incage    Full in-cage box (all components)"
            echo "  server    Data server only (no stimulus)"
            echo "  minimal   Minimal install (dserv + dlsh)"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ============ Helpers ============

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

info()  { echo -e "${BLUE}[info]${NC}  $*"; }
ok()    { echo -e "${GREEN}[ok]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[warn]${NC}  $*"; }
fail()  { echo -e "${RED}[fail]${NC}  $*"; exit 1; }

run() {
    if $DRY_RUN; then
        info "[dry-run] $*"
    else
        "$@" >> "$LOG_FILE" 2>&1
    fi
}

# run() cannot guard a shell redirection, so every "cat > /etc/..." in this
# script used to write for real even under --dry-run: a preview of what the
# bootstrap WOULD do quietly rewrote the agent's unit file and config on a live
# box. write_file is the redirect-shaped sibling of run -- content on stdin,
# path as the argument. (No backticks in here: this whole script is a Go raw
# string literal, and one would end it.)
#
# The dry branch still drains stdin. A heredoc feeding a command that never
# reads it leaves the writer blocked or killed by SIGPIPE, so "do nothing" has
# to mean "read it and throw it away".
write_file() {
    local path="$1"
    if $DRY_RUN; then
        local bytes
        bytes=$(cat | wc -c | tr -d ' ')
        info "[dry-run] write ${path} (${bytes} bytes)"
    else
        mkdir -p "$(dirname "$path")"
        cat > "$path"
    fi
}

# ---- Profile shape ----
#
# The profile is already resolved into COMPONENTS_JSON by the registry, so the
# script never has to know profile NAMES -- it asks what it is actually
# installing. That keeps a display box (profile=stim: stim2 + dlsh, no dserv)
# from running the dserv-only steps, and lets a new profile work here for free.

# The agent is in EVERY profile's component list, so the panel on any box can
# update its own management plane. It is not installed or started like the
# others, though: step_install_agent owns it and runs first, so the steps below
# filter it out rather than doing the work twice.
AGENT_COMPONENT_ID="dserv-agent"

# The display program. Named here rather than matched inline because two
# separate decisions key off it: whether its service is enabled, and whether a
# windowed launcher is installed in its place.
STIM_COMPONENT_ID="stim2"

has_component() {
    echo "$COMPONENTS_JSON" | jq -e --arg id "$1" \
        '.components[] | select(.id == $id)' &>/dev/null
}

# Services this box actually runs, in component order (e.g. "dserv", "stim2").
# Excludes the agent: it is the thing doing the managing, not a thing being
# managed, and pinning --service to it would have the panel report the agent
# where the box's actual payload service belongs.
profile_services() {
    local exclude="$AGENT_COMPONENT_ID"
    # A windowed (development) box must NOT have stim2.service enabled. That
    # unit starts its OWN X server on tty1 as user stim and conflicts with
    # getty -- correct in a cage, a fight on a desktop already running a
    # compositor on graphical.target. Excluding it here keeps it out of both
    # the enable/start loop and the verify loop, so a bootstrap re-run stops
    # switching back on what someone deliberately turned off.
    if [[ "$STIM_MODE" == "windowed" ]]; then
        exclude="${exclude}|${STIM_COMPONENT_ID}"
    fi
    echo "$COMPONENTS_JSON" | jq -r --arg ex "$exclude" \
        '.components[] | select(.id | test("^(" + $ex + ")$") | not) | .service // empty'
}

# The unit the agent manages as its primary: dserv where there is one,
# otherwise the first component that has a service (stim2 on a display box).
primary_service() {
    if has_component dserv; then
        echo "dserv"
    else
        profile_services | head -1
    fi
}

# ---- Agent unit reconciliation ----
#
# The declared configuration wins. These used to be add-if-absent only, which
# meant a unit that already had --registry was pronounced "already configured"
# and a WRONG value survived every future bootstrap -- a workgroup typo, or the
# unsubstituted --service __STIM2_SERVICE__ we found on a deployed display box.
# Re-running the provisioner should converge the box, not ratify its drift.

# The current value of one ExecStart flag, or empty. Handles both layouts we
# ship: the multi-line continuation form this script writes, and a single-line
# ExecStart from a packaged unit. Values never contain spaces, so stopping at
# whitespace also stops safely before a trailing line-continuation.
agent_flag_value() {
    sed -nE "s|^.*[[:space:]]$1[[:space:]]+([^[:space:]]+).*$|\1|p" "$2" | head -1
}

# Set one ExecStart flag to the declared value: replace in place if present,
# append after the binary if not, and say so either way. Silent when it already
# matches -- the common case, and not worth a line of output.
set_agent_flag() {
    local f="$1" flag="$2" want="$3" have
    have=$(agent_flag_value "$flag" "$f")
    [[ "$have" == "$want" ]] && return 0
    if [[ -z "$have" ]]; then
        run sed -i "s|ExecStart=.*dserv-agent|& ${flag} ${want}|" "$f"
        ok "  ${flag} ${want} (added)"
    else
        run sed -i -E "s|([[:space:]]${flag}[[:space:]]+)[^[:space:]]+|\1${want}|" "$f"
        ok "  ${flag}: ${have} -> ${want}"
    fi
}

check_not_registry() {
    [[ "${DSERV_BOOTSTRAP_FORCE:-}" == "1" ]] && return 0
    local status
    status=$(curl -sSL "http://localhost/setup/status" 2>/dev/null) || return 0
    if echo "$status" | jq -re '.mode == "server"' &>/dev/null; then
        fail "This appears to be the registry server — aborting. Set DSERV_BOOTSTRAP_FORCE=1 to override."
    fi
}

check_root() {
    if [[ $EUID -ne 0 ]]; then
        if command -v sudo &>/dev/null; then
            info "Re-running with sudo..."
            # An ARRAY, not a string: --time-role's value contains a space
            # ("client eth0"), and word-splitting a concatenated string would
            # hand the role and its argument to the parser as separate flags.
            # Every other flag rides along quoted correctly for free.
            local -a fwd=()
            [[ -n "$ROLE" ]]      && fwd+=(--role "$ROLE")
            [[ -n "$TIME_ROLE" ]] && fwd+=(--time-role "$TIME_ROLE")
            [[ -n "$WORKGROUP" ]] && fwd+=(--workgroup "$WORKGROUP")
            [[ -n "$ESS_USER_ARG" ]] && fwd+=(--user "$ESS_USER_ARG")
            $DRY_RUN              && fwd+=(--dry-run)
            $SKIP_AGENT           && fwd+=(--skip-agent)
            # Must be forwarded, like every other flag here: this branch
            # re-fetches and re-runs the script as root, so anything missed
            # is silently dropped -- and the one being dropped here protects
            # a tree of local work.
            $SKIP_SCRIPTS         && fwd+=(--skip-scripts)
            $REINSTALL            && fwd+=(--reinstall)

            # Re-fetch into a variable and CHECK IT, rather than substituting
            # the download straight into bash -c.
            #
            # Inline, a failed second fetch expands to the empty string and the
            # whole thing becomes "sudo bash -c '' -- args": does nothing,
            # exits 0. The operator sees "Re-running with sudo..." then silence
            # and a success status, having installed nothing. -sSL without -f
            # made that likelier still -- an HTTP 502 from the registry is
            # delivered as a body, so bash would have been handed an error page
            # to execute.
            #
            # -f turns an HTTP error into a curl failure; the shebang test
            # rejects anything that came back but is not this script (proxy
            # interstitial, captive portal, error page).
            # Forward the SHAPE of the original request too: an explicit
            # component list is not a profile, and re-fetching ?profile=custom
            # would resolve to the full set instead of the list the operator
            # typed.
            local refetch="${REGISTRY_URL}/setup?profile=${PROFILE}"
            [[ -n "$COMPONENTS_ARG" ]] && refetch="${REGISTRY_URL}/setup?components=${COMPONENTS_ARG}"
            local script=""
            script=$(curl -fsSL "$refetch") || script=""
            if [[ -z "$script" || "${script:0:2}" != '#!' ]]; then
                fail "Could not re-fetch the installer from ${REGISTRY_URL} to run as root — re-run this command under sudo yourself"
            fi
            exec sudo bash -c "$script" -- "${fwd[@]}"
        else
            fail "This script must be run as root"
        fi
    fi
}

# ============ Detection ============

KNOWN_CODENAMES="bullseye bookworm trixie forky jammy noble focal"

detect_platform() {
    ARCH=$(uname -m)
    case "$ARCH" in
        aarch64|arm64) PLATFORM="arm64" ;;
        armv7l|armhf)  PLATFORM="armhf" ;;
        x86_64)        PLATFORM="amd64" ;;
        *)             PLATFORM="$ARCH" ;;
    esac

    CODENAME=""
    if [[ -r /etc/os-release ]]; then
        CODENAME=$(. /etc/os-release && echo "${VERSION_CODENAME:-}")
    fi

    RPI_MODEL=""
    if [[ -f /proc/device-tree/model ]]; then
        RPI_MODEL=$(tr -d '\0' < /proc/device-tree/model)
    fi

    HOSTNAME_VAL=$(hostname)
    IP_ADDR=$(hostname -I 2>/dev/null | awk '{print $1}' || echo "unknown")

    echo ""
    info "Platform:  ${PLATFORM} (${ARCH})"
    [[ -n "$CODENAME" ]]  && info "Codename:  ${CODENAME}"
    info "Hostname:  ${HOSTNAME_VAL}"
    info "IP:        ${IP_ADDR}"
    [[ -n "$RPI_MODEL" ]] && info "Hardware:  ${RPI_MODEL}"
    [[ -n "$ROLE" ]]      && info "Role:      ${ROLE}"
    info "Workgroup: ${WORKGROUP}"
    info "Profile:   ${PROFILE}"
}

# ============ Component Install ============

# Fallback only: query GitHub directly and normalize to the same shape the
# registry inlines — {tag, assets:[{name,url}]}. Used when the registry could
# not pre-resolve a release (e.g. its cache was cold and GitHub was down).
github_release_fallback() {
    local repo="$1"
    curl -sSL "https://api.github.com/repos/${repo}/releases/latest" 2>/dev/null \
        | jq -c '{tag: .tag_name, assets: [.assets[] | {name: .name, url: .browser_download_url}]}' 2>/dev/null
}

# find_asset picks a download URL from a normalized assets array
# ([{name,url}, ...]) by pattern + arch.
find_asset() {
    local assets_json="$1"
    local pattern="$2"
    local arch="$3"

    # If the asset name embeds a known Debian/Ubuntu codename, only accept it
    # when that codename matches this host. Assets with no codename in the name
    # pass through (platform-independent). If $cn is empty (no /etc/os-release),
    # disable the filter entirely.
    local jq_codename_filter='
        ($known | split(" ")) as $cns
        | (.name | ascii_downcase) as $n
        | (([$cns[] as $c | select($n | contains($c)) | $c] | first) // "") as $assetCN
        | select($cn == "" or $assetCN == "" or $assetCN == ($cn | ascii_downcase))
    '

    # Arch-specific match first
    local url
    url=$(echo "$assets_json" | jq -r \
        --arg pat "$pattern" --arg arch "$arch" \
        --arg cn "$CODENAME" --arg known "$KNOWN_CODENAMES" \
        ".[] | select(.name | test(\$pat)) | select(.name | test(\$arch)) | ${jq_codename_filter} | .url" \
        | head -1)

    # Fall back to pattern only (arch-independent assets)
    if [[ -z "$url" || "$url" == "null" ]]; then
        url=$(echo "$assets_json" | jq -r \
            --arg pat "$pattern" \
            --arg cn "$CODENAME" --arg known "$KNOWN_CODENAMES" \
            ".[] | select(.name | test(\$pat)) | ${jq_codename_filter} | .url" \
            | head -1)
    fi

    echo "$url"
}

# What is installed right now, or empty. Mirrors how the agent reports a
# component's current version: a versionFile if the component declares one
# (dlsh), otherwise dpkg.
#
# For a dpkg component the STATUS matters as much as the version. A package can
# sit at the right version and still be half-configured -- the display box was
# found with "iF stim2 0.23.9", a failed postinst, and reinstalling is exactly
# what repaired it. Reporting a version for that state would make the skip
# below step over the one box that needed the work.
installed_version() {
    local comp_json="$1"
    local vfile pkg status_version
    vfile=$(echo "$comp_json" | jq -r '.versionFile // empty')
    pkg=$(echo "$comp_json" | jq -r '.package // empty')

    if [[ -n "$vfile" ]]; then
        [[ -f "$vfile" ]] && head -1 "$vfile" | tr -d '[:space:]'
        return 0
    fi
    if [[ -n "$pkg" ]]; then
        status_version=$(dpkg-query -W -f='${Status}|${Version}' "$pkg" 2>/dev/null || true)
        case "$status_version" in
            "install ok installed|"*) echo "${status_version#*|}" ;;
        esac
    fi
}

install_component() {
    local comp_json="$1"
    local comp_id=$(echo "$comp_json" | jq -r '.id')
    local comp_name=$(echo "$comp_json" | jq -r '.name')
    local repo=$(echo "$comp_json" | jq -r '.repo // empty')
    local asset_pattern=$(echo "$comp_json" | jq -r '.assetPattern // empty')
    local install_cmd=$(echo "$comp_json" | jq -r '.installCmd // empty')

    if [[ -z "$repo" ]]; then
        warn "${comp_id}: no repo, skipping"
        return 0
    fi

    info "Installing ${comp_name}..."

    # Release assets are normally pre-resolved by the registry and inlined
    # above, so this install touches GitHub's CDN only — no api.github.com call.
    local tag=$(echo "$comp_json" | jq -r '.latestTag // empty')
    local assets_json=$(echo "$comp_json" | jq -c '.resolvedAssets // empty')

    if [[ -z "$assets_json" || "$assets_json" == "null" ]]; then
        warn "  ${comp_id}: no pre-resolved release, querying GitHub..."
        local fallback
        fallback=$(github_release_fallback "$repo")
        if [[ -n "$fallback" && "$fallback" != "null" ]]; then
            tag=$(echo "$fallback" | jq -r '.tag // empty')
            assets_json=$(echo "$fallback" | jq -c '.assets')
        fi
    fi

    if [[ -z "$assets_json" || "$assets_json" == "null" ]]; then
        warn "${comp_id}: cannot determine release for ${repo}"
        return 1
    fi

    [[ -n "$tag" ]] && info "  Release: ${tag}"

    # Already at the target version: do nothing. Without this, re-running the
    # bootstrap to pick up a fix reinstalls every component, which stops and
    # starts their services -- on a display box that is a visible interruption
    # for no gain, and it is the only reason a re-run was disruptive rather
    # than merely idempotent. --reinstall forces the work anyway, which is the
    # escape hatch for a package that is present, correctly versioned, and
    # nonetheless broken.
    local have="" want="${tag#v}"
    have=$(installed_version "$comp_json")
    if [[ -n "$have" && "$have" == "$want" ]] && ! $REINSTALL; then
        ok "${comp_name} ${have} already current"
        return 0
    fi

    if [[ -z "$asset_pattern" ]]; then
        warn "${comp_id}: no assetPattern, skipping"
        return 1
    fi

    local asset_url
    asset_url=$(find_asset "$assets_json" "$asset_pattern" "$PLATFORM")

    if [[ -z "$asset_url" || "$asset_url" == "null" ]]; then
        warn "${comp_id}: no asset matching '${asset_pattern}' for ${PLATFORM}"
        return 1
    fi

    local asset_name=$(basename "$asset_url")
    local download_path="/tmp/${asset_name}"

    info "  Downloading ${asset_name}..."
    run curl -sSL -o "$download_path" "$asset_url"

    if [[ -n "$install_cmd" ]]; then
        local expanded="${install_cmd//\{file\}/$download_path}"
        expanded="${expanded//\{version\}/$tag}"
        info "  Custom install..."
        run bash -c "$expanded"
    elif [[ "$asset_name" == *.deb ]]; then
        info "  Installing .deb..."
        run dpkg -i "$download_path" || run apt-get install -f -y -qq -o DPkg::Lock::Timeout=300
    else
        warn "${comp_id}: no installCmd and not a .deb (${asset_name})"
        rm -f "$download_path"
        return 1
    fi

    rm -f "$download_path"
    ok "${comp_name} ${tag}"
    return 0
}

# ============ Steps ============

step_prerequisites() {
    info "Installing prerequisites..."
    # Always install (even in dry-run) — jq is needed by this script
    if ! command -v jq &>/dev/null; then
        apt-get update -qq >> "$LOG_FILE" 2>&1
        apt-get install -y -qq curl jq ca-certificates >> "$LOG_FILE" 2>&1
    fi
    ok "Prerequisites"
}

step_install_components() {
    local count=$(echo "$COMPONENTS_JSON" | jq '.components | length')
    local names=$(echo "$COMPONENTS_JSON" | jq -r '[.components[].name] | join(", ")')
    info "Components to install: ${names}"

    if [[ "$count" -eq 0 ]]; then
        warn "No components in profile"
        return
    fi

    # Dependency-ordered install
    local -a installed=()
    local -a remaining=()

    for i in $(seq 0 $((count - 1))); do
        remaining+=($i)
    done

    local max_passes=$((count + 1))
    local pass=0

    while [[ ${#remaining[@]} -gt 0 && $pass -lt $max_passes ]]; do
        local progress=false
        local -a next_remaining=()

        for i in "${remaining[@]}"; do
            local comp_json=$(echo "$COMPONENTS_JSON" | jq ".components[$i]")
            local comp_id=$(echo "$comp_json" | jq -r '.id')
            local deps=$(echo "$comp_json" | jq -r '.depends // [] | .[]' 2>/dev/null)

            local deps_met=true
            for dep in $deps; do
                local found=false
                for inst in "${installed[@]+"${installed[@]}"}"; do
                    [[ "$inst" == "$dep" ]] && found=true && break
                done
                $found || { deps_met=false; break; }
            done

            if $deps_met; then
                if [[ "$comp_id" == "$AGENT_COMPONENT_ID" ]]; then
                    # Already handled by step_install_agent. Counted as installed
                    # so the tally is honest and anything depending on it resolves.
                    installed+=("$comp_id")
                elif [[ $(echo "$comp_json" | jq -r '.optional // false') == "true" ]] \
                     && [[ -z $(installed_version "$comp_json") ]]; then
                    # Optional add-ons ride in the roster for the panel to offer,
                    # but a profile install never opts a box in. A box that DID
                    # opt in earlier has a version, and updates like anything else.
                    info "${comp_id}: optional add-on -- skipping (opt in from the agent panel)"
                else
                    install_component "$comp_json" && installed+=("$comp_id") || warn "Failed: ${comp_id}"
                fi
                progress=true
            else
                next_remaining+=($i)
            fi
        done

        remaining=("${next_remaining[@]+"${next_remaining[@]}"}")
        pass=$((pass + 1))

        if ! $progress && [[ ${#remaining[@]} -gt 0 ]]; then
            warn "Unresolvable dependencies, installing remaining"
            for i in "${remaining[@]}"; do
                local comp_json=$(echo "$COMPONENTS_JSON" | jq ".components[$i]")
                local comp_id=$(echo "$comp_json" | jq -r '.id')
                [[ "$comp_id" == "$AGENT_COMPONENT_ID" ]] && { installed+=("$comp_id"); continue; }
                if [[ $(echo "$comp_json" | jq -r '.optional // false') == "true" ]] \
                   && [[ -z $(installed_version "$comp_json") ]]; then
                    info "${comp_id}: optional add-on -- skipping (opt in from the agent panel)"
                    continue
                fi
                install_component "$comp_json" && installed+=("$comp_id") || warn "Failed: ${comp_id}"
            done
            break
        fi
    done

    echo ""
    ok "Installed ${#installed[@]}/${count}: ${installed[*]}"
}

step_install_agent() {
    if $SKIP_AGENT; then
        info "Skipping dserv-agent (--skip-agent)"
        return
    fi

    # No early-out on an existing binary. That check dated from when the agent
    # rode inside the dserv package, where "already there" meant "as new as
    # dserv is". It ships as its own dserv-agent deb now, so an agent already
    # on the box is exactly the one that may be stale -- skipping it is how a
    # display box ends up years behind with no way forward.
    info "Installing dserv-agent..."

    # Pre-resolved by the registry; falls back to GitHub if absent.
    local tag=$(echo "$AGENT_RELEASE_JSON" | jq -r '.tag // empty')
    local assets_json=$(echo "$AGENT_RELEASE_JSON" | jq -c '.assets // empty')

    if [[ -z "$assets_json" || "$assets_json" == "null" ]]; then
        local fallback
        # SheinbergLab/dserv, not SheinbergLab/dserv-agent: the agent has no
        # repo of its own and ships as an asset of the dserv release. The other
        # spelling 404s, so this fallback -- the one that runs precisely when
        # the registry could not resolve the release -- was guaranteed to fail
        # too. Same defect as the registry-side agentRepo, one layer down.
        fallback=$(github_release_fallback "SheinbergLab/dserv")
        if [[ -n "$fallback" && "$fallback" != "null" ]]; then
            tag=$(echo "$fallback" | jq -r '.tag // empty')
            assets_json=$(echo "$fallback" | jq -c '.assets')
        fi
    fi

    if [[ -z "$assets_json" || "$assets_json" == "null" ]]; then
        warn "Cannot fetch dserv-agent release"
        return
    fi

    [[ -n "$tag" ]] && info "  Release: ${tag}"

    # Already at this version: nothing to do. Deliberately keyed on the PACKAGE,
    # so a box still carrying the loose pre-split binary reports nothing here
    # and gets migrated -- which is the case this step exists for. Only a
    # properly installed package short-circuits it, for the same reason
    # install_component checks dpkg status and not just a version string.
    local have="" want="${tag#v}"
    have=$(installed_version '{"package":"'"$AGENT_COMPONENT_ID"'"}')
    if [[ -n "$have" && "$have" == "$want" ]] && ! $REINSTALL; then
        ok "dserv-agent ${have} already current"
        return 0
    fi

    local deb_url
    deb_url=$(find_asset "$assets_json" "dserv-agent.*\\.deb$" "$PLATFORM")

    if [[ -n "$deb_url" && "$deb_url" != "null" ]]; then
        local deb_file="/tmp/dserv-agent-${tag}.deb"
        run curl -sSL -o "$deb_file" "$deb_url"
        run dpkg -i "$deb_file" || run apt-get install -f -y -qq -o DPkg::Lock::Timeout=300
        rm -f "$deb_file"
    else
        local bin_url
        bin_url=$(find_asset "$assets_json" "dserv-agent.*linux.*${PLATFORM}" "$PLATFORM")
        if [[ -z "$bin_url" || "$bin_url" == "null" ]]; then
            warn "No dserv-agent package for ${PLATFORM}"
            return
        fi
        run curl -sSL -o /usr/local/bin/dserv-agent "$bin_url"
        run chmod +x /usr/local/bin/dserv-agent
    fi

    ok "dserv-agent ${tag}"
}

step_configure_agent() {
    if $SKIP_AGENT; then return; fi

    info "Configuring dserv-agent for mesh..."

    local svc_file="/etc/systemd/system/dserv-agent.service"
    local svc_primary
    svc_primary=$(primary_service)

    # Pin the agent to what this box actually has. Without these two flags it
    # falls back to --service dserv and the built-in component list, so a
    # display box would report a dserv that isn't there and offer to install
    # one. The unit name stays dserv-agent everywhere -- the flavour lives in
    # the flags, not in a second unit to keep in step.
    echo "$COMPONENTS_JSON" | jq '.' | write_file /etc/dserv-agent/components.json
    ok "agent components: $(profile_services | tr '\n' ' ')(primary: ${svc_primary:-none})"

    if [[ ! -f "$svc_file" ]]; then
        # No service file (standalone install) — create one
        write_file "$svc_file" <<EOF
[Unit]
Description=dserv management agent
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/local/bin/dserv-agent \\
    --registry ${REGISTRY_URL} \\
    --workgroup ${WORKGROUP} \\
    --service ${svc_primary} \\
    --components /etc/dserv-agent/components.json \\
    --listen 0.0.0.0:80
Restart=always
RestartSec=5
Environment=DSERV_AGENT_TOKEN=

[Install]
WantedBy=multi-user.target
EOF
        ok "dserv-agent service created"
    else
        # Unit already exists (deb-shipped, hand-rolled, or a previous run).
        # Converge its four managed flags on what this run declares, leaving
        # every other flag alone -- --no-tls, --allow-reboot and --listen are
        # site decisions this script has no business overwriting.
        #
        # Backed up first: a unit this script mangles is a box that cannot
        # start its own agent, which is the failure with no way back in.
        if ! $DRY_RUN; then
            cp -a "$svc_file" "${svc_file}.bootstrap-bak" 2>/dev/null || true
        fi
        ok "dserv-agent service exists — reconciling"
        set_agent_flag "$svc_file" --registry   "${REGISTRY_URL}"
        set_agent_flag "$svc_file" --workgroup  "${WORKGROUP}"
        set_agent_flag "$svc_file" --service    "${svc_primary}"
        set_agent_flag "$svc_file" --components /etc/dserv-agent/components.json
    fi

    run systemctl daemon-reload
    run systemctl enable dserv-agent
}

step_stim_launcher() {
    if [[ "$STIM_MODE" != "windowed" ]]; then
        # The reverse reclassification. A box leaving the windowed class
        # (dev -> incage, dev -> server) must not keep the development
        # launcher: it shadows nothing -- the cage unit execs
        # /usr/local/stim2/stim2 directly -- but a leftover "stim2" command
        # that opens a desktop window is exactly the identity confusion a
        # retype exists to end. The marker comment is the ownership test, so
        # a hand-written wrapper at the same path survives.
        if [[ -f /usr/local/bin/stim2 ]] && \
           grep -q "installed by the dserv bootstrap" /usr/local/bin/stim2 2>/dev/null; then
            info "Removing windowed stim2 launcher (this box is no longer windowed)"
            run rm -f /usr/local/bin/stim2
        fi
        return
    fi
    if ! has_component "$STIM_COMPONENT_ID"; then
        return
    fi

    info "Installing windowed stim2 launcher (development box)..."

    # Deliberately a command, not a service. stim2 is an X11 program, so on a
    # labwc desktop it runs through XWayland -- a system unit would have to
    # guess DISPLAY and XAUTHORITY out of a user session's runtime dir and
    # would break on the next logout. A script inherits all of that from
    # whoever runs it, and matches how development actually goes: stim2 stopped
    # and restarted constantly, with its output in front of you.
    #
    # No -F, and no xrandr --rate: forcing 120 Hz is right for a cage display
    # and wrong for whatever monitor is on a desk.
    write_file /usr/local/bin/stim2 <<'LAUNCHER'
#!/bin/sh
# Windowed stim2 for a development box (installed by the dserv bootstrap).
#
# Runs in the session that invokes it, so DISPLAY / XWayland come from your
# desktop. The cage unit stim2.service -- which starts its own X server on
# tty1 -- is deliberately left disabled on this kind of box.
#
#   stim2                          1024x768 window
#   STIM2_WIDTH=1280 STIM2_HEIGHT=1024 stim2
#   stim2 -F                       fullscreen anyway; extra args win
#
# For ESS to drive this rather than a remote display, point ess/rmt_host at
# localhost -- that is a per-session choice, so provisioning does not set it.
exec /usr/local/stim2/stim2 \
    -w "${STIM2_WIDTH:-1024}" -h "${STIM2_HEIGHT:-768}" \
    -f /usr/local/stim2/config/linux.cfg "$@"
LAUNCHER
    run chmod 0755 /usr/local/bin/stim2
    ok "stim2 (windowed) -- run 'stim2' from a desktop terminal"

    # Reclassification, not merely installation.
    #
    # Every other step here only ever turns things ON: it enables what the new
    # profile declares and never touches what a previous class left running.
    # That is fine everywhere except here. Converting an incage box to dev
    # would otherwise leave the cage unit enabled and holding tty1 with its own
    # X server, while ALSO installing the windowed launcher -- both display
    # paths at once, which is the exact confusion this profile exists to end.
    # Choosing "windowed" is a statement that the cage unit must not run.
    local stim_svc
    stim_svc=$(echo "$COMPONENTS_JSON" | jq -r --arg id "$STIM_COMPONENT_ID" \
        '.components[] | select(.id == $id) | .service // empty')
    [[ -z "$stim_svc" ]] && return

    if systemctl is-enabled "$stim_svc" &>/dev/null || \
       systemctl is-active --quiet "$stim_svc" 2>/dev/null; then
        warn "Disabling ${stim_svc}: it runs fullscreen on its own X server, which a windowed box must not do"
        run systemctl disable --now "$stim_svc" || \
            warn "  could not disable ${stim_svc}; do it by hand before using the launcher"
    fi
}

step_configure_dserv() {
    if ! has_component dserv; then
        info "No dserv in profile ${PROFILE} — skipping dserv config"
        return
    fi

    info "Configuring dserv..."

    write_file "${DSERV_INSTALL_DIR}/etc/mesh.conf" <<EOF
# Auto-generated by dserv bootstrap
registry ${REGISTRY_URL}
workgroup ${WORKGROUP}
EOF

    if [[ -n "$ROLE" ]]; then
        write_file "${DSERV_INSTALL_DIR}/etc/role.conf" <<EOF
role ${ROLE}
EOF
        info "  Role: ${ROLE}"
    fi

    ok "dserv configured"
}

step_sync_scripts() {
    # This step unzips the registry's export OVER ~/systems/ess with -o, so the
    # registry copy wins every collision. That is right for a box being
    # provisioned and wrong for one already in service: the .sync_base.json
    # files in that tree exist because a 3-way conflict-detecting sync owns it,
    # and this step knows nothing about them. --skip-scripts is what makes a
    # bootstrap re-run safe on a box with local work.
    if $SKIP_SCRIPTS; then
        info "Skipping ESS script sync (--skip-scripts)"
        return
    fi

    # ESS systems are run by dserv; a display box has nothing to run them with.
    if ! has_component dserv; then
        info "No dserv in profile ${PROFILE} — skipping ESS script sync"
        return
    fi

    info "Syncing ESS scripts from registry..."

    # The export endpoint returns a zip of all systems + libs for the workgroup
    local export_url="${REGISTRY_URL}/api/v1/ess/export/${WORKGROUP}"
    local zip_file="/tmp/ess-export-${WORKGROUP}.zip"

    # Check if the registry has ESS scripts for this workgroup
    local resp_code
    resp_code=$(curl -sSL -o /dev/null -w "%{http_code}" "$export_url" 2>/dev/null || echo "000")

    if [[ "$resp_code" != "200" ]]; then
        info "No ESS scripts available for ${WORKGROUP} (HTTP ${resp_code}), skipping"
        return
    fi

    run curl -sSL -o "$zip_file" "$export_url"

    if [[ ! -f "$zip_file" ]] || [[ ! -s "$zip_file" ]]; then
        warn "Empty or missing export zip, skipping script sync"
        return
    fi

    # Scripts live in <ess user>/systems/ess/, owned by that user. The user
    # resolves --user flag > $SUDO_USER (whoever invoked sudo) > legacy 'lab',
    # and the home dir comes from getent, never from a hardcoded /home path --
    # the /home/lab hardcode silently skipped the sync on any box provisioned
    # from a personal account.
    local ess_user="${ESS_USER_ARG:-${SUDO_USER:-lab}}"
    local user_home
    user_home=$(getent passwd "$ess_user" | cut -d: -f6)

    if [[ -z "$user_home" || ! -d "$user_home" ]]; then
        warn "No home directory for user '${ess_user}' (--user to override), skipping script sync"
        rm -f "$zip_file"
        return
    fi
    local dest="${user_home}/systems/ess"

    # Say what is about to be replaced. unzip -o is silent about collisions, so
    # without this the log of a destructive step looks identical to a first
    # install -- and "how many scripts were already there" is the one number
    # you want when someone asks what happened to their edits.
    local existing
    existing=$(find "$dest" -type f \( -name "*.tcl" -o -name "*.tm" \) 2>/dev/null | wc -l)
    if [[ "$existing" -gt 0 ]]; then
        warn "Overwriting ${existing} existing scripts in ${dest} (--skip-scripts to keep them)"
    fi

    run mkdir -p "$dest"
    run unzip -o "$zip_file" -d "$dest"
    rm -f "$zip_file"

    # dserv runs as root but the ess user keeps ownership of their tree
    run chown -R "${ess_user}:${ess_user}" "$dest" || true

    local file_count
    file_count=$(find "$dest" -type f \( -name "*.tcl" -o -name "*.tm" \) 2>/dev/null | wc -l)
    ok "Synced ${file_count} scripts to ${dest}"
}

step_power_mgmt() {
    info "Disabling network power management (wired EEE + Wi-Fi powersave)..."

    # Both were found ON by default on production Pis (2026-08-03) and both
    # sit directly on paths this platform cares about: EEE's low-power-idle
    # wake latency jitters PTP and burst delivery on the wired port, and
    # brcmfmac Wi-Fi powersave is a known goes-deaf/drops-association cause
    # (worst at in-cage signal levels), compounded by NetworkManager giving
    # up re-association after 4 tries. Canonical copies + full rationale:
    # dserv repo systemd/dserv-disable-eee@.service + wifi-powersave-off.conf.
    #
    # Applying the EEE setting renegotiates the link (~5 s) -- fine during
    # provisioning, which is why this runs before services start.

    if ! $DRY_RUN; then
        write_file /etc/systemd/system/dserv-disable-eee@.service <<'UNIT'
[Unit]
# Force Energy-Efficient Ethernet OFF on %i at every boot. LPI wake latency
# sits on the PTP/box path; the distro default is not off. Host-side disable
# is sufficient for the link (EEE is negotiated). Tolerant ExecStart: a NIC
# without EEE already IS off. Canonical copy: dserv repo systemd/.
Description=Disable Energy-Efficient Ethernet on %i (latency/PTP jitter)
ConditionPathExists=/sys/class/net/%i
After=sys-subsystem-net-devices-%i.device

[Service]
Type=oneshot
ExecStart=-/usr/sbin/ethtool --set-eee %i eee off
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
UNIT
    fi
    run systemctl daemon-reload

    local wired
    wired=$(ip -o link show 2>/dev/null | awk -F': ' '$2 ~ /^(eth|en)/ {print $2; exit}')
    if [[ -n "$wired" ]]; then
        run systemctl enable --now "dserv-disable-eee@${wired}"
        ok "EEE off on ${wired} (persists across boots)"
    else
        info "No wired interface detected; enable later: systemctl enable --now dserv-disable-eee@<iface>"
    fi

    if [[ -d /etc/NetworkManager/conf.d ]]; then
        if ! $DRY_RUN; then
            write_file /etc/NetworkManager/conf.d/wifi-powersave-off.conf <<'NMCONF'
# Installed by the dserv bootstrap. Canonical copy + rationale: dserv repo
# systemd/wifi-powersave-off.conf. [connection] keys are NM-level DEFAULTS,
# so netplan profile regeneration cannot revert them.
[connection]
# 2 = disable Wi-Fi power save (brcmfmac goes-deaf/drop syndrome)
wifi.powersave = 2
# 0 = retry association forever (NM default blocks the profile after 4 tries)
connection.autoconnect-retries = 0
NMCONF
        fi
        run systemctl reload NetworkManager
        local wifi
        wifi=$(ip -o link show 2>/dev/null | awk -F': ' '$2 ~ /^wl/ {print $2; exit}')
        if [[ -n "$wifi" ]] && command -v iw &>/dev/null; then
            run iw dev "$wifi" set power_save off
        fi
        ok "Wi-Fi powersave off + retry-forever (NetworkManager conf.d)"
    else
        info "No NetworkManager conf.d (dhcpcd-era OS): add 'iw dev wlan0 set power_save off' to a boot script if Wi-Fi matters"
    fi
}

step_retire_services() {
    # Turn OFF what the profile excludes. Every install step only ever turns
    # things on, so without this a retype leaves the previous class running
    # underneath the new one: the old dev Pi still starting fullscreen stim2
    # on every boot, the box demoted to a display still running dserv.
    # RETIRE_SERVICES is resolved by the registry from the components this
    # profile does NOT include, and is empty for explicit ?components= runs.
    # (The windowed case is different -- stim2 IS in the dev profile, its
    # service just must not run -- and stays with step_stim_launcher and
    # profile_services.)
    local svc
    for svc in $RETIRE_SERVICES; do
        if systemctl is-enabled "$svc" &>/dev/null || \
           systemctl is-active --quiet "$svc" 2>/dev/null; then
            warn "Disabling ${svc}: the ${PROFILE} profile does not include it"
            run systemctl disable --now "$svc" || \
                warn "  could not disable ${svc}; do it by hand: systemctl disable --now ${svc}"
        fi
    done
}

step_record_identity() {
    # A re-run WITHOUT --role/--time-role PRESERVES what box.conf already
    # declares: an absent flag means "no change", not "erase" -- otherwise a
    # routine components-only retype would silently strip declarations made
    # earlier (and a preserved time_role also re-applies below, which is the
    # convergence re-runs promise). Pass the literal word "none" to
    # deliberately clear one.
    local prev="${DSERV_BOX_CONF:-/etc/dserv-agent/box.conf}"
    if [[ "$ROLE" == none ]]; then
        ROLE=""
    elif [[ -z "$ROLE" && -f "$prev" ]]; then
        ROLE=$(sed -n 's/^role=//p' "$prev" | head -1)
    fi
    if [[ "$TIME_ROLE" == none ]]; then
        TIME_ROLE=""
    elif [[ -z "$TIME_ROLE" && -f "$prev" ]]; then
        TIME_ROLE=$(sed -n 's/^time_role=//p' "$prev" | head -1)
        [[ -n "$TIME_ROLE" ]] && info "Preserving declared time role: ${TIME_ROLE} (pass --time-role none to clear)"
    fi

    # The durable record of what this box was provisioned AS. Every other
    # trace of the profile is a side effect -- which packages landed, which
    # units are enabled -- so before this file existed a box could not be
    # asked what it is, and a repurposed one kept its old behavior with
    # nothing to flag the mismatch. Re-running the bootstrap with a different
    # ?profile= rewrites it, which makes that re-run the retype operation of
    # record. The agent reads this file and reports it on /api/status.
    local installed_components
    installed_components=$(echo "$COMPONENTS_JSON" | jq -r '[.components[].id] | join(",")')
    write_file /etc/dserv-agent/box.conf <<EOF
# Declared identity for this box -- written by the dserv bootstrap.
# Retype: curl -sSL ${REGISTRY_URL}/setup?profile=<name> | bash -s -- --skip-scripts
# (--skip-scripts keeps local work in ~/systems/ess)
#
# stim_mode "" = cage default (stim2.service fullscreen on its own X server);
# "windowed" = development launcher, fullscreen unit disabled.
profile=${PROFILE}
stim_mode=${STIM_MODE}
components=${installed_components}
workgroup=${WORKGROUP}
registry=${REGISTRY_URL}
role=${ROLE}
time_role=${TIME_ROLE}
provisioned=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF
    ok "Recorded identity: profile=${PROFILE}${STIM_MODE:+ (${STIM_MODE})} in /etc/dserv-agent/box.conf"
}

step_start_services() {
    info "Starting services..."

    if ! $SKIP_AGENT; then
        # restart, not start: we just replaced /usr/local/bin/dserv-agent, and
        # start is a no-op on a unit that is already running -- which would
        # leave the OLD binary serving from an unlinked inode, so a bootstrap
        # re-run to update the agent would appear to work and change nothing.
        # restart also starts a stopped unit, so it covers the fresh case.
        run systemctl restart dserv-agent || warn "dserv-agent failed to start"
    fi

    # Start what this profile installed, not a hardcoded dserv. On a display
    # box that is stim2; starting (and verifying) dserv there reported a
    # failure for something that was never meant to exist.
    #
    # enable --now, not start. A package install deliberately does not enable
    # daemons (see dpkg/postinst), but provisioning a box is the opposite
    # decision: the whole point is that it comes back by itself after a power
    # cut. Starting without enabling is how the display box ended up one
    # reboot away from coming up dark -- running, but only until it wasn't.
    local svc
    for svc in $(profile_services); do
        run systemctl enable --now "$svc" || warn "${svc} failed to enable/start"
    done

    sleep 2

    for svc in $(profile_services); do
        if systemctl is-active --quiet "$svc" 2>/dev/null; then
            ok "${svc} running"
        else
            warn "${svc} not running (journalctl -u ${svc})"
        fi
    done

    if ! $SKIP_AGENT; then
        if systemctl is-active --quiet dserv-agent 2>/dev/null; then
            ok "dserv-agent running"
        else
            warn "dserv-agent not running (journalctl -u dserv-agent)"
        fi
    fi
}

step_time_role() {
    [[ -z "$TIME_ROLE" ]] && return

    # Converge the DECLARED time role, exactly as a human would: fetch the
    # registry's own /ptp/setup installer and hand it the declaration. That
    # detour is what makes this work on a display box with no dserv package,
    # and keeps role semantics in one place -- dserv-ptp-setup still owns
    # every preflight and refusal.
    #
    # Failures WARN, never abort: a box whose grandmaster happens to be down
    # at provision time is still correctly provisioned -- the declaration is
    # already recorded in box.conf, and the panel's "Apply declared" (or a
    # re-run) converges it later.
    local trole="${TIME_ROLE%% *}"
    local targ="${TIME_ROLE#* }"
    case "$trole" in
        grandmaster|client|ntp-client) ;;
        *) warn "Unknown time role '${trole}' (grandmaster|client|ntp-client) — recorded but not applied"; return ;;
    esac
    if [[ "$targ" == "$TIME_ROLE" || -z "$targ" ]]; then
        warn "Time role '${trole}' needs an argument (IFACE or SERVER) — recorded but not applied"
        return
    fi

    info "Applying declared time role: ${trole} ${targ}..."
    local ts
    ts=$(curl -fsSL "${REGISTRY_URL}/ptp/setup") || ts=""
    if [[ -z "$ts" || "${ts:0:2}" != '#!' ]]; then
        warn "Could not fetch ${REGISTRY_URL}/ptp/setup — apply later: dserv-ptp-setup ${trole} ${targ}"
        return
    fi
    if $DRY_RUN; then
        info "[dry-run] dserv-ptp-setup ${trole} ${targ}"
        return
    fi
    if bash -c "$ts" ptp-setup "$trole" "$targ"; then
        ok "Time role applied: ${trole} ${targ}"
    else
        warn "Time role apply did not complete — its own output above says why."
        warn "  The declaration is recorded; converge later with: dserv-ptp-setup ${trole} ${targ}"
    fi
}

step_verify() {
    info "Verifying registry..."
    local resp
    resp=$(curl -sSL -o /dev/null -w "%{http_code}" \
        "${REGISTRY_URL}/setup/status" 2>/dev/null || echo "000")

    if [[ "$resp" == "200" ]]; then
        ok "Registry reachable"
    else
        warn "Registry not reachable (HTTP ${resp}) - agent will retry"
    fi
}

# ============ Main ============

main() {
    check_not_registry
    check_root

    echo ""
    echo -e "${BOLD}╔══════════════════════════════════════════╗${NC}"
    echo -e "${BOLD}║       dserv bootstrap installer          ║${NC}"
    echo -e "${BOLD}╚══════════════════════════════════════════╝${NC}"
    echo -e "  registry:  ${REGISTRY_URL}"
    echo -e "  workgroup: ${WORKGROUP}"
    echo -e "  profile:   ${PROFILE}"
    $DRY_RUN && echo -e "  ${YELLOW}** DRY RUN **${NC}"
    echo ""

    detect_platform

    echo ""
    info "Log: ${LOG_FILE}"
    echo ""

    step_prerequisites
    # Management plane first. The dserv package now Depends on dserv-agent, so
    # unpacking dserv before the agent exists would fail outright -- and even
    # without that, an agent that is already up is what lets you reach a box
    # whose component install went wrong.
    step_install_agent
    step_configure_agent
    step_install_components
    step_stim_launcher
    step_configure_dserv
    step_sync_scripts
    step_power_mgmt
    # Retire before start: a unit the new profile excludes goes down before
    # anything it might race with comes up. Identity is recorded before the
    # services start so a failed start still leaves the box declared.
    step_retire_services
    step_record_identity
    step_start_services
    # After services: apt work from the component installs is done, so the
    # role's own apt (linuxptp/chrony) contends only with the deferred agent
    # migration -- both sides now wait on the dpkg lock instead of failing.
    step_time_role
    step_verify

    local ip_addr
    ip_addr=$(hostname -I 2>/dev/null | awk '{print $1}' || echo "localhost")

    echo ""
    echo -e "${BOLD}╔══════════════════════════════════════════╗${NC}"
    echo -e "${BOLD}║          Bootstrap complete!             ║${NC}"
    echo -e "${BOLD}╚══════════════════════════════════════════╝${NC}"
    echo ""
    ok "Agent UI:  http://${ip_addr}/"
    ok "Mesh:      ${REGISTRY_URL}/w/${WORKGROUP}"
    ok "Log:       ${LOG_FILE}"
    echo ""
}

main "$@"
`

var bootstrapTmpl *template.Template

func init() {
	bootstrapTmpl = template.Must(template.New("bootstrap").Parse(bootstrapScriptTemplate))
}

// GET /setup - serve the bootstrap install script
//
// Query parameters:
//   profile    - named profile (default: "incage")
//   components - explicit comma-separated component IDs (overrides profile)
//   workgroup  - override default workgroup
//   role       - set box role
//
// Examples:
//   curl -sSL http://server/setup | bash              # incage profile (all)
//   curl -sSL http://server/setup?profile=server | bash
//   curl -sSL http://server/setup?components=dserv,dlsh | bash
func (a *Agent) handleBootstrap(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	scheme := "http"
	if r.TLS != nil || r.Header.Get("X-Forwarded-Proto") == "https" {
		scheme = "https"
	}
	host := r.Host
	if host == "" {
		host = r.Header.Get("Host")
	}
	serverURL := fmt.Sprintf("%s://%s", scheme, host)

	workgroup := r.URL.Query().Get("workgroup")
	if workgroup == "" {
		workgroup = a.cfg.Workgroup
	}

	profileName := r.URL.Query().Get("profile")
	if profileName == "" {
		profileName = "incage"
	}
	explicitComponents := r.URL.Query().Get("components")

	// Filter components based on profile or explicit list
	components := a.filterComponents(profileName, explicitComponents)

	// Profile mode also CARRIES eligible optional add-ons (camera): they ride
	// into the box's /etc/dserv-agent/components.json so the panel can offer
	// them, but the install loop skips them unless already installed. Eligible
	// means every dependency is in the profile's set -- a display box without
	// dserv gets no camera row, matching the deb's own Depends. Explicit
	// ?components= lists stay exactly as named: naming an optional is the
	// opt-in.
	if explicitComponents == "" {
		included := make(map[string]bool, len(components))
		for _, c := range components {
			included[c.ID] = true
		}
		for _, c := range a.components {
			if !c.Optional || included[c.ID] {
				continue
			}
			eligible := true
			for _, dep := range c.Depends {
				if !included[dep] {
					eligible = false
					break
				}
			}
			if eligible {
				components = append(components, c)
			}
		}
	}

	// An explicit ?components= list is not a profile. Without this the banner,
	// the recorded identity, and the sudo re-fetch all claim "incage" for a
	// box that was given a hand-picked set.
	if explicitComponents != "" {
		profileName = "custom"
	}

	// Units owned by components the profile leaves out -- see RetireServices.
	var retire []string
	if explicitComponents == "" {
		included := make(map[string]bool, len(components))
		for _, c := range components {
			included[c.ID] = true
		}
		for _, c := range a.components {
			if c.Service != "" && !included[c.ID] {
				retire = append(retire, c.Service)
			}
		}
	}

	// Pre-resolve each component's latest release server-side (shared cache),
	// so fresh boxes download straight from GitHub's CDN without ever calling
	// the rate-limited api.github.com.
	resolved := make([]bootstrapComponent, 0, len(components))
	for _, c := range components {
		tag, assets := a.resolveReleaseAssets(c.Repo)
		resolved = append(resolved, bootstrapComponent{Component: c, LatestTag: tag, Assets: assets})
	}

	compJSON, _ := json.Marshal(struct {
		Components []bootstrapComponent `json:"components"`
	}{Components: resolved})

	// dserv-agent itself is installed by step_install_agent; resolve it too.
	agentTag, agentAssets := a.resolveReleaseAssets(agentRepo)
	agentJSON, _ := json.Marshal(struct {
		Tag    string           `json:"tag"`
		Assets []bootstrapAsset `json:"assets"`
	}{Tag: agentTag, Assets: agentAssets})

	cfg := BootstrapConfig{
		ServerURL:        serverURL,
		DefaultWG:        workgroup,
		Version:          version,
		ComponentsJSON:   string(compJSON),
		AgentReleaseJSON: string(agentJSON),
		ProfileName:      profileName,
		StimMode:         a.profileStimMode(profileName),

		ExplicitComponents: explicitComponents,
		RetireServices:     strings.Join(retire, " "),
	}

	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.Header().Set("Content-Disposition", "inline; filename=\"dserv-setup.sh\"")

	if err := bootstrapTmpl.Execute(w, cfg); err != nil {
		http.Error(w, "Template error: "+err.Error(), 500)
	}
}

// GET /setup/status - health check for bootstrap script verification
func (a *Agent) handleBootstrapStatus(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, 200, map[string]interface{}{
		"ok":       true,
		"version":  version,
		"mode":     "server",
		"platform": runtime.GOOS + "/" + runtime.GOARCH,
	})
}

// GET /setup/profiles - list available profiles
func (a *Agent) handleBootstrapProfiles(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, 200, map[string]interface{}{
		"profiles": a.getProfiles(),
	})
}

// registerBootstrapHandlers adds bootstrap endpoints to the mux.
// No auth required — fresh boxes need bare curl access.
func registerBootstrapHandlers(mux *http.ServeMux, agent *Agent) {
	mux.HandleFunc("/setup", agent.handleBootstrap)
	mux.HandleFunc("/setup/status", agent.handleBootstrapStatus)
	mux.HandleFunc("/setup/profiles", agent.handleBootstrapProfiles)

	// Board-side sibling: curl -fsSL http://server/extio/setup | bash
	mux.HandleFunc("/extio/setup", agent.handleExtioSetup)

	// Time-role sibling: curl -sSL http://server/ptp/setup | sudo bash -s -- client IFACE
	// (the PTP tooling ships in the dserv .deb; this is how a box WITHOUT
	// dserv -- a stim-profile display box -- gets it. See ptp_setup.go.)
	mux.HandleFunc("/ptp/setup", agent.handlePTPSetup)

	mux.HandleFunc("/setup/", func(w http.ResponseWriter, r *http.Request) {
		path := strings.TrimPrefix(r.URL.Path, "/setup")
		path = strings.TrimPrefix(path, "/")
		switch path {
		case "", "index.sh":
			agent.handleBootstrap(w, r)
		case "status":
			agent.handleBootstrapStatus(w, r)
		case "profiles":
			agent.handleBootstrapProfiles(w, r)
		default:
			http.NotFound(w, r)
		}
	})
}
