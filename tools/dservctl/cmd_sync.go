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
	force := false

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
		case "--force", "-f":
			force = true
		default:
			if !strings.HasPrefix(args[i], "-") && system == "" {
				system = args[i]
			}
		}
	}

	if !syncAll && system == "" {
		fmt.Fprintf(os.Stderr, "Usage: dservctl sync <system> [--dir DIR] [--version V] [--dry-run] [--force]\n")
		fmt.Fprintf(os.Stderr, "       dservctl sync --all [--dir DIR] [--version V] [--dry-run] [--force]\n")
		return 2
	}

	if syncAll {
		return runSyncAll(cfg, dir, version, dryRun, force)
	}

	return syncOneSystem(cfg, system, dir, version, dryRun, force)
}

// syncResult tracks counts from a sync operation.
type syncResult struct {
	pulled    int
	unchanged int
	errors    int
}

// runSyncAll syncs all systems in the workgroup plus shared libs.
// Layout: dir/<system>/..., dir/lib/*.tm
func runSyncAll(cfg *Config, dir, version string, dryRun, force bool) int {
	client := NewRegistryClient(cfg)

	if err := os.MkdirAll(dir, 0755); err != nil {
		PrintError("creating directory: %v", err)
		return 1
	}

	// Sync libs first (systems may depend on them)
	fmt.Println("Syncing libs...")
	libResult := syncLibsToDir(cfg, client, filepath.Join(dir, "lib"), dryRun, force)

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
		result := syncSystemInner(cfg, client, name, sysDir, version, dryRun, force)
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
func syncLibsToDir(cfg *Config, client *AgentClient, libDir string, dryRun, force bool) syncResult {
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

	// Base manifest for the lib dir (keys are bare filenames).
	manifest := readBaseManifest(libDir, cfg.Workgroup)
	manifestDirty := false

	pulled := 0
	unchanged := 0
	skipped := 0
	errors := 0
	registryLibs := make(map[string]bool) // every lib filename the registry has

	for _, lib := range libs {
		filename := strVal(lib, "filename")
		serverChecksum := strVal(lib, "checksum")
		name := strVal(lib, "name")
		ver := strVal(lib, "version")
		localPath := filepath.Join(libDir, filename)
		registryLibs[filename] = true

		// Compare checksums
		localHash := ""
		if data, err := os.ReadFile(localPath); err == nil {
			localHash = fmt.Sprintf("%x", sha256.Sum256(data))
			if localHash == serverChecksum {
				unchanged++
				if manifest.get(filename) != serverChecksum {
					manifest.set(filename, serverChecksum, ver, syncedByOrDefault(cfg.User))
					manifestDirty = true
				}
				continue
			}
		}

		// 3-way decision (unless --force, which always takes the registry).
		decision := "pull"
		if !force {
			decision = baseDecide(manifest.get(filename), localHash, serverChecksum)
		}
		if decision == "keep_local" || decision == "conflict" || decision == "cold" {
			skipped++
			noteSyncSkip(decision, "lib/"+filename)
			continue
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

		manifest.set(filename, serverChecksum, ver, syncedByOrDefault(cfg.User))
		manifestDirty = true
		pulled++
		if cfg.Verbose {
			fmt.Printf("  ↓ lib/%s\n", filename)
		}
	}

	// Prune base entries for libs the registry no longer has (libs is
	// non-empty here — we returned early otherwise — so this is safe).
	if !dryRun {
		for k := range manifest.Entries {
			if !registryLibs[k] {
				manifest.unset(k)
				manifestDirty = true
			}
		}
	}

	if manifestDirty && !dryRun {
		if err := writeBaseManifest(libDir, manifest); err != nil {
			PrintError("writing lib base manifest: %v", err)
		}
	}

	if pulled > 0 || skipped > 0 || (cfg.Verbose && unchanged > 0) {
		fmt.Printf("  Libs: %d pulled, %d unchanged", pulled, unchanged)
		if skipped > 0 {
			fmt.Printf(", %d skipped (local changes — use --force to overwrite)", skipped)
		}
		fmt.Println()
	}

	return syncResult{pulled: pulled, unchanged: unchanged, errors: errors}
}

// noteSyncSkip prints a per-file warning when sync declines to overwrite a
// locally-changed or conflicting file. Unlike the rig (which displaces and
// overwrites to match the registry), dservctl is a dev tool and preserves
// the working copy, leaving resolution to the user.
func noteSyncSkip(decision, label string) {
	switch decision {
	case "keep_local":
		fmt.Printf("  ⚠ %s: local changes not on registry — kept local (use --force to discard)\n", label)
	case "conflict":
		fmt.Printf("  ⚠ %s: CONFLICT — changed locally AND on registry; resolve manually (or --force)\n", label)
	case "cold":
		fmt.Printf("  ⚠ %s: differs from registry, no base to compare — kept local (use --force to overwrite)\n", label)
	}
}

// syncOneSystem is the entry point for syncing a single system.
func syncOneSystem(cfg *Config, system, dir, version string, dryRun, force bool) int {
	if err := os.MkdirAll(dir, 0755); err != nil {
		PrintError("creating directory: %v", err)
		return 1
	}

	client := NewRegistryClient(cfg)
	result := syncSystemInner(cfg, client, system, dir, version, dryRun, force)

	if cfg.JSON {
		return 0
	}

	if result.errors > 0 {
		return 1
	}
	return 0
}

// syncSystemInner does the actual sync work for a single system.
func syncSystemInner(cfg *Config, client *AgentClient, system, dir, version string, dryRun, force bool) syncResult {
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

	// Step 2: Compute local checksums for comparison.
	// We iterate over the manifest entries (not a fixed type list) so that
	// all script types — including viewer — are included automatically.
	//   localChecksums  server-key ("proto/type") -> checksum (sent to server)
	//   localRelsum     manifest relkey -> local checksum
	//   sentToRel       server-key -> manifest relkey (to map "extra" back)
	localChecksums := make(map[string]string)
	localRelsum := make(map[string]string)
	sentToRel := make(map[string]string)
	registryKeys := make(map[string]bool) // every relkey the registry has
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
		serverKey := checksumProto + "/" + scriptType
		relKey := localRelPath(protocol, filename)
		registryKeys[relKey] = true

		localPath := localFilePath(dir, protocol, filename)
		data, err := os.ReadFile(localPath)
		if err == nil {
			h := hashBytes(data)
			localChecksums[serverKey] = h
			localRelsum[relKey] = h
			sentToRel[serverKey] = relKey
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

	// Load the base manifest (merge ancestor) for this system dir.
	base := readBaseManifest(dir, cfg.Workgroup)
	baseDirty := false
	seen := make(map[string]bool)

	// Step 5: Apply the 3-way decision per stale script and write pulls.
	downloaded := 0
	skipped := 0
	errors := 0
	for _, s := range stale {
		protocol := strVal(s, "protocol")
		filename := fileOrType(s)
		content := strVal(s, "content")
		regChecksum := strVal(s, "checksum")

		relKey := localRelPath(protocol, filename)
		localPath := localFilePath(dir, protocol, filename)
		seen[relKey] = true

		// Decide. A missing local file is an unambiguous pull (nothing to
		// lose); --force always takes the registry.
		decision := "pull"
		if !force {
			if data, err := os.ReadFile(localPath); err == nil {
				decision = baseDecide(base.get(relKey), hashBytes(data), regChecksum)
			}
		}

		// dservctl preserves the working copy on local-change/conflict/cold
		// rather than displacing-and-overwriting like the rig does.
		if decision == "keep_local" || decision == "conflict" || decision == "cold" {
			skipped++
			noteSyncSkip(decision, relKey)
			continue
		}

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
		// Base := the registry checksum we just wrote.
		base.set(relKey, regChecksum, version, syncedByOrDefault(cfg.User))
		baseDirty = true
		downloaded++
		if cfg.Verbose {
			fmt.Printf("  ↓ %s\n", localPath)
		}
	}

	// Step 6: Seed/refresh base for unchanged files. A local file that was
	// neither stale nor local-only matched the registry, so its local
	// checksum is the registry's — record it as the base. This is what
	// makes the first sync populate the whole manifest, not just changes.
	extraRel := make(map[string]bool)
	for _, k := range extra {
		if rk, ok := sentToRel[k]; ok {
			extraRel[rk] = true
		}
	}
	for relKey, h := range localRelsum {
		if seen[relKey] || extraRel[relKey] {
			continue
		}
		if base.get(relKey) != h {
			base.set(relKey, h, version, syncedByOrDefault(cfg.User))
			baseDirty = true
		}
	}

	// Step 7: prune base entries for scripts the registry no longer has.
	// registryKeys is the authoritative set from the server manifest (we're
	// past the empty-scripts guard, so it's non-empty), making this safe and
	// self-healing across tools — a script deleted by anyone gets dropped
	// from the base on the next sync.
	for k := range base.Entries {
		if !registryKeys[k] {
			base.unset(k)
			baseDirty = true
		}
	}

	if baseDirty {
		if err := writeBaseManifest(dir, base); err != nil {
			PrintError("writing base manifest for %s: %v", system, err)
		}
	}

	fmt.Printf("  %s: %d updated, %d unchanged", system, downloaded, unchanged)
	if skipped > 0 {
		fmt.Printf(", %d skipped (local changes)", skipped)
	}
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
