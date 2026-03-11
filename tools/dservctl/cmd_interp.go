package main

import (
	"bufio"
	"fmt"
	"os"
	"strings"
)

// runDirectCommand sends a single command directly to the dserv main interpreter.
func runDirectCommand(cfg *Config, cmd string) int {
	resp, err := SendToDserv(cfg.Host, cmd)
	if err != nil {
		PrintError("%v", err)
		return 1
	}
	if PrintResponse(resp, false) {
		return 1
	}
	return 0
}

// runInterpCommand sends a single command to an interpreter (or dserv if interp is empty).
func runInterpCommand(cfg *Config, interp string, cmd string) int {
	resp, err := Send(cfg.Host, interp, cmd)
	if err != nil {
		PrintError("%v", err)
		return 1
	}
	if PrintResponse(resp, false) {
		return 1
	}
	return 0
}

// runStdinCommands reads commands from stdin and sends them to an interpreter.
func runStdinCommands(cfg *Config, interp string) int {
	scanner := bufio.NewScanner(os.Stdin)
	anyErrors := false
	processed := 0

	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" {
			continue
		}

		resp, err := Send(cfg.Host, interp, line)
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

// runStim handles the stim command — direct connection to STIM port.
func runStim(cfg *Config, args []string) int {
	if len(args) == 0 {
		if isStdinPiped() {
			return runStdinCommands(cfg, "stim")
		}
		// Interactive shell targeting stim
		return runShell(cfg, []string{"-s", "stim"})
	}

	cmd := strings.Join(args, " ")

	// Handle -c flag for stim
	if args[0] == "-c" {
		if len(args) < 2 {
			fmt.Fprintf(os.Stderr, "Error: -c requires a command\n")
			return 2
		}
		cmd = strings.Join(args[1:], " ")
	}

	resp, err := SendToSTIM(cfg.Host, cmd)
	if err != nil {
		PrintError("%v", err)
		return 1
	}
	if PrintResponse(resp, false) {
		return 1
	}
	return 0
}
