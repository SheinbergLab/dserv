package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net"
	"net/url"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/charmbracelet/bubbles/textinput"
	"github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
	"github.com/gorilla/websocket"
)

// ============================================
// Configuration
// ============================================

var (
	// Connection mode
	connMode = flag.String("mode", "tcp", "Connection mode: tcp, ws, or sim")

	// TCP settings
	tcpCmdAddr    = flag.String("cmd", "", "TCP command interface address (host:port)")
	tcpCmdPort    = flag.Int("port", 2560, "TCP command port (2560=msg, 2570=ess, 4620=dserv)")
	tcpPubsubAddr = flag.String("pubsub", "", "TCP pub/sub address (empty = same as cmd)")
	tcpHost       = flag.String("host", "", "Host to connect to (skips discovery)")

	// Frame mode
	frameMode = flag.String("frame", "length", "Message framing: length (default) or newline")

	// WebSocket settings
	wsURL = flag.String("ws", "ws://localhost:9000/debug", "WebSocket URL")

	// Discovery
	discover     = flag.Bool("discover", true, "Enable UDP mesh discovery")
	discoverOnly = flag.Bool("list", false, "List discovered hosts and exit")
	discoverWait = flag.Int("wait", 2, "Seconds to wait for discovery before connecting")

	// Shorthand flags
	simulate = flag.Bool("sim", false, "Run in simulation mode (shorthand for -mode=sim)")
	debug    = flag.Bool("debug", false, "Enable protocol debug logging")
)

// ConnectionMode represents the backend connection type
type ConnectionMode int

const (
	ModeTCP ConnectionMode = iota
	ModeWebSocket
	ModeSimulate
)

// ============================================
// Styles (simplified)
// ============================================

var (
	// Colors
	colorMuted  = lipgloss.Color("#71717a")
	colorBlue   = lipgloss.Color("#60a5fa")
	colorGreen  = lipgloss.Color("#4ade80")
	colorRed    = lipgloss.Color("#f87171")
	colorYellow = lipgloss.Color("#fbbf24")
	colorCyan   = lipgloss.Color("#22d3ee")
	colorPurple = lipgloss.Color("#a78bfa")

	interpColors = []lipgloss.Color{
		colorBlue, colorGreen, colorYellow, colorPurple, colorCyan,
		lipgloss.Color("#f472b6"), lipgloss.Color("#fb923c"),
	}

	// Styles
	stylePrompt = lipgloss.NewStyle().Bold(true)
	styleError  = lipgloss.NewStyle().Foreground(colorRed)
	styleSystem = lipgloss.NewStyle().Foreground(colorMuted).Italic(true)
	styleEvent  = lipgloss.NewStyle().Foreground(colorCyan).Italic(true)
)

// ============================================
// Message Types (WebSocket Protocol)
// ============================================

type WSMessage struct {
	Type      string          `json:"type,omitempty"`
	Channel   string          `json:"channel,omitempty"`
	Interp    string          `json:"interp,omitempty"`
	Command   string          `json:"command,omitempty"`
	Timestamp int64           `json:"timestamp,omitempty"`
	Data      json.RawMessage `json:"data,omitempty"`
}

type WSErrorData struct {
	Message   string `json:"message"`
	ErrorInfo string `json:"errorInfo,omitempty"`
	ErrorCode string `json:"errorCode,omitempty"`
}

type WSInterpList struct {
	Interpreters []string `json:"interpreters"`
}

// ============================================
// Internal Messages (Bubble Tea)
// ============================================

type msgConnected struct{}
type msgDisconnected struct{ err error }
type msgInterpList struct{ interpreters []string }
type msgOutput struct{ interp, text string }
type msgEvent struct{ source, text string }
type msgError struct {
	interp    string
	message   string
	errorInfo string
	timestamp time.Time
}
type msgSetProgram struct{ program *tea.Program }
type msgAttemptReconnect struct{}
type msgReconnectSuccess struct{ client *TCPClient }
type msgReconnectFailed struct{ 
	client *TCPClient
	manual bool
	err    error
}

// ============================================
// Interpreter State
// ============================================

type Interpreter struct {
	Name  string
	Color lipgloss.Color
}

// ============================================
// WebSocket Client
// ============================================

type WSClient struct {
	url       string
	conn      *websocket.Conn
	mu        sync.Mutex
	connected bool
	sendCh    chan WSMessage
	program   *tea.Program
}

func NewWSClient(url string) *WSClient {
	return &WSClient{
		url:    url,
		sendCh: make(chan WSMessage, 100),
	}
}

func (c *WSClient) SetProgram(p *tea.Program) {
	c.program = p
}

func (c *WSClient) Connect() error {
	u, err := url.Parse(c.url)
	if err != nil {
		return err
	}

	conn, _, err := websocket.DefaultDialer.Dial(u.String(), nil)
	if err != nil {
		return err
	}

	c.mu.Lock()
	c.conn = conn
	c.connected = true
	c.mu.Unlock()

	go c.readPump()
	go c.writePump()

	if c.program != nil {
		c.program.Send(msgConnected{})
	}

	c.Send(WSMessage{Type: "list_interps"})
	return nil
}

func (c *WSClient) Send(msg WSMessage) {
	select {
	case c.sendCh <- msg:
	default:
	}
}

func (c *WSClient) Close() {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.conn != nil {
		c.conn.Close()
	}
	c.connected = false
}

func (c *WSClient) readPump() {
	defer func() {
		c.mu.Lock()
		c.connected = false
		if c.conn != nil {
			c.conn.Close()
		}
		c.mu.Unlock()
		if c.program != nil {
			c.program.Send(msgDisconnected{})
		}
	}()

	for {
		_, message, err := c.conn.ReadMessage()
		if err != nil {
			return
		}

		var msg WSMessage
		if err := json.Unmarshal(message, &msg); err != nil {
			continue
		}

		c.handleMessage(msg)
	}
}

func (c *WSClient) writePump() {
	for msg := range c.sendCh {
		c.mu.Lock()
		if !c.connected || c.conn == nil {
			c.mu.Unlock()
			continue
		}

		data, err := json.Marshal(msg)
		if err != nil {
			c.mu.Unlock()
			continue
		}

		err = c.conn.WriteMessage(websocket.TextMessage, data)
		c.mu.Unlock()

		if err != nil {
			return
		}
	}
}

func (c *WSClient) handleMessage(msg WSMessage) {
	if c.program == nil {
		return
	}

	switch msg.Type {
	case "interp_list":
		var list WSInterpList
		if err := json.Unmarshal(msg.Data, &list); err == nil {
			c.program.Send(msgInterpList{interpreters: list.Interpreters})
		} else {
			var interps []string
			if err := json.Unmarshal(msg.Data, &interps); err == nil {
				c.program.Send(msgInterpList{interpreters: interps})
			}
		}
	}

	switch msg.Channel {
	case "output":
		var text string
		json.Unmarshal(msg.Data, &text)
		c.program.Send(msgOutput{interp: msg.Interp, text: text})

	case "error":
		var errData WSErrorData
		if err := json.Unmarshal(msg.Data, &errData); err == nil {
			c.program.Send(msgError{
				interp:    msg.Interp,
				message:   errData.Message,
				errorInfo: errData.ErrorInfo,
				timestamp: time.Now(),
			})
		}

	case "event":
		var text string
		json.Unmarshal(msg.Data, &text)
		c.program.Send(msgEvent{source: msg.Interp, text: text})
	}
}

// ============================================
// Model
// ============================================

type model struct {
	input        textinput.Model
	interpreters []Interpreter
	activeIdx    int
	history      []string
	historyIdx   int
	connected    bool
	connMode     ConnectionMode
	showHelp     bool
	program      *tea.Program // Reference to the program for reconnection

	// Backend clients
	wsClient  *WSClient
	tcpClient *TCPClient
	discovery *MeshDiscovery

	// Connection info
	currentHost       string
	availableHosts    []MeshPeer
	showStackTraces   bool
	pendingCommand    string // Command to retry after reconnection
	pendingInterp     string // Interpreter for pending command
	silentReconnect   bool   // True if reconnecting without user action
}

func initialModel(mode ConnectionMode, wsURL, tcpCmd, tcpPubsub string, discovery *MeshDiscovery) model {
	ti := textinput.New()
	ti.Placeholder = "Type command or :help"
	ti.Prompt = "" // Disable textinput's own prompt - we handle it ourselves
	ti.Focus()
	ti.CharLimit = 256
	ti.Width = 80

	m := model{
		input:      ti,
		history:    make([]string, 0, 500),
		historyIdx: -1,
		connMode:   mode,
		discovery:  discovery,
		interpreters: []Interpreter{{
			Name:  "dserv",
			Color: colorBlue,
		}},
		activeIdx: 0,
	}

	switch mode {
	case ModeWebSocket:
		m.wsClient = NewWSClient(wsURL)
	case ModeTCP:
		m.tcpClient = NewTCPClient(tcpCmd, tcpPubsub)
	}

	return m
}

func (m model) Init() tea.Cmd {
	cmds := []tea.Cmd{textinput.Blink}

	switch m.connMode {
	case ModeSimulate:
		cmds = append(cmds, func() tea.Msg {
			return msgConnected{}
		}, func() tea.Msg {
			return msgInterpList{interpreters: []string{"dserv", "juicer", "ess", "eye_control", "sound", "display"}}
		})

	case ModeWebSocket:
		if m.wsClient != nil {
			cmds = append(cmds, func() tea.Msg {
				if err := m.wsClient.Connect(); err != nil {
					return msgDisconnected{err: err}
				}
				return nil
			})
		}

	case ModeTCP:
		if m.tcpClient != nil {
			cmds = append(cmds, func() tea.Msg {
				if err := m.tcpClient.Connect(); err != nil {
					return msgDisconnected{err: err}
				}
				return nil
			})
		}
	}

	return tea.Batch(cmds...)
}

func (m model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.KeyMsg:
		if m.showHelp {
			if msg.String() == "esc" || msg.String() == "q" {
				m.showHelp = false
				return m, nil
			}
			return m, nil
		}

		switch msg.String() {
		case "ctrl+c":
			return m, tea.Quit

		case "ctrl+l":
			return m, tea.ClearScreen

		case "tab":
			m.tabComplete()
			return m, nil

		case "up":
			if m.historyIdx < len(m.history)-1 {
				m.historyIdx++
				m.input.SetValue(m.history[m.historyIdx])
				m.input.CursorEnd()
			}

		case "down":
			if m.historyIdx > 0 {
				m.historyIdx--
				m.input.SetValue(m.history[m.historyIdx])
				m.input.CursorEnd()
			} else if m.historyIdx == 0 {
				m.historyIdx = -1
				m.input.SetValue("")
			}

		case "enter":
			cmd := m.input.Value()
			if cmd != "" {
				// Clear the current line first (Bubble Tea has already rendered the prompt)
				fmt.Print("\r\033[K")
				
				// Now print the command line we're executing
				var status string
				if m.connected {
					status = lipgloss.NewStyle().Foreground(colorGreen).Render("●")
				} else {
					status = lipgloss.NewStyle().Foreground(colorRed).Render("●")
				}
				
				prompt := "[none]"
				promptColor := colorMuted
				if len(m.interpreters) > 0 && m.activeIdx < len(m.interpreters) {
					interp := m.interpreters[m.activeIdx]
					prompt = interp.Name
					promptColor = interp.Color
				}
				
				promptStyled := lipgloss.NewStyle().Foreground(promptColor).Bold(true).Render(prompt)
				fmt.Printf("%s [%s] > %s\n", status, promptStyled, cmd)
				
				// Clear input before processing
				m.input.SetValue("")
				
				// Process the command
				m.processCommand(cmd)
				m.history = append([]string{cmd}, m.history...)
				if len(m.history) > 500 {
					m.history = m.history[:500]
				}
				m.historyIdx = -1
			}
			return m, nil
		}

		var cmd tea.Cmd
		m.input, cmd = m.input.Update(msg)
		return m, cmd

	case msgConnected:
		m.connected = true
		fmt.Println(styleSystem.Render("✓ Connected to backend"))
		m.reprintPrompt()

	case msgDisconnected:
		wasConnected := m.connected
		m.connected = false
		
		// Show error only if not doing silent reconnect
		if !m.silentReconnect {
			if msg.err != nil {
				fmt.Println(styleError.Render(fmt.Sprintf("✗ Disconnected: %v", msg.err)))
			} else {
				fmt.Println(styleSystem.Render("✗ Disconnected"))
			}
			m.reprintPrompt()
		}
		
		// Attempt automatic reconnection after a delay (asynchronously)
		// Only if we were previously connected (not if user tried command while already disconnected)
		if m.connMode == ModeTCP && wasConnected && !m.silentReconnect {
			m.silentReconnect = true // Set flag for this reconnection
			currentHost := m.currentHost
			program := m.program
			return m, func() tea.Msg {
				time.Sleep(2 * time.Second)
				
				// Create new client
				newClient := NewTCPClient(currentHost, currentHost)
				
				// Set frame mode
				cmdFrameMode := FrameLength
				if strings.ToLower(*frameMode) == "newline" {
					cmdFrameMode = FrameNewline
				}
				newClient.SetFrameMode(cmdFrameMode, FrameNewline)
				
				// Set program reference
				if program != nil {
					newClient.SetProgram(program)
				}
				
				// Try to connect
				if err := newClient.Connect(); err != nil {
					return msgReconnectFailed{client: newClient, manual: false, err: err}
				}
				
				return msgReconnectSuccess{client: newClient}
			}
		}

	case msgReconnectSuccess:
		// Close old client
		if m.tcpClient != nil {
			m.tcpClient.Close()
		}
		m.tcpClient = msg.client
		m.connected = true
		
		// If this was a silent reconnect with a pending command, retry it
		if m.silentReconnect && m.pendingCommand != "" {
			// Retry the command
			go m.tcpClient.SendToInterp(m.pendingInterp, m.pendingCommand)
			// Clear pending command
			m.pendingCommand = ""
			m.pendingInterp = ""
		} else {
			// Show success message for non-silent reconnects
			fmt.Println(styleSystem.Render("✓ Reconnected successfully"))
			m.reprintPrompt()
		}
		
		// Reset silent flag
		m.silentReconnect = false

	case msgReconnectFailed:
		// Clean up failed client
		if msg.client != nil {
			msg.client.Close()
		}
		
		// Show error for manual reconnect or silent reconnect (command failed)
		if msg.manual {
			fmt.Println(styleError.Render(fmt.Sprintf("Reconnection failed: %v", msg.err)))
			m.reprintPrompt()
		} else if m.silentReconnect {
			// Silent reconnect failed - show error since user's command failed
			fmt.Println(styleError.Render(fmt.Sprintf("Connection lost (reconnection failed: %v)", msg.err)))
			m.reprintPrompt()
		}
		
		// Clear pending command
		m.pendingCommand = ""
		m.pendingInterp = ""
		m.silentReconnect = false

	case msgSetProgram:
		m.program = msg.program
		// Set program reference on clients
		if m.tcpClient != nil {
			m.tcpClient.SetProgram(m.program)
		}
		if m.wsClient != nil {
			m.wsClient.SetProgram(m.program)
		}

	case msgAttemptReconnect:
		// This is now unused but keep for compatibility
		// The reconnection happens via msgDisconnected now

	case msgInterpList:
		m.updateInterpreters(msg.interpreters)
		fmt.Println(styleSystem.Render(fmt.Sprintf("Available interpreters: %s", strings.Join(msg.interpreters, ", "))))
		// Refresh completion cache for active interpreter
		if m.tcpClient != nil && m.connected && m.connMode == ModeTCP {
			if len(m.interpreters) > 0 && m.activeIdx < len(m.interpreters) {
				activeInterp := m.interpreters[m.activeIdx].Name
				go m.tcpClient.RefreshCompletionCache(activeInterp)
			}
		}
		m.reprintPrompt()

	case msgOutput:
		m.handleOutput(msg.interp, msg.text, false)
		m.reprintPrompt()

	case msgError:
		m.handleOutput(msg.interp, msg.message, true)
		if m.showStackTraces && msg.errorInfo != "" {
			fmt.Println(styleError.Render("  Stack trace:"))
			for _, line := range strings.Split(msg.errorInfo, "\n") {
				fmt.Println(styleError.Render("    " + line))
			}
		}
		m.reprintPrompt()

	case msgEvent:
		tag := styleEvent.Render(fmt.Sprintf("[event:%s]", msg.source))
		fmt.Println(fmt.Sprintf("%s %s", tag, msg.text))
		m.reprintPrompt()
	}

	return m, nil
}

func (m model) View() string {
	if m.showHelp {
		return m.renderHelp()
	}

	// Just render the prompt - output goes directly to stdout
	return m.renderPrompt()
}

func (m model) renderPrompt() string {
	// Connection status indicator
	var status string
	if m.connected {
		status = lipgloss.NewStyle().Foreground(colorGreen).Render("●")
	} else {
		status = lipgloss.NewStyle().Foreground(colorRed).Render("●")
	}

	// Active interpreter
	prompt := "[none]"
	promptColor := colorMuted
	if len(m.interpreters) > 0 && m.activeIdx < len(m.interpreters) {
		interp := m.interpreters[m.activeIdx]
		prompt = interp.Name
		promptColor = interp.Color
	}

	promptStyled := lipgloss.NewStyle().Foreground(promptColor).Bold(true).Render(prompt)
	return fmt.Sprintf("%s [%s] > %s", status, promptStyled, m.input.View())
}

func (m *model) reprintPrompt() {
	// Erase current line and print fresh prompt (without input shown)
	var status string
	if m.connected {
		status = lipgloss.NewStyle().Foreground(colorGreen).Render("●")
	} else {
		status = lipgloss.NewStyle().Foreground(colorRed).Render("●")
	}

	prompt := "[none]"
	promptColor := colorMuted
	if len(m.interpreters) > 0 && m.activeIdx < len(m.interpreters) {
		interp := m.interpreters[m.activeIdx]
		prompt = interp.Name
		promptColor = interp.Color
	}

	promptStyled := lipgloss.NewStyle().Foreground(promptColor).Bold(true).Render(prompt)
	fmt.Printf("%s [%s] > ", status, promptStyled)
}

func (m *model) processCommand(input string) {
	trimmed := strings.TrimSpace(input)

	// Meta commands (:...) - but NOT Tcl namespace commands (::...)
	if strings.HasPrefix(trimmed, ":") && !strings.HasPrefix(trimmed, "::") {
		m.handleMetaCommand(strings.TrimPrefix(trimmed, ":"))
		m.reprintPrompt()
		return
	}

	// Directed command (/interp cmd)
	if strings.HasPrefix(trimmed, "/") {
		parts := strings.SplitN(trimmed[1:], " ", 2)
		if len(parts) >= 1 {
			targetInterp := parts[0]
			cmd := ""
			if len(parts) > 1 {
				cmd = parts[1]
			}

			found := false
			for _, interp := range m.interpreters {
				if interp.Name == targetInterp {
					found = true
					break
				}
			}

			if found {
				m.sendCommand(targetInterp, cmd)
			} else {
				fmt.Println(styleError.Render(fmt.Sprintf("Unknown interpreter: %s", targetInterp)))
				m.reprintPrompt()
			}
		}
		return
	}

	// Default: send to active interpreter
	if len(m.interpreters) > 0 && m.activeIdx < len(m.interpreters) {
		m.sendCommand(m.interpreters[m.activeIdx].Name, trimmed)
	} else {
		fmt.Println(styleSystem.Render("No active interpreter"))
		m.reprintPrompt()
	}
}

func (m *model) handleMetaCommand(cmd string) {
	parts := strings.Fields(cmd)
	if len(parts) == 0 {
		return
	}

	action := parts[0]
	switch action {
	case "help", "h":
		m.showHelp = true

	case "list", "ls":
		if len(m.interpreters) == 0 {
			fmt.Println(styleSystem.Render("No interpreters available"))
		} else {
			fmt.Println(styleSystem.Render("Available interpreters:"))
			for i, interp := range m.interpreters {
				marker := " "
				if i == m.activeIdx {
					marker = "►"
				}
				fmt.Println(fmt.Sprintf("  %s %s",
					marker,
					lipgloss.NewStyle().Foreground(interp.Color).Render(interp.Name)))
			}
		}

	case "use":
		if len(parts) > 1 {
			name := parts[1]
			for i, interp := range m.interpreters {
				if interp.Name == name {
					m.activeIdx = i
					fmt.Println(styleSystem.Render(fmt.Sprintf("Switched to %s", name)))
					// Refresh completion cache for the new active interpreter
					if m.tcpClient != nil && m.connected && m.connMode == ModeTCP {
						go m.tcpClient.RefreshCompletionCache(name)
					}
					return
				}
			}
			fmt.Println(styleError.Render(fmt.Sprintf("Unknown interpreter: %s", name)))
		} else {
			fmt.Println(styleSystem.Render("Usage: :use <interpreter>"))
		}

	case "trace", "stack", "stacktrace":
		m.showStackTraces = !m.showStackTraces
		if m.showStackTraces {
			fmt.Println(styleSystem.Render("Stack traces enabled"))
		} else {
			fmt.Println(styleSystem.Render("Stack traces disabled"))
		}

	case "refresh", "reload":
		if m.tcpClient != nil && m.connected && m.connMode == ModeTCP {
			if len(m.interpreters) > 0 && m.activeIdx < len(m.interpreters) {
				interp := m.interpreters[m.activeIdx].Name
				fmt.Println(styleSystem.Render(fmt.Sprintf("Refreshing completions for %s...", interp)))
				go m.tcpClient.RefreshCompletionCache(interp)
			}
		} else {
			fmt.Println(styleSystem.Render("Completion cache only available in TCP mode"))
		}

	case "clear", "cls":
		// Just clear screen and reprint prompt
		fmt.Print("\033[2J\033[H")
		// Don't call reprintPrompt here - it will be called at end of handleMetaCommand

	case "scan", "discover":
		if m.discovery == nil {
			fmt.Println(styleSystem.Render("Discovery not enabled (use -discover flag)"))
		} else {
			fmt.Println(styleSystem.Render("Scanning for hosts (3 seconds)..."))
			// Wait for discovery to pick up any new hosts
			time.Sleep(3 * time.Second)
			m.updateAvailableHosts()
			if len(m.availableHosts) == 0 {
				fmt.Println(styleSystem.Render("No hosts found"))
			} else {
				fmt.Println(styleSystem.Render("Available hosts:"))
				for i, host := range m.availableHosts {
					hostType := "Remote"
					if host.IsLocal {
						hostType = "Local"
					}
					marker := " "
					if fmt.Sprintf("%s:%d", host.IPAddress, *tcpCmdPort) == m.currentHost {
						marker = "*"
					}
					fmt.Println(styleSystem.Render(fmt.Sprintf("  %s%d. %s (%s) [%s]",
						marker, i+1, host.Name, host.IPAddress, hostType)))
				}
				fmt.Println(styleSystem.Render("Use :connect <number> to switch hosts"))
			}
		}

	case "hosts", "servers":
		if m.discovery == nil {
			fmt.Println(styleSystem.Render("Discovery not enabled"))
		} else {
			m.updateAvailableHosts()
			if len(m.availableHosts) == 0 {
				fmt.Println(styleSystem.Render("No hosts discovered (use :scan to actively scan)"))
			} else {
				fmt.Println(styleSystem.Render("Available hosts:"))
				for i, host := range m.availableHosts {
					hostType := "Remote"
					if host.IsLocal {
						hostType = "Local"
					}
					marker := " "
					if fmt.Sprintf("%s:%d", host.IPAddress, *tcpCmdPort) == m.currentHost {
						marker = "*"
					}
					fmt.Println(styleSystem.Render(fmt.Sprintf("  %s%d. %s (%s) [%s]",
						marker, i+1, host.Name, host.IPAddress, hostType)))
				}
				fmt.Println(styleSystem.Render("Use :connect <number> to switch hosts"))
			}
		}

	case "connect":
		if len(parts) > 1 {
			var idx int
			if _, err := fmt.Sscanf(parts[1], "%d", &idx); err == nil && idx >= 1 {
				m.updateAvailableHosts()
				if idx <= len(m.availableHosts) {
					host := m.availableHosts[idx-1]
					newAddr := fmt.Sprintf("%s:%d", host.IPAddress, *tcpCmdPort)
					fmt.Println(styleSystem.Render(fmt.Sprintf("Connecting to %s...", host.Name)))

					if m.tcpClient != nil {
						m.tcpClient.Close()
					}

					m.tcpClient = NewTCPClient(newAddr, newAddr)
					m.currentHost = newAddr
					
					// Set frame mode
					cmdFrameMode := FrameLength
					if strings.ToLower(*frameMode) == "newline" {
						cmdFrameMode = FrameNewline
					}
					m.tcpClient.SetFrameMode(cmdFrameMode, FrameNewline)
					
					// Set program reference if we have it
					if m.program != nil {
						m.tcpClient.SetProgram(m.program)
					}

					if err := m.tcpClient.Connect(); err != nil {
						fmt.Println(styleError.Render(fmt.Sprintf("Connection failed: %v", err)))
						m.connected = false
					} else {
						m.connected = true
						fmt.Println(styleSystem.Render(fmt.Sprintf("Connected to %s", host.Name)))
						// Request interpreter list
					}
				} else {
					fmt.Println(styleSystem.Render("Invalid host number"))
				}
			} else {
				fmt.Println(styleSystem.Render("Usage: :connect <number>"))
			}
		} else {
			fmt.Println(styleSystem.Render("Usage: :connect <number> (see :hosts)"))
		}

	case "reconnect":
		if m.connMode == ModeTCP && m.tcpClient != nil {
			fmt.Println(styleSystem.Render("Reconnecting..."))
			// Trigger async reconnect
			currentHost := m.currentHost
			program := m.program
			
			// Clear any pending command - this is a manual reconnect
			m.pendingCommand = ""
			m.pendingInterp = ""
			m.silentReconnect = false
			
			// Close old client
			if m.tcpClient != nil {
				m.tcpClient.Close()
			}
			m.connected = false
			
			// Do connection asynchronously
			go func() {
				time.Sleep(100 * time.Millisecond)
				
				// Create new client
				newClient := NewTCPClient(currentHost, currentHost)
				
				// Set frame mode
				cmdFrameMode := FrameLength
				if strings.ToLower(*frameMode) == "newline" {
					cmdFrameMode = FrameNewline
				}
				newClient.SetFrameMode(cmdFrameMode, FrameNewline)
				
				// Set program reference
				if program != nil {
					newClient.SetProgram(program)
				}
				
				// Try to connect
				if err := newClient.Connect(); err != nil {
					if program != nil {
						program.Send(msgReconnectFailed{client: newClient, manual: true, err: err})
					}
					return
				}
				
				// Request interpreter list
				
				if program != nil {
					program.Send(msgReconnectSuccess{client: newClient})
				}
			}()
		} else {
			fmt.Println(styleSystem.Render("Reconnect only available in TCP mode"))
		}

	case "quit", "q", "exit":
		if m.wsClient != nil {
			m.wsClient.Close()
		}
		if m.tcpClient != nil {
			m.tcpClient.Close()
		}
		os.Exit(0)

	default:
		fmt.Println(styleSystem.Render(fmt.Sprintf("Unknown command: :%s (type :help for commands)", action)))
	}
}

func (m *model) sendCommand(interp, cmd string) {
	// Command is already visible from the prompt, no need to echo it
	
	switch m.connMode {
	case ModeSimulate:
		m.simulateResponse(interp, cmd)

	case ModeWebSocket:
		if m.wsClient != nil && m.connected {
			m.wsClient.Send(WSMessage{
				Type:    "eval",
				Interp:  interp,
				Command: cmd,
			})
		} else {
			fmt.Println(styleSystem.Render("Not connected to backend"))
		}

	case ModeTCP:
		if m.tcpClient != nil && m.connected {
			go m.tcpClient.SendToInterp(interp, cmd)
		} else {
			// Not connected - trigger silent reconnection and retry command
			m.pendingCommand = cmd
			m.pendingInterp = interp
			m.silentReconnect = true
			
			// Trigger async reconnect if we have the necessary info
			if m.program != nil && m.currentHost != "" {
				currentHost := m.currentHost
				program := m.program
				
				go func() {
					// Create new client
					newClient := NewTCPClient(currentHost, currentHost)
					
					// Set frame mode
					cmdFrameMode := FrameLength
					if strings.ToLower(*frameMode) == "newline" {
						cmdFrameMode = FrameNewline
					}
					newClient.SetFrameMode(cmdFrameMode, FrameNewline)
					
					// Set program reference
					newClient.SetProgram(program)
					
					// Try to connect
					if err := newClient.Connect(); err != nil {
						program.Send(msgReconnectFailed{client: newClient, manual: false, err: err})
						return
					}
					
					program.Send(msgReconnectSuccess{client: newClient})
				}()
			} else {
				// Can't reconnect - show error
				fmt.Println(styleSystem.Render("Not connected to backend"))
				m.reprintPrompt()
			}
		}
	}
}

func (m *model) handleOutput(interp, text string, isError bool) {
	color := colorMuted
	for _, i := range m.interpreters {
		if i.Name == interp {
			color = i.Color
			break
		}
	}

	// Determine if we need to show the interpreter tag
	// (only show it if different from active interpreter)
	showTag := false
	activeInterp := ""
	if len(m.interpreters) > 0 && m.activeIdx < len(m.interpreters) {
		activeInterp = m.interpreters[m.activeIdx].Name
	}
	if interp != activeInterp {
		showTag = true
	}

	if isError {
		if showTag {
			fmt.Println(styleError.Render(fmt.Sprintf("✗ [%s] %s", interp, text)))
		} else {
			fmt.Println(styleError.Render(fmt.Sprintf("✗ %s", text)))
		}
	} else {
		if showTag {
			tag := lipgloss.NewStyle().Foreground(color).Render(fmt.Sprintf("[%s]", interp))
			fmt.Printf("%s → %s\n", tag, text)
		} else {
			fmt.Printf("→ %s\n", text)
		}
	}
}

func (m *model) updateInterpreters(names []string) {
	m.interpreters = make([]Interpreter, len(names))
	for i, name := range names {
		m.interpreters[i] = Interpreter{
			Name:  name,
			Color: interpColors[i%len(interpColors)],
		}
	}
	if m.activeIdx >= len(m.interpreters) {
		m.activeIdx = 0
	}
}

func (m *model) updateAvailableHosts() {
	if m.discovery != nil {
		m.availableHosts = m.discovery.GetAvailableHosts(*tcpCmdPort)
	}
}

func (m *model) tabComplete() {
	input := m.input.Value()
	cursorPos := m.input.Position()

	beforeCursor := input[:cursorPos]
	afterCursor := input[cursorPos:]

	wordStart := strings.LastIndexAny(beforeCursor, " \t{}")
	if wordStart == -1 {
		wordStart = 0
	} else {
		wordStart++
	}
	partial := beforeCursor[wordStart:]

	var candidates []string

	// Meta commands (starting with : but NOT ::)
	if strings.HasPrefix(input, ":") && !strings.HasPrefix(input, "::") && !strings.Contains(beforeCursor, " ") {
		metaCommands := []string{":help", ":list", ":use", ":trace", ":stack",
			":clear", ":cls", ":hosts", ":servers", ":connect", ":quit", ":exit"}
		candidates = filterMatches(metaCommands, partial)

	// Interpreter routing (/interp command)
	} else if strings.HasPrefix(input, "/") && !strings.HasPrefix(input, "/:") {
		parts := strings.SplitN(input[1:], " ", 2)
		if len(parts) == 1 || cursorPos <= len(parts[0])+1 {
			// Completing interpreter name
			partial := parts[0]
			for _, interp := range m.interpreters {
				if strings.HasPrefix(interp.Name, partial) {
					candidates = append(candidates, interp.Name)
				}
			}
		} else if len(parts) == 2 {
			// Completing command after /interp
			targetInterp := parts[0]
			if m.tcpClient != nil && m.connMode == ModeTCP {
				cache := m.tcpClient.GetCompletionCache(targetInterp)
				if cache != nil {
					all := cache.GetAll()
					candidates = filterMatches(all, partial)
				}
			}
		}

	// Regular Tcl command completion
	} else {
		// Get active interpreter
		activeInterp := ""
		if len(m.interpreters) > 0 && m.activeIdx < len(m.interpreters) {
			activeInterp = m.interpreters[m.activeIdx].Name
		}

		// Query completion cache
		if m.tcpClient != nil && m.connMode == ModeTCP && activeInterp != "" {
			cache := m.tcpClient.GetCompletionCache(activeInterp)
			if cache != nil {
				all := cache.GetAll()
				candidates = filterMatches(all, partial)
			}
		}
	}

	// Apply completion if we have exactly one match
	if len(candidates) == 1 {
		completion := candidates[0]
		newValue := beforeCursor[:wordStart] + completion + afterCursor
		m.input.SetValue(newValue)
		m.input.SetCursor(wordStart + len(completion))
	} else if len(candidates) > 1 {
		// Show candidates (max 50 to avoid flooding)
		fmt.Println()
		fmt.Println(styleSystem.Render("Completions:"))
		maxShow := 50
		if len(candidates) > maxShow {
			for i := 0; i < maxShow; i++ {
				fmt.Println("  " + candidates[i])
			}
			fmt.Println(styleSystem.Render(fmt.Sprintf("  ... and %d more", len(candidates)-maxShow)))
		} else {
			for _, c := range candidates {
				fmt.Println("  " + c)
			}
		}
		m.reprintPrompt()
	}
}

func filterMatches(commands []string, partial string) []string {
	var matches []string
	for _, cmd := range commands {
		if strings.HasPrefix(cmd, partial) {
			matches = append(matches, cmd)
		}
	}
	return matches
}

func (m *model) simulateResponse(interp, cmd string) {
	responses := map[string]string{
		"info":    "Tcl version 8.6.12",
		"pwd":     "/home/user/project",
		"clock":   fmt.Sprintf("%d", time.Now().Unix()),
		"version": "dserv 2.0",
	}

	if resp, ok := responses[cmd]; ok {
		time.AfterFunc(100*time.Millisecond, func() {
			fmt.Println(styleSystem.Render(fmt.Sprintf("[%s] → %s", interp, resp)))
		})
	}
}

func (m model) renderHelp() string {
	help := `
Tcl Debug Console - Simplified Terminal Interface

COMMAND ROUTING:
  command          → Send to active interpreter
  /name command    → Send to specific interpreter
  :use name        → Switch active interpreter

META COMMANDS:
  :help            Show this help
  :list            List interpreters
  :use <name>      Switch active interpreter
  :trace           Toggle stack traces
  :clear           Clear screen
  :refresh         Refresh tab completions
  :scan            Actively scan for hosts (3 sec wait)
  :hosts           List discovered hosts
  :connect <n>     Connect to host
  :reconnect       Reconnect to current host
  :quit            Exit

KEYBOARD:
  Tab              Autocomplete (Tcl commands/procs/vars)
  ↑/↓              Command history
  Ctrl+L           Clear screen
  Ctrl+C           Quit

Press ESC or Q to close this help.
`
	return lipgloss.NewStyle().Foreground(colorBlue).Render(help)
}

// ============================================
// Main
// ============================================

func main() {
	flag.Parse()

	if *debug {
		DebugProtocol = true
	}

	// Determine connection mode
	var mode ConnectionMode
	if *simulate {
		mode = ModeSimulate
	} else {
		switch strings.ToLower(*connMode) {
		case "tcp":
			mode = ModeTCP
		case "ws", "websocket":
			mode = ModeWebSocket
		case "sim", "simulate":
			mode = ModeSimulate
		default:
			mode = ModeTCP
		}
	}

	// Set up discovery
	var discovery *MeshDiscovery
	if *discover && mode == ModeTCP {
		discovery = NewMeshDiscovery()
		if err := discovery.Start(); err != nil {
			fmt.Fprintf(os.Stderr, "Warning: mesh discovery failed: %v\n", err)
		}
	}

	// Handle --list flag
	if *discoverOnly {
		if discovery == nil {
			fmt.Println("Discovery not enabled")
			os.Exit(1)
		}
		fmt.Printf("Scanning for %d seconds...\n", *discoverWait)
		time.Sleep(time.Duration(*discoverWait) * time.Second)

		hosts := discovery.GetAvailableHosts(*tcpCmdPort)
		if len(hosts) == 0 {
			fmt.Println("No hosts found")
		} else {
			fmt.Println("Available hosts:")
			for i, host := range hosts {
				hostType := "Remote"
				if host.IsLocal {
					hostType = "Local"
				}
				fmt.Printf("  %d. %s (%s) [%s]\n", i+1, host.Name, host.IPAddress, hostType)
			}
		}
		os.Exit(0)
	}

	// Determine command address
	cmdAddr := *tcpCmdAddr
	if cmdAddr == "" {
		if *tcpHost != "" {
			cmdAddr = fmt.Sprintf("%s:%d", *tcpHost, *tcpCmdPort)
		} else if mode == ModeTCP && discovery != nil {
			fmt.Printf("Discovering servers (%ds)...\n", *discoverWait)
			time.Sleep(time.Duration(*discoverWait) * time.Second)

			hosts := discovery.GetAvailableHosts(*tcpCmdPort)
			if len(hosts) == 0 {
				fmt.Println("No hosts found. Use -host to specify directly.")
				os.Exit(1)
			} else if len(hosts) == 1 {
				cmdAddr = fmt.Sprintf("%s:%d", hosts[0].IPAddress, *tcpCmdPort)
				fmt.Printf("Connecting to %s (%s)\n", hosts[0].Name, cmdAddr)
			} else {
				peer, err := discovery.SelectHostInteractive(*tcpCmdPort)
				if err != nil {
					fmt.Fprintf(os.Stderr, "Error: %v\n", err)
					os.Exit(1)
				}
				cmdAddr = fmt.Sprintf("%s:%d", peer.IPAddress, *tcpCmdPort)
			}
		} else {
			cmdAddr = fmt.Sprintf("localhost:%d", *tcpCmdPort)
		}
	}

	// Handle pubsub address default
	pubsubAddr := *tcpPubsubAddr
	if pubsubAddr == "" {
		cmdHost, _, err := net.SplitHostPort(cmdAddr)
		if err == nil && cmdHost != "" {
			pubsubAddr = fmt.Sprintf("%s:4620", cmdHost)
		}
	}

	// Determine frame mode
	cmdFrameMode := FrameLength
	if strings.ToLower(*frameMode) == "newline" {
		cmdFrameMode = FrameNewline
	}

	m := initialModel(mode, *wsURL, cmdAddr, pubsubAddr, discovery)
	m.currentHost = cmdAddr

	if m.tcpClient != nil {
		m.tcpClient.SetFrameMode(cmdFrameMode, FrameNewline)
	}

	p := tea.NewProgram(m)

	// Store program reference in model by sending a message
	// (we can't set it directly because NewProgram copies the model)
	go func() {
		time.Sleep(10 * time.Millisecond)
		p.Send(msgSetProgram{program: p})
	}()

	// Handle signals
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-sigCh
		if m.tcpClient != nil {
			m.tcpClient.Close()
		}
		if m.wsClient != nil {
			m.wsClient.Close()
		}
		if discovery != nil {
			discovery.Stop()
		}
		p.Kill()
		os.Exit(0)
	}()

	// Set program reference
	switch mode {
	case ModeWebSocket:
		if m.wsClient != nil {
			m.wsClient.SetProgram(p)
		}
	case ModeTCP:
		if m.tcpClient != nil {
			m.tcpClient.SetProgram(p)
		}
	}

	if discovery != nil {
		discovery.SetProgram(p)
	}

	if _, err := p.Run(); err != nil {
		log.Fatal(err)
	}

	// Cleanup
	if discovery != nil {
		discovery.Stop()
	}
}