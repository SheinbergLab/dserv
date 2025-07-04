package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"log"
	"net"
	"strconv"
	"strings"
	"sync"
	"time"
)

// DservUpdate represents an update from dserv
type DservUpdate struct {
	Name      string
	Value     string
	Timestamp time.Time
}

// DservClient handles communication with dserv
type DservClient struct {
	address  string
	verbose  bool
	jsonMode bool  // Whether to request JSON format from dserv
	
	// Callback server for receiving updates
	callbackServer   net.Listener
	callbackPort     int
	localIP          string
	registrationID   int
	
	// Update processing
	updateChannel    chan DservUpdate
	
	// Statistics
	mutex            sync.RWMutex
	totalUpdates     int
	subscriptions    map[string]int  // variable -> every
	connected        bool
	lastUpdate       time.Time
}

// NewDservClient creates a new dserv client
func NewDservClient(address string, verbose bool) *DservClient {
	return &DservClient{
		address:       address,
		verbose:       verbose,
		jsonMode:      true,  // Default to JSON mode
		updateChannel: make(chan DservUpdate, 1000), // Large buffer for high-frequency updates
		subscriptions: make(map[string]int),
	}
}

// Initialize sets up the dserv connection and callback server
func (dc *DservClient) Initialize() error {
	log.Printf("🔌 Initializing dserv client for %s", dc.address)
	
	// Start callback server
	if err := dc.startCallbackServer(); err != nil {
		return fmt.Errorf("failed to start callback server: %v", err)
	}
	
	// Register with dserv
	if err := dc.registerWithDserv(); err != nil {
		return fmt.Errorf("failed to register with dserv: %v", err)
	}
	
	dc.connected = true
	log.Printf("✅ Dserv client initialized successfully")
	return nil
}

// startCallbackServer creates a TCP server to receive dserv updates
func (dc *DservClient) startCallbackServer() error {
	// Listen on random port
	listener, err := net.Listen("tcp", ":0")
	if err != nil {
		return err
	}
	
	dc.callbackServer = listener
	addr := listener.Addr().(*net.TCPAddr)
	dc.callbackPort = addr.Port
	
	// Determine local IP
	dc.localIP = dc.getLocalIP()
	
	if dc.verbose {
		log.Printf("📞 Callback server listening on %s:%d", dc.localIP, dc.callbackPort)
	}
	
	// Start accepting connections
	go dc.handleCallbackConnections()
	
	return nil
}

// getLocalIP determines the local IP address for dserv callbacks
func (dc *DservClient) getLocalIP() string {
	// Try to determine IP by connecting to a remote address
	conn, err := net.Dial("udp", "8.8.8.8:80")
	if err != nil {
		return "127.0.0.1" // Fallback to localhost
	}
	defer conn.Close()
	
	localAddr := conn.LocalAddr().(*net.UDPAddr)
	return localAddr.IP.String()
}

// registerWithDserv registers this client with dserv for callbacks
func (dc *DservClient) registerWithDserv() error {
	conn, err := dc.connectToDserv()
	if err != nil {
		return err
	}
	defer conn.Close()
	
	// Send registration command: %reg IP PORT TYPE
	// TYPE: 1 = text format, 2 = JSON format
	regType := 1
	if dc.jsonMode {
		regType = 2
	}
	
	command := fmt.Sprintf("%%reg %s %d %d\n", dc.localIP, dc.callbackPort, regType)
	if _, err := conn.Write([]byte(command)); err != nil {
		return err
	}
	
	// Read registration response
	scanner := bufio.NewScanner(conn)
	if scanner.Scan() {
		response := strings.TrimSpace(scanner.Text())
		if regID, err := strconv.Atoi(response); err == nil {
			dc.registrationID = regID
			if dc.verbose {
				format := "text"
				if dc.jsonMode {
					format = "JSON"
				}
				log.Printf("✅ Registered with dserv, ID: %d, format: %s", regID, format)
			}
			return nil
		}
		return fmt.Errorf("invalid registration response: %s", response)
	}
	
	return fmt.Errorf("no registration response received")
}

// handleCallbackConnections processes incoming dserv callback connections
func (dc *DservClient) handleCallbackConnections() {
	connectionCount := 0
	
	for {
		conn, err := dc.callbackServer.Accept()
		if err != nil {
			if dc.verbose {
				log.Printf("❌ Error accepting callback connection: %v", err)
			}
			return
		}
		
		connectionCount++
		if dc.verbose {
			log.Printf("📞 Accepted callback connection #%d from %s", connectionCount, conn.RemoteAddr())
		}
		
		go dc.handleCallbackConnection(conn, connectionCount)
	}
}

// handleCallbackConnection processes a single callback connection
func (dc *DservClient) handleCallbackConnection(conn net.Conn, connID int) {
	defer conn.Close()
	
	// Use a larger buffer for potentially long JSON messages
	scanner := bufio.NewScanner(conn)
	scanner.Buffer(make([]byte, 4096), 1024*1024) // Initial 4KB, max 1MB buffer
	
	updateCount := 0
	messageBuffer := ""
	
	// Alternative approach: read byte by byte like the C++ code
	reader := bufio.NewReader(conn)
	
	for {
		// Read one byte at a time to handle long messages properly
		char, err := reader.ReadByte()
		if err != nil {
			if dc.verbose {
				log.Printf("🔗 Connection #%d ended: %v", connID, err)
			}
			break
		}
		
		if char == '\n' {
			// We have a complete message
			if len(messageBuffer) > 0 {
				updateCount++
				dc.mutex.Lock()
				dc.totalUpdates++
				dc.lastUpdate = time.Now()
				dc.mutex.Unlock()
				
				// Debug: Show what we received
				if dc.verbose {
					log.Printf("🔍 Raw message received (%d bytes): %s", len(messageBuffer), messageBuffer)
				}
				
				// Parse the complete message
				update := dc.parseDservUpdate(strings.TrimSpace(messageBuffer))
				if update != nil {
					if dc.verbose {
						log.Printf("✅ Parsed: name=%s, value=%s", update.Name, update.Value)
					}
					
					// Send to update channel (non-blocking)
					select {
					case dc.updateChannel <- *update:
						// Successfully queued
					default:
						if dc.verbose {
							log.Printf("⚠️  Update channel full, dropping update for %s", update.Name)
						}
					}
				} else {
					if dc.verbose {
						log.Printf("❌ Failed to parse message: %s", messageBuffer)
					}
				}
				
				// Reset buffer for next message
				messageBuffer = ""
			}
		} else {
			// Accumulate characters for the current message
			messageBuffer += string(char)
		}
	}
	
	if dc.verbose && updateCount > 0 {
		log.Printf("🔗 Connection #%d ended after %d updates", connID, updateCount)
	}
}

// parseDservUpdate parses a dserv update line based on the configured format
func (dc *DservClient) parseDservUpdate(line string) *DservUpdate {
	if dc.jsonMode {
		// We know all messages are JSON format
		return dc.parseJSONUpdate(line)
	}
	
	// Legacy text format parsing (for when jsonMode = false)
	// Try to parse dserv protocol format
	// Format: **<id> <n> <datatype> <timestamp> <length> {<data>}
	if strings.HasPrefix(line, "**") {
		parts := strings.SplitN(line, " ", 6)
		if len(parts) >= 6 {
			name := parts[1]
			dataWithBraces := parts[5]
			
			// Extract data from braces
			if len(dataWithBraces) >= 2 && 
			   dataWithBraces[0] == '{' && 
			   dataWithBraces[len(dataWithBraces)-1] == '}' {
				data := dataWithBraces[1 : len(dataWithBraces)-1]
				
				return &DservUpdate{
					Name:      name,
					Value:     data,
					Timestamp: time.Now(),
				}
			}
		}
	}
	
	// Try simple format: variable_name value
	parts := strings.SplitN(line, " ", 2)
	if len(parts) >= 2 {
		return &DservUpdate{
			Name:      parts[0],
			Value:     parts[1],
			Timestamp: time.Now(),
		}
	}
	
	if dc.verbose {
		log.Printf("⚠️  Could not parse text format message: %s", line)
	}
	return nil
}

// parseJSONUpdate parses JSON-formatted dserv messages
func (dc *DservClient) parseJSONUpdate(line string) *DservUpdate {
	var jsonMsg struct {
		Name      string `json:"name"`
		Timestamp int64  `json:"timestamp"`
		Dtype     int    `json:"dtype"`
		Data      string `json:"data"`
	}
	
	if err := json.Unmarshal([]byte(line), &jsonMsg); err != nil {
		if dc.verbose {
			log.Printf("❌ Failed to parse JSON message: %v", err)
			log.Printf("❌ Raw message was: %s", line)
		}
		return nil
	}
	
	// Convert timestamp from milliseconds to time.Time
	var timestamp time.Time
	if jsonMsg.Timestamp > 0 {
		timestamp = time.UnixMilli(jsonMsg.Timestamp)
	} else {
		timestamp = time.Now()
	}
	
	return &DservUpdate{
		Name:      jsonMsg.Name,
		Value:     jsonMsg.Data,
		Timestamp: timestamp,
	}
}

// connectToDserv creates a connection to the dserv server
func (dc *DservClient) connectToDserv() (net.Conn, error) {
	conn, err := net.DialTimeout("tcp", dc.address, 5*time.Second)
	if err != nil {
		return nil, err
	}
	
	// Set read/write deadline
	conn.SetDeadline(time.Now().Add(10 * time.Second))
	return conn, nil
}

// Query sends a query command to dserv
func (dc *DservClient) Query(variable string) (string, error) {
	conn, err := dc.connectToDserv()
	if err != nil {
		return "", fmt.Errorf("connection failed: %v", err)
	}
	defer conn.Close()
	
	// Send query command
	command := fmt.Sprintf("%%get %s\n", variable)
	if _, err := conn.Write([]byte(command)); err != nil {
		return "", fmt.Errorf("write failed: %v", err)
	}
	
	// Read response
	scanner := bufio.NewScanner(conn)
	if scanner.Scan() {
		response := strings.TrimSpace(scanner.Text())
		if dc.verbose {
			log.Printf("🔍 Query %s = %s", variable, response)
		}
		return response, nil
	}
	
	if err := scanner.Err(); err != nil {
		return "", fmt.Errorf("read failed: %v", err)
	}
	
	return "", fmt.Errorf("no response received")
}

// Subscribe subscribes to updates for a variable
func (dc *DservClient) Subscribe(variable string, every int) error {
	conn, err := dc.connectToDserv()
	if err != nil {
		return fmt.Errorf("connection failed: %v", err)
	}
	defer conn.Close()
	
	// Send subscription command: %match IP PORT VARIABLE EVERY
	command := fmt.Sprintf("%%match %s %d %s %d\n", dc.localIP, dc.callbackPort, variable, every)
	if _, err := conn.Write([]byte(command)); err != nil {
		return fmt.Errorf("write failed: %v", err)
	}
	
	// Read response
	scanner := bufio.NewScanner(conn)
	if scanner.Scan() {
		response := strings.TrimSpace(scanner.Text())
		
		// Store subscription
		dc.mutex.Lock()
		dc.subscriptions[variable] = every
		dc.mutex.Unlock()
		
		if dc.verbose {
			log.Printf("📡 Subscribed to %s (every %d): %s", variable, every, response)
		}
		return nil
	}
	
	return fmt.Errorf("no subscription response received")
}

// Touch sends a touch command to refresh a variable
func (dc *DservClient) Touch(variable string) error {
	conn, err := dc.connectToDserv()
	if err != nil {
		return fmt.Errorf("connection failed: %v", err)
	}
	defer conn.Close()
	
	// Send touch command
	command := fmt.Sprintf("%%touch %s\n", variable)
	if _, err := conn.Write([]byte(command)); err != nil {
		return fmt.Errorf("write failed: %v", err)
	}
	
	if dc.verbose {
		log.Printf("👆 Touched variable: %s", variable)
	}
	return nil
}

// GetUpdateChannel returns the channel for receiving updates
func (dc *DservClient) GetUpdateChannel() <-chan DservUpdate {
	return dc.updateChannel
}

// GetStats returns client statistics
func (dc *DservClient) GetStats() map[string]interface{} {
	dc.mutex.RLock()
	defer dc.mutex.RUnlock()
	
	return map[string]interface{}{
		"connected":       dc.connected,
		"callback_port":   dc.callbackPort,
		"registration_id": dc.registrationID,
		"local_ip":        dc.localIP,
		"dserv_address":   dc.address,
		"subscriptions":   len(dc.subscriptions),
		"total_updates":   dc.totalUpdates,
		"last_update":     dc.lastUpdate,
		"json_mode":       dc.jsonMode,
	}
}

// IsHealthy returns whether the client is functioning properly
func (dc *DservClient) IsHealthy() bool {
	dc.mutex.RLock()
	defer dc.mutex.RUnlock()
	
	// Consider healthy if connected and received updates recently
	if !dc.connected {
		return false
	}
	
	// If we have subscriptions, we should get updates
	if len(dc.subscriptions) > 0 {
		return time.Since(dc.lastUpdate) < 1*time.Minute
	}
	
	// If no subscriptions, consider healthy if connected
	return true
}

// Close shuts down the dserv client
func (dc *DservClient) Close() error {
	dc.mutex.Lock()
	defer dc.mutex.Unlock()
	
	dc.connected = false
	
	if dc.callbackServer != nil {
		dc.callbackServer.Close()
	}
	
	close(dc.updateChannel)
	
	if dc.verbose {
		log.Printf("🔌 Dserv client shut down")
	}
	
	return nil
}
