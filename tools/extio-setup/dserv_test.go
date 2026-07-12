package main

import (
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"net"
	"strings"
	"testing"
	"time"
)

/* ---- %get reply parsing (dpoint_to_string format) ---- */

func b64le32(v int32) string {
	b := make([]byte, 4)
	binary.LittleEndian.PutUint32(b, uint32(v))
	return base64.StdEncoding.EncodeToString(b)
}

func TestParseDservGet(t *testing.T) {
	cases := []struct {
		name string
		in   string
		ok   bool
		val  any
	}{
		{"string", "extio/pico/state/desc 1 1751900000000000 9 {bench box}", true, "bench box"},
		{"string with braces", "extio/boxes 1 100 10 {io1 {pi}co}", true, "io1 {pi}co"},
		{"empty string", "extio/pico/state/pins/out 1 100 0 {}", true, ""},
		{"int", "extio/pico/state/di/13 5 1751900000000001 4 {" + b64le32(1) + "}", true, int32(1)},
		{"negative int", "extio/pico/state/obs_pin 5 100 4 {" + b64le32(-1) + "}", true, int32(-1)},
		{"truncated marker", "extio/pico/state/desc 1 100 500 {...}", false, nil},
		{"length lie", "extio/pico/state/desc 1 100 7 {four}", false, nil},
		{"bad base64", "extio/pico/state/di/1 5 100 4 {!!notb64!!}", false, nil},
		{"missing brace", "extio/pico/state/desc 1 100 4 four", false, nil},
		{"too few fields", "extio/pico/state/desc 1 100", false, nil},
		{"empty", "", false, nil},
	}
	for _, c := range cases {
		ev, ok := parseDservGet(c.in)
		if ok != c.ok {
			t.Errorf("%s: ok=%v want %v", c.name, ok, c.ok)
			continue
		}
		if ok && ev.Val != c.val {
			t.Errorf("%s: val=%v (%T) want %v (%T)", c.name, ev.Val, ev.Val, c.val, c.val)
		}
	}

	// field fidelity on one full example
	ev, ok := parseDservGet("extio/pico/state/watchdog 5 1751912345678901 4 {" + b64le32(4242) + "}")
	if !ok || ev.Name != "extio/pico/state/watchdog" || ev.TS != 1751912345678901 ||
		ev.Type != dtInt || ev.Val != int32(4242) {
		t.Errorf("full decode wrong: %+v ok=%v", ev, ok)
	}
}

/* ---- 128-byte frame building (mirrors dserv_msg.h / dpoint_to_binary) ---- */

func buildFrame(t *testing.T, name string, ts uint64, dtype uint32, data []byte) []byte {
	t.Helper()
	if 2+len(name)+8+4+4+len(data) > frameLen-1 {
		t.Fatalf("frame too big: %s", name)
	}
	f := make([]byte, frameLen)
	f[0] = '>'
	binary.LittleEndian.PutUint16(f[1:3], uint16(len(name)))
	copy(f[3:], name)
	p := 3 + len(name)
	binary.LittleEndian.PutUint64(f[p:], ts)
	binary.LittleEndian.PutUint32(f[p+8:], dtype)
	binary.LittleEndian.PutUint32(f[p+12:], uint32(len(data)))
	copy(f[p+16:], data)
	return f
}

func le32(v int32) []byte {
	b := make([]byte, 4)
	binary.LittleEndian.PutUint32(b, uint32(v))
	return b
}

func TestParseFrameRoundTrip(t *testing.T) {
	f := buildFrame(t, "extio/pico/state/di/13", 42, dtInt, le32(1))
	ev, ok := parseFrame(f)
	if !ok || ev.Name != "extio/pico/state/di/13" || ev.TS != 42 || ev.Val != int32(1) {
		t.Fatalf("round trip failed: %+v ok=%v", ev, ok)
	}
	fs := buildFrame(t, "extio/pico/state/fw", 7, dtString, []byte("0.47.12"))
	ev, ok = parseFrame(fs)
	if !ok || ev.Val != "0.47.12" {
		t.Fatalf("string frame failed: %+v ok=%v", ev, ok)
	}
}

/* ---- connect-back push pipeline: frames in, DataEvents out ---- */

// pushFrames connects to the link's listener and writes raw bytes, as dserv's
// binary SendClient would.
func pushFrames(t *testing.T, port string, chunks ...[]byte) net.Conn {
	t.Helper()
	c, err := net.Dial("tcp", "127.0.0.1:"+port)
	if err != nil {
		t.Fatalf("dial push: %v", err)
	}
	for _, b := range chunks {
		if _, err := c.Write(b); err != nil {
			t.Fatalf("push write: %v", err)
		}
	}
	return c
}

func collectEvents(ch chan DataEvent, n int, timeout time.Duration) []DataEvent {
	var out []DataEvent
	deadline := time.After(timeout)
	for len(out) < n {
		select {
		case ev, open := <-ch:
			if !open {
				return out
			}
			out = append(out, ev)
		case <-deadline:
			return out
		}
	}
	return out
}

// newBenchLink builds a DservLink with a live listener but no dserv: the
// push/subscribe/table pipeline under test, nothing else.
func newBenchLink(t *testing.T) *DservLink {
	t.Helper()
	ln, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	d := &DservLink{host: "test",
		subs:  map[chan DataEvent]bool{},
		table: map[string]DataEvent{},
		conns: map[net.Conn]bool{}}
	d.ln = ln
	_, d.lport, _ = net.SplitHostPort(ln.Addr().String())
	go d.acceptLoop()
	t.Cleanup(d.Close)
	return d
}

func TestPushPipeline(t *testing.T) {
	d := newBenchLink(t)
	ch, snap := d.Subscribe()
	defer d.Unsubscribe(ch)
	if len(snap) != 0 {
		t.Fatalf("fresh link should have an empty snapshot, got %d", len(snap))
	}

	f1 := buildFrame(t, "extio/pico/state/di/13", 100, dtInt, le32(1))
	f2 := buildFrame(t, "extio/pico/state/watchdog", 101, dtInt, le32(9))
	// split across writes mid-frame: TCP offers no message boundaries
	c := pushFrames(t, d.lport, f1[:50], f1[50:], f2)
	defer c.Close()

	evs := collectEvents(ch, 2, 2*time.Second)
	if len(evs) != 2 || evs[0].Name != "extio/pico/state/di/13" || evs[1].Val != int32(9) {
		t.Fatalf("push pipeline: got %+v", evs)
	}

	// table retains both; a second subscriber's snapshot replays di/do ONLY
	ch2, snap2 := d.Subscribe()
	defer d.Unsubscribe(ch2)
	if len(snap2) != 1 || snap2[0].Name != "extio/pico/state/di/13" || !snap2[0].Snap {
		t.Fatalf("snapshot should be the one di key, got %+v", snap2)
	}
	if st := d.StateFor("pico"); st["watchdog"] != int32(9) || st["di/13"] != int32(1) {
		t.Fatalf("StateFor wrong: %+v", st)
	}
}

func TestPushResync(t *testing.T) {
	d := newBenchLink(t)
	ch, _ := d.Subscribe()
	defer d.Unsubscribe(ch)

	f := buildFrame(t, "extio/pico/state/di/2", 5, dtInt, le32(1))
	c := pushFrames(t, d.lport, []byte("garbage-before-sync"), f)
	defer c.Close()

	evs := collectEvents(ch, 1, 2*time.Second)
	if len(evs) != 1 || evs[0].Name != "extio/pico/state/di/2" {
		t.Fatalf("resync failed: %+v", evs)
	}
	_, _, _, _, skip := d.Stats()
	if skip == 0 {
		t.Fatal("expected skip counter to record the resync")
	}
}

/* ---- write guard ---- */

func TestSetKeyGuard(t *testing.T) {
	d := &DservLink{} // Set validates before touching the network
	bad := []struct{ key, val string }{
		{"ess/in_obs", "1"},                               // outside extio/
		{"extio/pico/state/di/5", "1"},                    // state is read-only
		{"extio/pico/config/pin=evil", "1"},               // '=' in key
		{"extio/pico/config/desc", "two\nlines"},          // newline injection
		{"extio/pico/cmd/do/5", strings.Repeat("9", 120)}, // over the frame cap
	}
	for _, c := range bad {
		if err := d.Set(c.key, c.val); err == nil {
			t.Errorf("Set(%q,%q) should have been refused", c.key, c.val)
		}
	}
	// sanity: a good key fails only at the network layer (no conn here)
	err := d.Set("extio/pico/config/pin/13/mode", "in_pullup")
	if err == nil {
		t.Fatal("expected a network error, got success with no dserv")
	}
	if strings.Contains(err.Error(), "refusing") {
		t.Fatalf("valid key was refused: %v", err)
	}
}

func TestParseDservGetTypes(t *testing.T) {
	// int64 and short round through decodeVal like the frame path
	b8 := make([]byte, 8)
	binary.LittleEndian.PutUint64(b8, uint64(1751912345678901))
	in := fmt.Sprintf("extio/pico/state/sync/last 16 99 8 {%s}", base64.StdEncoding.EncodeToString(b8))
	ev, ok := parseDservGet(in)
	if !ok || ev.Val != int64(1751912345678901) {
		t.Fatalf("int64 decode: %+v ok=%v", ev, ok)
	}
}
