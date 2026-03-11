package main

import (
	"crypto/sha256"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// runSync syncs scripts from the registry to a local directory.
// Uses manifest-based diffing: computes local checksums, sends to server,
// server returns only changed scripts.
func runSync(cfg *Config, args []string) int {
	if len(args) < 1 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl sync <system> [--dir DIR] [--version V] [--dry-run]\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}

	system := args[0]
	dir := "."
	version := ""
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
		case "--dry-run", "-n":
			dryRun = true
		}
	}

	// Ensure directory exists
	if err := os.MkdirAll(dir, 0755); err != nil {
		PrintError("creating directory: %v", err)
		return 1
	}

	// Step 1: Get manifest from server
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
		fmt.Println("No scripts in manifest.")
		return 0
	}

	// Step 2: Compute local checksums for comparison
	localChecksums := make(map[string]string)
	for _, s := range scripts {
		protocol := strVal(s, "protocol")
		scriptType := strVal(s, "type")
		filename := strVal(s, "filename")

		if filename == "" {
			filename = scriptType
		}

		// Determine local path: protocol/filename (or just filename for _system)
		localPath := localFilePath(dir, protocol, filename)
		data, err := os.ReadFile(localPath)
		if err == nil {
			hash := sha256.Sum256(data)
			localChecksums[protocol+"/"+scriptType] = fmt.Sprintf("%x", hash)
		}
	}

	// Step 3: Send checksums to server for diff
	syncResult, err := client.SyncCheck(cfg.Workgroup, system, localChecksums, version)
	if err != nil {
		PrintError("sync check: %v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(syncResult)
		return 0
	}

	// Step 4: Process stale scripts (need updating)
	stale := extractStaleScripts(syncResult)
	extra := extractStringList(syncResult, "extra")
	unchanged := intVal(syncResult, "unchanged")

	if len(stale) == 0 && len(extra) == 0 {
		fmt.Printf("All %d scripts up to date.\n", unchanged)
		return 0
	}

	if dryRun {
		if len(stale) > 0 {
			fmt.Printf("Would download %d script(s):\n", len(stale))
			for _, s := range stale {
				fmt.Printf("  %s/%s → %s\n",
					strVal(s, "protocol"), strVal(s, "type"),
					localFilePath(dir, strVal(s, "protocol"), fileOrType(s)))
			}
		}
		if len(extra) > 0 {
			fmt.Printf("Local files not in registry (%d):\n", len(extra))
			for _, key := range extra {
				fmt.Printf("  %s\n", key)
			}
		}
		return 0
	}

	// Step 5: Write updated scripts to disk
	downloaded := 0
	for _, s := range stale {
		protocol := strVal(s, "protocol")
		filename := fileOrType(s)
		content := strVal(s, "content")

		localPath := localFilePath(dir, protocol, filename)

		// Ensure subdirectory exists
		if subdir := filepath.Dir(localPath); subdir != "." {
			if err := os.MkdirAll(subdir, 0755); err != nil {
				PrintError("creating directory %s: %v", subdir, err)
				continue
			}
		}

		if err := os.WriteFile(localPath, []byte(content), 0644); err != nil {
			PrintError("writing %s: %v", localPath, err)
			continue
		}
		downloaded++
		if cfg.Verbose {
			fmt.Printf("  ↓ %s\n", localPath)
		}
	}

	fmt.Printf("Synced %d updated, %d unchanged", downloaded, unchanged)
	if len(extra) > 0 {
		fmt.Printf(", %d local-only", len(extra))
	}
	fmt.Println()

	if len(extra) > 0 && cfg.Verbose {
		fmt.Println("Local files not in registry:")
		for _, key := range extra {
			fmt.Printf("  %s\n", key)
		}
	}

	return 0
}

// localFilePath returns the local file path for a script.
// Scripts in "_system" or "_" protocol go directly in the dir.
// Others go in a protocol subdirectory.
func localFilePath(dir, protocol, filename string) string {
	if protocol == "" || protocol == "_" || protocol == "_system" {
		return filepath.Join(dir, filename)
	}
	return filepath.Join(dir, protocol, filename)
}

// fileOrType returns filename if set, otherwise falls back to type.
func fileOrType(s map[string]interface{}) string {
	fn := strVal(s, "filename")
	if fn != "" {
		return fn
	}
	return strVal(s, "type")
}

// extractStaleScripts extracts the "stale" array from a sync response.
func extractStaleScripts(result map[string]interface{}) []map[string]interface{} {
	if items, ok := result["stale"].([]interface{}); ok {
		return toMapSlice(items)
	}
	return nil
}

// extractStringList extracts a string array from a result map.
func extractStringList(result map[string]interface{}, key string) []string {
	items, ok := result[key].([]interface{})
	if !ok {
		return nil
	}
	var out []string
	for _, v := range items {
		if s, ok := v.(string); ok {
			out = append(out, s)
		}
	}
	return out
}

// computeLocalChecksums walks a directory and builds protocol/type → checksum map.
// This is an alternative approach that doesn't require the manifest first.
func computeLocalChecksums(dir string) (map[string]string, error) {
	checksums := make(map[string]string)

	entries, err := os.ReadDir(dir)
	if err != nil {
		return checksums, nil // empty dir is fine
	}

	for _, entry := range entries {
		if entry.IsDir() {
			// Protocol subdirectory
			protocol := entry.Name()
			subEntries, err := os.ReadDir(filepath.Join(dir, protocol))
			if err != nil {
				continue
			}
			for _, sub := range subEntries {
				if sub.IsDir() {
					continue
				}
				data, err := os.ReadFile(filepath.Join(dir, protocol, sub.Name()))
				if err != nil {
					continue
				}
				hash := sha256.Sum256(data)
				key := protocol + "/" + stripExtension(sub.Name())
				checksums[key] = fmt.Sprintf("%x", hash)
			}
		} else {
			// System-level file
			data, err := os.ReadFile(filepath.Join(dir, entry.Name()))
			if err != nil {
				continue
			}
			hash := sha256.Sum256(data)
			key := "_system/" + stripExtension(entry.Name())
			checksums[key] = fmt.Sprintf("%x", hash)
		}
	}

	return checksums, nil
}

func stripExtension(name string) string {
	if idx := strings.LastIndex(name, "."); idx > 0 {
		return name[:idx]
	}
	return name
}
