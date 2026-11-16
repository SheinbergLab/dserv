// teensy_setup.go -- /extio/teensy: put extio firmware on a Teensy 4.x.
//
// The Teensy sibling of /extio/setup (which provisions RP2350 boards with
// picotool). A Teensy has no debug port: its HalfKay bootloader takes exactly
// one Intel HEX file over USB, so a blank board needs MCUboot and the signed
// app merged into one file -- the shelf's `hex` artifact, published by
// extio-zephyr/publish.sh. After that first flash the box updates itself over
// the wire from the shelf's `bin`, and nobody needs this again unless MCUboot
// changes.
//
//	curl -sSL "https://dserv.net/extio/teensy?board=teensy40" | bash
//	curl -sSL "https://dserv.net/extio/teensy?board=teensy41&channel=stable" | bash
//
// The SERVER picks the image -- the newest publish on the channel that carries
// a hex for that board -- and bakes its URL and sha256 into the script, so the
// script needs no JSON parsing on the user's machine. The same lookup answers
// /extio/teensy/info as JSON, which is what the landing page shows.
//
// Errors come back as a shell script that prints the message and exits 1
// (with a 4xx status), because the landing page's command is `curl -sSL`
// without -f: an HTML or plain-text error body piped into bash would be run as
// commands and fail with something meaningless.

package main

import (
	_ "embed"
	"fmt"
	"net/http"
	"path"
	"sort"
	"strings"
)

//go:embed teensy_setup.sh
var teensySetupScript string

// teensyBoard maps the name a person picks to what the rest of the chain
// calls it. Build is the shelf key, which is the box's own CONFIG_BOARD_TARGET
// with / flattened to _ (publish.sh and box_announce.c agree on it); MCU is
// teensy_loader_cli's --mcu.
type teensyBoard struct {
	Name  string
	Title string
	Build string
	MCU   string
}

var teensyBoards = map[string]teensyBoard{
	"teensy40": {"teensy40", "Teensy 4.0", "teensy40_mimxrt1062", "TEENSY40"},
	"teensy41": {"teensy41", "Teensy 4.1", "teensy41_mimxrt1062", "TEENSY41"},
}

func teensyBoardNames() []string {
	var names []string
	for n := range teensyBoards {
		names = append(names, n)
	}
	sort.Strings(names)
	return names
}

// teensyRelease is one hex on the shelf, resolved for one board.
type teensyRelease struct {
	Board       string `json:"board"`
	Title       string `json:"title"`
	MCU         string `json:"mcu"`
	Build       string `json:"build"`
	Channel     string `json:"channel"`
	Version     string `json:"version"`
	ImgVersion  string `json:"imgVersion,omitempty"`
	PublishedAt string `json:"publishedAt"`
	File        string `json:"file"`
	Size        int64  `json:"size"`
	SHA256      string `json:"sha256"`
	URL         string `json:"url"`
}

// teensyError carries the HTTP status a lookup failure should be served with.
type teensyError struct {
	code int
	msg  string
}

func (e *teensyError) Error() string { return e.msg }

// findTeensyRelease resolves board/channel/version to a hex on the shelf.
// With no version it takes the newest publish on the channel that has a hex
// for this board -- NOT the channel's latest pointer, which moves on every
// publish of every board and can name a version with no Teensy image at all.
func (a *Agent) findTeensyRelease(base, board, channel, version string) (*teensyRelease, error) {
	if a.firmwareRoot() == "" {
		return nil, &teensyError{503, "this registry does not host firmware (no --firmware-dir)"}
	}
	tb, ok := teensyBoards[board]
	if !ok {
		if board == "" {
			return nil, &teensyError{400, "which Teensy? add ?board=" + strings.Join(teensyBoardNames(), " or ?board=")}
		}
		return nil, &teensyError{400, fmt.Sprintf("unknown board %q (known: %s)", board, strings.Join(teensyBoardNames(), ", "))}
	}
	if channel == "" {
		channel = "dev"
	}
	if !fwChannelRe.MatchString(channel) {
		return nil, &teensyError{400, "invalid channel"}
	}
	if version != "" && !fwVersionRe.MatchString(version) {
		return nil, &teensyError{400, "invalid version"}
	}

	var manifests []FirmwareManifest
	if version != "" {
		m, err := a.readManifest(channel, version)
		if err != nil {
			return nil, &teensyError{404, fmt.Sprintf("no version %s on channel %s", version, channel)}
		}
		manifests = []FirmwareManifest{*m}
	} else {
		_, manifests = a.readChannel(channel) // newest publish first
	}
	for _, m := range manifests {
		for _, img := range m.Images {
			if img.Build != tb.Build || img.Hex == "" {
				continue
			}
			return &teensyRelease{
				Board: tb.Name, Title: tb.Title, MCU: tb.MCU, Build: tb.Build,
				Channel: channel, Version: m.Version, ImgVersion: img.ImgVersion,
				PublishedAt: m.PublishedAt, File: img.Hex, Size: img.HexSize, SHA256: img.HexSHA256,
				URL: base + "/" + path.Join("firmware", fwFamily, channel, m.Version, img.Hex),
			}, nil
		}
	}
	where := "channel " + channel
	if version != "" {
		where = channel + "/" + version
	}
	return nil, &teensyError{404, fmt.Sprintf("no %s first-flash image (.hex) published on %s yet", tb.Title, where)}
}

func (a *Agent) teensyLookup(r *http.Request) (*teensyRelease, error) {
	q := r.URL.Query()
	return a.findTeensyRelease(requestBaseURL(r), q.Get("board"), q.Get("channel"), q.Get("version"))
}

// handleTeensySetup serves the flashing script.
func (a *Agent) handleTeensySetup(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.Header().Set("Content-Disposition", "inline; filename=\"teensy-setup.sh\"")

	rel, err := a.teensyLookup(r)
	if err != nil {
		code := http.StatusInternalServerError
		if te, ok := err.(*teensyError); ok {
			code = te.code
		}
		w.WriteHeader(code)
		fmt.Fprintf(w, "#!/usr/bin/env bash\necho %s >&2\nexit 1\n", shellQuote("!! extio teensy: "+err.Error()))
		return
	}

	fmt.Fprintf(w, "#!/usr/bin/env bash\n")
	fmt.Fprintf(w, "# extio firmware for a %s -- served by dserv-agent %s\n", rel.Title, version)
	for _, kv := range [][2]string{
		{"TEENSY_BOARD", rel.Board},
		{"TEENSY_TITLE", rel.Title},
		{"TEENSY_MCU", rel.MCU},
		{"FW_SHELF_URL", requestBaseURL(r)},
		{"FW_CHANNEL", rel.Channel},
		{"FW_VERSION", rel.Version},
		{"FW_IMGVER", rel.ImgVersion},
		{"FW_PUBLISHED", rel.PublishedAt},
		{"HEX_URL", rel.URL},
		{"HEX_NAME", rel.File},
		{"HEX_SHA256", rel.SHA256},
		{"HEX_SIZE", fmt.Sprint(rel.Size)},
	} {
		fmt.Fprintf(w, "%s=%s\n", kv[0], shellQuote(kv[1]))
	}
	fmt.Fprint(w, teensySetupScript)
}

// handleTeensyInfo answers what /extio/teensy would flash, as JSON.
func (a *Agent) handleTeensyInfo(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	rel, err := a.teensyLookup(r)
	if err != nil {
		code := http.StatusInternalServerError
		if te, ok := err.(*teensyError); ok {
			code = te.code
		}
		writeJSON(w, code, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, 200, rel)
}
