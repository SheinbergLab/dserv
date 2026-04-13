package main

import (
	"fmt"
	"io"
	"os"
	"strings"
)

// runViewer handles viewer plugin management.
//
// Usage:
//   dservctl viewer list <system>                       — list viewer scripts for a system
//   dservctl viewer get <system> [protocol] [-o FILE]   — download viewer JS
//   dservctl viewer push <system> [protocol] -f FILE    — upload viewer JS
//   dservctl viewer delete <system> [protocol]          — remove viewer script
func runViewer(cfg *Config, args []string) int {
	if len(args) == 0 {
		printViewerUsage()
		return 2
	}

	switch args[0] {
	case "list":
		return viewerList(cfg, args[1:])
	case "get":
		return viewerGet(cfg, args[1:])
	case "push":
		return viewerPush(cfg, args[1:])
	case "delete":
		return viewerDelete(cfg, args[1:])
	default:
		PrintError("unknown viewer subcommand %q (use list, get, push, or delete)", args[0])
		printViewerUsage()
		return 2
	}
}

func printViewerUsage() {
	fmt.Fprintf(os.Stderr, `Usage:
  dservctl viewer list <system>                        List viewer scripts
  dservctl viewer get <system> [protocol] [-o FILE]    Download viewer JS
  dservctl viewer push <system> [protocol] -f FILE     Upload viewer JS
  dservctl viewer delete <system> [protocol]           Remove viewer script

The viewer type is used by dg_viewer.html to load experiment-specific
visualization plugins. System-level viewers render common elements;
protocol-level viewers can add stim-specific overlays.

Examples:
  dservctl viewer list planko
  dservctl viewer push planko -f planko_world.js
  dservctl viewer push planko bounce -f planko_bounce_overlay.js
  dservctl viewer get planko -o planko_world.js
  dservctl viewer delete planko bounce
`)
}

// viewerList shows viewer scripts for a system.
func viewerList(cfg *Config, args []string) int {
	if len(args) == 0 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl viewer list <system>\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}

	system := args[0]
	client := NewRegistryClient(cfg)
	result, err := client.GetScripts(cfg.Workgroup, system, "")
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	// Filter for viewer scripts
	scriptsMap, ok := result["scripts"].(map[string]interface{})
	if !ok {
		scriptsMap = result
	}

	headers := []string{"SYSTEM", "PROTOCOL", "FILENAME", "CHECKSUM", "UPDATED BY"}
	var rows [][]string

	for key, val := range scriptsMap {
		scripts, ok := val.([]interface{})
		if !ok {
			continue
		}
		proto := key
		if proto == "_system" {
			proto = "(system)"
		}
		for _, s := range scripts {
			script, ok := s.(map[string]interface{})
			if !ok {
				continue
			}
			if strVal(script, "type") != "viewer" {
				continue
			}
			checksum := strVal(script, "checksum")
			if len(checksum) > 8 {
				checksum = checksum[:8]
			}
			rows = append(rows, []string{
				system,
				proto,
				strVal(script, "filename"),
				checksum,
				strVal(script, "updatedBy"),
			})
		}
	}

	if len(rows) == 0 {
		fmt.Printf("No viewer scripts found for system %q.\n", system)
		return 0
	}

	PrintTable(headers, rows)
	return 0
}

// viewerGet downloads a viewer script.
func viewerGet(cfg *Config, args []string) int {
	if len(args) == 0 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl viewer get <system> [protocol] [-o FILE]\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}

	system := args[0]
	protocol := "_"
	output := ""

	for i := 1; i < len(args); i++ {
		switch args[i] {
		case "--output", "-o":
			if i+1 < len(args) {
				output = args[i+1]
				i++
			}
		default:
			// First non-flag arg after system is protocol
			if !strings.HasPrefix(args[i], "-") && protocol == "_" {
				protocol = args[i]
			}
		}
	}

	client := NewRegistryClient(cfg)
	result, err := client.GetScript(cfg.Workgroup, system, protocol, "viewer", "")
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	content := strVal(result, "content")

	if output != "" {
		if err := os.WriteFile(output, []byte(content), 0644); err != nil {
			PrintError("writing file: %v", err)
			return 1
		}
		fmt.Fprintf(os.Stderr, "Written to %s (%d bytes)\n", output, len(content))
	} else {
		fmt.Print(content)
	}

	return 0
}

// viewerPush uploads a viewer script from a local JS file.
func viewerPush(cfg *Config, args []string) int {
	if len(args) == 0 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl viewer push <system> [protocol] -f FILE [-m COMMENT]\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}
	if !requireUser(cfg) {
		return 2
	}

	system := args[0]
	protocol := "_"
	file := ""
	comment := ""

	for i := 1; i < len(args); i++ {
		switch args[i] {
		case "--file", "-f":
			if i+1 < len(args) {
				file = args[i+1]
				i++
			}
		case "--comment", "-m":
			if i+1 < len(args) {
				comment = args[i+1]
				i++
			}
		default:
			if !strings.HasPrefix(args[i], "-") && protocol == "_" {
				protocol = args[i]
			}
		}
	}

	// Read content from file or stdin
	var content []byte
	var err error
	if file != "" {
		content, err = os.ReadFile(file)
		if err != nil {
			PrintError("reading file: %v", err)
			return 1
		}
	} else {
		content, err = io.ReadAll(os.Stdin)
		if err != nil {
			PrintError("reading stdin: %v", err)
			return 1
		}
	}

	if len(content) == 0 {
		PrintError("no content to push (provide -f FILE or pipe JS to stdin)")
		return 2
	}

	client := NewRegistryClient(cfg)

	// Get current checksum for conflict detection
	expectedChecksum := ""
	existing, err := client.GetScript(cfg.Workgroup, system, protocol, "viewer", "")
	if err == nil && existing != nil {
		expectedChecksum = strVal(existing, "checksum")
	}

	// Determine filename
	filename := file
	if filename == "" {
		filename = system + "_viewer.js"
	}

	req := map[string]interface{}{
		"content":          string(content),
		"filename":         filename,
		"expectedChecksum": expectedChecksum,
		"updatedBy":        cfg.User,
		"comment":          comment,
	}

	result, err := client.SaveScript(cfg.Workgroup, system, protocol, "viewer", "", req)
	if err != nil {
		if strings.Contains(err.Error(), "conflict") {
			PrintError("conflict: viewer was modified by someone else. Fetch latest and retry.")
		} else {
			PrintError("%v", err)
		}
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	checksum := ""
	if result != nil {
		checksum = strVal(result, "checksum")
		if len(checksum) > 8 {
			checksum = checksum[:8]
		}
	}

	protoLabel := ""
	if protocol != "_" {
		protoLabel = "/" + protocol
	}
	fmt.Printf("Pushed viewer for %s%s (checksum: %s)\n", system, protoLabel, checksum)
	return 0
}

// viewerDelete removes a viewer script.
func viewerDelete(cfg *Config, args []string) int {
	if len(args) == 0 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl viewer delete <system> [protocol]\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}

	system := args[0]
	protocol := "_"
	if len(args) > 1 && !strings.HasPrefix(args[1], "-") {
		protocol = args[1]
	}

	client := NewRegistryClient(cfg)
	_, err := client.DeleteScript(cfg.Workgroup, system, protocol, "viewer")
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	protoLabel := ""
	if protocol != "_" {
		protoLabel = "/" + protocol
	}
	fmt.Printf("Deleted viewer for %s%s\n", system, protoLabel)
	return 0
}
