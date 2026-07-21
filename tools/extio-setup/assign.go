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

// nameValid mirrors the firmware's dserv_name_valid (non-empty, printable, no '/').
// Also rejects spaces so box names stay clean in datapoint paths.
func nameValid(s string) bool {
	if s == "" {
		return false
	}
	for _, r := range s {
		if r < 0x21 || r > 0x7e || r == '/' {
			return false
		}
	}
	return true
}

// assignTarget sends config/dserv/ip + port + cmd/save to boxIP:5010. If newName
// is set (and differs), a config/name frame goes FIRST (addressed by the current
// name), and the remaining frames address the box by its NEW name -- because the
// rename applies live, so the box's datapoint namespace changes mid-batch.
func assignTarget(boxIP, name, newName, dservIP string, dservPort int) error {
	var frames [][]byte
	eff := name // the name the target/save frames must be addressed to
	if newName != "" && newName != name {
		fn, err := buildFrame("extio/"+name+"/config/name", dsTypeStr, []byte(newName))
		if err != nil {
			return err
		}
		frames = append(frames, fn)
		eff = newName
	}
	pfx := "extio/" + eff
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
	frames = append(frames, f1, f2, f3)
	// The box's config server is a single-connection listener that must cycle
	// back to LISTEN after each close, so a connect can hit a brief re-listen
	// gap and get a RST -- retry a few times before giving up.
	addr := net.JoinHostPort(boxIP, strconv.Itoa(cfgSrvPort))
	var conn net.Conn
	for attempt := 0; attempt < 4; attempt++ {
		if conn, err = net.DialTimeout("tcp", addr, 2*time.Second); err == nil {
			break
		}
		time.Sleep(time.Duration(200*(attempt+1)) * time.Millisecond)
	}
	if err != nil {
		return fmt.Errorf("connect %s:%d: %w (config server busy or not listening?)", boxIP, cfgSrvPort, err)
	}
	defer conn.Close()
	conn.SetWriteDeadline(time.Now().Add(3 * time.Second))
	// In order: (rename,) target, save. A small gap so the box applies each frame
	// (it reads one 128B frame per loop pass) before the next -- the rename must
	// land before the frames addressed to the new name, and the save last.
	for _, f := range frames {
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
		NewName   string `json:"newName"` // optional rename
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
	if req.NewName != "" && !nameValid(req.NewName) {
		httpErr(w, 400, "bad name (printable, no '/' or spaces)")
		return
	}
	if err := assignTarget(req.IP, req.Name, req.NewName, req.DservIP, req.DservPort); err != nil {
		httpErr(w, 502, err.Error())
		return
	}
	writeJSON(w, map[string]any{"ok": true, "assigned": req.DservIP + ":" + strconv.Itoa(req.DservPort)})
}
