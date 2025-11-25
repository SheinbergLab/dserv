package main

import (
	"bufio"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"os"
	"strings"
	"sync"
	"time"

	tea "github.com/charmbracelet/bubbletea"
)

// Debug flag - set to true to enable protocol debugging
var DebugProtocol = false

// ============================================
// DSERV Data Types (from ds_datatype_t)
// ============================================

const (
	DSERV_BYTE           = 0
	DSERV_STRING         = 1
	DSERV_FLOAT          = 2
	DSERV_DOUBLE         = 3
	DSERV_SHORT          = 4
	DSERV_INT            = 5
	DSERV_DG             = 6
	DSERV_SCRIPT         = 7
	DSERV_TRIGGER_SCRIPT = 8
	DSERV_EVT            = 9
	DSERV_NONE           = 10
	DSERV_JSON           = 11
	DSERV_ARROW          = 12
	DSERV_MSGPACK        = 13
	DSERV_JPEG           = 14
	DSERV_PPM            = 15
	DSERV_INT64          = 16
	DSERV_UNKNOWN        = 17
)

// DservDatapoint represents a datapoint received from dserv
type DservDatapoint struct {
	Name      string          `json:"name"`
	Timestamp uint64          `json:"timestamp"`
	Dtype     uint32          `json:"dtype"`
	Data      json.RawMessage `json:"data"`

	// Event-specific fields (when dtype == DSERV_EVT)
	EType    uint8           `json:"e_type,omitempty"`
	ESubtype uint8           `json:"e_subtype,omitempty"`
	EDtype   uint8           `json:"e_dtype,omitempty"`
	EParams  json.RawMessage `json:"e_params,omitempty"`
}

// IsBinaryType returns true if the dtype requires base64 decoding
func (dp *DservDatapoint) IsBinaryType() bool {
	switch dp.Dtype {
	case DSERV_DG, DSERV_ARROW, DSERV_MSGPACK, DSERV_JPEG, DSERV_PPM:
		return true
	}
	return false
}

// IsEvent returns true if this is an event datapoint
func (dp *DservDatapoint) IsEvent() bool {
	return dp.Dtype == DSERV_EVT
}

// GetStringData extracts string data (for STRING, SCRIPT, JSON types)
func (dp *DservDatapoint) GetStringData() (string, error) {
	var s string
	if err := json.Unmarshal(dp.Data, &s); err != nil {
		return "", err
	}
	return s, nil
}

// GetBinaryData decodes base64 data (for DG, ARROW, JPEG, etc.)
func (dp *DservDatapoint) GetBinaryData() ([]byte, error) {
	var b64 string
	if err := json.Unmarshal(dp.Data, &b64); err != nil {
		return nil, err
	}
	return base64.StdEncoding.DecodeString(b64)
}

// GetFloatArray extracts float array data
func (dp *DservDatapoint) GetFloatArray() ([]float64, error) {
	// Could be single value or array
	var single float64
	if err := json.Unmarshal(dp.Data, &single); err == nil {
		return []float64{single}, nil
	}

	var arr []float64
	if err := json.Unmarshal(dp.Data, &arr); err != nil {
		return nil, err
	}
	return arr, nil
}

// GetIntArray extracts integer array data
func (dp *DservDatapoint) GetIntArray() ([]int64, error) {
	// Could be single value or array
	var single int64
	if err := json.Unmarshal(dp.Data, &single); err == nil {
		return []int64{single}, nil
	}

	var arr []int64
	if err := json.Unmarshal(dp.Data, &arr); err != nil {
		return nil, err
	}
	return arr, nil
}

// ============================================
// Completion Cache
// ============================================

// CompletionCache holds cached completion data for an interpreter
type CompletionCache struct {
	interp     string
	commands   []string  // From "info commands"
	procs      []string  // From "info procs"
	globals    []string  // From "info globals"
	lastUpdate time.Time
	mu         sync.RWMutex
}

// NewCompletionCache creates a new completion cache
func NewCompletionCache(interp string) *CompletionCache {
	return &CompletionCache{
		interp:     interp,
		commands:   make([]string, 0),
		procs:      make([]string, 0),
		globals:    make([]string, 0),
		lastUpdate: time.Time{},
	}
}

// Update refreshes the cache from the interpreter
func (cc *CompletionCache) Update(commands, procs, globals []string) {
	cc.mu.Lock()
	defer cc.mu.Unlock()
	cc.commands = commands
	cc.procs = procs
	cc.globals = globals
	cc.lastUpdate = time.Now()
}

// GetCommands returns cached commands (thread-safe)
func (cc *CompletionCache) GetCommands() []string {
	cc.mu.RLock()
	defer cc.mu.RUnlock()
	result := make([]string, len(cc.commands))
	copy(result, cc.commands)
	return result
}

// GetProcs returns cached procs (thread-safe)
func (cc *CompletionCache) GetProcs() []string {
	cc.mu.RLock()
	defer cc.mu.RUnlock()
	result := make([]string, len(cc.procs))
	copy(result, cc.procs)
	return result
}

// GetGlobals returns cached globals (thread-safe)
func (cc *CompletionCache) GetGlobals() []string {
	cc.mu.RLock()
	defer cc.mu.RUnlock()
	result := make([]string, len(cc.globals))
	copy(result, cc.globals)
	return result
}

// GetAll returns all completion candidates
func (cc *CompletionCache) GetAll() []string {
	cc.mu.RLock()
	defer cc.mu.RUnlock()
	
	all := make([]string, 0, len(cc.commands)+len(cc.procs)+len(cc.globals))
	all = append(all, cc.commands...)
	all = append(all, cc.procs...)
	all = append(all, cc.globals...)
	
	// Deduplicate
	seen := make(map[string]bool)
	result := make([]string, 0, len(all))
	for _, item := range all {
		if !seen[item] {
			seen[item] = true
			result = append(result, item)
		}
	}
	
	return result
}

// IsEmpty returns true if cache has no data
func (cc *CompletionCache) IsEmpty() bool {
	cc.mu.RLock()
	defer cc.mu.RUnlock()
	return len(cc.commands) == 0 && len(cc.procs) == 0 && len(cc.globals) == 0
}

// ============================================
// TCP Protocol Handler
// ============================================

// FrameMode determines how messages are delimited
type FrameMode int

const (
	FrameNewline FrameMode = iota // Newline-delimited (legacy)
	FrameLength                   // 4-byte big-endian length prefix
)

// TCPClient handles raw TCP communication with the backend
type TCPClient struct {
	// Command interface (message-oriented, length-prefixed)
	cmdAddr  string
	cmdConn  net.Conn
	cmdFrame FrameMode

	// Pub/sub interface - we create a listener, dserv connects to us
	pubsubAddr     string         // dserv's pub/sub port (e.g., host:4620)
	pubsubListener net.Listener   // our listener for incoming pushes
	pubsubHost     string         // our IP as seen by dserv
	pubsubPort     int            // our listener port

	mu        sync.Mutex
	connected bool
	program   *tea.Program

	// For synchronous command/response
	cmdMu sync.Mutex
	
	// Settings
	stackTracesEnabled bool
	
	// Completion cache (per interpreter)
	completionCaches map[string]*CompletionCache
	cacheMu          sync.RWMutex
}

func NewTCPClient(cmdAddr, pubsubAddr string) *TCPClient {
	return &TCPClient{
		cmdAddr:          cmdAddr,
		pubsubAddr:       pubsubAddr,
		cmdFrame:         FrameLength, // Default to length-prefixed (2560 style)
		completionCaches: make(map[string]*CompletionCache),
	}
}

// SetFrameMode allows configuring framing for command connection
func (c *TCPClient) SetFrameMode(cmd, _ FrameMode) {
	c.cmdFrame = cmd
}

// SetStackTracesEnabled controls whether to fetch full stack traces on errors
func (c *TCPClient) SetStackTracesEnabled(enabled bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.stackTracesEnabled = enabled
}

// GetStackTracesEnabled returns whether stack traces are enabled
func (c *TCPClient) GetStackTracesEnabled() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.stackTracesEnabled
}

func (c *TCPClient) SetProgram(p *tea.Program) {
	c.program = p
}

func (c *TCPClient) Connect() error {
	// Connect to command interface
	cmdConn, err := net.DialTimeout("tcp", c.cmdAddr, 5*time.Second)
	if err != nil {
		return fmt.Errorf("command connection failed: %w", err)
	}

	c.mu.Lock()
	c.cmdConn = cmdConn
	c.mu.Unlock()

	// Set up pub/sub: create listener, register with dserv
	if c.pubsubAddr != "" {
		if err := c.setupPubSubListener(); err != nil {
			// Non-fatal - continue without pub/sub
			if DebugProtocol {
				fmt.Fprintf(os.Stderr, "DEBUG pubsub setup failed: %v\n", err)
			}
		}
	}

	c.mu.Lock()
	c.connected = true
	c.mu.Unlock()

	if c.program != nil {
		c.program.Send(msgConnected{})
	}

	return nil
}

// setupPubSubListener creates a local listener and registers with dserv
// This follows the pattern from essterm.go
func (c *TCPClient) setupPubSubListener() error {
	// Create listener on ephemeral port (port 0 = OS assigns)
	listener, err := net.Listen("tcp4", ":0")
	if err != nil {
		return fmt.Errorf("failed to create listener: %w", err)
	}

	// Get assigned address
	listenerAddr := listener.Addr().(*net.TCPAddr)
	
	// Get our IP as seen by dserv (connect to dserv port to determine our IP)
	ourIP, err := c.getOurIP()
	if err != nil {
		listener.Close()
		return fmt.Errorf("failed to determine our IP: %w", err)
	}

	c.mu.Lock()
	c.pubsubListener = listener
	c.pubsubPort = listenerAddr.Port
	c.pubsubHost = ourIP
	c.mu.Unlock()

	if DebugProtocol {
		fmt.Fprintf(os.Stderr, "DEBUG pubsub listener: %s:%d\n", ourIP, listenerAddr.Port)
	}

	// Start accepting connections from dserv
	go c.acceptPubSubConnections(listener)

	// Register with dserv and subscribe to datapoints
	go c.registerAndSubscribe(ourIP, listenerAddr.Port)

	return nil
}

// getOurIP determines our IP address as seen from dserv's perspective
func (c *TCPClient) getOurIP() (string, error) {
	// Connect to pubsub address (4620) to determine our local IP
	conn, err := net.Dial("tcp", c.pubsubAddr)
	if err != nil {
		return "", err
	}
	defer conn.Close()

	// Get local address from the connection
	localAddr := conn.LocalAddr().(*net.TCPAddr)
	return localAddr.IP.String(), nil
}

// acceptPubSubConnections accepts incoming connections from dserv
func (c *TCPClient) acceptPubSubConnections(listener net.Listener) {
	for {
		conn, err := listener.Accept()
		if err != nil {
			// Listener was closed
			return
		}

		if DebugProtocol {
			fmt.Fprintf(os.Stderr, "DEBUG pubsub connection from %s\n", conn.RemoteAddr())
		}

		// Handle incoming data on this connection
		go c.handlePubSubConn(conn)
	}
}

// handlePubSubConn reads datapoints pushed from dserv
func (c *TCPClient) handlePubSubConn(conn net.Conn) {
	defer conn.Close()
	reader := bufio.NewReader(conn)

	for {
		// dserv sends newline-delimited JSON
		line, err := reader.ReadString('\n')
		if err != nil {
			if DebugProtocol && err != io.EOF {
				fmt.Fprintf(os.Stderr, "DEBUG pubsub read error: %v\n", err)
			}
			return
		}
		c.handlePubSubMessage(strings.TrimSpace(line))
	}
}

// registerAndSubscribe registers with dserv and subscribes to datapoints
func (c *TCPClient) registerAndSubscribe(ourIP string, ourPort int) {
	// Give listener time to start
	time.Sleep(50 * time.Millisecond)

	// Register with dserv: %reg <our_ip> <our_port> <flags>
	// flags: 0=text, 1=binary, 2=JSON
	const SendJSON = 2
	if err := c.dservRegister(ourIP, ourPort, SendJSON); err != nil {
		if DebugProtocol {
			fmt.Fprintf(os.Stderr, "DEBUG register failed: %v\n", err)
		}
		return
	}

	// Subscribe to key datapoints
	c.dservAddMatch(ourIP, ourPort, "dserv/interps")
	c.dservAddMatch(ourIP, ourPort, "dserv/interp_status")
	c.dservAddMatch(ourIP, ourPort, "dserv/errors")
	c.dservAddMatch(ourIP, ourPort, "print")

	// Touch to get current values
	time.Sleep(50 * time.Millisecond)
	c.TouchDatapoint("dserv/interps")
}

// dservRegister sends %reg command to dserv port 4620
func (c *TCPClient) dservRegister(ourIP string, ourPort int, flags int) error {
	cmd := fmt.Sprintf("%%reg %s %d %d", ourIP, ourPort, flags)
	return c.sendToPubSubServer(cmd)
}

// dservAddMatch sends %match command to dserv port 4620
func (c *TCPClient) dservAddMatch(ourIP string, ourPort int, pattern string) error {
	cmd := fmt.Sprintf("%%match %s %d %s 1", ourIP, ourPort, pattern)
	return c.sendToPubSubServer(cmd)
}

// sendToPubSubServer sends a command to the dserv pub/sub port (4620)
func (c *TCPClient) sendToPubSubServer(cmd string) error {
	conn, err := net.DialTimeout("tcp", c.pubsubAddr, 5*time.Second)
	if err != nil {
		return err
	}
	defer conn.Close()

	if DebugProtocol {
		fmt.Fprintf(os.Stderr, "DEBUG sendToPubSubServer: %s\n", cmd)
	}

	_, err = fmt.Fprintf(conn, "%s\n", cmd)
	if err != nil {
		return err
	}

	// Read response
	reader := bufio.NewReader(conn)
	response, err := reader.ReadString('\n')
	if err != nil && err != io.EOF {
		return err
	}

	if DebugProtocol {
		fmt.Fprintf(os.Stderr, "DEBUG pubsub response: %s\n", strings.TrimSpace(response))
	}

	return nil
}

// ============================================
// Framed Message I/O
// ============================================

// sendFramed sends a length-prefixed message (4-byte big-endian + payload)
func sendFramed(conn net.Conn, msg string) error {
	data := []byte(msg)
	length := uint32(len(data))

	// Write 4-byte big-endian length
	lenBuf := make([]byte, 4)
	binary.BigEndian.PutUint32(lenBuf, length)

	if DebugProtocol {
		fmt.Fprintf(os.Stderr, "DEBUG sendFramed: len=%d msg=%q\n", length, msg)
	}

	if _, err := conn.Write(lenBuf); err != nil {
		return fmt.Errorf("write length: %w", err)
	}

	// Write payload
	if _, err := conn.Write(data); err != nil {
		return fmt.Errorf("write payload: %w", err)
	}

	return nil
}

// recvFramed receives a length-prefixed message
func recvFramed(conn net.Conn) (string, error) {
	// Read 4-byte big-endian length
	lenBuf := make([]byte, 4)
	n, err := io.ReadFull(conn, lenBuf)
	if err != nil {
		return "", fmt.Errorf("read length (got %d bytes): %w", n, err)
	}

	length := binary.BigEndian.Uint32(lenBuf)

	if DebugProtocol {
		fmt.Fprintf(os.Stderr, "DEBUG recvFramed: expecting %d bytes\n", length)
	}

	// Sanity check - don't allocate huge buffers
	if length > 10*1024*1024 { // 10MB max
		return "", fmt.Errorf("message too large: %d bytes", length)
	}

	// Handle empty response
	if length == 0 {
		return "", nil
	}

	// Read payload
	data := make([]byte, length)
	n, err = io.ReadFull(conn, data)
	if err != nil {
		return "", fmt.Errorf("read payload (got %d of %d bytes): %w", n, length, err)
	}

	if DebugProtocol {
		fmt.Fprintf(os.Stderr, "DEBUG recvFramed: got %q\n", string(data))
	}

	return string(data), nil
}

// sendNewline sends a newline-terminated message
func sendNewline(conn net.Conn, msg string) error {
	msg = strings.TrimSuffix(msg, "\n") + "\n"
	_, err := conn.Write([]byte(msg))
	return err
}

// ============================================
// Command Interface (Synchronous)
// ============================================

// SendCommand sends a command and waits for response (framed protocol)
func (c *TCPClient) SendCommand(cmd string) (string, error) {
	c.cmdMu.Lock()
	defer c.cmdMu.Unlock()

	c.mu.Lock()
	conn := c.cmdConn
	frameMode := c.cmdFrame
	c.mu.Unlock()

	if conn == nil {
		return "", fmt.Errorf("not connected")
	}

	// Set deadline for this command - 5 second timeout
	deadline := time.Now().Add(5 * time.Second)
	if err := conn.SetDeadline(deadline); err != nil {
		return "", fmt.Errorf("failed to set deadline: %w", err)
	}
	defer conn.SetDeadline(time.Time{})

	if frameMode == FrameLength {
		// Length-prefixed protocol
		if err := sendFramed(conn, cmd); err != nil {
			c.handleDisconnect(err)
			return "", fmt.Errorf("send failed: %w", err)
		}

		response, err := recvFramed(conn)
		if err != nil {
			c.handleDisconnect(err)
			return "", fmt.Errorf("recv failed: %w", err)
		}
		return response, nil

	} else {
		// Newline-delimited protocol
		if err := sendNewline(conn, cmd); err != nil {
			c.handleDisconnect(err)
			return "", fmt.Errorf("send failed: %w", err)
		}

		reader := bufio.NewReader(conn)
		response, err := reader.ReadString('\n')
		if err != nil {
			c.handleDisconnect(err)
			return "", fmt.Errorf("recv failed: %w", err)
		}
		return strings.TrimSpace(response), nil
	}
}

// SendToInterp sends a command to a specific Tcl interpreter
func (c *TCPClient) SendToInterp(interp, cmd string) {
	var fullCmd string
	
	// "dserv" is the main interpreter - send directly without wrapping
	if interp == "dserv" {
		fullCmd = cmd
	} else {
		// Use dserv's "send" command format for other interpreters
		fullCmd = fmt.Sprintf("send %s {%s}", interp, cmd)
	}

	response, err := c.SendCommand(fullCmd)
	if err != nil {
		if c.program != nil {
			c.program.Send(msgOutput{interp: interp, text: fmt.Sprintf("error: %v", err)})
		}
		return
	}

	// Check for Tcl error response
	if strings.HasPrefix(response, "!TCL_ERROR ") {
		errPayload := strings.TrimPrefix(response, "!TCL_ERROR ")
		
		var message, errorInfo string
		
		// Try JSON format first: {"error":true,"message":"...","errorInfo":"..."}
		var jsonErr struct {
			Message   string `json:"message"`
			ErrorInfo string `json:"errorInfo"`
			ErrorCode string `json:"errorCode"`
		}
		if err := json.Unmarshal([]byte(errPayload), &jsonErr); err == nil {
			message = jsonErr.Message
			errorInfo = jsonErr.ErrorInfo
		} else {
			// Try Tcl list format: {message} {errorInfo}
			// Only parse as list if it starts with '{' (proper Tcl list format)
			if strings.HasPrefix(strings.TrimSpace(errPayload), "{") {
				message, errorInfo = parseTclError(errPayload)
			} else {
				// Not a proper Tcl list - treat entire payload as message
				message = errPayload
			}
		}
		
		// If we don't have errorInfo and stack traces are enabled, fetch it
		if errorInfo == "" && c.GetStackTracesEnabled() {
			if DebugProtocol {
				fmt.Fprintf(os.Stderr, "DEBUG: Fetching errorInfo for %s\n", interp)
			}
			
			var fetchCmd string
			if interp == "dserv" {
				fetchCmd = "set errorInfo"
			} else {
				fetchCmd = fmt.Sprintf("send %s {set errorInfo}", interp)
			}
			
			stackTrace, err := c.SendCommand(fetchCmd)
			if err == nil && !strings.HasPrefix(stackTrace, "!TCL_ERROR") {
				errorInfo = stackTrace
			}
		}
		
		if c.program != nil {
			c.program.Send(msgError{
				interp:    interp,
				message:   message,
				errorInfo: errorInfo,
				timestamp: time.Now(),
			})
		}
		return
	}

	if c.program != nil {
		c.program.Send(msgOutput{interp: interp, text: response})
	}
}

// RequestInterpList asks the server for available interpreters
func (c *TCPClient) RequestInterpList() {
	// This might be "dservinfo interps" or similar - adjust to your actual command
	response, err := c.SendCommand("dservinfo interps")
	if err != nil {
		return
	}

	// Parse response - adjust based on actual format
	// Might be space-separated list, JSON, or Tcl list
	interps := parseInterpList(response)
	if len(interps) > 0 && c.program != nil {
		c.program.Send(msgInterpList{interpreters: interps})
	}
}

// ============================================
// Completion Cache Management
// ============================================

// RefreshCompletionCache populates the completion cache for an interpreter
func (c *TCPClient) RefreshCompletionCache(interp string) {
	// Recover from panics
	defer func() {
		if r := recover(); r != nil {
			if DebugProtocol {
				fmt.Fprintf(os.Stderr, "PANIC in RefreshCompletionCache(%s): %v\n", interp, r)
			}
		}
	}()
	
	if DebugProtocol {
		fmt.Fprintf(os.Stderr, "DEBUG: Refreshing completion cache for %s\n", interp)
	}

	// Query the interpreter for completion data
	var cmdForCommands, cmdForProcs, cmdForGlobals, cmdForNamespaces string
	
	if interp == "dserv" {
		cmdForCommands = "info commands"
		cmdForProcs = "info procs"
		cmdForGlobals = "info globals"
		cmdForNamespaces = "namespace children ::"
	} else {
		cmdForCommands = fmt.Sprintf("send %s {info commands}", interp)
		cmdForProcs = fmt.Sprintf("send %s {info procs}", interp)
		cmdForGlobals = fmt.Sprintf("send %s {info globals}", interp)
		cmdForNamespaces = fmt.Sprintf("send %s {namespace children ::}", interp)
	}

	// Get commands
	var commands, procs, globals []string
	
	if resp, err := c.SendCommand(cmdForCommands); err == nil && !strings.HasPrefix(resp, "!TCL_ERROR") {
		commands = parseTclList(resp)
	} else if DebugProtocol && err != nil {
		fmt.Fprintf(os.Stderr, "DEBUG: Failed to get commands for %s: %v\n", interp, err)
	}
	
	if resp, err := c.SendCommand(cmdForProcs); err == nil && !strings.HasPrefix(resp, "!TCL_ERROR") {
		procs = parseTclList(resp)
	} else if DebugProtocol && err != nil {
		fmt.Fprintf(os.Stderr, "DEBUG: Failed to get procs for %s: %v\n", interp, err)
	}
	
	if resp, err := c.SendCommand(cmdForGlobals); err == nil && !strings.HasPrefix(resp, "!TCL_ERROR") {
		globals = parseTclList(resp)
	} else if DebugProtocol && err != nil {
		fmt.Fprintf(os.Stderr, "DEBUG: Failed to get globals for %s: %v\n", interp, err)
	}
	
	// Get procs from namespaces
	var namespaceProcs []string
	if resp, err := c.SendCommand(cmdForNamespaces); err == nil && !strings.HasPrefix(resp, "!TCL_ERROR") {
		namespaces := parseTclList(resp)
		if DebugProtocol {
			fmt.Fprintf(os.Stderr, "DEBUG: Found %d namespaces for %s: %v\n", len(namespaces), interp, namespaces)
		}
		for _, ns := range namespaces {
			// Get procs in this namespace
			var nsProcsCmd string
			if interp == "dserv" {
				nsProcsCmd = fmt.Sprintf("info procs %s::*", ns)
			} else {
				nsProcsCmd = fmt.Sprintf("send %s {info procs %s::*}", interp, ns)
			}
			
			if nsResp, nsErr := c.SendCommand(nsProcsCmd); nsErr == nil && !strings.HasPrefix(nsResp, "!TCL_ERROR") {
				nsProcs := parseTclList(nsResp)
				if DebugProtocol && len(nsProcs) > 0 {
					fmt.Fprintf(os.Stderr, "DEBUG: Found %d procs in %s\n", len(nsProcs), ns)
				}
				namespaceProcs = append(namespaceProcs, nsProcs...)
			} else if DebugProtocol && nsErr != nil {
				fmt.Fprintf(os.Stderr, "DEBUG: Failed to get procs for %s: %v\n", ns, nsErr)
			}
			
			// Also recursively check nested namespaces (one level deep for now)
			var nsChildrenCmd string
			if interp == "dserv" {
				nsChildrenCmd = fmt.Sprintf("namespace children %s", ns)
			} else {
				nsChildrenCmd = fmt.Sprintf("send %s {namespace children %s}", interp, ns)
			}
			
			if childResp, childErr := c.SendCommand(nsChildrenCmd); childErr == nil && !strings.HasPrefix(childResp, "!TCL_ERROR") {
				children := parseTclList(childResp)
				for _, child := range children {
					var childProcsCmd string
					if interp == "dserv" {
						childProcsCmd = fmt.Sprintf("info procs %s::*", child)
					} else {
						childProcsCmd = fmt.Sprintf("send %s {info procs %s::*}", interp, child)
					}
					
					if childProcsResp, childProcsErr := c.SendCommand(childProcsCmd); childProcsErr == nil && !strings.HasPrefix(childProcsResp, "!TCL_ERROR") {
						childProcs := parseTclList(childProcsResp)
						namespaceProcs = append(namespaceProcs, childProcs...)
					}
				}
			}
		}
	} else if DebugProtocol && err != nil {
		fmt.Fprintf(os.Stderr, "DEBUG: Failed to get namespaces for %s: %v\n", interp, err)
	}
	
	// Combine global procs and namespace procs
	allProcs := append(procs, namespaceProcs...)

	// Update or create cache
	c.cacheMu.Lock()
	cache, exists := c.completionCaches[interp]
	if !exists {
		cache = NewCompletionCache(interp)
		c.completionCaches[interp] = cache
	}
	c.cacheMu.Unlock()

	cache.Update(commands, allProcs, globals)
	
	if DebugProtocol {
		fmt.Fprintf(os.Stderr, "DEBUG: Cache updated for %s - commands:%d procs:%d (global:%d ns:%d) globals:%d\n",
			interp, len(commands), len(allProcs), len(procs), len(namespaceProcs), len(globals))
	}
}

// RefreshAllCaches refreshes completion caches for all known interpreters
func (c *TCPClient) RefreshAllCaches(interpreters []string) {
	for _, interp := range interpreters {
		go c.RefreshCompletionCache(interp)
	}
}

// GetCompletionCache returns the completion cache for an interpreter
func (c *TCPClient) GetCompletionCache(interp string) *CompletionCache {
	c.cacheMu.RLock()
	defer c.cacheMu.RUnlock()
	return c.completionCaches[interp]
}

// GetProcArgs queries the interpreter for procedure arguments
func (c *TCPClient) GetProcArgs(interp, procName string) []string {
	var cmd string
	if interp == "dserv" {
		cmd = fmt.Sprintf("info args %s", procName)
	} else {
		cmd = fmt.Sprintf("send %s {info args %s}", interp, procName)
	}

	resp, err := c.SendCommand(cmd)
	if err != nil || strings.HasPrefix(resp, "!TCL_ERROR") {
		return nil
	}

	return parseTclList(resp)
}

// ============================================
// Request Interpreter List (continued)
// ============================================

// parseInterpList handles various response formats
func parseInterpList(response string) []string {
	response = strings.TrimSpace(response)

	var interps []string

	// Try JSON array first
	var jsonInterps []string
	if err := json.Unmarshal([]byte(response), &jsonInterps); err == nil {
		interps = jsonInterps
	} else if strings.Contains(response, " ") {
		// Try space-separated (Tcl list style)
		interps = strings.Fields(response)
	} else if response != "" {
		// Single interpreter
		interps = []string{response}
	}

	// Ensure "dserv" is always available as primary bridge
	return ensureDserv(interps)
}

// ensureDserv ensures "dserv" is in the list (always available as main bridge)
func ensureDserv(interps []string) []string {
	for _, name := range interps {
		if name == "dserv" {
			return interps
		}
	}
	// Prepend dserv if not present
	return append([]string{"dserv"}, interps...)
}

// TouchDatapoint triggers immediate send of a datapoint's current value
func (c *TCPClient) TouchDatapoint(name string) {
	// dservTouch triggers subscribers to receive current value
	c.SendCommand(fmt.Sprintf("dservTouch %s", name))
}

// TouchDatapoints touches multiple datapoints in a single command (efficient)
func (c *TCPClient) TouchDatapoints(names ...string) {
	if len(names) == 0 {
		return
	}
	// Use foreach to batch touch in single round-trip
	vars := strings.Join(names, " ")
	cmd := fmt.Sprintf("foreach v {%s} { dservTouch $v }", vars)
	c.SendCommand(cmd)
}

// InitialTouch touches common datapoints to populate UI on connect
func (c *TCPClient) InitialTouch() {
	// Touch datapoints relevant for debug console
	c.TouchDatapoints(
		// Interpreter/system state
		"dserv/interps",
		"ess/state",
		"ess/system",
		"ess/protocol",
		"ess/subject",
		// Observation tracking
		"ess/obs_id",
		"ess/obs_total",
		// System info
		"system/hostname",
	)
}

// ============================================
// Pub/Sub Interface (Asynchronous)
// ============================================

func (c *TCPClient) handlePubSubMessage(msg string) {
	if c.program == nil || msg == "" {
		return
	}

	if DebugProtocol {
		fmt.Fprintf(os.Stderr, "DEBUG pubsub recv: %s\n", msg)
	}

	// Parse as DservDatapoint
	var dp DservDatapoint
	if err := json.Unmarshal([]byte(msg), &dp); err != nil {
		// Unknown format - log it to messages pane
		if DebugProtocol {
			fmt.Fprintf(os.Stderr, "DEBUG pubsub not JSON: %v\n", err)
		}
		c.program.Send(msgEvent{source: "pubsub", text: msg})
		return
	}

	if DebugProtocol {
		fmt.Fprintf(os.Stderr, "DEBUG pubsub parsed: name=%s dtype=%d\n", dp.Name, dp.Dtype)
	}

	// Handle events specially
	if dp.IsEvent() {
		c.handleEventDatapoint(&dp)
		return
	}

	// Handle by datapoint name
	c.handleNamedDatapoint(&dp)
}

func (c *TCPClient) handleEventDatapoint(dp *DservDatapoint) {
	// Events have e_type, e_subtype, e_params
	switch dp.EType {
	case 1: // NAMESET
		// Event type name definition - could store these
		return

	case 2: // FILEIO
		// File I/O events
		return

	case 3: // USER event
		switch dp.ESubtype {
		case 0:
			c.program.Send(msgEvent{source: "system", text: "System RUNNING"})
		case 1:
			c.program.Send(msgEvent{source: "system", text: "System STOPPED"})
		case 2:
			c.program.Send(msgEvent{source: "system", text: "Observation RESET"})
		}

	case 6: // SUBTYPE NAMES
		// Subtype name definitions - could store these
		return

	case 18: // SYSTEM CHANGES
		return

	case 19: // BEGINOBS
		c.program.Send(msgEvent{source: "system", text: fmt.Sprintf("Begin observation (t=%d)", dp.Timestamp)})

	case 20: // ENDOBS
		c.program.Send(msgEvent{source: "system", text: fmt.Sprintf("End observation (t=%d)", dp.Timestamp)})

	default:
		// Generic event - show type/subtype
		var params string
		if dp.EParams != nil {
			params = string(dp.EParams)
		}
		c.program.Send(msgEvent{
			source: "event",
			text:   fmt.Sprintf("evt:%d:%d %s", dp.EType, dp.ESubtype, params),
		})
	}
}

func (c *TCPClient) handleNamedDatapoint(dp *DservDatapoint) {
	switch dp.Name {
	case "dserv/interps", "sys/interps":
		// Interpreter list update
		c.handleInterpsDatapoint(dp)

	case "dserv/errors", "sys/errors":
		// Error event from any interpreter
		c.handleErrorDatapoint(dp)

	case "dserv/interp_status", "sys/interp_status":
		// Interpreter status change
		c.handleInterpStatusDatapoint(dp)

	case "print":
		if str, err := dp.GetStringData(); err == nil {
			c.program.Send(msgEvent{source: "print", text: str})
		}

	case "eventlog/events":
		// Events can also come as regular datapoints
		c.handleEventDatapoint(dp)

	default:
		// Display other datapoints based on type - async events go to messages
		c.handleGenericDatapoint(dp)
	}
}

func (c *TCPClient) handleInterpsDatapoint(dp *DservDatapoint) {
	// Try structured format first: [{"name":"...", "status":"..."},...]
	var structured []struct {
		Name       string `json:"name"`
		Status     string `json:"status"`
		ErrorCount int    `json:"error_count"`
	}
	if err := json.Unmarshal(dp.Data, &structured); err == nil {
		names := make([]string, len(structured))
		for i, interp := range structured {
			names[i] = interp.Name
		}
		c.program.Send(msgInterpList{interpreters: ensureDserv(names)})
		return
	}

	// Try simple string array
	var names []string
	if err := json.Unmarshal(dp.Data, &names); err == nil {
		c.program.Send(msgInterpList{interpreters: ensureDserv(names)})
		return
	}

	// Try space-separated string
	if str, err := dp.GetStringData(); err == nil {
		names = strings.Fields(str)
		if len(names) > 0 {
			c.program.Send(msgInterpList{interpreters: ensureDserv(names)})
		}
	}
}

func (c *TCPClient) handleErrorDatapoint(dp *DservDatapoint) {
	var errEvt struct {
		Interp    string `json:"interp"`
		Timestamp int64  `json:"timestamp"`
		Message   string `json:"message"`
		ErrorInfo string `json:"errorInfo"`
		ErrorCode string `json:"errorCode"`
		Command   string `json:"command"`
	}
	if err := json.Unmarshal(dp.Data, &errEvt); err == nil {
		ts := time.UnixMicro(errEvt.Timestamp)
		if errEvt.Timestamp < 1e12 {
			// Might be seconds instead of microseconds
			ts = time.Unix(errEvt.Timestamp, 0)
		}
		c.program.Send(msgError{
			interp:    errEvt.Interp,
			message:   errEvt.Message,
			errorInfo: errEvt.ErrorInfo,
			timestamp: ts,
		})
	}
}

func (c *TCPClient) handleInterpStatusDatapoint(dp *DservDatapoint) {
	var status struct {
		Interp    string `json:"interp"`
		Status    string `json:"status"`
		Timestamp int64  `json:"timestamp"`
	}
	if err := json.Unmarshal(dp.Data, &status); err == nil {
		c.program.Send(msgEvent{
			source: status.Interp,
			text:   fmt.Sprintf("[%s]", status.Status),
		})
		// Request updated interpreter list on status change
		if status.Status == "started" || status.Status == "stopped" {
			go c.RequestInterpList()
		}
	}
}

func (c *TCPClient) handleGenericDatapoint(dp *DservDatapoint) {
	// Format based on dtype
	var display string

	switch dp.Dtype {
	case DSERV_STRING, DSERV_SCRIPT, DSERV_JSON:
		if str, err := dp.GetStringData(); err == nil {
			display = str
		}

	case DSERV_FLOAT, DSERV_DOUBLE:
		if vals, err := dp.GetFloatArray(); err == nil {
			if len(vals) == 1 {
				display = fmt.Sprintf("%.4f", vals[0])
			} else {
				display = fmt.Sprintf("%v", vals)
			}
		}

	case DSERV_BYTE, DSERV_SHORT, DSERV_INT, DSERV_INT64:
		if vals, err := dp.GetIntArray(); err == nil {
			if len(vals) == 1 {
				display = fmt.Sprintf("%d", vals[0])
			} else {
				display = fmt.Sprintf("%v", vals)
			}
		}

	case DSERV_DG, DSERV_ARROW, DSERV_MSGPACK, DSERV_JPEG, DSERV_PPM:
		// Binary data - just show size
		if data, err := dp.GetBinaryData(); err == nil {
			display = fmt.Sprintf("[binary: %d bytes]", len(data))
		} else {
			display = "[binary data]"
		}

	case DSERV_NONE:
		display = "<none>"

	default:
		display = string(dp.Data)
	}

	if display != "" {
		// Shorten name for display
		shortName := dp.Name
		if idx := strings.LastIndex(dp.Name, "/"); idx >= 0 {
			shortName = dp.Name[idx+1:]
		}
		// Async datapoints go to messages pane
		c.program.Send(msgEvent{
			source: shortName,
			text:   display,
		})
	}
}

// Subscribe sends a subscription request for a datapoint pattern
// Uses the pubsub connection (port 4620) with newline protocol
// ============================================
// Connection Management
// ============================================

func (c *TCPClient) handleDisconnect(err error) {
	c.mu.Lock()
	wasConnected := c.connected
	c.connected = false
	c.mu.Unlock()

	if wasConnected && c.program != nil {
		c.program.Send(msgDisconnected{err: err})
	}
}

func (c *TCPClient) IsConnected() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.connected
}

func (c *TCPClient) Close() {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.cmdConn != nil {
		c.cmdConn.Close()
		c.cmdConn = nil
	}
	if c.pubsubListener != nil {
		c.pubsubListener.Close()
		c.pubsubListener = nil
	}
	c.connected = false
}

// ============================================
// Tcl List Parsing Helpers
// ============================================

// parseTclList parses a Tcl-style list: {elem1} {elem2} or elem1 elem2
func parseTclList(s string) []string {
	s = strings.TrimSpace(s)
	if s == "" {
		return nil
	}

	var result []string
	var current strings.Builder
	depth := 0
	inBraces := false

	for _, ch := range s {
		switch ch {
		case '{':
			if depth == 0 {
				inBraces = true
			} else {
				current.WriteRune(ch)
			}
			depth++

		case '}':
			depth--
			if depth == 0 {
				result = append(result, current.String())
				current.Reset()
				inBraces = false
			} else {
				current.WriteRune(ch)
			}

		case ' ', '\t':
			if inBraces {
				current.WriteRune(ch)
			} else if current.Len() > 0 {
				result = append(result, current.String())
				current.Reset()
			}

		default:
			current.WriteRune(ch)
		}
	}

	if current.Len() > 0 {
		result = append(result, current.String())
	}

	return result
}

// parseTclError extracts error message and info from Tcl error format
func parseTclError(s string) (message, errorInfo string) {
	parts := parseTclList(s)
	if len(parts) >= 1 {
		message = parts[0]
	}
	if len(parts) >= 2 {
		errorInfo = parts[1]
	}
	return
}