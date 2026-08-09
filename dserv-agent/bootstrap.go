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
}

// BootstrapConfig holds parameters for generating the bootstrap script
type BootstrapConfig struct {
	ServerURL        string
	DefaultWG        string
	Version          string
	ComponentsJSON   string // filtered component list as JSON, with resolved release assets
	AgentReleaseJSON string // dserv-agent release {tag, assets:[{name,url}]} as JSON
	ProfileName      string
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
		Description: "Development box (dserv + stim2 + dlsh; same set as incage)",
		Components:  []string{"*"},
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

	// Profile lookup
	if profileName == "" || profileName == "all" {
		return a.components
	}

	for _, p := range a.getProfiles() {
		if strings.EqualFold(p.Name, profileName) {
			// "*" means all
			for _, c := range p.Components {
				if c == "*" {
					return a.components
				}
			}
			wanted := make(map[string]bool)
			for _, id := range p.Components {
				wanted[id] = true
			}
			return a.resolveWithDeps(wanted)
		}
	}

	// Unknown profile - return all with a log
	return a.components
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
DSERV_INSTALL_DIR="/usr/local/dserv"
LOG_FILE="/tmp/dserv-bootstrap-$(date +%Y%m%d-%H%M%S).log"

# Component definitions (filtered by profile, with release assets pre-resolved
# by the registry so installs hit GitHub's CDN, not the rate-limited API)
COMPONENTS_JSON='{{.ComponentsJSON}}'

# dserv-agent release, pre-resolved by the registry: {tag, assets:[{name,url}]}
AGENT_RELEASE_JSON='{{.AgentReleaseJSON}}'

# ============ Parse Arguments ============

ROLE=""
WORKGROUP="${DEFAULT_WORKGROUP}"
DRY_RUN=false
SKIP_AGENT=false
ESS_USER_ARG=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --role)        ROLE="$2"; shift 2 ;;
        --workgroup)   WORKGROUP="$2"; shift 2 ;;
        --user)        ESS_USER_ARG="$2"; shift 2 ;;
        --dry-run)     DRY_RUN=true; shift ;;
        --skip-agent)  SKIP_AGENT=true; shift ;;
        --help|-h)
            echo "dserv bootstrap - provision a data acquisition box"
            echo ""
            echo "Usage: curl -sSL ${REGISTRY_URL}/setup | bash -s -- [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --role ROLE          Box role (e.g., eyetracker, stim, control)"
            echo "  --workgroup NAME     Workgroup name (default: ${DEFAULT_WORKGROUP})"
            echo "  --user USER          Local account owning the ESS systems tree"
            echo "                       (default: the user invoking sudo, else 'lab')"
            echo "  --dry-run            Show what would be done without making changes"
            echo "  --skip-agent         Skip dserv-agent install (components only)"
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

has_component() {
    echo "$COMPONENTS_JSON" | jq -e --arg id "$1" \
        '.components[] | select(.id == $id)' &>/dev/null
}

# Services this box actually runs, in component order (e.g. "dserv", "stim2").
# Excludes the agent: it is the thing doing the managing, not a thing being
# managed, and pinning --service to it would have the panel report the agent
# where the box's actual payload service belongs.
profile_services() {
    echo "$COMPONENTS_JSON" | jq -r --arg self "$AGENT_COMPONENT_ID" \
        '.components[] | select(.id != $self) | .service // empty'
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
            local args=""
            [[ -n "$ROLE" ]]      && args="$args --role $ROLE"
            [[ -n "$WORKGROUP" ]] && args="$args --workgroup $WORKGROUP"
            [[ -n "$ESS_USER_ARG" ]] && args="$args --user $ESS_USER_ARG"
            $DRY_RUN              && args="$args --dry-run"
            $SKIP_AGENT           && args="$args --skip-agent"
            exec sudo bash -c "$(curl -sSL "${REGISTRY_URL}/setup?profile=${PROFILE}")" -- $args
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
        run dpkg -i "$download_path" || run apt-get install -f -y -qq
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
        fallback=$(github_release_fallback "SheinbergLab/dserv-agent")
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

    local deb_url
    deb_url=$(find_asset "$assets_json" "dserv-agent.*\\.deb$" "$PLATFORM")

    if [[ -n "$deb_url" && "$deb_url" != "null" ]]; then
        local deb_file="/tmp/dserv-agent-${tag}.deb"
        run curl -sSL -o "$deb_file" "$deb_url"
        run dpkg -i "$deb_file" || run apt-get install -f -y -qq
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
    step_configure_dserv
    step_sync_scripts
    step_power_mgmt
    step_start_services
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
