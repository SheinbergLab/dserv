// data.go -- live event view: read the box's DATA CDC interface and decode
// the 128-byte dserv datapoint frames it publishes (DI edges, DO readbacks,
// group chords, heartbeat). Same wire format as wiznet-io/common/dserv_msg.h:
//
//	off 0: '>'   1-2: u16 namelen   3..: name   then u64 ts, u32 type,
//	u32 datalen, data, zero pad to 128. Resync = skip bytes until '>'.
package main

import (
	"encoding/binary"
	"encoding/hex"
	"math"
	"strings"
	"sync"
	"time"

	"go.bug.st/serial"
)

const frameLen = 128

// dserv datatype subset (src/Datapoint.h via dserv_msg.h)
const (
	dtByte   = 0
	dtString = 1
	dtFloat  = 2
	dtDouble = 3
	dtShort  = 4
	dtInt    = 5
	dtEvt    = 9
	dtInt64  = 16
)

type DataEvent struct {
	Name string `json:"name"`
	TS   uint64 `json:"ts"`
	Type uint32 `json:"type"`
	Val  any    `json:"val"`
	Snap bool   `json:"snap,omitempty"` // replayed state, not a fresh edge
}

type DataLink struct {
	mu     sync.Mutex
	port   serial.Port
	name   string
	closed bool
	died   string // why the reader gave up (shared stream); "" = normal life
	subs   map[chan DataEvent]bool
	last   map[string]DataEvent // latest di/do value per name: replayed on subscribe
	frames uint64               // decoded frames
	bad    uint64               // 128-byte chunks that failed to parse (desync tell)
	drops  uint64               // events dropped on full subscriber channels
	bytes  uint64               // raw bytes read off the port
	skip   uint64               // bytes skipped during resync (should stay 0)
}

// A sole reader resyncs at most once, skipping under one frame of bytes at
// the initial mid-stream join. Skip climbing past this means ANOTHER PROCESS
// is consuming the same port and we each see fragments -- observed live
// 2026-07-11: dserv's usbio (root) held the data CDC, macOS admitted our open
// anyway (TIOCEXCL can't evict an earlier opener), and both sides lost ~half
// the frames. Reading on would corrupt the RIG's event stream, so we quit.
const skipSharedThreshold = 1024

func openDataLink(portName string) (*DataLink, error) {
	p, err := serial.Open(portName, &serial.Mode{BaudRate: 115200})
	if err != nil {
		return nil, err
	}
	p.SetReadTimeout(100 * time.Millisecond)
	// The box gates data-CDC publishing on DTR (tud_ready): assert it.
	p.SetDTR(true)
	p.SetRTS(true)
	d := &DataLink{port: p, name: portName,
		subs: map[chan DataEvent]bool{}, last: map[string]DataEvent{}}
	go d.reader()
	return d, nil
}

func (d *DataLink) Close() {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.closed {
		return
	}
	d.closed = true
	d.port.Close()
	for ch := range d.subs {
		close(ch)
	}
	d.subs = nil
}

func (d *DataLink) isClosed() bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.closed
}

// Subscribe returns an event channel plus a snapshot of the latest known
// di/do values -- so a fresh or RECONNECTED consumer repaints correct pin
// state instead of inheriting whatever it last saw before a gap.
func (d *DataLink) Subscribe() (ch chan DataEvent, snap []DataEvent) {
	ch = make(chan DataEvent, 1024)
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.closed {
		close(ch)
		return ch, nil
	}
	d.subs[ch] = true
	for _, ev := range d.last {
		ev.Snap = true
		snap = append(snap, ev)
	}
	return ch, snap
}

// Stats reports pipeline health: decoded frames, parse failures (desync
// tell), events dropped on slow subscribers, raw bytes read, resync skips.
func (d *DataLink) Stats() (frames, bad, drops, bytes, skip uint64) {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.frames, d.bad, d.drops, d.bytes, d.skip
}

func (d *DataLink) Unsubscribe(ch chan DataEvent) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.subs[ch] {
		delete(d.subs, ch)
		close(ch)
	}
}

func (d *DataLink) reader() {
	buf := make([]byte, 512)
	var frame [frameLen]byte
	have := 0
	for {
		if d.isClosed() {
			return
		}
		n, err := d.port.Read(buf)
		if err != nil {
			d.Close()
			return
		}
		if n > 0 {
			d.mu.Lock()
			d.bytes += uint64(n)
			d.mu.Unlock()
		}
		for _, b := range buf[:n] {
			if have == 0 && b != '>' {
				d.mu.Lock()
				d.skip++
				shared := d.skip > skipSharedThreshold
				d.mu.Unlock()
				if shared {
					d.mu.Lock()
					d.died = "another process is reading " + d.name +
						" (a second extio-setup, or a rig's dserv usbio module) -- frames " +
						"were splitting between us, so live events are OFF. Close the other " +
						"opener, or switch to the dserv driver (it shares cleanly)."
					d.mu.Unlock()
					d.Close()
					return
				}
				continue // resync
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

// Reason reports why the link closed itself ("" while alive / normal close).
func (d *DataLink) Reason() string {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.died
}

func (d *DataLink) emit(ev DataEvent) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.frames++
	// Track latest pin state for subscribe-time replay (di/do only: the
	// names are bounded by pin count, so this map stays small).
	if isPinState(ev.Name) {
		d.last[ev.Name] = ev
	}
	for ch := range d.subs {
		select {
		case ch <- ev:
		default:
			d.drops++ // slow subscriber: drop (visible in /api/status)
		}
	}
}

// isPinState reports whether name is a live pin level (.../state/di/N or
// do/N) -- the values replayed to fresh subscribers so a UI repaints pin
// state after a gap.
func isPinState(name string) bool {
	if i := strings.Index(name, "/state/d"); i >= 0 {
		leaf := name[i+len("/state/"):]
		return strings.HasPrefix(leaf, "di/") || strings.HasPrefix(leaf, "do/")
	}
	return false
}

func parseFrame(f []byte) (DataEvent, bool) {
	var ev DataEvent
	if f[0] != '>' {
		return ev, false
	}
	namelen := int(binary.LittleEndian.Uint16(f[1:3]))
	if namelen > frameLen-19 {
		return ev, false
	}
	p := 3 + namelen
	ev.Name = string(f[3:p])
	ev.TS = binary.LittleEndian.Uint64(f[p : p+8])
	ev.Type = binary.LittleEndian.Uint32(f[p+8 : p+12])
	datalen := int(binary.LittleEndian.Uint32(f[p+12 : p+16]))
	if namelen+datalen > frameLen-19 {
		return ev, false
	}
	ev.Val = decodeVal(ev.Type, f[p+16:p+16+datalen])
	return ev, true
}

// decodeVal turns a datapoint's raw little-endian payload into a native Go
// value. Shared by the serial frame parser and the dserv driver's %get
// snapshot decoder so both produce identical DataEvent values.
func decodeVal(dtype uint32, data []byte) any {
	switch dtype {
	case dtString:
		return string(data)
	case dtInt:
		if len(data) >= 4 {
			return int32(binary.LittleEndian.Uint32(data))
		}
	case dtShort:
		if len(data) >= 2 {
			return int16(binary.LittleEndian.Uint16(data))
		}
	case dtInt64:
		if len(data) >= 8 {
			return int64(binary.LittleEndian.Uint64(data))
		}
	case dtFloat:
		if len(data) >= 4 {
			return math.Float32frombits(binary.LittleEndian.Uint32(data))
		}
	case dtDouble:
		if len(data) >= 8 {
			return math.Float64frombits(binary.LittleEndian.Uint64(data))
		}
	case dtByte:
		if len(data) == 1 {
			return int(data[0])
		}
		return hex.EncodeToString(data)
	default: // EVT and anything else: hex, small and lossless
		return hex.EncodeToString(data)
	}
	return nil
}
