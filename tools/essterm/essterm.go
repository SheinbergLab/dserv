package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"github.com/charmbracelet/bubbles/table"
	"github.com/charmbracelet/bubbles/textinput"
	"github.com/charmbracelet/bubbles/viewport"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
	"io"
	"log"
	"net"
	"os"
	"sort"
	"strings"
	"time"
)

const (
	SendText   = 0
	SendBinary = 1
	SendJSON   = 2
)

const (
	PtypeByte   = 0
	PtypeString = 1
	PtypeFloat  = 2
	PtypeShort  = 4
	PtypeInt    = 5
)

const (
	SystemRunning = 1
	SystemStopped = 0
)

const (
	MESH_DISCOVERY_PORT = 12346
	PEER_TIMEOUT_MS     = 30000
	CLEANUP_INTERVAL_MS = 10000
)

var SystemState = SystemStopped

var connect string
var recvHost string
var recvPort string
var rows []table.Row

// event type names (exactly 256)
var evtTypeNames [256]string

// event subtype names (specified by special events)
var evtSubtypeNames map[string]string

type ObsInfo struct {
	ObsStart uint64
	ObsCount int
	Events   [][]Evt
}

var obsInfo ObsInfo

// debug here using fmt.Fprintf()
var debugLog *os.File

// Mesh discovery types
type MeshPeer struct {
	ApplianceID string            `json:"applianceId"`
	Name        string            `json:"name"`
	Status      string            `json:"status"`
	IPAddress   string            `json:"ipAddress"`
	WebPort     int               `json:"webPort"`
	IsLocal     bool              `json:"isLocal"`
	LastSeen    int64             `json:"lastSeen"`
	CustomFields map[string]string `json:"customFields"`
}

var discoveredPeers = make(map[string]MeshPeer)

type history struct {
	count int
	index int
	cmds  []string
}

func (h *history) append(cmd string) {
	if cmd != "" {
		h.cmds = append(h.cmds, cmd)
		h.index = len(h.cmds) - 1
	}
}

func (h *history) previous() string {
	if len(h.cmds) == 0 {
		return ""
	}
	if h.index < 0 || h.index >= len(h.cmds)-1 {
		h.index = len(h.cmds) - 1
	}
	p := h.cmds[h.index]
	h.index--
	return p
}

func (h *history) next() string {
	if len(h.cmds) == 0 {
		return ""
	}
	if h.index >= len(h.cmds)-1 || h.index < 0 {
		h.index = 0
	}
	p := h.cmds[h.index]
	h.index++
	return p
}

type Dpoint struct {
	Name      string `json:"name"`
	Timestamp uint64 `json:"timestamp"`
	Dtype     uint32 `json:"dtype"`
	B64data   string `json:"data"`
}

type Evt struct {
	Type      uint8           `json:"e_type"`
	Subtype   uint8           `json:"e_subtype"`
	Timestamp uint64          `json:"timestamp"`
	Ptype     uint8           `json:"e_dtype"`
	Params    json.RawMessage `json:"e_params,omitempty"`
}

type evtMsg struct {
	evt Evt
}

type printMsg struct {
	message string
}

type peerDiscoveredMsg struct {
	peer MeshPeer
}

type peerCleanupMsg struct{}

type hostSelectMsg struct{}

// for storing output messages
var Messages []string

func initializeNames() {
	for i := 0; i < 16; i++ {
		evtTypeNames[i] = fmt.Sprintf("Reserved%d", i)
	}
	for i := 16; i < 128; i++ {
		evtTypeNames[i] = fmt.Sprintf("System%d", i)
	}
	for i := 128; i < 256; i++ {
		evtTypeNames[i] = fmt.Sprintf("User%d", i)
	}
	evtSubtypeNames = make(map[string]string)
}

func ProcessFileIO(e Evt, p *tea.Program) {

}

func ObsReset(o *ObsInfo) {
	o.ObsCount = -1
	o.Events = [][]Evt{}
	rows = []table.Row{}
}

func ObsStart(o *ObsInfo, e Evt) {
	o.ObsCount++
	o.ObsStart = e.Timestamp
	o.Events = append(o.Events, []Evt{e})
	rows = []table.Row{}
}

func ObsAddEvent(o *ObsInfo, e Evt) {
	if o.ObsCount == -1 {
		return
	}
	e.Timestamp -= o.ObsStart
	o.Events[o.ObsCount] =
		append(o.Events[o.ObsCount], e)
}

func ObsEnd(o *ObsInfo, e Evt) {

}

func processEvent(e Evt, p *tea.Program) {
	switch e.Type {
	case 3: // USER event
		switch e.Subtype {
		case 0:
			SystemState = SystemRunning
		case 1:
			SystemState = SystemStopped
		case 2:
			ObsReset(&obsInfo)
		}
	case 2: // FILEIO
		ProcessFileIO(e, p)
	case 1: // NAMESET
		evtTypeNames[e.Subtype] = string(e.Params)
		return
	case 6: // SUBTYPE NAMES
		stypes := strings.Split(string(e.Params), " ")
		for i := 0; i < len(stypes); i += 2 {
			s := fmt.Sprintf("%d:%s", e.Subtype, stypes[i+1])
			evtSubtypeNames[s] = stypes[i]
		}
		return
	case 18: // SYSTEM CHANGES

	case 19: // BEGINOBS
		ObsStart(&obsInfo, e)
	case 20: // ENDOBS
		ObsEnd(&obsInfo, e)
	}
	if obsInfo.ObsCount >= 0 {
		e.Timestamp -= obsInfo.ObsStart
	}
	ObsAddEvent(&obsInfo, e)
	p.Send(evtMsg{e})
}

func processDatapoint(dpointStr string, p *tea.Program) {
	var dpoint Dpoint

	err := json.Unmarshal([]byte(dpointStr), &dpoint)
	if err != nil {
		return
	}

	switch dpoint.Name {
	case "eventlog/events":
		var evt Evt
		err := json.Unmarshal([]byte(dpointStr), &evt)
		if err != nil {
			return
		}
		// peel off leading and trailing "" or []
		if len(evt.Params) > 1 {
			evt.Params = evt.Params[1 : len(evt.Params)-1]
		}

		processEvent(evt, p)
	case "print":
		if dpoint.Dtype == 1 {
			Messages = append(Messages, string(dpoint.B64data))
			p.Send(printMsg{})
		}
	default:
	}
}

func handleConnection(c net.Conn, p *tea.Program) {
	defer c.Close()
	reader := bufio.NewReader(c)

	for {
		// read incoming data
		bytes, err := reader.ReadBytes(byte('\n'))
		if err != nil {
			if err != io.EOF {
				fmt.Println("failed to read data, err:", err)
			}
			return
		}
		processDatapoint(string(bytes), p)
	}
}

func startTCPServer(l net.Listener, p *tea.Program) {
	defer l.Close()

	for {
		c, err := l.Accept()
		if err != nil {
			fmt.Println(err)
			return
		}
		go handleConnection(c, p)
	}
}

// Mesh discovery functions
func startMeshDiscovery(p *tea.Program) {
	addr, err := net.ResolveUDPAddr("udp", fmt.Sprintf(":%d", MESH_DISCOVERY_PORT))
	if err != nil {
		fmt.Printf("Failed to resolve mesh discovery address: %v\n", err)
		return
	}

	conn, err := net.ListenUDP("udp", addr)
	if err != nil {
		fmt.Printf("Failed to start mesh discovery: %v\n", err)
		return
	}

	fmt.Printf("Mesh discovery listening on port %d\n", MESH_DISCOVERY_PORT)

	// Start peer cleanup timer
	go func() {
		ticker := time.NewTicker(time.Duration(CLEANUP_INTERVAL_MS) * time.Millisecond)
		defer ticker.Stop()
		for {
			select {
			case <-ticker.C:
				p.Send(peerCleanupMsg{})
			}
		}
	}()

	// Listen for heartbeats
	go func() {
		defer conn.Close()
		buffer := make([]byte, 1024)

		for {
			n, clientAddr, err := conn.ReadFromUDP(buffer)
			if err != nil {
				continue
			}

			processMeshHeartbeat(buffer[:n], clientAddr.IP.String(), p)
		}
	}()
}

func processMeshHeartbeat(data []byte, senderIP string, p *tea.Program) {
	var heartbeat struct {
		Type        string `json:"type"`
		ApplianceID string `json:"applianceId"`
		Data        struct {
			Name   string `json:"name"`
			Status string `json:"status"`
			WebPort int   `json:"webPort"`
		} `json:"data"`
	}

	if err := json.Unmarshal(data, &heartbeat); err != nil {
		return
	}

	if heartbeat.Type != "heartbeat" {
		return
	}

	// Clean up IPv6-mapped IPv4 addresses
	cleanIP := senderIP
	if strings.HasPrefix(cleanIP, "::ffff:") {
		cleanIP = cleanIP[7:]
	}

	// Skip localhost variants since we handle that separately
	if cleanIP == "127.0.0.1" || cleanIP == "localhost" {
		return
	}

	peer := MeshPeer{
		ApplianceID: heartbeat.ApplianceID,
		Name:        heartbeat.Data.Name,
		Status:      heartbeat.Data.Status,
		IPAddress:   cleanIP,
		WebPort:     heartbeat.Data.WebPort,
		IsLocal:     false,
		LastSeen:    time.Now().UnixMilli(),
		CustomFields: make(map[string]string),
	}

	discoveredPeers[heartbeat.ApplianceID] = peer
	p.Send(peerDiscoveredMsg{peer})
}

func cleanupExpiredPeers() {
	now := time.Now().UnixMilli()
	for id, peer := range discoveredPeers {
		if now-peer.LastSeen > PEER_TIMEOUT_MS {
			delete(discoveredPeers, id)
		}
	}
}

func getAvailableHosts() []MeshPeer {
	var hosts []MeshPeer

	// Add localhost if available
	if isLocalhostAvailable() {
		hosts = append(hosts, MeshPeer{
			ApplianceID: "localhost",
			Name:        "localhost",
			Status:      "local",
			IPAddress:   "localhost",
			IsLocal:     true,
			LastSeen:    time.Now().UnixMilli(),
		})
	}

	// Add discovered mesh peers
	for _, peer := range discoveredPeers {
		hosts = append(hosts, peer)
	}

	// Sort by name for consistent ordering
	sort.Slice(hosts, func(i, j int) bool {
		return hosts[i].Name < hosts[j].Name
	})

	return hosts
}

func isLocalhostAvailable() bool {
	conn, err := net.DialTimeout("tcp", "localhost:4620", 500*time.Millisecond)
	if err != nil {
		return false
	}
	conn.Close()
	return true
}

func doCmd(connect string, cmd string) (error, string) {
	c, err := net.Dial("tcp", connect)
	if err != nil {
		return err, ""
	}

	fmt.Fprintf(c, "%s\n", cmd)

	message, _ := bufio.NewReader(c).ReadString('\n')

	c.Close()
	return nil, message
}

func getHostIP(dserv_host, dserv_port string) (error, string) {
	c, err := net.Dial("tcp", dserv_host+":"+dserv_port)
	if err != nil {
		return err, ""
	}
	addr := c.LocalAddr().String()
	s := strings.Split(addr, ":")
	c.Close()
	return nil, s[0]
}

func dservRegister(dserv_host string, dserv_port string, flags int) (error, string) {
	cmd := fmt.Sprintf("%%reg %s %s %d", recvHost, recvPort, flags)
	err, result := doCmd(dserv_host+":"+dserv_port, cmd)
	return err, result
}

func dservAddMatch(dserv_host, dserv_port, varname string) (error, string) {
	return doCmd(dserv_host+":"+dserv_port,
		"%match "+recvHost+" "+recvPort+" "+varname+" 1")
}

func main() {
	arguments := os.Args
	var host string
	var port int
	if len(arguments) > 1 {
		host = arguments[1]
	} else {
		host = "localhost"
	}

	port = 2570
	connect = fmt.Sprintf("%s:%d", host, port)

	initializeNames()
	ObsReset(&obsInfo)

	p := tea.NewProgram(initialModel(), tea.WithAltScreen())

	l, err := net.Listen("tcp4", ":0")
	if err != nil {
		fmt.Println(err)
		return
	}

	recvHost, recvPort, err = net.SplitHostPort(l.Addr().String())
	_, recvHost = getHostIP(host, "4620")
	go startTCPServer(l, p)

	// Start mesh discovery
	startMeshDiscovery(p)

	dservRegister(host, "4620", SendJSON)
	dservAddMatch(host, "4620", "qpcs/*")
	dservAddMatch(host, "4620", "eventlog/events")
	dservAddMatch(host, "4620", "print")

	if _, err := p.Run(); err != nil {
		log.Fatal(err)
	}
}

type (
	errMsg error
)

type model struct {
	textinput     textinput.Model
	viewport      viewport.Model
	table         table.Model
	output        viewport.Model
	err           error
	messages      *[]string
	cmd_history   *history
	showingHosts  bool
	hostTable     table.Model
	selectedHost  string
	currentHost   string
}

var baseStyle = lipgloss.NewStyle().
	BorderStyle(lipgloss.NormalBorder()).
	BorderForeground(lipgloss.Color("240"))

func initialModel() model {
	ti := textinput.New()
	ti.Placeholder = "cmd"
	ti.Focus()
	ti.CharLimit = 156
	ti.Width = 80

	vp := viewport.New(80, 2)
	vp.SetContent(``)

	// Event table
	columns := []table.Column{
		{Title: "Timestamp", Width: 7},
		{Title: "Type", Width: 11},
		{Title: "Subtype", Width: 11},
		{Title: "Vals", Width: 8},
	}

	t := table.New(
		table.WithColumns(columns),
		table.WithRows(rows),
		table.WithFocused(true),
		table.WithHeight(11),
	)
	t.Blur()

	// Host selection table
	hostColumns := []table.Column{
		{Title: "Name", Width: 20},
		{Title: "IP Address", Width: 15},
		{Title: "Status", Width: 12},
		{Title: "Type", Width: 8},
	}

	hostTable := table.New(
		table.WithColumns(hostColumns),
		table.WithRows([]table.Row{}),
		table.WithFocused(false),
		table.WithHeight(11),
	)

	s := table.DefaultStyles()
	s.Header = s.Header.
		BorderStyle(lipgloss.NormalBorder()).
		BorderForeground(lipgloss.Color("240")).
		BorderBottom(true).
		Bold(false)
	s.Selected = s.Selected.
		Foreground(lipgloss.Color("229")).
		Background(lipgloss.Color("57")).
		Bold(false)
	t.SetStyles(s)
	hostTable.SetStyles(s)

	var h history

	output := viewport.New(80, 8)
	output.SetContent(``)

	return model{
		textinput:    ti,
		viewport:     vp,
		table:        t,
		hostTable:    hostTable,
		cmd_history:  &h,
		output:       output,
		showingHosts: false,
		currentHost:  connect,
		err:          nil,
	}
}

func (m model) Init() tea.Cmd {
	tea.SetWindowTitle("ESSTerm")
	return textinput.Blink
}

func (m model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	var (
		tiCmd tea.Cmd
		vpCmd tea.Cmd
		tbCmd tea.Cmd
		htCmd tea.Cmd
		ouCmd tea.Cmd
	)

	m.textinput, tiCmd = m.textinput.Update(msg)
	m.viewport, vpCmd = m.viewport.Update(msg)

	if m.showingHosts {
		m.hostTable, htCmd = m.hostTable.Update(msg)
	} else {
		m.table, tbCmd = m.table.Update(msg)
	}

	m.output, ouCmd = m.output.Update(msg)

	switch msg := msg.(type) {
	case peerDiscoveredMsg:
		// Update host table if showing hosts
		if m.showingHosts {
			m = updateHostTable(m)
		}

	case peerCleanupMsg:
		cleanupExpiredPeers()
		if m.showingHosts {
			m = updateHostTable(m)
		}

	case hostSelectMsg:
		m.showingHosts = true
		m.hostTable.Focus()
		m.textinput.Blur()
		m = updateHostTable(m)

	case printMsg:
		m.output.SetContent(strings.Join(Messages, "\n"))
		m.output.GotoBottom()

	case evtMsg:
		if obsInfo.ObsCount < 0 {
			m.table.SetRows([]table.Row{})
			return m, tbCmd
		}
		e_type := evtTypeNames[msg.evt.Type]
		st := fmt.Sprintf("%d:%d", msg.evt.Type, msg.evt.Subtype)
		e_subtype := evtSubtypeNames[st]
		if e_subtype == "" {
			e_subtype = fmt.Sprintf("%d", msg.evt.Subtype)
		}
		e_time := fmt.Sprintf("%d", msg.evt.Timestamp/1000)

		rows = append(rows, table.Row{e_time, e_type, e_subtype, string(msg.evt.Params)})
		m.table.SetRows(rows)
		m.table.GotoBottom()

	case tea.KeyMsg:
		switch msg.Type {

		case tea.KeyEnter:
			if m.showingHosts {
				// Connect to selected host
				hosts := getAvailableHosts()
				if len(hosts) > 0 && m.hostTable.Cursor() < len(hosts) {
					selectedPeer := hosts[m.hostTable.Cursor()]
					m = connectToHost(m, selectedPeer)
				}
				// Return to normal mode
				m.showingHosts = false
				m.hostTable.Blur()
				m.textinput.Focus()
				return m, nil
			}

			if m.textinput.Value() == "exit" {
				return m, tea.Quit
			}

			err, result := doCmd(connect, m.textinput.Value()+"\n")
			if err != nil {
				m.err = err
				return m, nil
			}
			if m.textinput.Value() != "" {
				m.cmd_history.append(m.textinput.Value())
			}
			m.textinput.Reset()
			m.viewport.SetContent(result)

		case tea.KeyCtrlH:
			// Host selection mode
			return m, func() tea.Msg { return hostSelectMsg{} }

		case tea.KeyCtrlT:
			if !m.showingHosts {
				m.table.Focus()
				m.textinput.Blur()
			}

		case tea.KeyCtrlP, tea.KeyUp:
			if m.textinput.Focused() {
				if cmd := m.cmd_history.previous(); cmd != "" {
					m.textinput.SetValue(cmd)
					m.textinput.SetCursor(len(cmd) + 1)
				}
			}

		case tea.KeyCtrlN, tea.KeyDown:
			if m.textinput.Focused() {
				if cmd := m.cmd_history.next(); cmd != "" {
					m.textinput.SetValue(cmd)
					m.textinput.SetCursor(len(cmd) + 1)
				}
			}

		case tea.KeyCtrlE:
			if !m.showingHosts {
				m.table.Blur()
				m.textinput.Focus()
			}

		case tea.KeyEsc:
			if m.showingHosts {
				// Return to normal mode
				m.showingHosts = false
				m.hostTable.Blur()
				m.textinput.Focus()
				return m, nil
			}
			return m, tea.Quit
		}

	// We handle errors just like any other message
	case errMsg:
		m.err = msg
		return m, nil
	}

	return m, tea.Batch(tiCmd, vpCmd, tbCmd, htCmd, ouCmd)
}

func updateHostTable(m model) model {
	hosts := getAvailableHosts()
	hostRows := make([]table.Row, len(hosts))

	for i, host := range hosts {
		hostType := "Remote"
		if host.IsLocal {
			hostType = "Local"
		}

		// Highlight current host
		name := host.Name
		if host.IPAddress == strings.Split(m.currentHost, ":")[0] {
			name = fmt.Sprintf("* %s", name)
		}

		hostRows[i] = table.Row{
			name,
			host.IPAddress,
			host.Status,
			hostType,
		}
	}

	m.hostTable.SetRows(hostRows)
	return m
}

func connectToHost(m model, peer MeshPeer) model {
	// Determine connection string
	var newConnect string
	if peer.IsLocal {
		newConnect = "localhost:2570"
	} else {
		newConnect = fmt.Sprintf("%s:2570", peer.IPAddress)
	}

	// Test connection
	err, result := doCmd(newConnect, "status\n")
	if err != nil {
		Messages = append(Messages, fmt.Sprintf("Failed to connect to %s: %v", peer.Name, err))
	} else {
		// Update global connection string
		connect = newConnect
		m.currentHost = newConnect
		Messages = append(Messages, fmt.Sprintf("Connected to %s (%s)", peer.Name, peer.IPAddress))
		Messages = append(Messages, result)
	}

	return m
}

func (m model) View() string {
	var tableView string
	var headerText string

	if m.showingHosts {
		headerText = "Host Selection (Enter to connect, Esc to cancel):"
		tableView = m.hostTable.View()
	} else {
		currentHostDisplay := strings.Split(m.currentHost, ":")[0]
		headerText = fmt.Sprintf("Connected to: %s (Ctrl+H for host selection)", currentHostDisplay)
		tableView = m.table.View()
	}

	return fmt.Sprintf(
		"ess%s\n%s\n%s\n%s\n%s",
		m.textinput.View(),
		headerText,
		m.viewport.View(),
		tableView,
		m.output.View(),
	) + "\n"
}
