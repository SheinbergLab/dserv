package main

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	"github.com/chzyer/readline"
)

// DservCompleter implements readline.AutoCompleter using the backend complete_token command.
type DservCompleter struct {
	host   string
	interp string // current target interpreter ("" = dserv direct)
}

func (c *DservCompleter) Do(line []rune, pos int) ([][]rune, int) {
	// Only complete at end of line
	input := string(line[:pos])
	if input == "" {
		return nil, 0
	}

	// Build completion command
	var completeCmd string
	if c.interp == "" || c.interp == "dserv" {
		completeCmd = fmt.Sprintf("complete_token {%s}", input)
	} else {
		completeCmd = fmt.Sprintf("send %s {complete_token {%s}}", c.interp, input)
	}

	// Send to dserv
	resp, err := SendToDserv(c.host, completeCmd)
	if err != nil || resp == "" {
		return nil, 0
	}

	// Parse Tcl list response
	matches := ParseTclList(resp)
	if len(matches) == 0 {
		return nil, 0
	}

	// Find the last token being completed
	lastSpace := strings.LastIndexAny(input, " \t")
	partial := input
	if lastSpace >= 0 {
		partial = input[lastSpace+1:]
	}

	// Build completion candidates (what readline needs to append)
	var candidates [][]rune
	for _, match := range matches {
		if strings.HasPrefix(match, partial) {
			suffix := match[len(partial):]
			candidates = append(candidates, []rune(suffix))
		} else {
			// If match doesn't start with partial, offer the full match
			candidates = append(candidates, []rune(match))
		}
	}

	return candidates, len(partial)
}

func runShell(cfg *Config, args []string) int {
	// Parse shell-specific flags
	interp := ""
	for i := 0; args != nil && i < len(args); i++ {
		switch args[i] {
		case "-s":
			if i+1 < len(args) {
				interp = args[i+1]
				i++
			}
		}
	}

	// Discover available interpreters
	availInterps, err := DiscoverInterpreters(cfg.Host)
	if err != nil && cfg.Verbose {
		fmt.Fprintf(os.Stderr, "Warning: could not discover interpreters: %v\n", err)
	}

	// Build prompt
	prompt := promptForInterp(interp)

	completer := &DservCompleter{
		host:   cfg.Host,
		interp: interp,
	}

	// History file
	historyFile := filepath.Join(homeDir(), ".dservctl_history")

	rl, err := readline.NewEx(&readline.Config{
		Prompt:            prompt,
		HistoryFile:       historyFile,
		AutoComplete:      completer,
		InterruptPrompt:   "^C",
		EOFPrompt:         "exit",
		HistorySearchFold: true,
	})
	if err != nil {
		PrintError("initializing readline: %v", err)
		return 1
	}
	defer rl.Close()

	if cfg.Verbose {
		fmt.Fprintf(os.Stderr, "Connected to %s:%d\n", cfg.Host, DservPort)
		if len(availInterps) > 0 {
			fmt.Fprintf(os.Stderr, "Available interpreters: %s\n", strings.Join(availInterps, ", "))
		}
		fmt.Fprintf(os.Stderr, "Type /help for commands, exit to quit\n")
	}

	for {
		line, err := rl.Readline()
		if err == readline.ErrInterrupt {
			continue
		}
		if err == io.EOF {
			break
		}

		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}

		if line == "exit" || line == "quit" {
			break
		}

		// Handle meta-commands starting with /
		if strings.HasPrefix(line, "/") {
			handled := handleMetaCommand(rl, completer, cfg, line, availInterps)
			if handled {
				continue
			}
		}

		// Send command to current interpreter
		resp, err := Send(cfg.Host, interp, line)
		if err != nil {
			PrintError("%v", err)
			continue
		}
		PrintResponse(resp, true)
	}

	return 0
}

// handleMetaCommand processes /commands in the shell. Returns true if handled.
func handleMetaCommand(rl *readline.Instance, completer *DservCompleter, cfg *Config, line string, availInterps []string) bool {
	parts := strings.SplitN(line, " ", 2)
	cmd := parts[0]

	switch cmd {
	case "/help":
		fmt.Println("Shell commands:")
		fmt.Println("  /dserv            Switch to dserv main interpreter")
		fmt.Println("  /<interp>         Switch to interpreter (e.g., /ess, /db, /pg)")
		fmt.Println("  /<interp> cmd     Send one command to interpreter without switching")
		fmt.Println("  /interps          List available interpreters")
		fmt.Println("  /help             Show this help")
		fmt.Println("  exit              Exit shell")
		return true

	case "/interps":
		interps, err := DiscoverInterpreters(cfg.Host)
		if err != nil {
			PrintError("discovering interpreters: %v", err)
		} else {
			fmt.Println("dserv (main)")
			for _, name := range interps {
				fmt.Printf("  %s\n", name)
			}
			fmt.Println("stim (direct, port 4612)")
		}
		return true

	case "/dserv":
		if len(parts) > 1 {
			// One-off command to dserv
			resp, err := SendToDserv(cfg.Host, parts[1])
			if err != nil {
				PrintError("%v", err)
			} else {
				PrintResponse(resp, true)
			}
		} else {
			completer.interp = ""
			rl.SetPrompt(promptForInterp(""))
		}
		return true
	}

	// Check if it's an interpreter switch: /ess, /db, etc.
	interpName := cmd[1:] // strip leading /
	if interpName == "stim" || isKnownInterp(interpName, availInterps) {
		if len(parts) > 1 {
			// One-off command
			resp, err := Send(cfg.Host, interpName, parts[1])
			if err != nil {
				PrintError("%v", err)
			} else {
				PrintResponse(resp, true)
			}
		} else {
			// Switch interpreter
			completer.interp = interpName
			rl.SetPrompt(promptForInterp(interpName))
		}
		return true
	}

	fmt.Printf("Unknown command: %s (use /help for available commands)\n", cmd)
	return true
}

func isKnownInterp(name string, interps []string) bool {
	for _, i := range interps {
		if i == name {
			return true
		}
	}
	return false
}

func promptForInterp(interp string) string {
	if interp == "" || interp == "dserv" {
		return "dserv> "
	}
	return interp + "> "
}

func homeDir() string {
	home, err := os.UserHomeDir()
	if err != nil {
		return "."
	}
	return home
}
