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

// boxSectionHTML renders the workgroup page's Boxes table + the grandmaster
// line. GMs are COUNTED and NAMED, never auto-alarmed: this workgroup
// legitimately runs three (rig switch, office segment, one island), and no
// per-workgroup or per-subnet rule can tell those apart -- two of the
// segments even share 192.168.88.0/24. A declared segment= label is the
// future fix; until then the human who knows the topology reads the list.
func boxSectionHTML(workgroup string) string {
	reports := getBoxReports(workgroup)
	if len(reports) == 0 {
		return ""
	}

	var gms []string
	var rows strings.Builder
	for _, r := range reports {
		if r.Time != nil && r.Time.Role == "grandmaster" {
			gms = append(gms, fmt.Sprintf("%s (%s)", r.Hostname, r.Time.Iface))
		}

		profile := `<span style="color:#71767b">unrecorded</span>`
		if r.Box != nil && r.Box.Profile != "" {
			profile = html.EscapeString(r.Box.Profile)
			if r.Box.StimMode == "windowed" {
				profile += " · windowed"
			}
			if r.Box.Role != "" {
				profile += fmt.Sprintf(` · <span style="color:#fbbf24">%s</span>`, html.EscapeString(r.Box.Role))
			}
		}

		age := time.Since(r.ReceivedAt).Round(time.Second)
		ageColor := "#34d399"
		if age > 10*time.Minute {
			ageColor = "#f87171"
		} else if age > 3*time.Minute {
			ageColor = "#fbbf24"
		}

		versions := html.EscapeString(r.AgentVersion)
		if r.DservVersion != "" {
			versions = "dserv " + html.EscapeString(r.DservVersion) + " · agent " + versions
		}

		rows.WriteString(fmt.Sprintf(`
                <tr>
                    <td>%s</td>
                    <td><code>%s</code></td>
                    <td>%s</td>
                    <td>%s</td>
                    <td style="font-size:12px;color:#8b98a5">%s</td>
                    <td><span style="color:%s">%s ago</span></td>
                </tr>`,
			html.EscapeString(r.Hostname), html.EscapeString(r.IP), profile,
			timeCellHTML(r), versions, ageColor, age))
	}

	gmLine := ""
	if len(gms) > 0 {
		gmLine = fmt.Sprintf(`<p style="color:#8b98a5;font-size:13px;margin:6px 0 14px">grandmasters: <strong>%d</strong> — %s <span style="color:#71767b">(one per segment is correct; the count is yours to judge)</span></p>`,
			len(gms), html.EscapeString(strings.Join(gms, ", ")))
	}

	return fmt.Sprintf(`
        <h2 style="font-size:13px;font-weight:600;text-transform:uppercase;letter-spacing:1.5px;color:#71767b;margin:36px 0 6px">Boxes</h2>
        %s
        <table>
            <thead>
                <tr><th>Host</th><th>IP</th><th>Identity</th><th>Time</th><th>Versions</th><th>Reported</th></tr>
            </thead>
            <tbody>%s</tbody>
        </table>`, gmLine, rows.String())
}
