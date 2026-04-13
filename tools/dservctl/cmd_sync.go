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
//
// With --all, syncs every system in the workgroup (each into its own
// subdirectory) plus shared libs into a lib/ subdirectory.
func runSync(cfg *Config, args []string) int {
	if !requireWorkgroup(cfg) {
		return 2
	}

	system := ""
	dir := "."
	version := ""
	dryRun := false
	syncAll := false

	for i := 0; i < len(args); i++ {
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
		case "--all", "-a":
			syncAll = true
		default:
			if !strings.HasPrefix(args[i], "-") && system == "" {
				system = args[i]
			}
		}
	}

	if !syncAll && system == "" {
		fmt.Fprintf(os.Stderr, "Usage: dservctl sync <system> [--dir DIR] [--version V] [--dry-run]\n")
		fmt.Fprintf(os.Stderr, "       dservctl sync --all [--dir DIR] [--version V] [--dry-run]\n")
		return 2
	}

	if syncAll {
		return runSyncAll(cfg, dir, version, dryRun)
	}

	return syncOneSystem(cfg, system, dir, version, dryRun)
}

// syncResult tracks counts from a sync operation.
type syncResult struct {
	pulled    int
	unchanged int
	errors    int
}

// runSyncAll syncs all systems in the workgroup plus shared libs.
// Layout: dir/<system>/..., dir/lib/*.tm
func runSyncAll(cfg *Config, dir, version string, dryRun bool) int {
	client := NewRegistryClient(cfg)

	if err := os.MkdirAll(dir, 0755); err != nil {
		PrintError("creating directory: %v", err)
		return 1
	}

	// Sync libs first (systems may depend on them)
	fmt.Println("Syncing libs...")
	libResult := syncLibsToDir(cfg, client, filepath.Join(dir, "lib"), dryRun)

	// Get system list
	systems, err := client.ListSystems(cfg.Workgroup)
	if err != nil {
		PrintError("listing systems: %v", err)
		return 1
	}

	if len(systems) == 0 {
		fmt.Printf("No systems found in workgroup %q.\n", cfg.Workgroup)
		return 0
	}

	totalPulled := libResult.pulled
	totalUnchanged := libResult.unchanged
	totalErrors := libResult.errors

	for _, sys := range systems {
		name := strVal(sys, "name")
		sysDir := filepath.Join(dir, name)

		fmt.Printf("Syncing %s...\n", name)
		result := syncSystemInner(cfg, client, name, sysDir, version, dryRun)
		totalPulled += result.pulled
		totalUnchanged += result.unchanged
		totalErrors += result.errors
	}

	fmt.Printf("\nSync complete: %d pulled, %d unchanged", totalPulled, totalUnchanged)
	if totalErrors > 0 {
		fmt.Printf(", %d error(s)", totalErrors)
	}
	fmt.Println()

	if totalErrors > 0 {
		return 1
	}
	return 0
}

// syncLibsToDir syncs shared libraries to a local directory.
func syncLibsToDir(cfg *Config, client *AgentClient, libDir string, dryRun bool) syncResult {
	libs, err := client.ListLibs(cfg.Workgroup)
	if err != nil {
		PrintError("fetching libs: %v", err)
		return syncResult{errors: 1}
	}

	if len(libs) == 0 {
		return syncResult{}
	}

	if err := os.MkdirAll(libDir, 0755); err != nil {
		PrintError("creating lib directory: %v", err)
		return syncResult{errors: 1}
	}

	pulled := 0
	unchanged := 0
	errors := 0

	for _, lib := range libs {
		filename := strVal(lib, "filename")
		serverChecksum := strVal(lib, "checksum")
		name := strVal(lib, "name")
		ver := strVal(lib, "version")
		localPath := filepath.Join(libDir, filename)

		// Compare checksums
		if data, err := os.ReadFile(localPath); err == nil {
			localHash := fmt.Sprintf("%x", sha256.Sum256(data))
			if localHash == serverChecksum {
				unchanged++
				continue
			}
		}

		if dryRun {
			fmt.Printf("  Would pull lib: %s\n", filename)
			pulled++
			continue
		}

		// Fetch full content
		libData, err := client.GetLib(cfg.Workgroup, name, ver)
		if err != nil {
			PrintError("fetching lib %s: %v", filename, err)
			errors++
			continue
		}

		content := strVal(libData, "content")
		if err := os.WriteFile(localPath, []byte(content), 0644); err != nil {
			PrintError("writing lib %s: %v", filename, err)
			errors++
			continue
		}

		pulled++
		if cfg.Verbose {
			fmt.Printf("  ↓ lib/%s\n", filename)
		}
	}

	if pulled > 0 || (cfg.Verbose && unchanged > 0) {
		fmt.Printf("  Libs: %d pulled, %d unchanged\n", pulled, unchanged)
	}

	return syncResult{pulled: pulled, unchanged: unchanged, errors: errors}
}

// syncOneSystem is the entry point for syncing a single system.
func syncOneSystem(cfg *Config, system, dir, version string, dryRun bool) int {
	if err := os.MkdirAll(dir, 0755); err != nil {
		PrintError("creating directory: %v", err)
		return 1
	}

	client := NewRegistryClient(cfg)
	result := syncSystemInner(cfg, client, system, dir, version, dryRun)

	if cfg.JSON {
		return 0
	}

	if result.errors > 0 {
		return 1
	}
	return 0
}

// syncSystemInner does the actual sync work for a single system.
func syncSystemInner(cfg *Config, client *AgentClient, system, dir, version string, dryRun bool) syncResult {
	if err := os.MkdirAll(dir, 0755); err != nil {
		PrintError("creating directory: %v", err)
		return syncResult{errors: 1}
	}

	// Step 1: Get manifest from server
	manifest, err := client.GetManifest(cfg.Workgroup, system, version)
	if err != nil {
		PrintError("fetching manifest for %s: %v", system, err)
		return syncResult{errors: 1}
	}

	if manifest == nil {
		PrintError("system %q not found", system)
		return syncResult{errors: 1}
	}

	scripts := extractList(manifest, "scripts")
	if len(scripts) == 0 {
		fmt.Printf("  No scripts in %s.\n", system)
		return syncResult{}
	}

	// Step 2: Compute local checksums for comparison
	// We iterate over the manifest entries (not a fixed type list) so that
	// all script types — including viewer — are included automatically.
	localChecksums := make(map[string]string)
	for _, s := range scripts {
		protocol := strVal(s, "protocol")
		scriptType := strVal(s, "type")
		filename := strVal(s, "filename")

		if filename == "" {
			filename = scriptType
		}

		// Normalize empty protocol to "_system" to match server's key format
		checksumProto := protocol
		if checksumProto == "" || checksumProto == "_" {
			checksumProto = "_system"
		}

		localPath := localFilePath(dir, protocol, filename)
		data, err := os.ReadFile(localPath)
		if err == nil {
			hash := sha256.Sum256(data)
			localChecksums[checksumProto+"/"+scriptType] = fmt.Sprintf("%x", hash)
		}
	}

	// Step 3: Send checksums to server for diff
	syncResp, err := client.SyncCheck(cfg.Workgroup, system, localChecksums, version)
	if err != nil {
		PrintError("sync check for %s: %v", system, err)
		return syncResult{errors: 1}
	}

	if cfg.JSON {
		printJSON(syncResp)
		return syncResult{}
	}

	// Step 4: Process stale scripts (need updating)
	stale := extractStaleScripts(syncResp)
	extra := extractStringList(syncResp, "extra")
	unchanged := intVal(syncResp, "unchanged")

	if len(stale) == 0 && len(extra) == 0 {
		fmt.Printf("  %s: all %d scripts up to date.\n", system, unchanged)
		return syncResult{unchanged: unchanged}
	}

	if dryRun {
		if len(stale) > 0 {
			fmt.Printf("  Would download %d script(s) for %s:\n", len(stale), system)
			for _, s := range stale {
				fmt.Printf("    %s/%s → %s\n",
					strVal(s, "protocol"), strVal(s, "type"),
					localFilePath(dir, strVal(s, "protocol"), fileOrType(s)))
			}
		}
		if len(extra) > 0 {
			fmt.Printf("  Local files not in registry (%d):\n", len(extra))
			for _, key := range extra {
				fmt.Printf("    %s\n", key)
			}
		}
		return syncResult{pulled: len(stale), unchanged: unchanged}
	}

	// Step 5: Write updated scripts to disk
	downloaded := 0
	errors := 0
	for _, s := range stale {
		protocol := strVal(s, "protocol")
		filename := fileOrType(s)
		content := strVal(s, "content")

		localPath := localFilePath(dir, protocol, filename)

		if subdir := filepath.Dir(localPath); subdir != "." {
			if err := os.MkdirAll(subdir, 0755); err != nil {
				PrintError("creating directory %s: %v", subdir, err)
				errors++
				continue
			}
		}

		if err := os.WriteFile(localPath, []byte(content), 0644); err != nil {
			PrintError("writing %s: %v", localPath, err)
			errors++
			continue
		}
		downloaded++
		if cfg.Verbose {
			fmt.Printf("  ↓ %s\n", localPath)
		}
	}

	fmt.Printf("  %s: %d updated, %d unchanged", system, downloaded, unchanged)
	if len(extra) > 0 {
		fmt.Printf(", %d local-only", len(extra))
	}
	fmt.Println()

	if len(extra) > 0 && cfg.Verbose {
		fmt.Println("  Local files not in registry:")
		for _, key := range extra {
			fmt.Printf("    %s\n", key)
		}
	}

	return syncResult{pulled: downloaded, unchanged: unchanged, errors: errors}
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
