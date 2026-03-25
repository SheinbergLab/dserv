package main

import (
	"crypto/sha256"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// scriptInfo holds registry coordinates and checksum for a script.
type scriptInfo struct {
	protocol string
	stype    string
	filename string
	checksum string
}

// runPush pushes locally modified scripts to the registry.
// Fetches the server manifest to determine what's changed, then
// uploads only modified files. No local manifest file needed.
//
// With --add, also uploads local-only scripts (new protocols) that
// don't yet exist in the registry. If the system itself doesn't
// exist, --add will scaffold it first.
//
// Usage:
//
//	dservctl push <system> [--dir DIR] [--version V] [--comment MSG] [--dry-run] [--add]
func runPush(cfg *Config, args []string) int {
	if len(args) < 1 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl push <system> [--dir DIR] [--version V] [--comment MSG] [--dry-run] [--add]\n")
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
	addNew := false

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
		case "--add":
			addNew = true
		}
	}

	// Step 1: Fetch server manifest
	client := NewRegistryClient(cfg)
	manifest, err := client.GetManifest(cfg.Workgroup, system, version)
	if err != nil {
		PrintError("fetching manifest: %v", err)
		return 1
	}

	systemExists := manifest != nil

	if !systemExists && !addNew {
		PrintError("system %q not found in registry (use --add to create it)", system)
		return 1
	}

	// Step 2: Build server checksum map from existing scripts
	serverScripts := make(map[string]scriptInfo) // localRelPath -> info
	if systemExists {
		scripts := extractList(manifest, "scripts")
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
	}

	// Step 3: Compare local files against server checksums
	type pendingPush struct {
		info    scriptInfo
		relPath string
		content []byte
		isNew   bool
	}
	var changed []pendingPush
	var newScripts []pendingPush
	var missing []string
	unchanged := 0

	// Check existing server scripts for modifications
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

	// Step 4: If --add, scan for local-only files (new protocols/scripts)
	if addNew {
		localOnly := findLocalOnlyScripts(dir, serverScripts)
		for _, lo := range localOnly {
			newScripts = append(newScripts, pendingPush{
				info:    lo.info,
				relPath: lo.relPath,
				content: lo.content,
				isNew:   true,
			})
		}
	}

	if len(changed) == 0 && len(newScripts) == 0 {
		fmt.Printf("All %d scripts unchanged.\n", unchanged)
		if len(missing) > 0 && cfg.Verbose {
			fmt.Printf("(%d files not found locally)\n", len(missing))
		}
		// Still report local-only if --add wasn't used
		if !addNew {
			localOnly := findLocalOnlyScripts(dir, serverScripts)
			if len(localOnly) > 0 {
				fmt.Printf("%d local-only script(s) not pushed (use --add to include)\n", len(localOnly))
			}
		}
		return 0
	}

	// --- Dry-run output ---
	if cfg.JSON && dryRun {
		var changedItems []map[string]string
		for _, c := range changed {
			changedItems = append(changedItems, map[string]string{
				"protocol":  c.info.protocol,
				"type":      c.info.stype,
				"localPath": c.relPath,
			})
		}
		var newItems []map[string]string
		for _, n := range newScripts {
			newItems = append(newItems, map[string]string{
				"protocol":  n.info.protocol,
				"type":      n.info.stype,
				"localPath": n.relPath,
			})
		}
		printJSON(map[string]interface{}{
			"systemExists": systemExists,
			"changed":      changedItems,
			"new":          newItems,
			"unchanged":    unchanged,
			"missing":      missing,
		})
		return 0
	}

	if dryRun {
		if !systemExists {
			fmt.Printf("Would create system %q in workgroup %q\n", system, cfg.Workgroup)
		}
		if len(changed) > 0 {
			fmt.Printf("Would push %d changed script(s):\n", len(changed))
			for _, c := range changed {
				fmt.Printf("  ↑ %s/%s ← %s\n", c.info.protocol, c.info.stype, c.relPath)
			}
		}
		if len(newScripts) > 0 {
			fmt.Printf("Would add %d new script(s):\n", len(newScripts))
			for _, n := range newScripts {
				fmt.Printf("  + %s/%s ← %s\n", n.info.protocol, n.info.stype, n.relPath)
			}
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

	// Step 5: Scaffold system if it doesn't exist
	if !systemExists {
		// Find the first protocol name to use for scaffolding
		firstProto := "_"
		if len(newScripts) > 0 {
			firstProto = newScripts[0].info.protocol
		}
		fmt.Printf("Creating system %q in workgroup %q...\n", system, cfg.Workgroup)
		_, err := client.ScaffoldSystem(cfg.Workgroup, system, firstProto, cfg.User)
		if err != nil {
			PrintError("creating system: %v", err)
			return 1
		}
	}

	// Step 6: Push changed files
	pushed := 0
	added := 0
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

	// Step 7: Push new scripts
	for _, n := range newScripts {
		req := map[string]interface{}{
			"content":          string(n.content),
			"expectedChecksum": "", // new script, no prior checksum
			"updatedBy":        cfg.User,
			"comment":          comment,
		}

		result, err := client.SaveScript(cfg.Workgroup, system, n.info.protocol, n.info.stype, version, req)
		if err != nil {
			PrintError("%s/%s: %v", n.info.protocol, n.info.stype, err)
			errors++
			continue
		}

		added++
		if cfg.Verbose {
			cs := strVal(result, "checksum")
			if len(cs) > 8 {
				cs = cs[:8]
			}
			fmt.Printf("  + %s/%s (%s)\n", n.info.protocol, n.info.stype, cs)
		}
	}

	fmt.Printf("Pushed %d, added %d", pushed, added)
	if unchanged > 0 {
		fmt.Printf(", unchanged %d", unchanged)
	}
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

// localOnlyScript represents a local file not present in the server manifest.
type localOnlyScript struct {
	info    scriptInfo
	relPath string
	content []byte
}

// findLocalOnlyScripts walks the local directory and returns scripts
// that are not present in the server manifest.
func findLocalOnlyScripts(dir string, serverScripts map[string]scriptInfo) []localOnlyScript {
	var results []localOnlyScript

	entries, err := os.ReadDir(dir)
	if err != nil {
		return results
	}

	for _, entry := range entries {
		if entry.IsDir() {
			if isHiddenFile(entry.Name()) {
				continue
			}
			// Protocol subdirectory
			proto := entry.Name()
			subEntries, err := os.ReadDir(filepath.Join(dir, proto))
			if err != nil {
				continue
			}
			for _, sub := range subEntries {
				if sub.IsDir() || shouldIgnoreFile(sub.Name()) {
					continue
				}
				relPath := filepath.Join(proto, sub.Name())
				if _, exists := serverScripts[relPath]; exists {
					continue
				}
				data, err := os.ReadFile(filepath.Join(dir, relPath))
				if err != nil {
					continue
				}
				stype := deriveScriptType(proto, sub.Name())
				results = append(results, localOnlyScript{
					info: scriptInfo{
						protocol: proto,
						stype:    stype,
						filename: sub.Name(),
					},
					relPath: relPath,
					content: data,
				})
			}
		} else {
			// System-level file
			name := entry.Name()
			if shouldIgnoreFile(name) {
				continue
			}
			relPath := name
			if _, exists := serverScripts[relPath]; exists {
				continue
			}
			data, err := os.ReadFile(filepath.Join(dir, relPath))
			if err != nil {
				continue
			}
			stype := deriveSystemScriptType(name)
			results = append(results, localOnlyScript{
				info: scriptInfo{
					protocol: "_",
					stype:    stype,
					filename: name,
				},
				relPath: relPath,
				content: data,
			})
		}
	}

	return results
}

// deriveScriptType determines the registry script type from a protocol
// script filename. Convention:
//
//	<proto>.tcl           → "protocol"
//	<proto>_loaders.tcl   → "loaders"
//	<proto>_stim.tcl      → "stim"
//	<proto>_variants.tcl  → "variants"
//	<proto>_extract.tcl   → "extract"
//	<proto>_<suffix>.tcl  → "<suffix>"
func deriveScriptType(protocol, filename string) string {
	base := stripExtension(filename)
	if base == protocol {
		return "protocol"
	}
	prefix := protocol + "_"
	if strings.HasPrefix(base, prefix) {
		return base[len(prefix):]
	}
	// Fallback: use the bare name
	return base
}

// deriveSystemScriptType determines the script type for a system-level
// file (no protocol). Convention:
//
//	<system>.tcl          → "system"
//	<system>_extract.tcl  → "extract"
//	<system>_analyze.tcl  → "analyze"
//
// Since we don't know the system name here, we check for known suffixes.
func deriveSystemScriptType(filename string) string {
	base := stripExtension(filename)
	if strings.HasSuffix(base, "_extract") {
		return "extract"
	}
	if strings.HasSuffix(base, "_analyze") {
		return "analyze"
	}
	return "system"
}
