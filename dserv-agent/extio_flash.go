// extio_flash.go -- /extio/flash: put extio firmware on a blank Zephyr board.
//
// The Zephyr sibling of /extio/setup (which provisions RP2350 boards with
// picotool). Every board here boots MCUboot, so a blank one needs MCUboot and
// the signed app written together -- the shelf's `hex` artifact, merged and
// published by extio-zephyr/publish.sh. After that first flash the box updates
// itself over the wire from the shelf's `bin`, and nobody needs this again
// unless MCUboot changes.
//
//	curl -sSL "https://dserv.net/extio/flash?board=teensy40" | bash
//	curl -sSL "https://dserv.net/extio/flash?board=frdm_mcxn947&channel=stable" | bash
//
// The tool that writes the hex differs by board, so the board table names a
// FLASHER and the endpoint serves that flasher's script:
//
//	teensy  teensy_loader_cli over HalfKay (USB; the Program button)
//	pyocd   pyOCD over the board's own CMSIS-DAP probe (the FRDM MCU-Link)
//
// /extio/teensy is the original URL, kept because it was handed out; it is the
// same endpoint restricted to the teensy boards.
//
// The SERVER picks the image -- the newest publish on the channel that carries
// a hex for that board -- and bakes its URL and sha256 into the script, so the
// script needs no JSON parsing on the user's machine. The same lookup answers
// /extio/flash/info as JSON, which is what the landing page shows.
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

//go:embed pyocd_setup.sh
var pyocdSetupScript string

const (
	flasherTeensy = "teensy"
	flasherPyOCD  = "pyocd"
)

// flashBoard maps the name a person picks to what the rest of the chain calls
// it. Name is publish.sh's BOARD_ID (the board target up to its first /), so
// the hint publish.sh prints is a URL that works. Build is the shelf key: the
// box's own CONFIG_BOARD_TARGET with / flattened to _ (publish.sh and
// box_announce.c agree on it). Target is the flasher's name for the part:
// teensy_loader_cli's --mcu, or pyOCD's -t.
type flashBoard struct {
	Name    string
	Title   string
	Build   string
	Flasher string
	Target  string
	PyOCD   *pyocdPin // flasher == pyocd only
}

// pyocdPin is everything a pyOCD flash depends on that is NOT the image, pinned
// on the server so every user flashes with the combination that was tested.
//
// THE PACK VERSION IS LOAD-BEARING for the MCXN947 (extio-zephyr/pyocd.yaml):
// `pyocd pack install mcxn947` takes the newest from the index, and 26.06.00
// fails with "flash init timed out" PARTWAY THROUGH ERASING, leaving the
// bootloader region half-erased. 19.0.0 programs cleanly. NXP's pack repo is
// public (no login); the sha256 is checked before pyOCD ever sees the file.
type pyocdPin struct {
	Version    string // pip spec pyocd==<Version>, in a venv of its own
	PackURL    string
	PackSHA256 string
}

var flashBoards = map[string]flashBoard{
	"teensy40": {Name: "teensy40", Title: "Teensy 4.0", Build: "teensy40_mimxrt1062", Flasher: flasherTeensy, Target: "TEENSY40"},
	"teensy41": {Name: "teensy41", Title: "Teensy 4.1", Build: "teensy41_mimxrt1062", Flasher: flasherTeensy, Target: "TEENSY41"},
	"frdm_mcxn947": {Name: "frdm_mcxn947", Title: "FRDM-MCXN947", Build: "frdm_mcxn947_mcxn947_cpu0",
		Flasher: flasherPyOCD, Target: "mcxn947", PyOCD: &pyocdPin{
			Version:    "0.45.1",
			PackURL:    "https://mcuxpresso.nxp.com/cmsis_pack/repo/NXP.MCXN947_DFP.19.0.0.pack",
			PackSHA256: "c7a72e3db7327296910c4b0542f3ebdbec96beca973e4c1c15249b00cf00bf1b",
		}},
}

func flashBoardNames(flasher string) []string {
	var names []string
	for n, b := range flashBoards {
		if flasher == "" || b.Flasher == flasher {
			names = append(names, n)
		}
	}
	sort.Strings(names)
	return names
}

// flashRelease is one hex on the shelf, resolved for one board.
type flashRelease struct {
	Board       string `json:"board"`
	Title       string `json:"title"`
	Flasher     string `json:"flasher"`
	Target      string `json:"target"`
	Build       string `json:"build"`
	Channel     string `json:"channel"`
	Version     string `json:"version"`
	ImgVersion  string `json:"imgVersion,omitempty"`
	PublishedAt string `json:"publishedAt"`
	File        string `json:"file"`
	Size        int64  `json:"size"`
	SHA256      string `json:"sha256"`
	URL         string `json:"url"`

	pyocd *pyocdPin
}

// flashError carries the HTTP status a lookup failure should be served with.
type flashError struct {
	code int
	msg  string
}

func (e *flashError) Error() string { return e.msg }

// findFlashRelease resolves board/channel/version to a hex on the shelf. A
// non-empty flasher restricts which boards are accepted (/extio/teensy).
// With no version it takes the newest publish on the channel that has a hex
// for this board -- NOT the channel's latest pointer, which moves on every
// publish of every board and can name a version with no image for this one.
func (a *Agent) findFlashRelease(base, flasher, board, channel, version string) (*flashRelease, error) {
	if a.firmwareRoot() == "" {
		return nil, &flashError{503, "this registry does not host firmware (no --firmware-dir)"}
	}
	known := flashBoardNames(flasher)
	fb, ok := flashBoards[board]
	if !ok || (flasher != "" && fb.Flasher != flasher) {
		if board == "" {
			return nil, &flashError{400, "which board? add ?board=" + strings.Join(known, " or ?board=")}
		}
		return nil, &flashError{400, fmt.Sprintf("unknown board %q (known: %s)", board, strings.Join(known, ", "))}
	}
	if channel == "" {
		channel = "dev"
	}
	if !fwChannelRe.MatchString(channel) {
		return nil, &flashError{400, "invalid channel"}
	}
	if version != "" && !fwVersionRe.MatchString(version) {
		return nil, &flashError{400, "invalid version"}
	}

	var manifests []FirmwareManifest
	if version != "" {
		m, err := a.readManifest(channel, version)
		if err != nil {
			return nil, &flashError{404, fmt.Sprintf("no version %s on channel %s", version, channel)}
		}
		manifests = []FirmwareManifest{*m}
	} else {
		_, manifests = a.readChannel(channel) // newest publish first
	}
	for _, m := range manifests {
		for _, img := range m.Images {
			if img.Build != fb.Build || img.Hex == "" {
				continue
			}
			return &flashRelease{
				Board: fb.Name, Title: fb.Title, Flasher: fb.Flasher, Target: fb.Target, Build: fb.Build,
				Channel: channel, Version: m.Version, ImgVersion: img.ImgVersion,
				PublishedAt: m.PublishedAt, File: img.Hex, Size: img.HexSize, SHA256: img.HexSHA256,
				URL:   base + "/" + path.Join("firmware", fwFamily, channel, m.Version, img.Hex),
				pyocd: fb.PyOCD,
			}, nil
		}
	}
	where := "channel " + channel
	if version != "" {
		where = channel + "/" + version
	}
	return nil, &flashError{404, fmt.Sprintf("no %s first-flash image (.hex) published on %s yet", fb.Title, where)}
}

func (a *Agent) flashLookup(r *http.Request, flasher string) (*flashRelease, error) {
	q := r.URL.Query()
	return a.findFlashRelease(requestBaseURL(r), flasher, q.Get("board"), q.Get("channel"), q.Get("version"))
}

// serveFlashScript writes the preamble (the picked image, as shell variables)
// and the flasher's script.
func (a *Agent) serveFlashScript(w http.ResponseWriter, r *http.Request, flasher string) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")

	rel, err := a.flashLookup(r, flasher)
	if err != nil {
		code := http.StatusInternalServerError
		if fe, ok := err.(*flashError); ok {
			code = fe.code
		}
		w.WriteHeader(code)
		fmt.Fprintf(w, "#!/usr/bin/env bash\necho %s >&2\nexit 1\n", shellQuote("!! extio flash: "+err.Error()))
		return
	}

	vars := [][2]string{
		{"BOARD_NAME", rel.Board},
		{"BOARD_TITLE", rel.Title},
		{"FW_SHELF_URL", requestBaseURL(r)},
		{"FW_CHANNEL", rel.Channel},
		{"FW_VERSION", rel.Version},
		{"FW_IMGVER", rel.ImgVersion},
		{"FW_PUBLISHED", rel.PublishedAt},
		{"HEX_URL", rel.URL},
		{"HEX_NAME", rel.File},
		{"HEX_SHA256", rel.SHA256},
		{"HEX_SIZE", fmt.Sprint(rel.Size)},
	}
	var body string
	switch rel.Flasher {
	case flasherTeensy:
		body = teensySetupScript
		vars = append(vars, [2]string{"TEENSY_MCU", rel.Target})
	case flasherPyOCD:
		body = pyocdSetupScript
		vars = append(vars,
			[2]string{"PYOCD_TARGET", rel.Target},
			[2]string{"PYOCD_VERSION", rel.pyocd.Version},
			[2]string{"PACK_URL", rel.pyocd.PackURL},
			[2]string{"PACK_NAME", path.Base(rel.pyocd.PackURL)},
			[2]string{"PACK_SHA256", rel.pyocd.PackSHA256})
	}
	w.Header().Set("Content-Disposition", fmt.Sprintf("inline; filename=%q", rel.Flasher+"-setup.sh"))
	fmt.Fprintf(w, "#!/usr/bin/env bash\n")
	fmt.Fprintf(w, "# extio firmware for a %s -- served by dserv-agent %s\n", rel.Title, version)
	for _, kv := range vars {
		fmt.Fprintf(w, "%s=%s\n", kv[0], shellQuote(kv[1]))
	}
	fmt.Fprint(w, body)
}

// serveFlashInfo answers what the script would flash, as JSON.
func (a *Agent) serveFlashInfo(w http.ResponseWriter, r *http.Request, flasher string) {
	if r.Method != http.MethodGet {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	rel, err := a.flashLookup(r, flasher)
	if err != nil {
		code := http.StatusInternalServerError
		if fe, ok := err.(*flashError); ok {
			code = fe.code
		}
		writeJSON(w, code, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, 200, rel)
}

func (a *Agent) handleFlashSetup(w http.ResponseWriter, r *http.Request) {
	a.serveFlashScript(w, r, "")
}
func (a *Agent) handleFlashInfo(w http.ResponseWriter, r *http.Request) { a.serveFlashInfo(w, r, "") }

func (a *Agent) handleTeensySetup(w http.ResponseWriter, r *http.Request) {
	a.serveFlashScript(w, r, flasherTeensy)
}
func (a *Agent) handleTeensyInfo(w http.ResponseWriter, r *http.Request) {
	a.serveFlashInfo(w, r, flasherTeensy)
}
