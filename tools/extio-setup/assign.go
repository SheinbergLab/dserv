package main

// Adopt a discovered box: push its dserv target + a save straight to the box's
// config server (TCP :5010), the same fixed-128B dserv frames dserv itself would
// send. Works on a FRESH box (no dserv target) because nothing else holds :5010
// yet. dserv/ip is read live each loop, so the box connects to the rig
// immediately; the save persists it across reboots. No reboot needed.

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"strconv"
	"time"
)

const (
	dsMsgLen   = 128 // common/dserv_msg.h DSERV_MSG_LEN
	dsChar     = '>'
	dsTypeStr  = 1 // DSERV_STRING
	dsTypeInt  = 5 // DSERV_INT
	cfgSrvPort = 5010
)

// buildFrame lays out one datapoint frame: '>' + u16 varlen + name + u64 ts(0) +
// u32 type + u32 datalen + data, zero-padded to 128B, all little-endian.
func buildFrame(name string, dtype uint32, data []byte) ([]byte, error) {
	if 3+len(name)+16+len(data) > dsMsgLen {
		return nil, fmt.Errorf("frame overflows 128B (name=%d data=%d)", len(name), len(data))
	}
	f := make([]byte, dsMsgLen)
	f[0] = dsChar
	binary.LittleEndian.PutUint16(f[1:], uint16(len(name)))
	p := 3 + copy(f[3:], name)
	binary.LittleEndian.PutUint64(f[p:], 0) // ts 0 => box stamps on arrival
	p += 8
	binary.LittleEndian.PutUint32(f[p:], dtype)
	p += 4
	binary.LittleEndian.PutUint32(f[p:], uint32(len(data)))
	p += 4
	copy(f[p:], data)
	return f, nil
}

func intData(v int32) []byte {
	b := make([]byte, 4)
	binary.LittleEndian.PutUint32(b, uint32(v))
	return b
}

// assignTarget sends config/dserv/ip + config/dserv/port + cmd/save to boxIP:5010.
func assignTarget(boxIP, name, dservIP string, dservPort int) error {
	pfx := "extio/" + name
	f1, err := buildFrame(pfx+"/config/dserv/ip", dsTypeStr, []byte(dservIP))
	if err != nil {
		return err
	}
	f2, err := buildFrame(pfx+"/config/dserv/port", dsTypeInt, intData(int32(dservPort)))
	if err != nil {
		return err
	}
	f3, err := buildFrame(pfx+"/cmd/save", dsTypeInt, intData(1))
	if err != nil {
		return err
	}
	conn, err := net.DialTimeout("tcp", net.JoinHostPort(boxIP, strconv.Itoa(cfgSrvPort)), 3*time.Second)
	if err != nil {
		return fmt.Errorf("connect %s:%d: %w (config server not listening?)", boxIP, cfgSrvPort, err)
	}
	defer conn.Close()
	conn.SetWriteDeadline(time.Now().Add(3 * time.Second))
	// In order: target then save. A small gap so the box applies each frame (it
	// reads one 128B frame per loop pass) before the save snapshots the config.
	for _, f := range [][]byte{f1, f2, f3} {
		if _, err := conn.Write(f); err != nil {
			return fmt.Errorf("write to box: %w", err)
		}
		time.Sleep(40 * time.Millisecond)
	}
	return nil
}

func (s *server) handleAssign(w http.ResponseWriter, r *http.Request) {
	var req struct {
		IP        string `json:"ip"`
		Name      string `json:"name"`
		DservIP   string `json:"dservIP"`
		DservPort int    `json:"dservPort"`
	}
	if json.NewDecoder(r.Body).Decode(&req) != nil || req.IP == "" || req.Name == "" || req.DservIP == "" {
		httpErr(w, 400, "need {ip, name, dservIP, dservPort}")
		return
	}
	if req.DservPort == 0 {
		req.DservPort = 4620
	}
	if net.ParseIP(req.IP) == nil || net.ParseIP(req.DservIP) == nil {
		httpErr(w, 400, "bad IP")
		return
	}
	if err := assignTarget(req.IP, req.Name, req.DservIP, req.DservPort); err != nil {
		httpErr(w, 502, err.Error())
		return
	}
	writeJSON(w, map[string]any{"ok": true, "assigned": req.DservIP + ":" + strconv.Itoa(req.DservPort)})
}
