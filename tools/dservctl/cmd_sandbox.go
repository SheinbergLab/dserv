package main

import (
	"fmt"
	"os"
)

// runSandbox dispatches sandbox subcommands.
func runSandbox(cfg *Config, args []string) int {
	if len(args) == 0 {
		fmt.Fprintf(os.Stderr, "Usage:\n")
		fmt.Fprintf(os.Stderr, "  dservctl sandbox create <system> <version> [--from VERSION] [--comment MSG]\n")
		fmt.Fprintf(os.Stderr, "  dservctl sandbox promote <system> [--from VERSION] [--to VERSION] [--comment MSG]\n")
		fmt.Fprintf(os.Stderr, "  dservctl sandbox sync <system> <version> [--from VERSION]\n")
		fmt.Fprintf(os.Stderr, "  dservctl sandbox delete <system> <version>\n")
		fmt.Fprintf(os.Stderr, "  dservctl sandbox versions <system>\n")
		return 2
	}

	switch args[0] {
	case "create":
		return runSandboxCreate(cfg, args[1:])
	case "promote":
		return runSandboxPromote(cfg, args[1:])
	case "sync":
		return runSandboxSync(cfg, args[1:])
	case "delete":
		return runSandboxDelete(cfg, args[1:])
	case "versions":
		return runSandboxVersions(cfg, args[1:])
	default:
		PrintError("unknown sandbox subcommand %q (use create, promote, sync, delete, or versions)", args[0])
		return 2
	}
}

func runSandboxCreate(cfg *Config, args []string) int {
	if len(args) < 2 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl sandbox create <system> <version> [--from VERSION] [--comment MSG]\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}
	if !requireUser(cfg) {
		return 2
	}

	system := args[0]
	toVersion := args[1]
	fromVersion := "main"
	comment := ""

	for i := 2; i < len(args); i++ {
		switch args[i] {
		case "--from":
			if i+1 < len(args) {
				fromVersion = args[i+1]
				i++
			}
		case "--comment", "-m":
			if i+1 < len(args) {
				comment = args[i+1]
				i++
			}
		}
	}

	client := NewAgentClient(cfg)
	result, err := client.CreateSandbox(cfg.Workgroup, system, fromVersion, toVersion, cfg.User, comment)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	fmt.Printf("Created sandbox %q from %q for system %q (%d scripts copied)\n",
		toVersion, fromVersion, system, intVal(result, "scriptCount"))
	return 0
}

func runSandboxPromote(cfg *Config, args []string) int {
	if len(args) < 1 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl sandbox promote <system> [--from VERSION] [--to VERSION] [--comment MSG]\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}
	if !requireUser(cfg) {
		return 2
	}

	system := args[0]
	fromVersion := ""
	toVersion := "main"
	comment := ""

	for i := 1; i < len(args); i++ {
		switch args[i] {
		case "--from":
			if i+1 < len(args) {
				fromVersion = args[i+1]
				i++
			}
		case "--to":
			if i+1 < len(args) {
				toVersion = args[i+1]
				i++
			}
		case "--comment", "-m":
			if i+1 < len(args) {
				comment = args[i+1]
				i++
			}
		}
	}

	if fromVersion == "" {
		PrintError("--from VERSION is required (the sandbox version to promote)")
		return 2
	}

	client := NewAgentClient(cfg)
	result, err := client.PromoteSandbox(cfg.Workgroup, system, fromVersion, toVersion, cfg.User, comment)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	fmt.Printf("Promoted %q → %q for system %q (%d scripts)\n",
		fromVersion, toVersion, system, intVal(result, "scriptCount"))
	return 0
}

func runSandboxSync(cfg *Config, args []string) int {
	if len(args) < 2 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl sandbox sync <system> <version> [--from VERSION]\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}
	if !requireUser(cfg) {
		return 2
	}

	system := args[0]
	toVersion := args[1]
	fromVersion := "main"

	for i := 2; i < len(args); i++ {
		if args[i] == "--from" && i+1 < len(args) {
			fromVersion = args[i+1]
			i++
		}
	}

	client := NewAgentClient(cfg)
	result, err := client.SyncSandbox(cfg.Workgroup, system, fromVersion, toVersion, cfg.User)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	fmt.Printf("Synced %q → %q for system %q (%d scripts)\n",
		fromVersion, toVersion, system, intVal(result, "scriptCount"))
	return 0
}

func runSandboxDelete(cfg *Config, args []string) int {
	if len(args) < 2 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl sandbox delete <system> <version>\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}

	system := args[0]
	version := args[1]

	if version == "main" {
		PrintError("cannot delete main version")
		return 1
	}

	client := NewAgentClient(cfg)
	_, err := client.DeleteSandbox(cfg.Workgroup, system, version)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(map[string]interface{}{"deleted": true, "version": version})
		return 0
	}

	fmt.Printf("Deleted sandbox %q for system %q\n", version, system)
	return 0
}

func runSandboxVersions(cfg *Config, args []string) int {
	if len(args) < 1 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl sandbox versions <system>\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}

	system := args[0]

	client := NewAgentClient(cfg)
	result, err := client.ListVersions(cfg.Workgroup, system)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	versions := extractList(result, "versions")
	if len(versions) == 0 {
		fmt.Printf("No versions found for system %q.\n", system)
		return 0
	}

	headers := []string{"VERSION", "SCRIPTS", "UPDATED BY", "UPDATED", "DESCRIPTION"}
	var rows [][]string
	for _, v := range versions {
		ver := strVal(v, "version")
		label := ver
		if ver == "main" {
			label = "main *"
		}
		rows = append(rows, []string{
			label,
			fmt.Sprintf("%d", intVal(v, "scriptCount")),
			strVal(v, "updatedBy"),
			formatTime(strVal(v, "updatedAt")),
			strVal(v, "description"),
		})
	}

	PrintTable(headers, rows)
	return 0
}
