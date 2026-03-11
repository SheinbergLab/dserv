package main

import (
	"fmt"
	"os"
	"strings"
)

// runGet retrieves a datapoint value via dservGet.
func runGet(cfg *Config, args []string) int {
	if len(args) == 0 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl get <datapoint>\n")
		return 2
	}

	cmd := "dservGet " + args[0]
	resp, err := SendToDserv(cfg.Host, cmd)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	output, isError := ProcessResponse(resp)
	if isError {
		PrintError("%s", output)
		return 1
	}
	fmt.Println(output)
	return 0
}

// runSet sets a datapoint value via dservSet.
func runSet(cfg *Config, args []string) int {
	if len(args) < 2 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl set <datapoint> <value>\n")
		return 2
	}

	value := strings.Join(args[1:], " ")
	cmd := fmt.Sprintf("dservSet %s {%s}", args[0], value)
	resp, err := SendToDserv(cfg.Host, cmd)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	output, isError := ProcessResponse(resp)
	if isError {
		PrintError("%s", output)
		return 1
	}
	if cfg.Verbose && output != "" {
		fmt.Println(output)
	}
	return 0
}

// runTouch triggers subscriber notification for a datapoint via dservTouch.
func runTouch(cfg *Config, args []string) int {
	if len(args) == 0 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl touch <datapoint>\n")
		return 2
	}

	cmd := "dservTouch " + args[0]
	resp, err := SendToDserv(cfg.Host, cmd)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	output, isError := ProcessResponse(resp)
	if isError {
		PrintError("%s", output)
		return 1
	}
	if cfg.Verbose && output != "" {
		fmt.Println(output)
	}
	return 0
}

// runClear clears datapoint(s) via dservClear.
func runClear(cfg *Config, args []string) int {
	var cmd string
	if len(args) == 0 {
		cmd = "dservClear"
	} else {
		cmd = "dservClear " + strings.Join(args, " ")
	}

	resp, err := SendToDserv(cfg.Host, cmd)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	output, isError := ProcessResponse(resp)
	if isError {
		PrintError("%s", output)
		return 1
	}
	if cfg.Verbose && output != "" {
		fmt.Println(output)
	}
	return 0
}
