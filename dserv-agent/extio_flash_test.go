package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"os/exec"
	"strings"
	"testing"
	"time"
)

const fakeHex = ":020000040000FA\n:00000001FF\n"

// publishFiles publishes one build with any of uf2/bin/hex (nil = omit).
func publishFiles(t *testing.T, a *Agent, channel, version, build string, files map[string][]byte) *httptest.ResponseRecorder {
	t.Helper()
	var body bytes.Buffer
	mw := multipart.NewWriter(&body)
	_ = mw.WriteField("version", version)
	_ = mw.WriteField("build", build)
	_ = mw.WriteField("board", strings.SplitN(build, "_", 2)[0])
	_ = mw.WriteField("imgver", "0.4.0+102")
	_ = mw.WriteField("ota", "1")
	for field, data := range files {
		fw, err := mw.CreateFormFile(field, build+"."+field)
		if err != nil {
			t.Fatal(err)
		}
		fw.Write(data)
	}
	mw.Close()
	req := httptest.NewRequest(http.MethodPost, "/api/firmware/extio/"+channel, &body)
	req.Header.Set("Content-Type", mw.FormDataContentType())
	rr := httptest.NewRecorder()
	a.handleFirmwareAPI(rr, req)
	return rr
}

func TestFirmwarePublishHex(t *testing.T) {
	a := newFwAgent(t)
	rr := publishFiles(t, a, "dev", "0.61.8", "teensy40_mimxrt1062",
		map[string][]byte{"bin": []byte("slot image"), "hex": []byte(fakeHex)})
	if rr.Code != 200 {
		t.Fatalf("publish: %d %s", rr.Code, rr.Body.String())
	}
	m, err := a.readManifest("dev", "0.61.8")
	if err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256([]byte(fakeHex))
	img := m.Images[0]
	if img.Hex != "teensy40_mimxrt1062.hex" || img.HexSHA256 != hex.EncodeToString(sum[:]) || img.HexSize != int64(len(fakeHex)) {
		t.Fatalf("hex fields = %+v", img)
	}
	if img.Bin == "" {
		t.Fatalf("bin lost alongside hex: %+v", img)
	}

	// hex alone is a valid publish
	if rr := publishFiles(t, a, "dev", "0.61.9", "teensy41_mimxrt1062",
		map[string][]byte{"hex": []byte(fakeHex)}); rr.Code != 200 {
		t.Fatalf("hex-only publish: %d %s", rr.Code, rr.Body.String())
	}

	// a binary renamed .hex is refused, and not left on disk
	rr = publishFiles(t, a, "dev", "0.61.10", "teensy40_mimxrt1062",
		map[string][]byte{"hex": {0x3d, 0xb8, 0xf3, 0x96}})
	if rr.Code != 400 || !strings.Contains(rr.Body.String(), "Intel HEX") {
		t.Fatalf("non-hex upload: %d %s", rr.Code, rr.Body.String())
	}

	// the artifact is served
	req := httptest.NewRequest(http.MethodGet, "/firmware/extio/dev/0.61.8/teensy40_mimxrt1062.hex", nil)
	w := httptest.NewRecorder()
	a.handleFirmwareArtifact(w, req)
	if w.Code != 200 || w.Body.String() != fakeHex {
		t.Fatalf("serve hex: %d %q", w.Code, w.Body.String())
	}
}

func teensyGet(a *Agent, h func(*Agent, http.ResponseWriter, *http.Request), url string) *httptest.ResponseRecorder {
	req := httptest.NewRequest(http.MethodGet, url, nil)
	req.Host = "registry.test"
	rr := httptest.NewRecorder()
	h(a, rr, req)
	return rr
}

func TestTeensyPicksNewestPublishWithHex(t *testing.T) {
	a := newFwAgent(t)
	publishFiles(t, a, "dev", "0.61.0", "teensy40_mimxrt1062",
		map[string][]byte{"bin": []byte("old"), "hex": []byte(fakeHex)})
	time.Sleep(1100 * time.Millisecond) // PublishedAt has 1 s resolution
	// newer publish of the same board WITHOUT a hex, and a newer one of another board:
	// neither may be picked, and the channel's latest pointer names the last one.
	publishFiles(t, a, "dev", "0.61.1", "teensy40_mimxrt1062", map[string][]byte{"bin": []byte("new")})
	publishFiles(t, a, "dev", "0.61.2", "frdm_mcxn947_mcxn947_cpu0", map[string][]byte{"bin": []byte("x")})

	rr := teensyGet(a, (*Agent).handleTeensyInfo, "/extio/teensy/info?board=teensy40")
	if rr.Code != 200 {
		t.Fatalf("info: %d %s", rr.Code, rr.Body.String())
	}
	var rel flashRelease
	json.Unmarshal(rr.Body.Bytes(), &rel)
	if rel.Version != "0.61.0" || rel.Target != "TEENSY40" || rel.Flasher != "teensy" ||
		rel.URL != "http://registry.test/firmware/extio/dev/0.61.0/teensy40_mimxrt1062.hex" {
		t.Fatalf("picked %+v", rel)
	}

	// no hex at all for the 4.1 -> 404 naming the board
	rr = teensyGet(a, (*Agent).handleTeensyInfo, "/extio/teensy/info?board=teensy41")
	if rr.Code != 404 || !strings.Contains(rr.Body.String(), "Teensy 4.1") {
		t.Fatalf("teensy41: %d %s", rr.Code, rr.Body.String())
	}
	// pinned version that has no hex -> 404
	rr = teensyGet(a, (*Agent).handleTeensyInfo, "/extio/teensy/info?board=teensy40&version=0.61.1")
	if rr.Code != 404 {
		t.Fatalf("pinned no-hex version: %d %s", rr.Code, rr.Body.String())
	}
}

func TestTeensyScript(t *testing.T) {
	a := newFwAgent(t)
	publishFiles(t, a, "dev", "0.61.0", "teensy40_mimxrt1062",
		map[string][]byte{"bin": []byte("b"), "hex": []byte(fakeHex)})

	rr := teensyGet(a, (*Agent).handleTeensySetup, "/extio/teensy?board=teensy40")
	if rr.Code != 200 {
		t.Fatalf("script: %d %s", rr.Code, rr.Body.String())
	}
	s := rr.Body.String()
	for _, want := range []string{
		"TEENSY_MCU='TEENSY40'",
		"BOARD_TITLE='Teensy 4.0'",
		"HEX_URL='http://registry.test/firmware/extio/dev/0.61.0/teensy40_mimxrt1062.hex'",
		"FW_IMGVER='0.4.0+102'",
		"\nmain \"$@\"\n",
	} {
		if !strings.Contains(s, want) {
			t.Errorf("script lacks %q", want)
		}
	}
	bashN(t, s)

	// errors are a script that says so, not text bash would try to run
	for _, url := range []string{"/extio/teensy", "/extio/teensy?board=arduino", "/extio/teensy?board=teensy41",
		"/extio/teensy?board=teensy40&channel=../x", "/extio/teensy?board=frdm_mcxn947"} {
		rr := teensyGet(a, (*Agent).handleTeensySetup, url)
		if rr.Code < 400 || !strings.Contains(rr.Body.String(), "exit 1") {
			t.Errorf("%s: %d %q", url, rr.Code, rr.Body.String())
		}
		bashN(t, rr.Body.String())
	}

	// a registry with no shelf
	none := &Agent{cfg: Config{ServerMode: true}}
	if rr := teensyGet(none, (*Agent).handleTeensySetup, "/extio/teensy?board=teensy40"); rr.Code != 503 {
		t.Errorf("no shelf: %d", rr.Code)
	}
}

func bashN(t *testing.T, script string) {
	t.Helper()
	if _, err := exec.LookPath("bash"); err != nil {
		return
	}
	cmd := exec.Command("bash", "-n")
	cmd.Stdin = strings.NewReader(script)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Errorf("bash -n: %v\n%s", err, out)
	}
}

func TestFlashPyOCDScript(t *testing.T) {
	a := newFwAgent(t)
	publishFiles(t, a, "dev", "0.61.0", "frdm_mcxn947_mcxn947_cpu0",
		map[string][]byte{"bin": []byte("b"), "hex": []byte(fakeHex)})

	rr := teensyGet(a, (*Agent).handleFlashInfo, "/extio/flash/info?board=frdm_mcxn947")
	var rel flashRelease
	json.Unmarshal(rr.Body.Bytes(), &rel)
	if rr.Code != 200 || rel.Flasher != "pyocd" || rel.Target != "mcxn947" || rel.File != "frdm_mcxn947_mcxn947_cpu0.hex" {
		t.Fatalf("info: %d %s", rr.Code, rr.Body.String())
	}

	rr = teensyGet(a, (*Agent).handleFlashSetup, "/extio/flash?board=frdm_mcxn947")
	if rr.Code != 200 {
		t.Fatalf("script: %d %s", rr.Code, rr.Body.String())
	}
	s := rr.Body.String()
	pin := flashBoards["frdm_mcxn947"].PyOCD
	for _, want := range []string{
		"PYOCD_TARGET='mcxn947'",
		"PYOCD_VERSION='" + pin.Version + "'",
		"PACK_SHA256='" + pin.PackSHA256 + "'",
		"PACK_NAME='NXP.MCXN947_DFP.19.0.0.pack'",
		"HEX_URL='http://registry.test/firmware/extio/dev/0.61.0/frdm_mcxn947_mcxn947_cpu0.hex'",
		"--no-config",
		"\nmain \"$@\"\n",
	} {
		if !strings.Contains(s, want) {
			t.Errorf("script lacks %q", want)
		}
	}
	if strings.Contains(s, "teensy_loader_cli") {
		t.Errorf("pyocd board was served the teensy script")
	}
	bashN(t, s)

	// the generic endpoint serves the teensy boards too, with the teensy script
	publishFiles(t, a, "dev", "0.61.1", "teensy40_mimxrt1062", map[string][]byte{"hex": []byte(fakeHex)})
	rr = teensyGet(a, (*Agent).handleFlashSetup, "/extio/flash?board=teensy40")
	if rr.Code != 200 || !strings.Contains(rr.Body.String(), "teensy_loader_cli") {
		t.Fatalf("teensy via /extio/flash: %d", rr.Code)
	}
}

// Every pyocd board's pack pin must be complete: an empty sha256 would make the
// script compare against "" and refuse every download.
func TestFlashBoardsPinned(t *testing.T) {
	for name, b := range flashBoards {
		if b.Name != name || b.Build == "" || b.Target == "" {
			t.Errorf("%s: incomplete entry %+v", name, b)
		}
		switch b.Flasher {
		case flasherTeensy:
		case flasherPyOCD:
			if b.PyOCD == nil || b.PyOCD.Version == "" || b.PyOCD.PackURL == "" || len(b.PyOCD.PackSHA256) != 64 {
				t.Errorf("%s: incomplete pyocd pin %+v", name, b.PyOCD)
			}
		default:
			t.Errorf("%s: unknown flasher %q", name, b.Flasher)
		}
	}
}
