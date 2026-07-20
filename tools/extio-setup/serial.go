// serial.go -- the USB-CDC console link to an extio box.
//
// Ports are matched by USB identity (VID:PID 2e8a:100b), never by "highest
// ttyACM"; the box presents two CDC interfaces and the CONSOLE is the first
// (macOS ...usbmodemXXX1 vs ...XXX3, Linux -if00 vs -if02 -> lower ACM).
// All reads are time-bounded and a single reader goroutine owns the port;
// exec() correlates a command with its OK/ERR response line.
package main

import (
	"errors"
	"sort"
	"strings"
	"sync"
	"time"

	"go.bug.st/serial"
	"go.bug.st/serial/enumerator"
)

const (
	extioVID = "2E8A"
	extioPID = "100B" // extio USB box
	// The BLE handheld enumerates as "dserv handheld" / 0x100C (a distinct PID so
	// the rig's usbio/extioconf box-globs never grab its dead data CDC). Accept it
	// here too so a DOCKED handheld can be configured over its USB console -- full
	// config (pins, name, mcp, save/reboot); its live DI/DO events ride the radio,
	// not this CDC, so the event stream just shows unavailable (handled).
	handheldPID = "100C"
)

// BoxPort is one physical box: its console port plus the data sibling.
type BoxPort struct {
	Console string `json:"console"`
	Data    string `json:"data,omitempty"`
	Serial  string `json:"serial,omitempty"`
}

// listBoxPorts finds extio boxes by USB identity and pairs their CDC
// interfaces (sorted: console first).
func listBoxPorts() ([]BoxPort, error) {
	ports, err := enumerator.GetDetailedPortsList()
	if err != nil {
		return nil, err
	}
	bySerial := map[string][]string{}
	for _, p := range ports {
		if !p.IsUSB {
			continue
		}
		if !strings.EqualFold(p.VID, extioVID) ||
			(!strings.EqualFold(p.PID, extioPID) && !strings.EqualFold(p.PID, handheldPID)) {
			continue
		}
		bySerial[p.SerialNumber] = append(bySerial[p.SerialNumber], p.Name)
	}
	var out []BoxPort
	for sn, names := range bySerial {
		sort.Strings(names)
		bp := BoxPort{Console: names[0], Serial: sn}
		if len(names) > 1 {
			bp.Data = names[1]
		}
		out = append(out, bp)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Console < out[j].Console })
	return out, nil
}

// Link is an open console connection. One reader goroutine assembles lines
// and fans them out to subscribers (the SSE console stream) and to at most
// one pending exec collector.
type Link struct {
	mu      sync.Mutex // guards port writes, subscribers, collector, closed
	port    serial.Port
	name    string
	closed  bool
	subs    map[chan string]bool
	collect chan string // non-nil while an exec is collecting response lines
	ring    []string    // recent lines, replayed to new console subscribers
}

func openLink(portName string) (*Link, error) {
	mode := &serial.Mode{BaudRate: 115200}
	p, err := serial.Open(portName, mode)
	if err != nil {
		return nil, err
	}
	p.SetReadTimeout(100 * time.Millisecond)
	l := &Link{port: p, name: portName, subs: map[chan string]bool{}}
	go l.reader()
	return l, nil
}

func (l *Link) Close() {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.closed {
		return
	}
	l.closed = true
	l.port.Close()
	for ch := range l.subs {
		close(ch)
	}
	l.subs = nil
}

func (l *Link) isClosed() bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.closed
}

// Subscribe returns a channel of console lines plus a replay of recent ones.
func (l *Link) Subscribe() (ch chan string, replay []string) {
	ch = make(chan string, 256)
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.closed {
		close(ch)
		return ch, nil
	}
	l.subs[ch] = true
	return ch, append([]string(nil), l.ring...)
}

func (l *Link) Unsubscribe(ch chan string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.subs[ch] {
		delete(l.subs, ch)
		close(ch)
	}
}

func (l *Link) reader() {
	buf := make([]byte, 512)
	var line []byte
	for {
		if l.isClosed() {
			return
		}
		n, err := l.port.Read(buf)
		if err != nil {
			// Unplugged or the port died: tear the link down so the UI
			// sees a clean disconnect instead of a wedged connection.
			l.Close()
			return
		}
		for _, b := range buf[:n] {
			switch b {
			case '\n':
				l.emit(strings.TrimRight(string(line), "\r"))
				line = line[:0]
			case '\r':
				// handled with the \n that follows (or dropped if bare)
			default:
				line = append(line, b)
			}
		}
	}
}

func (l *Link) emit(s string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.closed {
		return
	}
	l.ring = append(l.ring, s)
	if len(l.ring) > 500 {
		l.ring = l.ring[len(l.ring)-500:]
	}
	if l.collect != nil {
		select {
		case l.collect <- s:
		default:
		}
	}
	for ch := range l.subs {
		select {
		case ch <- s:
		default: // slow subscriber: drop rather than block the reader
		}
	}
}

// WriteRaw sends bytes to the console (interactive console input).
func (l *Link) WriteRaw(b []byte) error {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.closed {
		return errors.New("link closed")
	}
	_, err := l.port.Write(b)
	return err
}

var errExecBusy = errors.New("another command is in progress")

// Exec sends one CLI line and collects the response: lines up to and
// including the first OK/ERR, or until the console goes quiet. Every wait
// is bounded -- a wedged box costs at most `overall`.
func (l *Link) Exec(cmd string, quiet, overall time.Duration) ([]string, error) {
	l.mu.Lock()
	if l.closed {
		l.mu.Unlock()
		return nil, errors.New("link closed")
	}
	if l.collect != nil {
		l.mu.Unlock()
		return nil, errExecBusy
	}
	col := make(chan string, 512)
	l.collect = col
	_, err := l.port.Write([]byte(cmd + "\n"))
	l.mu.Unlock()
	if err != nil {
		l.clearCollect()
		return nil, err
	}

	var lines []string
	deadline := time.NewTimer(overall)
	defer deadline.Stop()
	for {
		idle := time.NewTimer(quiet)
		select {
		case s := <-col:
			idle.Stop()
			// The console echoes what we typed; skip that echo line.
			if len(lines) == 0 && strings.TrimSpace(s) == strings.TrimSpace(cmd) {
				continue
			}
			lines = append(lines, s)
			if strings.HasPrefix(s, "OK") || strings.HasPrefix(s, "ERR") {
				l.clearCollect()
				return lines, nil
			}
		case <-idle.C:
			if len(lines) > 0 || l.isClosed() {
				l.clearCollect()
				return lines, nil
			}
		case <-deadline.C:
			idle.Stop()
			l.clearCollect()
			return lines, errors.New("timeout waiting for response")
		}
	}
}

func (l *Link) clearCollect() {
	l.mu.Lock()
	l.collect = nil
	l.mu.Unlock()
}
