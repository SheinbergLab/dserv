package main

import (
	"bufio"
	"fmt"
	"log"
	"net"
	"os"
	"io"
	"strings"
	
	"github.com/charmbracelet/bubbles/table"
	"github.com/charmbracelet/bubbles/textinput"
	"github.com/charmbracelet/bubbles/viewport"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

var connect string
var recv_host string
var recv_port string
var rows []table.Row

type history struct {
	count int
	index int
	cmds []string
}

func (h *history) append(cmd string) {
	if cmd != "" {
		h.cmds = append(h.cmds, cmd)
		h.index = len(h.cmds)-1
	}
}

func (h *history) previous() string {
	if len(h.cmds) == 0 { return "" }
	if h.index == -1 { h.index = len(h.cmds)-1 }
	p := h.cmds[h.index]
	h.index--
	return p
}

func (h *history) next() string {
	if len(h.cmds) == 0 { return "" }
	if h.index == len(h.cmds) { h.index = 0 }
	p := h.cmds[h.index]
	h.index++
	return p
}

type dservMsg struct {
	varname   string
	timestamp uint64
}

func (d dservMsg) String() string {
	return fmt.Sprintf("%s", d.varname)
}

func processDatapoint(dpointStr string, p *tea.Program) {
	p.Send(dservMsg{varname: dpointStr, timestamp: 100000})
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

func dservRegister(dserv_host string, dserv_port string) (error, string) {
	return doCmd("localhost:4620", "%reg "+recv_host+" "+recv_port)
}

func dservAddMatch(dserv_host, dserv_port, varname string) (error, string) {
	return doCmd("localhost:4620",
		"%match "+recv_host+" "+recv_port+" "+varname+" 1")
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

	/*
	f, err := tea.LogToFile("essterm.log", "debug")
	if err != nil {
		fmt.Println("fatal:", err)
		os.Exit(1)
	}
	defer f.Close()
	*/
	
	port = 2570
	connect = fmt.Sprintf("%s:%d", host, port)

	p := tea.NewProgram(initialModel(), tea.WithAltScreen())

	l, err := net.Listen("tcp4", ":0")
	if err != nil {
		fmt.Println(err)
		return
	}
	
	recv_host, recv_port, err = net.SplitHostPort(l.Addr().String())
	
	go startTCPServer(l, p)

	dservRegister(recv_host, recv_port)
	dservAddMatch(recv_host, recv_port, "qpcs/*")
	dservAddMatch(recv_host, recv_port, "eventlog/events")
	dservAddMatch(recv_host, recv_port, "eventlog/names")
	
	if _, err := p.Run(); err != nil {
		log.Fatal(err)
	}
}

type (
	errMsg error
)

type model struct {
	textinput textinput.Model
	viewport  viewport.Model
	table     table.Model
	err       error
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
		{Title: "Name", Width: 16},
		{Title: "Timestamp", Width: 16},
		{Title: "Vals", Width: 24},
	}

	t := table.New(
		table.WithColumns(columns),
		table.WithRows(rows),
		table.WithFocused(true),
		table.WithHeight(7),
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
		textinput: ti,
		viewport:  vp,
		table:     t,
		cmd_history: &h,
		err:       nil,
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

	case dservMsg:
		words := strings.Fields(msg.varname)
		rows = append(rows, table.Row{words[0], words[2], words[4]})
		m.table.SetRows(rows)
		
	case tea.KeyMsg:
		switch msg.Type {
			
		case tea.KeyEnter:
			if (m.textinput.Value() == "exit") {
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
					m.textinput.SetCursor(len(cmd)+1)
				}
				
			}

		case tea.KeyCtrlN, tea.KeyDown:
			if m.textinput.Focused() {
				if cmd := m.cmd_history.next(); cmd != "" {
					m.textinput.SetValue(cmd)
					m.textinput.SetCursor(len(cmd)+1)
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

