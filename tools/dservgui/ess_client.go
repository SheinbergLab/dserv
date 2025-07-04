package main

import (
	"bufio"
	"fmt"
	"net"
	"strings"
	"time"
)

// EssClient handles communication with the ESS system
type EssClient struct {
	address string
}

// NewEssClient creates a new ESS client
func NewEssClient(address string) *EssClient {
	return &EssClient{
		address: address,
	}
}

// Eval executes a command/script on the ESS system
func (ec *EssClient) Eval(script string) (string, error) {
	// Connect to ESS
	conn, err := net.DialTimeout("tcp", ec.address, 5*time.Second)
	if err != nil {
		return "", fmt.Errorf("failed to connect to ESS at %s: %v", ec.address, err)
	}
	defer conn.Close()
	
	// Set deadline for the operation
	conn.SetDeadline(time.Now().Add(10 * time.Second))
	
	// Send the script/command
	if _, err := conn.Write([]byte(script + "\n")); err != nil {
		return "", fmt.Errorf("failed to send command to ESS: %v", err)
	}
	
	// Read the response
	scanner := bufio.NewScanner(conn)
	if scanner.Scan() {
		response := strings.TrimSpace(scanner.Text())
		return response, nil
	}
	
	// Check for scanner errors
	if err := scanner.Err(); err != nil {
		return "", fmt.Errorf("failed to read response from ESS: %v", err)
	}
	
	return "", fmt.Errorf("no response received from ESS")
}

// LoadSystem loads a specific system configuration
func (ec *EssClient) LoadSystem(system string) (string, error) {
	script := fmt.Sprintf(`ess::load_system "%s"`, system)
	return ec.Eval(script)
}

// LoadSystemProtocol loads a system with a specific protocol
func (ec *EssClient) LoadSystemProtocol(system, protocol string) (string, error) {
	script := fmt.Sprintf(`ess::load_system "%s" "%s"`, system, protocol)
	return ec.Eval(script)
}

// LoadSystemProtocolVariant loads a system with protocol and variant
func (ec *EssClient) LoadSystemProtocolVariant(system, protocol, variant string) (string, error) {
	script := fmt.Sprintf(`ess::load_system "%s" "%s" "%s"`, system, protocol, variant)
	return ec.Eval(script)
}

// GetVersion gets the ESS version
func (ec *EssClient) GetVersion() (string, error) {
	return ec.Eval("return $ess_version")
}

// GetHostname gets the system hostname
func (ec *EssClient) GetHostname() (string, error) {
	return ec.Eval("return [info hostname]")
}

// GetCurrentTime gets the current time from ESS
func (ec *EssClient) GetCurrentTime() (string, error) {
	return ec.Eval("return [clock format [clock seconds]]")
}

// TestConnection tests the connection to ESS
func (ec *EssClient) TestConnection() error {
	_, err := ec.Eval("return [expr {2 + 2}]")
	return err
}

// IsHealthy checks if ESS is responding properly
func (ec *EssClient) IsHealthy() bool {
	err := ec.TestConnection()
	return err == nil
}
