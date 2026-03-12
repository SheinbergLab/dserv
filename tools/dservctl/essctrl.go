package main

import (
	"bufio"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// servicePort maps essctrl service names to their direct TCP ports.
// These match the port definitions in essctrl.c.
var servicePort = map[string]int{
	"ess":      2560, // dserv main interpreter (binary protocol)
	"legacy":   2570,
	"db":       2571,
	"dserv":    4620, // text protocol
	"vstream":  4630,
	"stim":     4612,
	"pg":       2572,
	"git":      2573,
	"openiris": 2574,
}

// isEssctrlMode checks if the binary was invoked as "essctrl" (e.g., via symlink).
func isEssctrlMode() bool {
	base := filepath.Base(os.Args[0])
	// Strip any extension (e.g., .exe on Windows)
	base = strings.TrimSuffix(base, filepath.Ext(base))
	return base == "essctrl"
}

// runEssctrl handles essctrl-compatible argument parsing and execution.
// Usage: essctrl [server] [-c command] [-s service] [-h]
func runEssctrl(cfg *Config) int {
	var command string
	var service string

	// Parse essctrl-style arguments
	args := os.Args[1:]
	i := 0
	for i < len(args) {
		switch args[i] {
		case "-h", "--help":
			printEssctrlUsage()
			return 0
		case "-c":
			if i+1 >= len(args) {
				fmt.Fprintf(os.Stderr, "Error: -c option requires a command\n")
				return 1
			}
			command = args[i+1]
			i += 2
		case "-s":
			if i+1 >= len(args) {
				fmt.Fprintf(os.Stderr, "Error: -s option requires a service name\n")
				return 1
			}
			service = args[i+1]
			i += 2
		default:
			if args[i][0] != '-' {
				// Positional arg = server hostname
				cfg.Host = args[i]
				i++
			} else {
				fmt.Fprintf(os.Stderr, "Error: Unknown option %s\n", args[i])
				printEssctrlUsage()
				return 1
			}
		}
	}

	// Default service is "ess" (port 2560)
	if service == "" {
		service = "ess"
	}

	port, ok := servicePort[service]
	if !ok {
		fmt.Fprintf(os.Stderr, "Error: Unknown service '%s'\n", service)
		fmt.Fprintf(os.Stderr, "Valid services: ess, legacy, db, dserv, vstream, stim, pg, git, openiris\n")
		return 1
	}

	// Execute single command
	if command != "" {
		return essctrlSend(cfg.Host, port, service, command)
	}

	// Read commands from stdin
	if isStdinPiped() {
		return essctrlStdin(cfg.Host, port, service)
	}

	// No command, no stdin — print usage (no interactive mode)
	printEssctrlUsage()
	return 0
}

// essctrlSend sends a single command via the appropriate protocol.
func essctrlSend(host string, port int, service, cmd string) int {
	var resp string
	var err error

	if service == "dserv" {
		// Text protocol (newline-terminated) for dserv service on port 4620
		resp, err = SendText(host, port, cmd)
	} else {
		// Binary protocol (4-byte length prefix) for all others
		resp, err = SendBinary(host, port, cmd)
	}

	if err != nil {
		PrintError("%v", err)
		return 1
	}
	if PrintResponse(resp, false) {
		return 1
	}
	return 0
}

// essctrlStdin reads commands from stdin and sends them.
func essctrlStdin(host string, port int, service string) int {
	scanner := bufio.NewScanner(os.Stdin)
	anyErrors := false
	processed := 0

	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" {
			continue
		}

		var resp string
		var err error
		if service == "dserv" {
			resp, err = SendText(host, port, line)
		} else {
			resp, err = SendBinary(host, port, line)
		}

		if err != nil {
			PrintError("%v", err)
			anyErrors = true
			continue
		}
		if PrintResponse(resp, false) {
			anyErrors = true
		}
		processed++
	}

	if err := scanner.Err(); err != nil {
		PrintError("reading stdin: %v", err)
		return 1
	}
	if processed == 0 {
		return 1
	}
	if anyErrors {
		return 1
	}
	return 0
}

func printEssctrlUsage() {
	fmt.Fprintf(os.Stderr, "essctrl (via dservctl %s) - ESS control client\n\n", version)
	fmt.Fprintf(os.Stderr, "Usage: essctrl [server] [options]\n")
	fmt.Fprintf(os.Stderr, "  server          Server address (default: localhost)\n")
	fmt.Fprintf(os.Stderr, "  -c command      Execute command and exit\n")
	fmt.Fprintf(os.Stderr, "  -s service      Target service (ess, legacy, db, dserv, vstream, stim, pg, git, openiris)\n")
	fmt.Fprintf(os.Stderr, "  -h              Show this help\n\n")
	fmt.Fprintf(os.Stderr, "Examples:\n")
	fmt.Fprintf(os.Stderr, "  essctrl -c \"return 100\"\n")
	fmt.Fprintf(os.Stderr, "  essctrl myserver -c \"ess::status\"\n")
	fmt.Fprintf(os.Stderr, "  essctrl myserver -s db -c \"SELECT * FROM users\"\n")
	fmt.Fprintf(os.Stderr, "  echo \"expr 5*5\" | essctrl myserver\n")
}
