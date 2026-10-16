package main

import (
	"bufio"
	"crypto/sha256"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// pruneCandidate is one local lib file and what prune decides about it.
type pruneCandidate struct {
	filename string
	action   string // prune | keep_modified
}

// planLibsPrune decides, for a lib directory, which files are synced
// copies of libraries the registry no longer has.
//
// A file is pruned only when ALL of these hold:
//   - .sync_base.json has an entry for it (the sync put it there; a file
//     with no entry was made by hand and is never touched);
//   - the registry no longer lists it;
//   - its checksum still equals that entry's (unchanged since the sync;
//     a locally edited file is kept and reported).
//
// staleEntries are base entries for files that are gone both locally and
// from the registry; prune drops them from the manifest.
func planLibsPrune(dir string, m *BaseManifest, registry map[string]bool) (cands []pruneCandidate, staleEntries []string, err error) {
	for key, entry := range m.Entries {
		if registry[key] || strings.Contains(key, "/") {
			continue
		}
		data, rerr := os.ReadFile(filepath.Join(dir, key))
		if os.IsNotExist(rerr) {
			staleEntries = append(staleEntries, key)
			continue
		}
		if rerr != nil {
			return nil, nil, rerr
		}
		if fmt.Sprintf("%x", sha256.Sum256(data)) == entry.Checksum {
			cands = append(cands, pruneCandidate{key, "prune"})
		} else {
			cands = append(cands, pruneCandidate{key, "keep_modified"})
		}
	}
	sort.Slice(cands, func(i, j int) bool { return cands[i].filename < cands[j].filename })
	sort.Strings(staleEntries)
	return cands, staleEntries, nil
}

// runLibsPrune removes local libs that were synced from the registry and
// have since been deleted there.
//
//	dservctl libs prune [--dir DIR] [--dry-run] [--yes]
//
// Pruned files are MOVED to <parent of DIR>/.lib_pruned/<timestamp>/, not
// deleted. Run it before the next `libs sync` on a rig after a registry
// delete: the Tcl ess_sync (GUI pull) still drops base entries for libs
// the registry no longer has, and without the entry prune cannot tell a
// synced copy from a hand-made file, so it leaves the file alone.
func runLibsPrune(cfg *Config, args []string) int {
	dir := "lib"
	dryRun, yes := false, false
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--dir", "-d":
			if i+1 >= len(args) {
				PrintError("--dir needs a directory")
				return 2
			}
			dir = args[i+1]
			i++
		case "--dry-run", "-n":
			dryRun = true
		case "--yes", "-y":
			yes = true
		default:
			PrintError("unknown option %q (usage: dservctl libs prune [--dir DIR] [--dry-run] [--yes])", args[i])
			return 2
		}
	}
	if !requireWorkgroup(cfg) {
		return 2
	}
	if refuseForeignTree("libs prune", dir,
		filterForeign(foreignWorkgroup(dir, cfg.Workgroup)), cfg.Workgroup) {
		return 1
	}

	client := NewRegistryClient(cfg)
	libs, err := client.ListLibs(cfg.Workgroup)
	if err != nil {
		PrintError("fetching libs: %v", err)
		return 1
	}
	// An empty list is far more likely a wrong workgroup or a bad response
	// than a registry with no libraries; pruning against it would remove
	// every synced lib.
	if len(libs) == 0 {
		PrintError("the registry lists no libs for workgroup %q; refusing to prune against an empty list", cfg.Workgroup)
		return 1
	}
	registry := map[string]bool{}
	for _, lib := range libs {
		registry[strVal(lib, "filename")] = true
	}

	m := readBaseManifest(dir, cfg.Workgroup)
	if len(m.Entries) == 0 {
		fmt.Printf("No sync record (%s) in %s; nothing can be pruned safely.\n", baseManifestFile, dir)
		return 0
	}
	cands, stale, err := planLibsPrune(dir, m, registry)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	var prune, keep []string
	for _, c := range cands {
		if c.action == "prune" {
			prune = append(prune, c.filename)
		} else {
			keep = append(keep, c.filename)
		}
	}
	if len(keep) > 0 {
		fmt.Printf("Kept, deleted from the registry but edited locally (%d):\n", len(keep))
		for _, f := range keep {
			fmt.Printf("  ! %s\n", f)
		}
	}
	if len(prune) == 0 {
		fmt.Println("Nothing to prune.")
		if len(stale) > 0 && !dryRun {
			for _, k := range stale {
				m.unset(k)
			}
			if err := writeBaseManifest(dir, m); err != nil {
				PrintError("updating %s: %v", baseManifestPath(dir), err)
				return 1
			}
		}
		return 0
	}

	absDir, _ := filepath.Abs(dir)
	dest := filepath.Join(filepath.Dir(absDir), ".lib_pruned", time.Now().Format("20060102-150405"))
	verb := "Pruning"
	if dryRun {
		verb = "Would prune"
	}
	fmt.Printf("%s %d lib(s) the registry no longer has (moved to %s):\n", verb, len(prune), dest)
	for _, f := range prune {
		fmt.Printf("  - %s\n", f)
	}
	if dryRun {
		return 0
	}
	if !yes {
		if !isTerminal() {
			PrintError("refusing to prune without confirmation; pass --yes (or --dry-run to preview)")
			return 1
		}
		fmt.Printf("Move these %d file(s) out of %s? [y/N] ", len(prune), dir)
		line, _ := bufio.NewReader(confirmIn).ReadString('\n')
		if a := strings.ToLower(strings.TrimSpace(line)); a != "y" && a != "yes" {
			fmt.Println("Nothing pruned.")
			return 1
		}
	}

	if err := os.MkdirAll(dest, 0755); err != nil {
		PrintError("creating %s: %v", dest, err)
		return 1
	}
	moved, errors := 0, 0
	for _, f := range prune {
		if err := os.Rename(filepath.Join(dir, f), filepath.Join(dest, f)); err != nil {
			PrintError("%s: %v", f, err)
			errors++
			continue
		}
		m.unset(f)
		moved++
	}
	for _, k := range stale {
		m.unset(k)
	}
	if err := writeBaseManifest(dir, m); err != nil {
		PrintError("updating %s: %v", baseManifestPath(dir), err)
		errors++
	}

	fmt.Printf("Pruned %d", moved)
	if errors > 0 {
		fmt.Printf(", %d error(s)", errors)
	}
	fmt.Println()
	if errors > 0 {
		return 1
	}
	return 0
}
