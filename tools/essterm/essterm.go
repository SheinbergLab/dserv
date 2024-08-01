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
	"strings"
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
	fmt.Fprintf(debugLog, "Event: %d %d %s\n", e.Type, e.Subtype, e.Params)
}

func processDatapoint(dpointStr string, p *tea.Program) {
	var dpoint Dpoint

	fmt.Fprintf(debugLog, "%s\n", dpointStr)

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
	//	fmt.Fprintf(debugLog, "%s -> %s\n", cmd, result)
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

	var err error
	debugLog, err = tea.LogToFile("essterm.log", "debug")
	if err != nil {
		fmt.Println("fatal:", err)
		os.Exit(1)
	}
	defer debugLog.Close()

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

	dservRegister(host, "4620", SendJSON)
	dservAddMatch(host, "4620", "qpcs/*")
	dservAddMatch(host, "4620", "eventlog/events")
	dservAddMatch(host, "4620", "eventlog/names")

	if _, err := p.Run(); err != nil {
		log.Fatal(err)
	}
}

type (
	errMsg error
)

type model struct {
	textinput   textinput.Model
	viewport    viewport.Model
	table       table.Model
	err         error
	cmd_history *history
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

	// table
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

	var h history

	return model{
		textinput:   ti,
		viewport:    vp,
		table:       t,
		cmd_history: &h,
		err:         nil,
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
	)

	m.textinput, tiCmd = m.textinput.Update(msg)
	m.viewport, vpCmd = m.viewport.Update(msg)
	m.table, tbCmd = m.table.Update(msg)

	switch msg := msg.(type) {
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

		case tea.KeyCtrlT:
			m.table.Focus()
			m.textinput.Blur()

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
			m.table.Blur()
			m.textinput.Focus()

		case tea.KeyCtrlD, tea.KeyEsc:
			return m, tea.Quit
		}

	// We handle errors just like any other message
	case errMsg:
		m.err = msg
		return m, nil
	}

	return m, tea.Batch(tiCmd, vpCmd, tbCmd)
}

func (m model) View() string {
	return fmt.Sprintf(
		"ess%s\n%s\n%s",
		m.textinput.View(),
		m.viewport.View(),
		m.table.View(),
	) + "\n"
}
