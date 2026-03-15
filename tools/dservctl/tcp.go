package main

import (
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"strings"
	"sync"
	"time"
)

const (
	DservPort      = 2560
	DservTextPort  = 4620
	STIMPort       = 4612
	ConnectTimeout = 5 * time.Second
	ReadTimeout    = 30 * time.Second
	ResolveTimeout = 15 * time.Second
)

var (
	resolveCache   = map[string]string{}
	resolveCacheMu sync.Mutex
)

// resolveHost resolves a hostname once and caches the result.
// Returns the IP address, or the original string if it's already an IP.
// Prefers IPv4 addresses since dserv typically binds to 0.0.0.0.
func resolveHost(host string) string {
	if net.ParseIP(host) != nil {
		return host
	}

	resolveCacheMu.Lock()
	defer resolveCacheMu.Unlock()

	if cached, ok := resolveCache[host]; ok {
		return cached
	}

	addrs, err := net.LookupHost(host)
	if err != nil || len(addrs) == 0 {
		return host // fall back to original, let DialTimeout report the error
	}

	// Prefer IPv4 address if available
	chosen := addrs[0]
	for _, a := range addrs {
		if net.ParseIP(a) != nil && net.ParseIP(a).To4() != nil {
			chosen = a
			break
		}
	}

	resolveCache[host] = chosen
	return chosen
}

// hostPort formats a host:port address, wrapping IPv6 addresses in brackets.
func hostPort(host string, port int) string {
	if strings.Contains(host, ":") {
		return fmt.Sprintf("[%s]:%d", host, port)
	}
	return fmt.Sprintf("%s:%d", host, port)
}

// SendBinary sends a command using the binary message protocol (4-byte length prefix).
// Used for dserv (port 2560) and STIM (port 4612).
func SendBinary(host string, port int, msg string) (string, error) {
	resolved := resolveHost(host)
	addr := hostPort(resolved, port)
	conn, err := net.DialTimeout("tcp", addr, ConnectTimeout)
	if err != nil {
		return "", fmt.Errorf("connection to %s failed: %w", addr, err)
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(ReadTimeout))

	msgBytes := []byte(msg)

	// Send: 4-byte big-endian length + body
	lenBuf := make([]byte, 4)
	binary.BigEndian.PutUint32(lenBuf, uint32(len(msgBytes)))
	if _, err := conn.Write(lenBuf); err != nil {
		return "", fmt.Errorf("write length failed: %w", err)
	}
	if _, err := conn.Write(msgBytes); err != nil {
		return "", fmt.Errorf("write body failed: %w", err)
	}

	// Receive: 4-byte big-endian length + body
	var respLen uint32
	if err := binary.Read(conn, binary.BigEndian, &respLen); err != nil {
		return "", fmt.Errorf("read response length failed: %w", err)
	}

	respBuf := make([]byte, respLen)
	if _, err := io.ReadFull(conn, respBuf); err != nil {
		return "", fmt.Errorf("read response body failed: %w", err)
	}

	return string(respBuf), nil
}

// SendToDserv sends a command directly to the dserv main interpreter (port 2560).
func SendToDserv(host string, cmd string) (string, error) {
	return SendBinary(host, DservPort, cmd)
}

// SendToInterp sends a command to a dserv subprocess via `send <interp> {cmd}`.
// Routes through the dserv main interpreter on port 2560.
func SendToInterp(host string, interp string, cmd string) (string, error) {
	wrapped := fmt.Sprintf("send %s {%s}", interp, cmd)
	return SendBinary(host, DservPort, wrapped)
}

// SendToSTIM sends a command directly to the STIM process (port 4612).
func SendToSTIM(host string, cmd string) (string, error) {
	return SendBinary(host, STIMPort, cmd)
}

// Send dispatches a command to the appropriate target.
// If interp is empty, sends directly to dserv.
// If interp is "stim", sends directly to STIM port.
// Otherwise, wraps with send command.
func Send(host string, interp string, cmd string) (string, error) {
	switch interp {
	case "", "dserv":
		return SendToDserv(host, cmd)
	case "stim":
		return SendToSTIM(host, cmd)
	default:
		return SendToInterp(host, interp, cmd)
	}
}

// DiscoverInterpreters queries the dserv/interps datapoint to get available subprocesses.
func DiscoverInterpreters(host string) ([]string, error) {
	resp, err := SendToDserv(host, "dservGet dserv/interps")
	if err != nil {
		return nil, err
	}
	resp = strings.TrimSpace(resp)
	if resp == "" {
		return nil, nil
	}
	interps := strings.Fields(resp)
	return interps, nil
}

// SendText sends a command using the text protocol (newline-terminated) on port 4620.
// Used for %reg and %match commands.
func SendText(host string, port int, cmd string) (string, error) {
	resolved := resolveHost(host)
	addr := hostPort(resolved, port)
	conn, err := net.DialTimeout("tcp", addr, ConnectTimeout)
	if err != nil {
		return "", fmt.Errorf("connection to %s failed: %w", addr, err)
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(ReadTimeout))

	fmt.Fprintf(conn, "%s\n", cmd)

	buf := make([]byte, 4096)
	n, _ := conn.Read(buf)
	return strings.TrimSpace(string(buf[:n])), nil
}

// GetLocalIP determines our IP address as seen by the dserv host.
func GetLocalIP(host string) (string, error) {
	resolved := resolveHost(host)
	addr := hostPort(resolved, DservTextPort)
	conn, err := net.DialTimeout("tcp", addr, ConnectTimeout)
	if err != nil {
		return "", err
	}
	defer conn.Close()
	localAddr := conn.LocalAddr().String()
	ip, _, _ := net.SplitHostPort(localAddr)
	return ip, nil
}

// TestConnection verifies dserv is reachable on port 2560.
func TestConnection(host string) error {
	_, err := SendToDserv(host, "expr {2+2}")
	return err
}
