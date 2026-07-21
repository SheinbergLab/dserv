package main

// LAN discovery: listen for the UDP beacons an Ethernet extio box broadcasts
// (wizchip_dserv_config.c beacon_send -> box_net_beacon, port 5011). Lets the UI
// FIND a box that has a DHCP lease but no dserv target yet, so a fresh box can be
// adopted over the web instead of a serial cable. Read-only here: the beacon +
// this listing prove discovery; assigning the target is a follow-up.

import (
	"encoding/json"
	"log"
	"net"
	"net/http"
	"sync"
	"time"
)

const beaconPort = 5011
const beaconTTL = 12 * time.Second // drop a box this long after its last beacon

// beacon mirrors the firmware's UDP JSON payload.
type beacon struct {
	Name   string `json:"name"`
	IP     string `json:"ip"`
	FW     string `json:"fw"`
	Board  string `json:"board"`
	Build  string `json:"build"`
	Target string `json:"target"` // "a.b.c.d:port"; 0.0.0.0 => unconfigured
}

// discovered is a beacon plus liveness for the UI.
type discovered struct {
	beacon
	AgeMs      int64 `json:"ageMs"`
	Configured bool  `json:"configured"` // has a real dserv target
	lastSeen   time.Time
}

type discovery struct {
	mu   sync.Mutex
	seen map[string]*discovered // keyed by box IP
	conn *net.UDPConn
}

func newDiscovery() *discovery { return &discovery{seen: map[string]*discovered{}} }

// start opens the UDP listener. Best effort: a taken port (another instance)
// just disables discovery rather than failing the whole tool.
func (d *discovery) start() error {
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{Port: beaconPort, IP: net.IPv4zero})
	if err != nil {
		return err
	}
	d.conn = conn
	go d.loop()
	return nil
}

func (d *discovery) loop() {
	buf := make([]byte, 2048)
	for {
		n, _, err := d.conn.ReadFromUDP(buf)
		if err != nil {
			return // socket closed
		}
		var b struct {
			T string `json:"t"`
			beacon
		}
		if json.Unmarshal(buf[:n], &b) != nil || b.T != "extio" || b.IP == "" {
			continue // not one of ours / malformed
		}
		d.mu.Lock()
		d.seen[b.IP] = &discovered{beacon: b.beacon, lastSeen: time.Now()}
		d.mu.Unlock()
	}
}

// list returns the currently-live beacons, expiring stale ones.
func (d *discovery) list() []discovered {
	d.mu.Lock()
	defer d.mu.Unlock()
	now := time.Now()
	out := []discovered{}
	for ip, v := range d.seen {
		age := now.Sub(v.lastSeen)
		if age > beaconTTL {
			delete(d.seen, ip)
			continue
		}
		cp := *v
		cp.AgeMs = age.Milliseconds()
		// unconfigured => target host is all-zero (e.g. "0.0.0.0:4620")
		cp.Configured = cp.Target != "" && cp.Target[:min(7, len(cp.Target))] != "0.0.0.0"
		out = append(out, cp)
	}
	return out
}

func (s *server) handleDiscover(w http.ResponseWriter, r *http.Request) {
	var list []discovered
	if s.disco != nil {
		list = s.disco.list()
	}
	writeJSON(w, map[string]any{"boxes": list, "enabled": s.disco != nil})
}

// startDiscovery is called once at server construction; a failure is logged and
// leaves discovery disabled (s.disco stays nil, the API reports enabled=false).
func (s *server) startDiscovery() {
	d := newDiscovery()
	if err := d.start(); err != nil {
		log.Printf("discovery disabled: %v (UDP :%d unavailable)", err, beaconPort)
		return
	}
	s.disco = d
}
