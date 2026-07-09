package main

import (
	"bufio"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"math"
	"net"
	"os"
	"os/signal"
	"strconv"
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

func listenUsage() {
	fmt.Fprintf(os.Stderr, "Usage: dservctl listen [options] <pattern> [pattern...]\n")
	fmt.Fprintf(os.Stderr, "\nOptions:\n")
	fmt.Fprintf(os.Stderr, "  --jsonl        One decoded JSON object per line: {t,name,dtype,value}\n")
	fmt.Fprintf(os.Stderr, "                 (values are native types -- no base64 step for consumers)\n")
	fmt.Fprintf(os.Stderr, "  --for <dur>    Stop after a duration (e.g. 30s, 10m)\n")
	fmt.Fprintf(os.Stderr, "  --count <n>    Stop after n datapoints\n")
	fmt.Fprintf(os.Stderr, "\nPatterns support wildcards: * (any chars), ? (single char)\n")
	fmt.Fprintf(os.Stderr, "\nExamples:\n")
	fmt.Fprintf(os.Stderr, "  dservctl listen \"ess/*\"                Watch all ess datapoints\n")
	fmt.Fprintf(os.Stderr, "  dservctl listen --jsonl --for 10m \\\n")
	fmt.Fprintf(os.Stderr, "      \"extio/*/state/sync/*\" \"ess/in_obs\" > sync_run.jsonl\n")
	fmt.Fprintf(os.Stderr, "  dservctl --json listen \"ess/*\"         Raw dserv JSON passthrough\n")
}

// runListen subscribes to datapoint updates and prints them as they arrive.
func runListen(cfg *Config, args []string) int {
	var (
		jsonl    bool
		forDur   time.Duration
		maxCount int64
		patterns []string
	)
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--jsonl":
			jsonl = true
		case "--for":
			i++
			if i >= len(args) {
				listenUsage()
				return 2
			}
			d, err := time.ParseDuration(args[i])
			if err != nil || d <= 0 {
				PrintError("bad --for duration %q (want e.g. 30s, 10m)", args[i])
				return 2
			}
			forDur = d
		case "--count":
			i++
			if i >= len(args) {
				listenUsage()
				return 2
			}
			n, err := strconv.ParseInt(args[i], 10, 64)
			if err != nil || n <= 0 {
				PrintError("bad --count %q", args[i])
				return 2
			}
			maxCount = n
		default:
			if strings.HasPrefix(args[i], "--") {
				PrintError("unknown listen option %q", args[i])
				listenUsage()
				return 2
			}
			patterns = append(patterns, args[i])
		}
	}
	if len(patterns) == 0 {
		listenUsage()
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

	// Unregister on every exit path so dserv stops pushing to a dead port
	// (otherwise the registration lingers until its next send fails).
	defer SendText(cfg.Host, DservTextPort,
		fmt.Sprintf("%%unreg %s %s", localIP, listenPort))

	// Add match patterns
	for _, pattern := range patterns {
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

	bounds := ""
	if forDur > 0 {
		bounds = fmt.Sprintf(" for %v", forDur)
	}
	fmt.Fprintf(os.Stderr, "Listening for datapoints on %s%s (Ctrl-C to stop)...\n",
		cfg.Host, bounds)

	// Handle Ctrl-C gracefully
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

	// Connections feed raw lines into one channel; the main loop prints,
	// counts, and enforces the bounds (single writer -> ordered output).
	lineCh := make(chan []byte, 256)
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go feedDatapointConn(conn, lineCh)
		}
	}()

	var timeoutCh <-chan time.Time
	if forDur > 0 {
		timeoutCh = time.After(forDur)
	}

	var n int64
	for {
		select {
		case <-sigCh:
			fmt.Fprintf(os.Stderr, "\nStopped (%d datapoints).\n", n)
			return 0
		case <-timeoutCh:
			fmt.Fprintf(os.Stderr, "Done: %v elapsed (%d datapoints).\n", forDur, n)
			return 0
		case line := <-lineCh:
			emitDatapoint(line, cfg.JSON, jsonl)
			n++
			if maxCount > 0 && n >= maxCount {
				fmt.Fprintf(os.Stderr, "Done: %d datapoints.\n", n)
				return 0
			}
		}
	}
}

// feedDatapointConn reads newline-terminated JSON datapoints and forwards
// the raw lines to the printer loop.
func feedDatapointConn(conn net.Conn, lineCh chan<- []byte) {
	defer conn.Close()
	reader := bufio.NewReader(conn)
	for {
		line, err := reader.ReadBytes('\n')
		if err != nil {
			return
		}
		line = []byte(strings.TrimSpace(string(line)))
		if len(line) > 0 {
			lineCh <- line
		}
	}
}

// emitDatapoint prints one datapoint in the selected format.
func emitDatapoint(line []byte, rawJSON, jsonl bool) {
	if jsonl {
		var dp Datapoint
		if err := json.Unmarshal(line, &dp); err != nil {
			fmt.Println(string(line)) // not a datapoint: pass through
			return
		}
		rec := struct {
			T     uint64      `json:"t"`
			Name  string      `json:"name"`
			Dtype uint32      `json:"dtype"`
			Value interface{} `json:"value"`
		}{dp.Timestamp, dp.Name, dp.Dtype, decodeValue(dp.Data)}
		out, err := json.Marshal(rec)
		if err != nil {
			fmt.Println(string(line))
			return
		}
		fmt.Println(string(out))
		return
	}
	if rawJSON {
		fmt.Println(string(line)) // dserv's JSON, data still base64
		return
	}
	var dp Datapoint
	if err := json.Unmarshal(line, &dp); err != nil {
		fmt.Println(string(line))
		return
	}
	formatDatapoint(dp)
}

// formatDatapoint prints a human-readable datapoint update.
func formatDatapoint(dp Datapoint) {
	ts := time.UnixMicro(int64(dp.Timestamp))
	timeStr := ts.Format("15:04:05.000")

	// Try to decode the data field
	dataStr := decodeData(dp.Dtype, dp.Data)

	fmt.Printf("%s  %-30s  %s\n", timeStr, dp.Name, dataStr)
}

// decodeValue turns a dserv JSON data field into a native Go value for JSONL
// output: numbers stay numbers (int64 when integral), strings pass through a
// base64 layer, and raw little-endian 2/4/8-byte payloads become integers.
// Anything unrecognized survives as-is rather than erroring.
func decodeValue(raw json.RawMessage) interface{} {
	if len(raw) == 0 {
		return nil
	}
	var f float64
	if err := json.Unmarshal(raw, &f); err == nil {
		if f == math.Trunc(f) && math.Abs(f) < (1<<62) {
			return int64(f)
		}
		return f
	}
	var s string
	if err := json.Unmarshal(raw, &s); err != nil {
		return raw // array/object: pass through untouched
	}
	b, err := base64.StdEncoding.DecodeString(s)
	if err != nil {
		return numOrString(s) // plain (non-base64) string
	}
	if printable(b) {
		return numOrString(strings.TrimRight(string(b), "\x00"))
	}
	switch len(b) {
	case 8:
		return int64(binary.LittleEndian.Uint64(b))
	case 4:
		return int32(binary.LittleEndian.Uint32(b))
	case 2:
		return int16(binary.LittleEndian.Uint16(b))
	}
	return s // opaque binary: keep the base64 form
}

func numOrString(s string) interface{} {
	t := strings.TrimSpace(s)
	if v, err := strconv.ParseInt(t, 10, 64); err == nil {
		return v
	}
	if v, err := strconv.ParseFloat(t, 64); err == nil {
		return v
	}
	return s
}

func printable(b []byte) bool {
	if len(b) == 0 {
		return false
	}
	for _, c := range b {
		if c == 0 { // allow trailing NULs only
			continue
		}
		if (c < 0x20 || c > 0x7e) && c != '\t' && c != '\n' && c != '\r' {
			return false
		}
	}
	return true
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
