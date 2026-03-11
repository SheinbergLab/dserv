package main

import (
	"fmt"
	"os"
	"strings"
)

const version = "0.1.0"

// Command represents a CLI subcommand
type Command struct {
	Name        string
	Description string
	Run         func(cfg *Config, args []string) int
}

var commands []Command

func init() {
	commands = []Command{
		{"shell", "Interactive REPL with tab completion", runShell},
		{"stim", "Send command to STIM (separate process, port 4612)", runStim},
		{"get", "Get a datapoint value (dservGet)", runGet},
		{"set", "Set a datapoint value (dservSet)", runSet},
		{"touch", "Touch a datapoint (notify subscribers)", runTouch},
		{"clear", "Clear datapoint(s) (dservClear)", runClear},
		{"status", "Show agent and dserv status", runStatus},
		{"mesh", "List mesh nodes", runMesh},
		{"service", "Control dserv service (start/stop/restart)", runService},
		{"components", "List or install components", runComponents},
		{"logs", "View service logs", runLogs},
		{"systems", "List ESS systems in workgroup", runSystems},
		{"scripts", "List scripts for a system", runScripts},
		{"script", "Get or save a script", runScript},
		{"history", "Show script version history", runHistory},
		{"templates", "List or add templates", runTemplates},
		{"sandbox", "Manage sandboxes (create/promote/sync/delete)", runSandbox},
		{"export", "Export workgroup or system as ZIP", runExport},
		{"sync", "Sync scripts with local directory", runSync},
		{"version", "Show version", runVersion},
		{"help", "Show help", nil},
	}
}

func printUsage() {
	fmt.Fprintf(os.Stderr, "dservctl %s - CLI for dserv + dserv-agent\n\n", version)
	fmt.Fprintf(os.Stderr, "Usage:\n")
	fmt.Fprintf(os.Stderr, "  dservctl [flags] [command] [args...]\n")
	fmt.Fprintf(os.Stderr, "  dservctl [flags] <interpreter> [command]     Send to dserv interpreter\n")
	fmt.Fprintf(os.Stderr, "  dservctl [flags] -c \"command\"                Send to dserv main interpreter\n\n")
	fmt.Fprintf(os.Stderr, "Global flags:\n")
	fmt.Fprintf(os.Stderr, "  -H, --host HOST        Target host (default: localhost, env: DSERV_HOST)\n")
	fmt.Fprintf(os.Stderr, "  --agent-port PORT       Agent HTTP port (default: 80, env: DSERV_AGENT_PORT)\n")
	fmt.Fprintf(os.Stderr, "  -t, --token TOKEN       Bearer token (env: DSERV_AGENT_TOKEN)\n")
	fmt.Fprintf(os.Stderr, "  -w, --workgroup WG      Default workgroup (env: DSERV_WORKGROUP)\n")
	fmt.Fprintf(os.Stderr, "  -u, --user USER         Username for registry ops (env: DSERV_USER)\n")
	fmt.Fprintf(os.Stderr, "  --json                  Output JSON\n")
	fmt.Fprintf(os.Stderr, "  --verbose               Verbose output\n\n")
	fmt.Fprintf(os.Stderr, "Commands:\n")
	maxLen := 0
	for _, c := range commands {
		if len(c.Name) > maxLen {
			maxLen = len(c.Name)
		}
	}
	for _, c := range commands {
		fmt.Fprintf(os.Stderr, "  %-*s  %s\n", maxLen, c.Name, c.Description)
	}
	fmt.Fprintf(os.Stderr, "\nInterpreter commands (discovered dynamically from dserv):\n")
	fmt.Fprintf(os.Stderr, "  dservctl ess \"command\"   Send to ESS subprocess\n")
	fmt.Fprintf(os.Stderr, "  dservctl db \"command\"    Send to db subprocess\n")
	fmt.Fprintf(os.Stderr, "  dservctl stim \"command\"  Send to STIM (direct, port 4612)\n\n")
	fmt.Fprintf(os.Stderr, "Examples:\n")
	fmt.Fprintf(os.Stderr, "  dservctl -c \"expr 2+2\"                       Direct to dserv interpreter\n")
	fmt.Fprintf(os.Stderr, "  dservctl ess \"ess::status\"                    Send to ESS subprocess\n")
	fmt.Fprintf(os.Stderr, "  echo \"expr 5*5\" | dservctl ess               Pipe to ESS\n")
	fmt.Fprintf(os.Stderr, "  dservctl shell                                Interactive REPL\n")
	fmt.Fprintf(os.Stderr, "  dservctl shell -s ess                         REPL targeting ESS\n")
	fmt.Fprintf(os.Stderr, "  dservctl get ess/status                       Get a datapoint value\n")
	fmt.Fprintf(os.Stderr, "  dservctl set ess/state running                Set a datapoint value\n")
	fmt.Fprintf(os.Stderr, "  dservctl touch ess/em_pos                    Touch (notify subscribers)\n")
	fmt.Fprintf(os.Stderr, "  dservctl script get sys proto type > f.tcl    Download script\n")
	fmt.Fprintf(os.Stderr, "  dservctl sandbox create sys mybranch          Create sandbox\n")
}

func main() {
	cfg := LoadConfig()

	// Parse global flags from os.Args
	args := os.Args[1:]
	args = cfg.ParseFlags(args)

	if len(args) == 0 {
		// No command: if stdin is piped, read commands; if -c was set, already handled; otherwise shell
		if cfg.Command != "" {
			// -c flag: send to dserv main interpreter
			os.Exit(runDirectCommand(cfg, cfg.Command))
		}
		if isStdinPiped() {
			os.Exit(runStdinCommands(cfg, ""))
		}
		// Interactive: launch shell
		os.Exit(runShell(cfg, nil))
	}

	cmdName := args[0]
	cmdArgs := args[1:]

	// Check for help
	if cmdName == "help" || cmdName == "--help" || cmdName == "-h" {
		printUsage()
		os.Exit(0)
	}

	// Check if it's a known command
	for _, c := range commands {
		if c.Name == cmdName {
			if c.Run == nil {
				printUsage()
				os.Exit(0)
			}
			os.Exit(c.Run(cfg, cmdArgs))
		}
	}

	// Not a known command — treat as interpreter name
	// If there's a following argument, send it as a command
	// If stdin is piped, read commands from stdin
	// Otherwise, launch interactive shell for that interpreter
	interp := cmdName
	if len(cmdArgs) > 0 {
		// Remaining args are joined as the command
		cmd := strings.Join(cmdArgs, " ")
		os.Exit(runInterpCommand(cfg, interp, cmd))
	}
	if isStdinPiped() {
		os.Exit(runStdinCommands(cfg, interp))
	}
	// Interactive shell targeting this interpreter
	os.Exit(runShell(cfg, []string{"-s", interp}))
}

func isStdinPiped() bool {
	fi, err := os.Stdin.Stat()
	if err != nil {
		return false
	}
	return (fi.Mode() & os.ModeCharDevice) == 0
}

// runVersion prints version info
func runVersion(cfg *Config, args []string) int {
	fmt.Printf("dservctl %s\n", version)
	return 0
}
