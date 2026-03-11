package main

import (
	"fmt"
	"os"
	"strings"
)

const (
	errorPrefix    = "!TCL_ERROR "
	errorPrefixLen = 11
	ansiRed        = "\x1b[31m"
	ansiReset      = "\x1b[0m"
)

// ProcessResponse checks for Tcl error prefix and returns the cleaned output and error status.
func ProcessResponse(response string) (string, bool) {
	if strings.HasPrefix(response, errorPrefix) {
		return response[errorPrefixLen:], true
	}
	return response, false
}

// PrintResponse prints a response, handling errors with color when appropriate.
func PrintResponse(response string, interactive bool) bool {
	if response == "" {
		return false
	}
	output, isError := ProcessResponse(response)
	if isError {
		if interactive && supportsColor() {
			fmt.Printf("%s%s%s\n", ansiRed, output, ansiReset)
		} else {
			fmt.Println(output)
		}
		return true
	}
	if strings.TrimSpace(output) != "" {
		fmt.Println(output)
	}
	return false
}

// supportsColor checks if the terminal supports ANSI colors.
func supportsColor() bool {
	if os.Getenv("NO_COLOR") != "" {
		return false
	}
	fi, err := os.Stdout.Stat()
	if err != nil {
		return false
	}
	if (fi.Mode() & os.ModeCharDevice) == 0 {
		return false // not a terminal
	}
	return os.Getenv("TERM") != ""
}

// PrintTable prints a formatted table with headers and rows.
func PrintTable(headers []string, rows [][]string) {
	if len(rows) == 0 {
		return
	}

	// Calculate column widths
	widths := make([]int, len(headers))
	for i, h := range headers {
		widths[i] = len(h)
	}
	for _, row := range rows {
		for i, cell := range row {
			if i < len(widths) && len(cell) > widths[i] {
				widths[i] = len(cell)
			}
		}
	}

	// Print header
	for i, h := range headers {
		if i > 0 {
			fmt.Print("  ")
		}
		fmt.Printf("%-*s", widths[i], h)
	}
	fmt.Println()

	// Print separator
	for i, w := range widths {
		if i > 0 {
			fmt.Print("  ")
		}
		fmt.Print(strings.Repeat("-", w))
	}
	fmt.Println()

	// Print rows
	for _, row := range rows {
		for i, cell := range row {
			if i >= len(widths) {
				break
			}
			if i > 0 {
				fmt.Print("  ")
			}
			fmt.Printf("%-*s", widths[i], cell)
		}
		fmt.Println()
	}
}

// PrintError prints an error message to stderr.
func PrintError(format string, args ...interface{}) {
	fmt.Fprintf(os.Stderr, "Error: "+format+"\n", args...)
}
