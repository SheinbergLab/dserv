package main

import (
	"bufio"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"
)

const (
	sendJSON = 2
)

// Datapoint represents a JSON datapoint update from dserv.
type Datapoint struct {
	Name      string          `json:"name"`
	Timestamp uint64          `json:"timestamp"`
	Dtype     uint32          `json:"dtype"`
	Data      json.RawMessage `json:"data"`
}

// runListen subscribes to datapoint updates and prints them as they arrive.
func runListen(cfg *Config, args []string) int {
	if len(args) == 0 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl listen <pattern> [pattern...]\n")
		fmt.Fprintf(os.Stderr, "\nPatterns support wildcards: * (any chars), ? (single char)\n")
		fmt.Fprintf(os.Stderr, "\nExamples:\n")
		fmt.Fprintf(os.Stderr, "  dservctl listen \"ess/*\"                Watch all ess datapoints\n")
		fmt.Fprintf(os.Stderr, "  dservctl listen \"ess/state\"            Watch a single datapoint\n")
		fmt.Fprintf(os.Stderr, "  dservctl listen \"ess/*\" \"print\"        Watch multiple patterns\n")
		fmt.Fprintf(os.Stderr, "  dservctl listen \"*\"                    Watch everything\n")
		return 2
	}

	// Start local TCP listener on random port
	listener, err := net.Listen("tcp4", ":0")
	if err != nil {
		PrintError("failed to start listener: %v", err)
		return 1
	}
	defer listener.Close()

	_, listenPort, _ := net.SplitHostPort(listener.Addr().String())

	// Determine our IP as seen by the dserv host
	localIP, err := GetLocalIP(cfg.Host)
	if err != nil {
		PrintError("cannot reach dserv text port on %s:%d: %v", cfg.Host, DservTextPort, err)
		return 1
	}

	if cfg.Verbose {
		fmt.Fprintf(os.Stderr, "Listening on %s:%s\n", localIP, listenPort)
	}

	// Register with dserv for JSON updates
	regCmd := fmt.Sprintf("%%reg %s %s %d", localIP, listenPort, sendJSON)
	_, err = SendText(cfg.Host, DservTextPort, regCmd)
	if err != nil {
		PrintError("failed to register: %v", err)
		return 1
	}

	// Add match patterns
	for _, pattern := range args {
		matchCmd := fmt.Sprintf("%%match %s %s %s 1", localIP, listenPort, pattern)
		_, err = SendText(cfg.Host, DservTextPort, matchCmd)
		if err != nil {
			PrintError("failed to add match %q: %v", pattern, err)
			return 1
		}
		if cfg.Verbose {
			fmt.Fprintf(os.Stderr, "Subscribed to %q\n", pattern)
		}
	}

	fmt.Fprintf(os.Stderr, "Listening for datapoints on %s (Ctrl-C to stop)...\n", cfg.Host)

	// Handle Ctrl-C gracefully
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

	// Accept connections and print datapoints
	doneCh := make(chan struct{})
	go func() {
		defer close(doneCh)
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go handleDatapointConn(conn, cfg.JSON)
		}
	}()

	// Wait for signal
	<-sigCh
	fmt.Fprintf(os.Stderr, "\nStopped.\n")
	listener.Close()
	return 0
}

// handleDatapointConn reads newline-terminated JSON datapoints from a connection.
func handleDatapointConn(conn net.Conn, jsonOutput bool) {
	defer conn.Close()
	reader := bufio.NewReader(conn)

	for {
		line, err := reader.ReadBytes('\n')
		if err != nil {
			return
		}

		line = []byte(strings.TrimSpace(string(line)))
		if len(line) == 0 {
			continue
		}

		if jsonOutput {
			// Pass through raw JSON
			fmt.Println(string(line))
			continue
		}

		// Parse and format nicely
		var dp Datapoint
		if err := json.Unmarshal(line, &dp); err != nil {
			fmt.Println(string(line))
			continue
		}

		formatDatapoint(dp)
	}
}

// formatDatapoint prints a human-readable datapoint update.
func formatDatapoint(dp Datapoint) {
	ts := time.UnixMicro(int64(dp.Timestamp))
	timeStr := ts.Format("15:04:05.000")

	// Try to decode the data field
	dataStr := decodeData(dp.Dtype, dp.Data)

	fmt.Printf("%s  %-30s  %s\n", timeStr, dp.Name, dataStr)
}

// decodeData extracts a human-readable string from the data field.
func decodeData(dtype uint32, raw json.RawMessage) string {
	if len(raw) == 0 {
		return ""
	}

	// Try as string first (base64 encoded data comes as a quoted string)
	var s string
	if err := json.Unmarshal(raw, &s); err == nil {
		switch dtype {
		case 1: // STRING
			// base64-encoded string - decode it
			if decoded, err := base64.StdEncoding.DecodeString(s); err == nil {
				return string(decoded)
			}
			return s
		default:
			// For other types, try base64 decode, fall back to raw
			if decoded, err := base64.StdEncoding.DecodeString(s); err == nil {
				return string(decoded)
			}
			return s
		}
	}

	// Try as number
	var f float64
	if err := json.Unmarshal(raw, &f); err == nil {
		return fmt.Sprintf("%g", f)
	}

	// Fall back to raw JSON
	return string(raw)
}
