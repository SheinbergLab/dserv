package main

import (
	"crypto/sha256"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

// runLibs dispatches lib subcommands.
func runLibs(cfg *Config, args []string) int {
	if len(args) == 0 {
		fmt.Fprintf(os.Stderr, "Usage:\n")
		fmt.Fprintf(os.Stderr, "  dservctl libs list                              List libs in registry\n")
		fmt.Fprintf(os.Stderr, "  dservctl libs sync [--dir DIR]                  Pull libs to local directory\n")
		fmt.Fprintf(os.Stderr, "  dservctl libs push [--dir DIR] [-m MSG]         Push changed libs to registry\n")
		fmt.Fprintf(os.Stderr, "  dservctl libs status [--dir DIR]                Compare local vs registry\n")
		return 2
	}

	switch args[0] {
	case "list":
		return runLibsList(cfg, args[1:])
	case "sync":
		return runLibsSync(cfg, args[1:])
	case "push":
		return runLibsPush(cfg, args[1:])
	case "status":
		return runLibsStatus(cfg, args[1:])
	default:
		PrintError("unknown libs subcommand %q (use list, sync, push, or status)", args[0])
		return 2
	}
}

func runLibsList(cfg *Config, args []string) int {
	if !requireWorkgroup(cfg) {
		return 2
	}

	client := NewRegistryClient(cfg)
	libs, err := client.ListLibs(cfg.Workgroup)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(libs)
		return 0
	}

	if len(libs) == 0 {
		fmt.Println("No libs found.")
		return 0
	}

	headers := []string{"NAME", "VERSION", "FILENAME", "CHECKSUM", "UPDATED BY"}
	var rows [][]string
	for _, lib := range libs {
		checksum := strVal(lib, "checksum")
		if len(checksum) > 8 {
			checksum = checksum[:8]
		}
		rows = append(rows, []string{
			strVal(lib, "name"),
			strVal(lib, "version"),
			strVal(lib, "filename"),
			checksum,
			strVal(lib, "updatedBy"),
		})
	}

	PrintTable(headers, rows)
	return 0
}

func runLibsSync(cfg *Config, args []string) int {
	if !requireWorkgroup(cfg) {
		return 2
	}

	dir := "lib"
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
		}
	}

	client := NewRegistryClient(cfg)
	result := syncLibsToDir(cfg, client, dir, dryRun)

	if result.pulled == 0 && result.unchanged > 0 && result.errors == 0 {
		fmt.Printf("All %d libs up to date.\n", result.unchanged)
	}

	if result.errors > 0 {
		return 1
	}
	return 0
}

func runLibsPush(cfg *Config, args []string) int {
	if !requireWorkgroup(cfg) {
		return 2
	}
	if !requireUser(cfg) {
		return 2
	}

	dir := "lib"
	comment := ""
	dryRun := false
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--dir", "-d":
			if i+1 < len(args) {
				dir = args[i+1]
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

	_ = comment // used in request body below

	client := NewRegistryClient(cfg)

	// Get registry checksums
	libs, err := client.ListLibs(cfg.Workgroup)
	if err != nil {
		PrintError("fetching libs: %v", err)
		return 1
	}

	serverChecksums := make(map[string]string) // filename -> checksum
	for _, lib := range libs {
		serverChecksums[strVal(lib, "filename")] = strVal(lib, "checksum")
	}

	// Walk local lib directory
	localFiles, err := os.ReadDir(dir)
	if err != nil {
		PrintError("reading directory %s: %v", dir, err)
		return 1
	}

	// Pattern for name-version.tm files
	tmPattern := regexp.MustCompile(`^(.+)-(\d+[\._]\d+)\.tm$`)

	type pendingLib struct {
		filename string
		name     string
		version  string
		content  []byte
	}

	var changed []pendingLib
	unchanged := 0
	skipped := 0

	for _, entry := range localFiles {
		if entry.IsDir() {
			continue
		}
		filename := entry.Name()

		matches := tmPattern.FindStringSubmatch(filename)
		if matches == nil {
			skipped++
			continue
		}
		name := matches[1]
		version := strings.ReplaceAll(matches[2], "_", ".")

		localPath := filepath.Join(dir, filename)
		data, err := os.ReadFile(localPath)
		if err != nil {
			PrintError("reading %s: %v", filename, err)
			continue
		}

		localHash := fmt.Sprintf("%x", sha256.Sum256(data))

		// Compare against server — use the registry filename format (dots)
		regFilename := fmt.Sprintf("%s-%s.tm", name, version)
		if serverHash, ok := serverChecksums[regFilename]; ok && localHash == serverHash {
			unchanged++
			continue
		}

		changed = append(changed, pendingLib{
			filename: filename,
			name:     name,
			version:  version,
			content:  data,
		})
	}

	if len(changed) == 0 {
		fmt.Printf("All %d libs unchanged", unchanged)
		if skipped > 0 {
			fmt.Printf(" (%d skipped)", skipped)
		}
		fmt.Println()
		return 0
	}

	if dryRun {
		fmt.Printf("Would push %d changed lib(s):\n", len(changed))
		for _, lib := range changed {
			fmt.Printf("  ↑ %s (%s v%s)\n", lib.filename, lib.name, lib.version)
		}
		fmt.Printf("Unchanged: %d\n", unchanged)
		return 0
	}

	// Push changed libs
	pushed := 0
	errors := 0

	for _, lib := range changed {
		req := map[string]interface{}{
			"content":   string(lib.content),
			"updatedBy": cfg.User,
		}

		_, err := client.SaveLib(cfg.Workgroup, lib.name, lib.version, req)
		if err != nil {
			PrintError("%s: %v", lib.filename, err)
			errors++
			continue
		}

		pushed++
		if cfg.Verbose {
			fmt.Printf("  ↑ %s\n", lib.filename)
		}
	}

	fmt.Printf("Pushed %d, unchanged %d", pushed, unchanged)
	if errors > 0 {
		fmt.Printf(", %d error(s)", errors)
	}
	fmt.Println()

	if errors > 0 {
		return 1
	}
	return 0
}

func runLibsStatus(cfg *Config, args []string) int {
	if !requireWorkgroup(cfg) {
		return 2
	}

	dir := "lib"
	for i := 0; i < len(args); i++ {
		if (args[i] == "--dir" || args[i] == "-d") && i+1 < len(args) {
			dir = args[i+1]
			i++
		}
	}

	client := NewRegistryClient(cfg)

	// Get registry libs
	libs, err := client.ListLibs(cfg.Workgroup)
	if err != nil {
		PrintError("fetching libs: %v", err)
		return 1
	}

	serverMap := make(map[string]string) // filename -> checksum
	for _, lib := range libs {
		serverMap[strVal(lib, "filename")] = strVal(lib, "checksum")
	}

	type statusEntry struct {
		filename string
		status   string
	}
	var entries []statusEntry
	seen := make(map[string]bool)

	// Check local files against server
	localFiles, _ := os.ReadDir(dir)
	for _, entry := range localFiles {
		if entry.IsDir() {
			continue
		}
		filename := entry.Name()
		if !strings.HasSuffix(filename, ".tm") {
			continue
		}

		localPath := filepath.Join(dir, filename)
		data, err := os.ReadFile(localPath)
		if err != nil {
			continue
		}
		localHash := fmt.Sprintf("%x", sha256.Sum256(data))

		if serverHash, ok := serverMap[filename]; ok {
			if localHash == serverHash {
				entries = append(entries, statusEntry{filename, "synced"})
			} else {
				entries = append(entries, statusEntry{filename, "modified"})
			}
			seen[filename] = true
		} else {
			entries = append(entries, statusEntry{filename, "local_only"})
		}
	}

	// Registry-only
	for filename := range serverMap {
		if !seen[filename] {
			entries = append(entries, statusEntry{filename, "registry_only"})
		}
	}

	if cfg.JSON {
		printJSON(entries)
		return 0
	}

	if len(entries) == 0 {
		fmt.Println("No libs found.")
		return 0
	}

	counts := map[string]int{}
	headers := []string{"STATUS", "FILENAME"}
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
		rows = append(rows, []string{marker + " " + e.status, e.filename})
		counts[e.status]++
	}

	PrintTable(headers, rows)
	fmt.Printf("\n%d synced, %d modified, %d local-only, %d registry-only\n",
		counts["synced"], counts["modified"], counts["local_only"], counts["registry_only"])

	return 0
}
