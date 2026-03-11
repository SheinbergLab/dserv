package main

import (
	"encoding/binary"
	"io"
	"net"
	"testing"
	"time"
)

// startMockBinaryServer creates a TCP server that echoes using the binary protocol.
func startMockBinaryServer(t *testing.T, response string) (int, func()) {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("failed to start mock server: %v", err)
	}

	port := listener.Addr().(*net.TCPAddr).Port

	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go func(c net.Conn) {
				defer c.Close()

				// Read incoming binary message
				var msgLen uint32
				if err := binary.Read(c, binary.BigEndian, &msgLen); err != nil {
					return
				}
				buf := make([]byte, msgLen)
				if _, err := io.ReadFull(c, buf); err != nil {
					return
				}

				// Send response using binary protocol
				respBytes := []byte(response)
				respLen := make([]byte, 4)
				binary.BigEndian.PutUint32(respLen, uint32(len(respBytes)))
				c.Write(respLen)
				c.Write(respBytes)
			}(conn)
		}
	}()

	return port, func() { listener.Close() }
}

func TestSendBinary(t *testing.T) {
	port, cleanup := startMockBinaryServer(t, "4")
	defer cleanup()

	time.Sleep(10 * time.Millisecond) // Let server start

	resp, err := SendBinary("127.0.0.1", port, "expr 2+2")
	if err != nil {
		t.Fatalf("SendBinary failed: %v", err)
	}
	if resp != "4" {
		t.Errorf("SendBinary response = %q, want %q", resp, "4")
	}
}

func TestSendBinaryLargeMessage(t *testing.T) {
	expected := "hello world this is a longer response with more content"
	port, cleanup := startMockBinaryServer(t, expected)
	defer cleanup()

	time.Sleep(10 * time.Millisecond)

	resp, err := SendBinary("127.0.0.1", port, "some command")
	if err != nil {
		t.Fatalf("SendBinary failed: %v", err)
	}
	if resp != expected {
		t.Errorf("SendBinary response = %q, want %q", resp, expected)
	}
}

func TestSendBinaryConnectionRefused(t *testing.T) {
	_, err := SendBinary("127.0.0.1", 19999, "test")
	if err == nil {
		t.Error("expected error for connection refused, got nil")
	}
}

func TestProcessResponse(t *testing.T) {
	tests := []struct {
		input   string
		output  string
		isError bool
	}{
		{"hello", "hello", false},
		{"", "", false},
		{"!TCL_ERROR invalid command", "invalid command", true},
		{"!TCL_ERROR ", "", true},
		{"normal response", "normal response", false},
	}
	for _, tt := range tests {
		output, isError := ProcessResponse(tt.input)
		if output != tt.output || isError != tt.isError {
			t.Errorf("ProcessResponse(%q) = (%q, %v), want (%q, %v)",
				tt.input, output, isError, tt.output, tt.isError)
		}
	}
}
