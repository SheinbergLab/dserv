// boxreport.go - agents report their identity + time state to the registry.
//
// PUSH, necessarily: the registry is a cloud box that cannot reach into rig
// LANs (it cannot even see 192.168.88.50), so it can never poll agents. The
// direction matches the house pattern -- dserv's mesh subprocess already
// heartbeats outward -- and gives every box a voice regardless of whether it
// runs dserv: a stim-profile display box reports exactly like an acquisition
// box, which today's dserv-driven heartbeats cannot do.
//
// The report is the two things this session made declarable and observable:
// box.conf (what the box says it is) and TimeStatus (what actually steers
// its clock). The workgroup page renders them side by side; comparing them
// is the whole point.

package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"html"
	"net"
	"net/http"
	"os"
	"sort"
	"strings"
	"sync"
	"time"
)

// BoxReport is one agent's periodic self-description.
type BoxReport struct {
	Hostname     string       `json:"hostname"`
	IP           string       `json:"ip,omitempty"`
	Workgroup    string       `json:"workgroup"`
	AgentVersion string       `json:"agentVersion,omitempty"`
	DservVersion string       `json:"dservVersion,omitempty"`
	Box          *BoxIdentity `json:"box,omitempty"`
	Time         *TimeStatus  `json:"time,omitempty"`

	// Stamped by the registry, never by the sender.
	ReceivedAt time.Time `json:"receivedAt,omitempty"`
	RemoteAddr string    `json:"remoteAddr,omitempty"`
}

// ---- agent side (client mode) ----

// boxReportInterval is deliberately relaxed: the fleet view answers "what is
// this box and what clock does it live on", which changes on retypes and
// role changes, not per second. Changes ALSO trigger an immediate report.
const boxReportInterval = 60 * time.Second

// localIPv4 returns the first global unicast IPv4 -- the address that means
// something within the site, as opposed to whatever NAT the registry sees.
func localIPv4() string {
	addrs, err := net.InterfaceAddrs()
	if err != nil {
		return ""
	}
	for _, addr := range addrs {
		if ipn, ok := addr.(*net.IPNet); ok && ipn.IP.To4() != nil && !ipn.IP.IsLoopback() && !ipn.IP.IsLinkLocalUnicast() {
			return ipn.IP.String()
		}
	}
	return ""
}

func (a *Agent) buildBoxReport() BoxReport {
	hostname, _ := os.Hostname()
	r := BoxReport{
		Hostname:     hostname,
		IP:           localIPv4(),
		Workgroup:    a.cfg.Workgroup,
		AgentVersion: version,
		Box:          readBoxIdentity(boxConfPath),
	}
	ts := a.getTimeStatus()
	r.Time = &ts
	if d := a.getDservStatus(); d.Version != "" {
		r.DservVersion = d.Version
	}
	return r
}

// sendBoxReports posts one report to every configured registry. Best-effort
// and quiet on failure: a box on a flaky uplink should not fill its own log
// with the registry's unavailability every minute.
func (a *Agent) sendBoxReports() {
	report := a.buildBoxReport()
	payload, err := json.Marshal(report)
	if err != nil {
		return
	}
	for _, reg := range a.cfg.RegistryURLs {
		url := strings.TrimRight(reg, "/") + "/api/v1/boxes/report"
		resp, err := a.http.Post(url, "application/json", bytes.NewReader(payload))
		if err == nil {
			resp.Body.Close()
		}
	}
}

// boxReportLoop runs for the life of a client-mode agent with a registry and
// workgroup configured. First report immediately -- an agent restart (which
// every retype and self-update ends in) doubles as the change notification.
func (a *Agent) boxReportLoop() {
	a.sendBoxReports()
	for range time.Tick(boxReportInterval) {
		a.sendBoxReports()
	}
}

// ---- registry side (server mode) ----

// boxReportStore: workgroup -> hostname -> latest report. Memory only, like
// the mesh cache: a registry restart loses at most one interval's worth.
var (
	boxReportMu    sync.Mutex
	boxReportStore = map[string]map[string]*BoxReport{}
)

// boxReportTTL: entries older than this are dropped entirely (a
// decommissioned box should eventually leave the page, not haunt it red).
const boxReportTTL = 24 * time.Hour

// POST /api/v1/boxes/report - unauthenticated like /api/v1/heartbeat: fresh
// boxes need to report before anyone has arranged tokens.
func (a *Agent) handleBoxReport(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		http.Error(w, "Method not allowed", 405)
		return
	}
	var report BoxReport
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 64*1024)).Decode(&report); err != nil {
		writeJSON(w, 400, map[string]string{"error": err.Error()})
		return
	}
	if report.Hostname == "" || report.Workgroup == "" {
		writeJSON(w, 400, map[string]string{"error": "hostname and workgroup required"})
		return
	}
	report.ReceivedAt = time.Now()
	report.RemoteAddr = r.RemoteAddr

	boxReportMu.Lock()
	wg := boxReportStore[report.Workgroup]
	if wg == nil {
		wg = map[string]*BoxReport{}
		boxReportStore[report.Workgroup] = wg
	}
	wg[report.Hostname] = &report
	for host, old := range wg {
		if time.Since(old.ReceivedAt) > boxReportTTL {
			delete(wg, host)
		}
	}
	boxReportMu.Unlock()

	writeJSON(w, 200, map[string]string{"status": "ok"})
}

func getBoxReports(workgroup string) []*BoxReport {
	boxReportMu.Lock()
	defer boxReportMu.Unlock()
	var out []*BoxReport
	for _, r := range boxReportStore[workgroup] {
		out = append(out, r)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Hostname < out[j].Hostname })
	return out
}

// GET /api/v1/boxes?workgroup=X - the rollup as data, for tooling.
func (a *Agent) handleBoxesQuery(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" {
		http.Error(w, "Method not allowed", 405)
		return
	}
	workgroup := r.URL.Query().Get("workgroup")
	if workgroup == "" {
		writeJSON(w, 400, map[string]string{"error": "workgroup required"})
		return
	}
	writeJSON(w, 200, map[string]interface{}{"workgroup": workgroup, "boxes": getBoxReports(workgroup)})
}

// timeCellHTML compresses a report's time story into one table cell, with
// the same judgments as the panel: green when declared and observed agree,
// amber on drift, red for free-running, gray nudges otherwise.
func timeCellHTML(r *BoxReport) string {
	ts := r.Time
	if ts == nil {
		return `<span style="color:#71767b">—</span>`
	}
	var obs string
	switch ts.Role {
	case "grandmaster":
		obs = fmt.Sprintf(`<span style="color:#34d399">grandmaster</span> on %s`, html.EscapeString(ts.Iface))
	case "client":
		obs = fmt.Sprintf(`<span style="color:#34d399">ptp client</span> on %s`, html.EscapeString(ts.Iface))
	case "ntp-client":
		obs = fmt.Sprintf(`chrony → %s`, html.EscapeString(ts.Server))
		if ts.Tracking != nil {
			obs += fmt.Sprintf(` <span style="color:#71767b">(%.0f µs)</span>`, ts.Tracking.Offset*1e6)
		}
	default:
		if ts.FreeRunning {
			return `<span style="color:#f87171">free-running ⚠</span>`
		}
		obs = fmt.Sprintf(`<span style="color:#71767b">ntp (%s)</span>`, html.EscapeString(ts.NTPDaemon))
	}

	declared := ts.Declared
	if declared == "" {
		if ts.Role != "none" {
			obs += ` <span style="color:#71767b" title="running role not declared in box.conf">†</span>`
		}
		return obs
	}
	sp := strings.IndexByte(declared, ' ')
	dr, da := declared, ""
	if sp > 0 {
		dr, da = declared[:sp], strings.TrimSpace(declared[sp+1:])
	}
	if dr == ts.Role && (da == ts.Iface || da == ts.Server) {
		return obs + ` <span style="color:#34d399">✓</span>`
	}
	return obs + fmt.Sprintf(` <span style="color:#fbbf24" title="declared but not running">⇢ declared: %s</span>`, html.EscapeString(declared))
}

// fleetTableHTML renders ONE ROW PER MACHINE, joined from the page's two
// independent reporters: dserv's mesh heartbeat (proof a dserv is alive)
// and the agent's self-report (what the machine IS -- which exists even
// where dserv does not). Rendering them as two tables put officepi in both
// and stim boxes only in one, which read as two different populations
// instead of two properties of the same fleet. dserv liveness is a cell
// now, not a table.
//
// GMs are COUNTED and NAMED, never auto-alarmed: this workgroup
// legitimately runs three (rig switch, office segment, one island), and no
// per-workgroup or per-subnet rule can tell those apart -- two of the
// segments even share 192.168.88.0/24. A declared segment= label is the
// future fix; until then the human who knows the topology reads the list.
func fleetTableHTML(nodes []*MeshNode, reports []*BoxReport) (string, int) {
	type machine struct {
		node   *MeshNode
		report *BoxReport
	}
	// Join on the SHORT hostname: the two stacks disagree about domain
	// suffixes (Go's os.Hostname says MacBook-Air.local, dserv's heartbeat
	// may say .lan or nothing), and any mismatch splits one machine into
	// two rows. Everything before the first dot is the name this fleet
	// actually uses; nobody runs two boxes distinguished only by domain.
	key := func(h string) string {
		h = strings.ToLower(h)
		if i := strings.IndexByte(h, '.'); i > 0 {
			h = h[:i]
		}
		return h
	}

	machines := map[string]*machine{}
	name := map[string]string{} // key -> display name (first seen wins)
	for _, n := range nodes {
		k := key(n.Hostname)
		machines[k] = &machine{node: n}
		name[k] = n.Hostname
	}
	for _, r := range reports {
		k := key(r.Hostname)
		if m, ok := machines[k]; ok {
			m.report = r
		} else {
			machines[k] = &machine{report: r}
			name[k] = r.Hostname
		}
	}
	keys := make([]string, 0, len(machines))
	for k := range machines {
		keys = append(keys, k)
	}
	sort.Strings(keys)

	var gms []string
	var rows strings.Builder
	for _, k := range keys {
		m := machines[k]
		r := m.report

		// Host links to the box's day-to-day surface: ESS Control (dserv's
		// web UI, :2565 by convention) wherever a dserv exists -- system
		// management is one click inside it now, which makes it the more
		// useful landing than the management panel. A display box has no
		// :2565, so its name goes to the agent panel instead. The gear is
		// the direct path to management for anyone who wants it first.
		host := html.EscapeString(name[k])
		ip := ""
		if r != nil && r.IP != "" {
			ip = r.IP
		} else if m.node != nil {
			ip = m.node.IP
		}
		hasDserv := m.node != nil || (r != nil && r.DservVersion != "")
		if ip != "" {
			eip := html.EscapeString(ip)
			if hasDserv {
				host = fmt.Sprintf(`<a href="http://%s:2565/ess_control.html" target="_blank">%s</a>`, eip, host)
				if r != nil {
					host += fmt.Sprintf(` <a href="http://%s/" target="_blank" title="management panel (dserv-agent)" style="text-decoration:none;font-size:12px">⚙</a>`, eip)
				}
			} else if r != nil {
				host = fmt.Sprintf(`<a href="http://%s/" target="_blank">%s</a>`, eip, host)
			}
		}

		identity := `<span style="color:#71767b">—</span>`
		timeCell := `<span style="color:#71767b">—</span>`
		agentCell := `<span style="color:#71767b">no agent report</span>`
		if r != nil {
			identity = `<span style="color:#71767b">unrecorded</span>`
			if r.Box != nil && r.Box.Profile != "" {
				identity = html.EscapeString(r.Box.Profile)
				if r.Box.StimMode == "windowed" {
					identity += " · windowed"
				}
				if r.Box.Role != "" {
					identity += fmt.Sprintf(` · <span style="color:#fbbf24">%s</span>`, html.EscapeString(r.Box.Role))
				}
			}
			timeCell = timeCellHTML(r)
			if r.Time != nil && r.Time.Role == "grandmaster" {
				gms = append(gms, fmt.Sprintf("%s (%s)", r.Hostname, r.Time.Iface))
			}
			age := time.Since(r.ReceivedAt).Round(time.Second)
			ageColor := "#34d399"
			if age > 10*time.Minute {
				ageColor = "#f87171"
			} else if age > 3*time.Minute {
				ageColor = "#fbbf24"
			}
			agentCell = fmt.Sprintf(`%s · <span style="color:%s">%s ago</span>`,
				html.EscapeString(r.AgentVersion), ageColor, age)
		}

		// dserv liveness from the heartbeat (its absence on a display box is
		// correct, not missing data); version from the agent's report, which
		// asks the running dserv itself.
		dservCell := `<span style="color:#71767b">—</span>`
		if m.node != nil {
			stateColor := "#34d399"
			switch m.node.State {
			case "stale":
				stateColor = "#fbbf24"
			case "unresponsive":
				stateColor = "#f87171"
			}
			ver := ""
			if r != nil && r.DservVersion != "" {
				ver = " " + html.EscapeString(r.DservVersion)
			}
			dservCell = fmt.Sprintf(`<span style="color:%s">●</span>%s <span style="color:#71767b">%s</span>`,
				stateColor, ver, html.EscapeString(m.node.State))
		} else if r != nil && r.DservVersion != "" {
			// Installed (the agent could ask it) but not heartbeating.
			dservCell = fmt.Sprintf(`%s <span style="color:#71767b">(no heartbeat)</span>`, html.EscapeString(r.DservVersion))
		}

		rows.WriteString(fmt.Sprintf(`
                <tr>
                    <td>%s</td>
                    <td><code>%s</code></td>
                    <td>%s</td>
                    <td>%s</td>
                    <td style="font-size:12px">%s</td>
                    <td style="font-size:12px;color:#8b98a5">%s</td>
                </tr>`,
			host, html.EscapeString(ip), identity, timeCell, dservCell, agentCell))
	}

	gmLine := ""
	if len(gms) > 0 {
		gmLine = fmt.Sprintf(`<p style="color:#8b98a5;font-size:13px;margin:6px 0 14px">grandmasters: <strong>%d</strong> — %s <span style="color:#71767b">(one per segment is correct; the count is yours to judge)</span></p>`,
			len(gms), html.EscapeString(strings.Join(gms, ", ")))
	}

	table := fmt.Sprintf(`
        %s
        <table>
            <thead>
                <tr><th>Host</th><th>IP</th><th>Identity</th><th>Time</th><th>dserv</th><th>Agent</th></tr>
            </thead>
            <tbody>%s</tbody>
        </table>`, gmLine, rows.String())
	return table, len(machines)
}
