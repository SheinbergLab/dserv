// timerole.go - the panel's window onto this box's time role.
//
// The panel never implements time logic: reads are computed from stable
// contracts (the dserv-ptp4l*/dserv-phc2sys* unit names, the ntp-client
// tier's conf file, chronyc's CSV), and every write is dserv-ptp-setup run
// with the caller's arguments -- so each refusal the tool owns (no sane
// grandmaster visible, PHC-less NIC, active-role conflicts) reaches the
// panel verbatim, and the panel can never do something the CLI would refuse.
//
// On a box without the tooling (a stim-profile display box), apply falls
// back to fetching the registry's own /ptp/setup installer and running it
// with the same arguments -- the identical one-liner a human would paste.

package main

import (
	"context"
	"fmt"
	"net/http"
	"os"
	"os/exec"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"sync/atomic"
	"time"
)

// ptpSetupPath is where both delivery paths (the dserv .deb and the
// registry's /ptp/setup installer) place the tool.
const ptpSetupPath = "/usr/local/dserv/scripts/dserv-ptp-setup"

// ntpClientConf mirrors NTP_CONF in dserv-ptp-setup -- the ntp-client tier's
// one artifact.
const ntpClientConf = "/etc/chrony/conf.d/10-dserv-ntp-client.conf"

// ChronyTracking is the slice of `chronyc -c tracking` the panel shows.
// Seconds, signed, straight from chrony; the panel renders microseconds.
type ChronyTracking struct {
	Ref     string  `json:"ref"`
	Stratum int     `json:"stratum"`
	Offset  float64 `json:"offset"`
	RMS     float64 `json:"rms"`
}

// TimeStatus is the observed answer to "what steers this box's clock".
type TimeStatus struct {
	// Role: "grandmaster" | "client" | "ntp-client" | "none", from which
	// dserv time units are ACTIVE and whether the ntp-client conf exists.
	Role   string `json:"role"`
	Iface  string `json:"iface,omitempty"`  // grandmaster / client
	Server string `json:"server,omitempty"` // ntp-client tier

	// NTPDaemon is whichever NTP daemon is active, dserv-managed or not.
	NTPDaemon string `json:"ntpDaemon,omitempty"`

	// FreeRunning: nothing steers CLOCK_REALTIME -- no phc2sys client and
	// no NTP daemon. The one state a status surface must never hide: it is
	// how the rig Pi spent 29 h skewed with every box reading "offline".
	FreeRunning bool `json:"freeRunning"`

	// Units maps every loaded dserv time-unit INSTANCE to its active state,
	// so an enabled-but-failed unit is visible rather than just "no role".
	Units map[string]string `json:"units,omitempty"`

	Tracking      *ChronyTracking `json:"tracking,omitempty"`
	ToolInstalled bool            `json:"toolInstalled"`
	Registry      string          `json:"registry,omitempty"` // for the fetch hint / fallback
}

// timeUnitInstances returns loaded instances of the four dserv time-unit
// templates mapped to their active state. The unit names are the published
// contract between dserv-ptp-setup and everything else, which is what makes
// reading them here not a second implementation.
func timeUnitInstances() map[string]string {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	out, err := exec.CommandContext(ctx, "systemctl", "list-units", "--all",
		"--no-legend", "--plain", "--no-pager",
		"dserv-ptp4l*.service", "dserv-phc2sys*.service").Output()
	if err != nil {
		return nil
	}
	units := map[string]string{}
	for _, line := range strings.Split(string(out), "\n") {
		f := strings.Fields(line)
		if len(f) < 3 || !strings.Contains(f[0], "@") {
			continue
		}
		units[f[0]] = f[2] // ACTIVE column
	}
	if len(units) == 0 {
		return nil
	}
	return units
}

// activeNTPDaemon names the running NTP daemon, deduplicating the
// chrony/chronyd alias pair that makes naive lists report it twice.
func activeNTPDaemon() string {
	for _, u := range []string{"chrony.service", "chronyd.service", "systemd-timesyncd.service", "ntp.service", "ntpsec.service"} {
		if unitActive(u) {
			return strings.TrimSuffix(strings.TrimPrefix(u, "systemd-"), ".service")
		}
	}
	return ""
}

// chronyTracking parses `chronyc -c tracking`: field 1 is the reference
// name, 2 stratum, 4 the current system-time offset, 6 the RMS offset --
// positions verified against a live chrony 4.x on the fleet.
func chronyTracking() *ChronyTracking {
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	out, err := exec.CommandContext(ctx, "chronyc", "-c", "tracking").Output()
	if err != nil {
		return nil
	}
	f := strings.Split(strings.TrimSpace(string(out)), ",")
	if len(f) < 7 {
		return nil
	}
	t := &ChronyTracking{Ref: f[1]}
	t.Stratum, _ = strconv.Atoi(f[2])
	t.Offset, _ = strconv.ParseFloat(f[4], 64)
	t.RMS, _ = strconv.ParseFloat(f[6], 64)
	return t
}

func (a *Agent) getTimeStatus() TimeStatus {
	ts := TimeStatus{Role: "none", Units: timeUnitInstances(), Registry: a.registryBase()}
	if _, err := os.Stat(ptpSetupPath); err == nil {
		ts.ToolInstalled = true
	}

	phcClientActive := false
	for unit, state := range ts.Units {
		if state != "active" {
			continue
		}
		switch {
		case strings.HasPrefix(unit, "dserv-ptp4l-client@"):
			ts.Role = "client"
			ts.Iface = strings.TrimSuffix(strings.TrimPrefix(unit, "dserv-ptp4l-client@"), ".service")
		case strings.HasPrefix(unit, "dserv-ptp4l@"):
			ts.Role = "grandmaster"
			ts.Iface = strings.TrimSuffix(strings.TrimPrefix(unit, "dserv-ptp4l@"), ".service")
		case strings.HasPrefix(unit, "dserv-phc2sys-client@"):
			phcClientActive = true
		}
	}

	ts.NTPDaemon = activeNTPDaemon()
	if data, err := os.ReadFile(ntpClientConf); err == nil {
		for _, line := range strings.Split(string(data), "\n") {
			if f := strings.Fields(line); len(f) >= 2 && f[0] == "server" {
				ts.Server = f[1]
				if ts.Role == "none" {
					ts.Role = "ntp-client"
				}
				break
			}
		}
	}
	if strings.HasPrefix(ts.NTPDaemon, "chrony") {
		ts.Tracking = chronyTracking()
	}

	// Mirrors the script's own "time source" reasoning: the client's phc2sys
	// steers CLOCK_REALTIME from the PHC; a grandmaster's phc2sys steers the
	// PHC FROM the system clock and is not itself a source for it.
	ts.FreeRunning = !phcClientActive && ts.NTPDaemon == ""

	// The detection above is systemd-shaped; on macOS the OS's own timed
	// keeps the clock and "free-running" would be a false alarm on every
	// Mac rig's panel.
	if runtime.GOOS == "darwin" {
		ts.FreeRunning = false
		if ts.NTPDaemon == "" {
			ts.NTPDaemon = "timed (macOS)"
		}
	}
	return ts
}

// TimeCandidates feeds the panel's Set-role form: the tool's own candidates
// text as guidance (never parsed), and the host's interfaces for the picker.
// The tool remains the validator -- a non-capable choice fails at apply with
// the tool's message, which is the message worth reading.
type TimeCandidates struct {
	Candidates string   `json:"candidates,omitempty"`
	Ifaces     []string `json:"ifaces,omitempty"`
}

func (a *Agent) getTimeCandidates() TimeCandidates {
	tc := TimeCandidates{}
	if _, err := os.Stat(ptpSetupPath); err == nil {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		out, _ := exec.CommandContext(ctx, ptpSetupPath, "candidates").CombinedOutput()
		tc.Candidates = strings.TrimSpace(string(out))
	}
	if entries, err := os.ReadDir("/sys/class/net"); err == nil {
		for _, e := range entries {
			if n := e.Name(); n != "lo" {
				tc.Ifaces = append(tc.Ifaces, n)
			}
		}
	}
	return tc
}

// One time-role operation at a time: two concurrent applies would race each
// other's systemctl/apt work, and the second click is only ever impatience.
var timeRoleBusy int32

var timeArgRe = regexp.MustCompile(`^[A-Za-z0-9._:-]+$`)

// startTimeRole validates and launches an apply/disable in the background;
// the outcome is broadcast as time_role_result. Backgrounded because apply
// can legitimately take a minute (apt install + the tool's 30 s reach wait),
// and the WS read loop must not sit behind it.
func (a *Agent) startTimeRole(action, role, arg string) error {
	switch action {
	case "disable":
	case "apply":
		switch role {
		case "grandmaster", "client", "ntp-client":
		default:
			return fmt.Errorf("unknown role %q", role)
		}
		if !timeArgRe.MatchString(arg) {
			return fmt.Errorf("invalid %s argument %q", role, arg)
		}
	default:
		return fmt.Errorf("unknown action %q", action)
	}
	if !atomic.CompareAndSwapInt32(&timeRoleBusy, 0, 1) {
		return fmt.Errorf("a time-role operation is already running")
	}

	go func() {
		defer atomic.StoreInt32(&timeRoleBusy, 0)
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
		defer cancel()

		var cmd *exec.Cmd
		label := action
		if action == "disable" {
			cmd = exec.CommandContext(ctx, "sudo", ptpSetupPath, "disable")
		} else {
			label = role + " " + arg
			if _, err := os.Stat(ptpSetupPath); err == nil {
				cmd = exec.CommandContext(ctx, "sudo", ptpSetupPath, role, arg)
			} else {
				// No tooling here (display box). Fetch the registry's own
				// installer and hand it the same arguments -- the one-liner
				// a human would paste, with the same shebang guard as the
				// retype path.
				registry := a.registryBase()
				if registry == "" {
					a.broadcast(WSResponse{Type: "time_role_result", Error: "time tooling is not installed and no registry is configured to fetch it from"})
					return
				}
				resp, err := a.http.Get(registry + "/ptp/setup")
				if err != nil {
					a.broadcast(WSResponse{Type: "time_role_result", Error: "could not fetch " + registry + "/ptp/setup: " + err.Error()})
					return
				}
				body := make([]byte, 0, 512*1024)
				buf := make([]byte, 32*1024)
				for {
					n, rerr := resp.Body.Read(buf)
					body = append(body, buf[:n]...)
					if rerr != nil {
						break
					}
				}
				resp.Body.Close()
				if resp.StatusCode != 200 || !strings.HasPrefix(string(body), "#!") {
					a.broadcast(WSResponse{Type: "time_role_result", Error: registry + "/ptp/setup did not return an installer script"})
					return
				}
				tmp, err := os.CreateTemp("", "dserv-ptp-setup-*.sh")
				if err != nil {
					a.broadcast(WSResponse{Type: "time_role_result", Error: err.Error()})
					return
				}
				tmp.Write(body)
				tmp.Close()
				defer os.Remove(tmp.Name())
				cmd = exec.CommandContext(ctx, "sudo", "bash", tmp.Name(), role, arg)
			}
		}

		out, err := cmd.CombinedOutput()
		result := WSResponse{Type: "time_role_result", Success: err == nil,
			Data: map[string]interface{}{"action": action, "label": label, "output": strings.TrimSpace(string(out))}}
		if err != nil && strings.TrimSpace(string(out)) == "" {
			result.Error = err.Error()
		}
		a.broadcast(result)
	}()
	return nil
}

// GET /api/time/status - what steers this box's clock right now.
func (a *Agent) handleTimeStatus(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" {
		http.Error(w, "Method not allowed", 405)
		return
	}
	writeJSON(w, 200, a.getTimeStatus())
}

// POST /api/time/role?action=apply&role=client&arg=eth0 (or action=disable) -
// curl-scriptable twin of the panel flow; result lands in the agent log and
// any connected panel's broadcast. GET with action=status returns the tool's
// full status text synchronously.
func (a *Agent) handleTimeRole(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	if r.Method == "GET" && q.Get("action") == "status" {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		out, _ := exec.CommandContext(ctx, ptpSetupPath, "status").CombinedOutput()
		writeJSON(w, 200, map[string]string{"output": string(out)})
		return
	}
	if r.Method != "POST" {
		http.Error(w, "Method not allowed", 405)
		return
	}
	if err := a.startTimeRole(q.Get("action"), q.Get("role"), q.Get("arg")); err != nil {
		writeJSON(w, 400, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, 202, map[string]string{"status": "started"})
}
