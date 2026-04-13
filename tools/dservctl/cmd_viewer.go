package main

import (
	"crypto/sha256"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// runViewer handles viewer plugin management.
//
// Usage:
//   dservctl viewer list [system]                       — list viewer scripts
//   dservctl viewer get <system> [protocol] [-o FILE]   — download viewer JS
//   dservctl viewer push <system> [protocol] -f FILE    — upload viewer JS
//   dservctl viewer delete <system> [protocol]          — remove viewer script
//   dservctl viewer sync [system] --dir DIR             — sync viewer JS to local directory
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
	case "sync":
		return viewerSync(cfg, args[1:])
	default:
		PrintError("unknown viewer subcommand %q (use list, get, push, sync, or delete)", args[0])
		printViewerUsage()
		return 2
	}
}

func printViewerUsage() {
	fmt.Fprintf(os.Stderr, `Usage:
  dservctl viewer list [system]                        List viewer scripts
  dservctl viewer get <system> [protocol] [-o FILE]    Download viewer JS
  dservctl viewer push <system> [protocol] -f FILE     Upload viewer JS
  dservctl viewer delete <system> [protocol]           Remove viewer script
  dservctl viewer sync [system] --dir DIR              Sync viewers to local directory

The viewer type is used by dg_viewer.html to load experiment-specific
visualization plugins. Sync pulls viewer JS from the registry into a
local viewers/ directory where dg_viewer.html can find them.

Examples:
  dservctl viewer push planko -f planko_world.js
  dservctl viewer sync --dir www/viewers               Sync all viewers
  dservctl viewer sync planko --dir www/viewers         Sync one system
  dservctl viewer list
  dservctl viewer get planko -o planko_world.js
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

	// Viewer filename convention: {system}_viewer.js or {protocol}_viewer.js
	var filename string
	if protocol != "_" && protocol != "" {
		filename = protocol + "_viewer.js"
	} else {
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

// viewerSync pulls viewer scripts from the registry into a local directory.
// Files are named {system}.js so that dg_viewer.html can load them via
// import('./viewers/{system}.js').
//
// Usage:
//   dservctl viewer sync --dir www/viewers              — sync all viewers
//   dservctl viewer sync planko --dir www/viewers       — sync one system
func viewerSync(cfg *Config, args []string) int {
	if !requireWorkgroup(cfg) {
		return 2
	}

	system := ""
	dir := ""
	dryRun := false

	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--dir", "-d":
			if i+1 < len(args) {
				dir = args[i+1]
				i++
			}
		case "--dry-run", "-n":
			dryRun = true
		default:
			if !strings.HasPrefix(args[i], "-") && system == "" {
				system = args[i]
			}
		}
	}

	if dir == "" {
		fmt.Fprintf(os.Stderr, "Usage: dservctl viewer sync [system] --dir DIR\n")
		fmt.Fprintf(os.Stderr, "\nDir should be the viewers/ directory served by dg_viewer.html\n")
		return 2
	}

	if err := os.MkdirAll(dir, 0755); err != nil {
		PrintError("creating directory: %v", err)
		return 1
	}

	client := NewRegistryClient(cfg)

	// Get systems to scan for viewer scripts
	var systemNames []string
	if system != "" {
		systemNames = []string{system}
	} else {
		systems, err := client.ListSystems(cfg.Workgroup)
		if err != nil {
			PrintError("listing systems: %v", err)
			return 1
		}
		for _, sys := range systems {
			systemNames = append(systemNames, strVal(sys, "name"))
		}
	}

	pulled := 0
	unchanged := 0
	errors := 0

	for _, sysName := range systemNames {
		// Check manifest for viewer scripts
		manifest, err := client.GetManifest(cfg.Workgroup, sysName, "")
		if err != nil {
			if cfg.Verbose {
				fmt.Printf("  %s: %v\n", sysName, err)
			}
			continue
		}
		if manifest == nil {
			continue
		}

		scripts := extractList(manifest, "scripts")
		for _, s := range scripts {
			if strVal(s, "type") != "viewer" {
				continue
			}

			protocol := strVal(s, "protocol")
			serverChecksum := strVal(s, "checksum")

			// Determine local filename: {system}.js
			// (protocol-level viewers could be {system}_{protocol}.js in future)
			localName := sysName + ".js"
			if protocol != "" && protocol != "_" {
				localName = sysName + "_" + protocol + ".js"
			}
			localPath := filepath.Join(dir, localName)

			// Compare checksums
			if data, err := os.ReadFile(localPath); err == nil {
				localHash := fmt.Sprintf("%x", sha256.Sum256(data))
				if localHash == serverChecksum {
					unchanged++
					if cfg.Verbose {
						fmt.Printf("  %s: up to date\n", localName)
					}
					continue
				}
			}

			if dryRun {
				fmt.Printf("  Would pull: %s\n", localName)
				pulled++
				continue
			}

			// Fetch full script content
			fetchProto := protocol
			if fetchProto == "" {
				fetchProto = "_"
			}
			result, err := client.GetScript(cfg.Workgroup, sysName, fetchProto, "viewer", "")
			if err != nil {
				PrintError("fetching viewer for %s: %v", sysName, err)
				errors++
				continue
			}

			content := strVal(result, "content")
			if content == "" {
				continue
			}

			if err := os.WriteFile(localPath, []byte(content), 0644); err != nil {
				PrintError("writing %s: %v", localPath, err)
				errors++
				continue
			}

			pulled++
			fmt.Printf("  \u2193 %s\n", localName)
		}
	}

	if pulled == 0 && unchanged == 0 && errors == 0 {
		fmt.Println("No viewer scripts found in registry.")
	} else {
		fmt.Printf("Viewer sync: %d pulled, %d unchanged", pulled, unchanged)
		if errors > 0 {
			fmt.Printf(", %d error(s)", errors)
		}
		fmt.Println()
	}

	if errors > 0 {
		return 1
	}
	return 0
}
