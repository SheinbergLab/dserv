package main

import (
	"crypto/sha256"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// runPush pushes locally modified scripts to the registry.
// Fetches the server manifest to determine what's changed, then
// uploads only modified files. No local manifest file needed.
//
// Usage:
//
//	dservctl push <system> [--dir DIR] [--version V] [--comment MSG] [--dry-run]
func runPush(cfg *Config, args []string) int {
	if len(args) < 1 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl push <system> [--dir DIR] [--version V] [--comment MSG] [--dry-run]\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}
	if !requireUser(cfg) {
		return 2
	}

	system := args[0]
	if system == "--all" || system == "-a" {
		PrintError("push operates on a single system (e.g., dservctl push prf --dir ./prf)")
		return 2
	}
	dir := "."
	version := ""
	comment := ""
	dryRun := false

	for i := 1; i < len(args); i++ {
		switch args[i] {
		case "--dir", "-d":
			if i+1 < len(args) {
				dir = args[i+1]
				i++
			}
		case "--version":
			if i+1 < len(args) {
				version = args[i+1]
				i++
			}
		case "--comment", "-m":
			if i+1 < len(args) {
				comment = args[i+1]
				i++
			}
		case "--dry-run", "-n":
			dryRun = true
		}
	}

	// Step 1: Fetch server manifest
	client := NewRegistryClient(cfg)
	manifest, err := client.GetManifest(cfg.Workgroup, system, version)
	if err != nil {
		PrintError("fetching manifest: %v", err)
		return 1
	}
	if manifest == nil {
		PrintError("system %q not found", system)
		return 1
	}

	scripts := extractList(manifest, "scripts")
	if len(scripts) == 0 {
		fmt.Println("No scripts in server manifest.")
		return 0
	}

	// Step 2: Build server checksum map and registry coordinate lookup
	type scriptInfo struct {
		protocol string
		stype    string
		filename string
		checksum string
	}

	serverScripts := make(map[string]scriptInfo) // localRelPath -> info
	for _, s := range scripts {
		protocol := strVal(s, "protocol")
		stype := strVal(s, "type")
		filename := strVal(s, "filename")
		if filename == "" {
			filename = stype
		}
		relPath := localRelPath(protocol, filename)
		serverScripts[relPath] = scriptInfo{
			protocol: protocol,
			stype:    stype,
			filename: filename,
			checksum: strVal(s, "checksum"),
		}
	}

	// Step 3: Compare local files against server checksums
	type pendingPush struct {
		info    scriptInfo
		relPath string
		content []byte
	}
	var changed []pendingPush
	var missing []string
	unchanged := 0

	for relPath, info := range serverScripts {
		localPath := filepath.Join(dir, relPath)
		data, err := os.ReadFile(localPath)
		if err != nil {
			missing = append(missing, relPath)
			continue
		}

		localHash := fmt.Sprintf("%x", sha256.Sum256(data))
		if localHash == info.checksum {
			unchanged++
			continue
		}

		changed = append(changed, pendingPush{
			info:    info,
			relPath: relPath,
			content: data,
		})
	}

	if len(changed) == 0 {
		fmt.Printf("All %d scripts unchanged.\n", unchanged)
		if len(missing) > 0 && cfg.Verbose {
			fmt.Printf("(%d files not found locally)\n", len(missing))
		}
		return 0
	}

	if cfg.JSON && dryRun {
		var items []map[string]string
		for _, c := range changed {
			items = append(items, map[string]string{
				"protocol":  c.info.protocol,
				"type":      c.info.stype,
				"localPath": c.relPath,
			})
		}
		printJSON(map[string]interface{}{
			"changed":   items,
			"unchanged": unchanged,
			"missing":   missing,
		})
		return 0
	}

	if dryRun {
		fmt.Printf("Would push %d changed script(s):\n", len(changed))
		for _, c := range changed {
			fmt.Printf("  ↑ %s/%s ← %s\n", c.info.protocol, c.info.stype, c.relPath)
		}
		if len(missing) > 0 {
			fmt.Printf("Not found locally (%d):\n", len(missing))
			for _, m := range missing {
				fmt.Printf("  ? %s\n", m)
			}
		}
		fmt.Printf("Unchanged: %d\n", unchanged)
		return 0
	}

	// Step 4: Push changed files
	pushed := 0
	errors := 0

	for _, c := range changed {
		req := map[string]interface{}{
			"content":          string(c.content),
			"expectedChecksum": c.info.checksum,
			"updatedBy":        cfg.User,
			"comment":          comment,
		}

		result, err := client.SaveScript(cfg.Workgroup, system, c.info.protocol, c.info.stype, version, req)
		if err != nil {
			if strings.Contains(err.Error(), "conflict") {
				PrintError("%s/%s: conflict (modified on server — run sync to update)", c.info.protocol, c.info.stype)
			} else {
				PrintError("%s/%s: %v", c.info.protocol, c.info.stype, err)
			}
			errors++
			continue
		}

		pushed++
		if cfg.Verbose {
			cs := strVal(result, "checksum")
			if len(cs) > 8 {
				cs = cs[:8]
			}
			fmt.Printf("  ↑ %s/%s (%s)\n", c.info.protocol, c.info.stype, cs)
		}
	}

	fmt.Printf("Pushed %d, unchanged %d", pushed, unchanged)
	if errors > 0 {
		fmt.Printf(", %d error(s)", errors)
	}
	if len(missing) > 0 {
		fmt.Printf(", %d not local", len(missing))
	}
	fmt.Println()

	if errors > 0 {
		return 1
	}
	return 0
}

// localRelPath returns the relative path for a script.
// Same convention as localFilePath in cmd_sync.go but without the dir prefix.
func localRelPath(protocol, filename string) string {
	if protocol == "" || protocol == "_" || protocol == "_system" {
		return filename
	}
	return filepath.Join(protocol, filename)
}
