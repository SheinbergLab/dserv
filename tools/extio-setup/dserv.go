// dserv.go -- the dserv driver: see a rig's extio boxes through its dserv
// datapoint table instead of a local USB cable.
//
// Speaks only the TEXT datapoint port (4620) -- least privilege, no Tcl eval:
//
//	%getkeys / %get       discovery + retained-state snapshot (manifest, di/do)
//	%set key=value        config/cmd writes (guarded here to extio/...)
//	%reg <ip> <port> 1    subscription connect-back. Flag 1 = BINARY: dserv
//	%match ... extio/* 1  pushes each matched datapoint as the same 128-byte
//	                      '>' frame the box's data CDC carries, so parseFrame
//	                      and the DataEvent pipeline are shared with serial.
//
// dserv's binary sender silently skips any datapoint whose name+data exceed
// the 128-byte frame. Fine for extio/* (the box authors those keys as such
// frames; host-side ones are tiny) -- which is why the match stays scoped and
// fat keys (stimdg, ess/*) must never be added.
//
// Registration is one-shot on dserv's side: it dials us once per %reg and
// never retries. We heal it like the box firmware does: when the last push
// conn closes and we didn't close it, re-%reg with backoff; while healthy,
// re-assert the %match set periodically (a lost match is otherwise invisible
// -- and %match always replies rc=1, so only the push conn tells the truth).
//
// Retained state is NEVER seeded via %touch: touching config/* would re-apply
// config to the box and touching cmd/* would re-fire commands (re-pulse an
// output). Seeding is read-only %getkeys + %get.
package main

import (
	"bufio"
	"encoding/base64"
	"errors"
	"fmt"
	"net"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	dservTextPort   = "4620"
	dservCmdTimeout = 5 * time.Second
	matchRefresh    = 30 * time.Second
)

// dtypes with a raw (non-base64) payload in %get replies (dpoint_to_string).
const (
	dtScript = 7
	dtJSON   = 11
)

type DservLink struct {
	mu                              sync.Mutex
	host                            string // as the user typed it (display)
	addr                            string // resolved "ip:4620"
	local                           string // our IP as dserv sees us (%reg connect-back target)
	lport                           string // connect-back listener port
	ln                              net.Listener
	closed                          bool
	subs                            map[chan DataEvent]bool
	table                           map[string]DataEvent // retained extio/* values (seed + live updates)
	conns                           map[net.Conn]bool    // live push conns, closed explicitly on teardown
	pushes                          int                  // live connect-back conns from dserv
	regGen                          uint64               // completed registrations (1 = initial)
	frames, bad, drops, bytes, skip uint64

	cmdMu sync.Mutex // lockstep command connection (one request in flight)
	cmd   net.Conn
	cmdRd *bufio.Reader
}

// openDservLink dials host's dserv, registers a binary connect-back
// subscription on extio/*, and seeds the retained-state table.
func openDservLink(host string) (*DservLink, error) {
	host = strings.TrimSpace(host)
	if host == "" || strings.ContainsAny(host, " \t\r\n") {
		return nil, errors.New("need a host name or IP")
	}
	if strings.Contains(host, ":") {
		return nil, errors.New("host only -- the datapoint port is fixed at 4620")
	}
	ip, err := resolveIPv4(host)
	if err != nil {
		return nil, err
	}
	d := &DservLink{host: host, addr: net.JoinHostPort(ip, dservTextPort),
		subs:  map[chan DataEvent]bool{},
		table: map[string]DataEvent{},
		conns: map[net.Conn]bool{}}

	d.cmdMu.Lock()
	err = d.dialCmdLocked()
	d.cmdMu.Unlock()
	if err != nil {
		return nil, err
	}

	// The connect-back listener dserv will push frames to. tcp4: %reg wants
	// the dotted quad dserv can dial, and dserv prefers IPv4 itself.
	ln, err := net.Listen("tcp4", ":0")
	if err != nil {
		d.Close()
		return nil, fmt.Errorf("connect-back listener: %w", err)
	}
	d.ln = ln
	_, d.lport, _ = net.SplitHostPort(ln.Addr().String())
	go d.acceptLoop()

	if err := d.register(); err != nil {
		d.Close()
		return nil, err
	}
	d.mu.Lock()
	d.regGen = 1
	d.mu.Unlock()
	d.seed()
	go d.matchRefresher()
	return d, nil
}

// resolveIPv4 picks an IPv4 address for host (dserv binds 0.0.0.0 and the
// %reg host argument must be dialable from its side).
func resolveIPv4(host string) (string, error) {
	if ip := net.ParseIP(host); ip != nil {
		if ip.To4() == nil {
			return "", errors.New("need an IPv4 address")
		}
		return host, nil
	}
	addrs, err := net.LookupHost(host)
	if err != nil {
		return "", fmt.Errorf("resolve %s: %w", host, err)
	}
	for _, a := range addrs {
		if ip := net.ParseIP(a); ip != nil && ip.To4() != nil {
			return a, nil
		}
	}
	return "", fmt.Errorf("no IPv4 address for %s", host)
}

func (d *DservLink) dialCmdLocked() error {
	c, err := net.DialTimeout("tcp", d.addr, dservCmdTimeout)
	if err != nil {
		return fmt.Errorf("dserv %s: %w", d.addr, err)
	}
	if tc, ok := c.(*net.TCPConn); ok {
		tc.SetKeepAlive(true)
		tc.SetKeepAlivePeriod(30 * time.Second)
	}
	if d.local == "" { // first dial fixes our identity; %reg/%unreg reuse it
		d.local, _, _ = net.SplitHostPort(c.LocalAddr().String())
	}
	d.cmd, d.cmdRd = c, bufio.NewReader(c)
	return nil
}

// command runs one text-protocol request in lockstep and returns dserv's
// reply, which is always "<rc> <body>\n". A dead connection (dserv restart)
// gets one retry on a fresh dial.
func (d *DservLink) command(cmd string) (int, string, error) {
	d.cmdMu.Lock()
	defer d.cmdMu.Unlock()
	var lastErr error
	for attempt := 0; attempt < 2; attempt++ {
		if d.cmd == nil {
			if err := d.dialCmdLocked(); err != nil {
				return 0, "", err
			}
		}
		d.cmd.SetDeadline(time.Now().Add(dservCmdTimeout))
		if _, err := d.cmd.Write([]byte(cmd + "\n")); err != nil {
			lastErr = err
		} else if line, err := d.cmdRd.ReadString('\n'); err != nil {
			lastErr = err
		} else {
			rcs, body, _ := strings.Cut(strings.TrimRight(line, "\r\n"), " ")
			rc, cerr := strconv.Atoi(rcs)
			if cerr != nil {
				return 0, "", fmt.Errorf("malformed dserv reply %q", line)
			}
			return rc, body, nil
		}
		d.cmd.Close()
		d.cmd, d.cmdRd = nil, nil
	}
	return 0, "", lastErr
}

func (d *DservLink) register() error {
	rc, _, err := d.command(fmt.Sprintf("%%reg %s %s 1", d.local, d.lport))
	if err != nil {
		return fmt.Errorf("%%reg: %w", err)
	}
	if rc != 1 {
		return fmt.Errorf("dserv could not connect back to %s:%s (rc=%d) -- firewall?",
			d.local, d.lport, rc)
	}
	return d.rematch()
}

func (d *DservLink) rematch() error {
	_, _, err := d.command(fmt.Sprintf("%%match %s %s extio/* 1", d.local, d.lport))
	return err // rc is always 1, even without a registration; only transport errors count
}

// matchRefresher re-asserts the %match set while the link lives. Idempotent
// dict insert on dserv -- no connection churn (unlike a %reg, which replaces
// the send client).
func (d *DservLink) matchRefresher() {
	t := time.NewTicker(matchRefresh)
	defer t.Stop()
	for range t.C {
		if d.isClosed() {
			return
		}
		d.rematch()
	}
}

func (d *DservLink) acceptLoop() {
	for {
		c, err := d.ln.Accept()
		if err != nil {
			return // listener closed
		}
		d.mu.Lock()
		if d.closed {
			d.mu.Unlock()
			c.Close()
			return
		}
		d.conns[c] = true
		d.pushes++
		d.mu.Unlock()
		go d.readFrames(c)
	}
}

// readFrames consumes 128-byte '>' frames from one dserv push connection --
// the same assembly the serial data CDC reader uses. When the LAST push conn
// closes and we didn't ask for it, the registration is gone (dserv restart or
// send failure): heal it.
func (d *DservLink) readFrames(c net.Conn) {
	defer func() {
		c.Close()
		d.mu.Lock()
		delete(d.conns, c)
		d.pushes--
		lost := d.pushes == 0 && !d.closed
		d.mu.Unlock()
		if lost {
			go d.reregister()
		}
	}()
	if tc, ok := c.(*net.TCPConn); ok {
		tc.SetKeepAlive(true)
		tc.SetKeepAlivePeriod(30 * time.Second)
	}
	buf := make([]byte, 4096)
	var frame [frameLen]byte
	have := 0
	for {
		n, err := c.Read(buf)
		if err != nil {
			return
		}
		if d.isClosed() {
			return
		}
		d.mu.Lock()
		d.bytes += uint64(n)
		d.mu.Unlock()
		for _, b := range buf[:n] {
			if have == 0 && b != '>' {
				d.mu.Lock()
				d.skip++
				d.mu.Unlock()
				continue // resync (shouldn't happen on TCP; counted if it does)
			}
			frame[have] = b
			have++
			if have == frameLen {
				have = 0
				if ev, ok := parseFrame(frame[:]); ok {
					d.emit(ev)
				} else {
					d.mu.Lock()
					d.bad++
					d.mu.Unlock()
				}
			}
		}
	}
}

// reregister re-establishes a lost registration with backoff, then re-seeds
// (a restarted dserv comes back with an empty table that the boxes refill via
// their own self-heal; the seed picks up whatever is already back).
func (d *DservLink) reregister() {
	// A %reg replace closes the old conn and opens a new one around the same
	// moment; give the new conn a beat to arrive before deciding we're dark.
	time.Sleep(500 * time.Millisecond)
	backoff := time.Second
	for {
		d.mu.Lock()
		done := d.closed || d.pushes > 0
		d.mu.Unlock()
		if done {
			return
		}
		if err := d.register(); err == nil {
			d.mu.Lock()
			d.regGen++
			d.mu.Unlock()
			d.seed()
			return
		}
		time.Sleep(backoff)
		if backoff < 10*time.Second {
			backoff *= 2
		}
	}
}

// seed loads the retained extio/* values (box manifests, last di/do levels,
// extio/boxes) so the UI has state before any live frame arrives. A key that
// already has a live value keeps it -- live frames postdate the snapshot.
func (d *DservLink) seed() {
	_, body, err := d.command("%getkeys")
	if err != nil {
		return
	}
	for _, k := range strings.Fields(body) {
		if !strings.HasPrefix(k, "extio/") {
			continue
		}
		rc, rep, err := d.command("%get " + k)
		if err != nil || rc != 1 {
			continue
		}
		if ev, ok := parseDservGet(rep); ok {
			d.mu.Lock()
			if _, live := d.table[ev.Name]; !live {
				d.table[ev.Name] = ev
			}
			d.mu.Unlock()
		}
	}
}

// parseDservGet decodes a %get reply body: "name dtype timestamp datalen
// {payload}", payload raw for string-ish dtypes and base64 otherwise
// (dpoint_to_string). A truncated reply ("...}") fails the length check.
func parseDservGet(s string) (DataEvent, bool) {
	var ev DataEvent
	name, rest, ok1 := strings.Cut(s, " ")
	dts, rest, ok2 := strings.Cut(rest, " ")
	tss, rest, ok3 := strings.Cut(rest, " ")
	lens, rest, ok4 := strings.Cut(rest, " ")
	if !ok1 || !ok2 || !ok3 || !ok4 ||
		!strings.HasPrefix(rest, "{") || !strings.HasSuffix(rest, "}") {
		return ev, false
	}
	payload := rest[1 : len(rest)-1]
	dt, err1 := strconv.ParseUint(dts, 10, 32)
	ts, err2 := strconv.ParseUint(tss, 10, 64)
	dlen, err3 := strconv.Atoi(lens)
	if err1 != nil || err2 != nil || err3 != nil || dlen < 0 {
		return ev, false
	}
	ev.Name, ev.TS, ev.Type = name, ts, uint32(dt)
	switch ev.Type {
	case dtString, dtScript, dtJSON:
		if len(payload) != dlen {
			return ev, false
		}
		ev.Val = payload
	default:
		raw, err := base64.StdEncoding.DecodeString(payload)
		if err != nil || len(raw) != dlen {
			return ev, false
		}
		ev.Val = decodeVal(ev.Type, raw)
	}
	return ev, true
}

func (d *DservLink) emit(ev DataEvent) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.closed {
		return
	}
	d.frames++
	d.table[ev.Name] = ev // bounded: only extio/* is matched
	for ch := range d.subs {
		select {
		case ch <- ev:
		default:
			d.drops++ // slow subscriber: drop (visible in /api/status)
		}
	}
}

// Subscribe mirrors DataLink.Subscribe: an event channel plus a replay of the
// latest di/do levels (and the live obs state) so a fresh or reconnected
// consumer repaints correctly instead of waiting for the next edge.
func (d *DservLink) Subscribe() (ch chan DataEvent, snap []DataEvent) {
	ch = make(chan DataEvent, 1024)
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.closed {
		close(ch)
		return ch, nil
	}
	d.subs[ch] = true
	for name, ev := range d.table {
		if isPinState(name) || strings.HasSuffix(name, "/state/in_obs") {
			ev.Snap = true
			snap = append(snap, ev)
		}
	}
	return ch, snap
}

func (d *DservLink) Unsubscribe(ch chan DataEvent) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.subs[ch] {
		delete(d.subs, ch)
		close(ch)
	}
}

func (d *DservLink) Stats() (frames, bad, drops, bytes, skip uint64) {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.frames, d.bad, d.drops, d.bytes, d.skip
}

// Health reports registration state for /api/status: completed registrations
// and live push connections (0 pushes = re-registration in progress).
func (d *DservLink) Health() (regGen uint64, pushes int) {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.regGen, d.pushes
}

// RegAddr is the connect-back endpoint we registered with dserv ("ip port"),
// as %unreg would want it. Shown in status for debugging.
func (d *DservLink) RegAddr() string { return d.local + " " + d.lport }

func (d *DservLink) isClosed() bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.closed
}

// Boxes returns the rig's live box list (extio/boxes, maintained by the extio
// subprocess from watchdog freshness) and its designated primary.
func (d *DservLink) Boxes() (boxes []string, primary string) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if ev, ok := d.table["extio/boxes"]; ok {
		if s, ok := ev.Val.(string); ok {
			boxes = strings.Fields(s)
		}
	}
	if ev, ok := d.table["extio/primary"]; ok {
		primary, _ = ev.Val.(string)
	}
	return
}

// StateFor returns box's retained state/* leaves (manifest + live levels) --
// the dserv driver's answer to the serial driver's `dump`.
func (d *DservLink) StateFor(box string) map[string]any {
	prefix := "extio/" + box + "/state/"
	out := map[string]any{}
	d.mu.Lock()
	defer d.mu.Unlock()
	for k, ev := range d.table {
		if strings.HasPrefix(k, prefix) {
			out[strings.TrimPrefix(k, prefix)] = ev.Val
		}
	}
	return out
}

// Keys a write may target: box config and commands, nothing else. No spaces,
// '=' or control bytes (they would change the %set line's meaning).
var dservSetKeyRe = regexp.MustCompile(`^extio/[A-Za-z0-9_-]+/(config|cmd)/[A-Za-z0-9_/-]+$`)

// Set publishes key=value (dserv stamps it and forwards per the extio
// subprocess's matches). Values travel as strings; the box coerces (mode
// words, strtol for ints).
func (d *DservLink) Set(key, value string) error {
	if !dservSetKeyRe.MatchString(key) {
		return fmt.Errorf("refusing key %q: only extio/<box>/config|cmd/... is writable", key)
	}
	if strings.ContainsAny(value, "\r\n") {
		return errors.New("value cannot contain newlines")
	}
	if len(key)+len(value) > 100 {
		return errors.New("key+value too long for the box's 128-byte frame")
	}
	rc, _, err := d.command("%set " + key + "=" + value)
	if err != nil {
		return err
	}
	if rc != 1 {
		return fmt.Errorf("%%set rejected (rc=%d)", rc)
	}
	return nil
}

func (d *DservLink) Close() {
	d.mu.Lock()
	if d.closed {
		d.mu.Unlock()
		return
	}
	d.closed = true
	subs := d.subs
	d.subs = nil
	conns := make([]net.Conn, 0, len(d.conns))
	for c := range d.conns {
		conns = append(conns, c)
	}
	d.mu.Unlock()

	// Best-effort %unreg so dserv stops pushing at a dead port (otherwise the
	// registration lingers until its next send fails). On the existing conn
	// only -- never re-dial just to say goodbye.
	d.cmdMu.Lock()
	if d.cmd != nil {
		d.cmd.SetDeadline(time.Now().Add(2 * time.Second))
		fmt.Fprintf(d.cmd, "%%unreg %s %s\n", d.local, d.lport)
		d.cmdRd.ReadString('\n')
		d.cmd.Close()
		d.cmd, d.cmdRd = nil, nil
	}
	d.cmdMu.Unlock()

	if d.ln != nil {
		d.ln.Close() // wakes acceptLoop
	}
	for _, c := range conns {
		c.Close() // wakes any blocked readFrames
	}
	for ch := range subs {
		close(ch)
	}
}
