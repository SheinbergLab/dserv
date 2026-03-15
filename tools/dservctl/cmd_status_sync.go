package main

import (
	"crypto/sha256"
	"fmt"
	"os"
	"path/filepath"
	"sort"
)

// runSyncStatus compares local scripts against the registry and reports
// which are synced, modified locally, local-only, or registry-only.
//
// Usage:
//
//	dservctl status <system> [--dir DIR] [--version V]
func runSyncStatus(cfg *Config, args []string) int {
	if len(args) < 1 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl status <system> [--dir DIR] [--version V]\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}

	system := args[0]
	dir := "."
	version := ""

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
		}
	}

	client := NewRegistryClient(cfg)

	// Get server manifest
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

	// Build server checksum map: relPath -> {protocol, type, checksum}
	type scriptInfo struct {
		protocol string
		stype    string
		checksum string
		relPath  string
	}

	serverMap := make(map[string]scriptInfo)
	for _, s := range scripts {
		protocol := strVal(s, "protocol")
		stype := strVal(s, "type")
		filename := strVal(s, "filename")
		if filename == "" {
			filename = stype
		}
		relPath := localRelPath(protocol, filename)
		serverMap[relPath] = scriptInfo{
			protocol: protocol,
			stype:    stype,
			checksum: strVal(s, "checksum"),
			relPath:  relPath,
		}
	}

	// Compare local files
	type statusEntry struct {
		relPath  string
		protocol string
		stype    string
		status   string // "synced", "modified", "local_only", "registry_only"
	}

	var entries []statusEntry
	seen := make(map[string]bool)

	// Check server scripts against local
	for relPath, info := range serverMap {
		localPath := filepath.Join(dir, relPath)
		data, err := os.ReadFile(localPath)
		if err != nil {
			entries = append(entries, statusEntry{
				relPath:  relPath,
				protocol: info.protocol,
				stype:    info.stype,
				status:   "registry_only",
			})
			continue
		}

		localHash := fmt.Sprintf("%x", sha256.Sum256(data))
		status := "synced"
		if localHash != info.checksum {
			status = "modified"
		}

		entries = append(entries, statusEntry{
			relPath:  relPath,
			protocol: info.protocol,
			stype:    info.stype,
			status:   status,
		})
		seen[relPath] = true
	}

	// Check for local-only files (walk protocol subdirectories)
	dirEntries, _ := os.ReadDir(dir)
	for _, entry := range dirEntries {
		if !entry.IsDir() {
			// System-level file
			relPath := entry.Name()
			if !seen[relPath] && !isHiddenFile(relPath) {
				entries = append(entries, statusEntry{
					relPath:  relPath,
					protocol: "_",
					stype:    stripExtension(relPath),
					status:   "local_only",
				})
			}
		} else {
			// Protocol subdirectory
			proto := entry.Name()
			if isHiddenFile(proto) {
				continue
			}
			subEntries, _ := os.ReadDir(filepath.Join(dir, proto))
			for _, sub := range subEntries {
				if sub.IsDir() {
					continue
				}
				relPath := filepath.Join(proto, sub.Name())
				if !seen[relPath] {
					entries = append(entries, statusEntry{
						relPath:  relPath,
						protocol: proto,
						stype:    stripExtension(sub.Name()),
						status:   "local_only",
					})
				}
			}
		}
	}

	// Sort by relPath
	sort.Slice(entries, func(i, j int) bool {
		return entries[i].relPath < entries[j].relPath
	})

	if cfg.JSON {
		var items []map[string]string
		for _, e := range entries {
			items = append(items, map[string]string{
				"path":     e.relPath,
				"protocol": e.protocol,
				"type":     e.stype,
				"status":   e.status,
			})
		}
		printJSON(map[string]interface{}{"system": system, "scripts": items})
		return 0
	}

	// Summary counts
	counts := map[string]int{}
	for _, e := range entries {
		counts[e.status]++
	}

	// Print table
	headers := []string{"STATUS", "PROTOCOL", "TYPE", "PATH"}
	var rows [][]string
	for _, e := range entries {
		marker := ""
		switch e.status {
		case "synced":
			marker = "✓"
		case "modified":
			marker = "M"
		case "local_only":
			marker = "+"
		case "registry_only":
			marker = "−"
		}
		rows = append(rows, []string{
			marker + " " + e.status,
			e.protocol,
			e.stype,
			e.relPath,
		})
	}

	PrintTable(headers, rows)

	fmt.Printf("\n%d synced, %d modified, %d local-only, %d registry-only\n",
		counts["synced"], counts["modified"], counts["local_only"], counts["registry_only"])

	return 0
}

func isHiddenFile(name string) bool {
	return len(name) > 0 && name[0] == '.'
}
