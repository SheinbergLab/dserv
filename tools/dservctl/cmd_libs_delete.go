package main

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// libTarget is one registry library version selected for deletion.
type libTarget struct {
	name     string
	version  string
	filename string
}

// resolveLibTargets selects registry libs by explicit names and/or globs.
//
// An explicit argument is either a registry filename ("name-1.0.tm") or a
// bare library name. A bare name must have exactly one version in the
// registry; with several, the filename is required so a delete never picks
// a version for you. Each --match glob is tested against both the filename
// and the name (filepath.Match syntax: * ? [..]). Every explicit argument
// and every glob must select something, so a typo fails instead of quietly
// deleting less than intended. The result is de-duplicated and sorted.
func resolveLibTargets(libs []map[string]interface{}, names, globs []string) ([]libTarget, error) {
	byFile := map[string]libTarget{}
	byName := map[string][]libTarget{}
	for _, lib := range libs {
		t := libTarget{name: strVal(lib, "name"), version: strVal(lib, "version"), filename: strVal(lib, "filename")}
		if t.filename == "" {
			t.filename = fmt.Sprintf("%s-%s.tm", t.name, t.version)
		}
		byFile[t.filename] = t
		byName[t.name] = append(byName[t.name], t)
	}

	picked := map[string]libTarget{}
	var problems []string

	for _, arg := range names {
		if t, ok := byFile[arg]; ok {
			picked[t.filename] = t
			continue
		}
		vers := byName[strings.TrimSuffix(arg, ".tm")]
		switch len(vers) {
		case 0:
			problems = append(problems, fmt.Sprintf("no library %q in the registry", arg))
		case 1:
			picked[vers[0].filename] = vers[0]
		default:
			var files []string
			for _, v := range vers {
				files = append(files, v.filename)
			}
			sort.Strings(files)
			problems = append(problems, fmt.Sprintf("%q has %d versions; name one: %s",
				arg, len(vers), strings.Join(files, ", ")))
		}
	}

	for _, g := range globs {
		if _, err := filepath.Match(g, ""); err != nil {
			problems = append(problems, fmt.Sprintf("bad pattern %q: %v", g, err))
			continue
		}
		n := 0
		for file, t := range byFile {
			mf, _ := filepath.Match(g, file)
			mn, _ := filepath.Match(g, t.name)
			if mf || mn {
				picked[file] = t
				n++
			}
		}
		if n == 0 {
			problems = append(problems, fmt.Sprintf("pattern %q matches no library", g))
		}
	}

	if len(problems) > 0 {
		return nil, fmt.Errorf("%s", strings.Join(problems, "\n  "))
	}
	out := make([]libTarget, 0, len(picked))
	for _, t := range picked {
		out = append(out, t)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].filename < out[j].filename })
	return out, nil
}

// confirmIn / isTerminal are swapped in tests.
var confirmIn io.Reader = os.Stdin
var isTerminal = func() bool {
	fi, err := os.Stdin.Stat()
	return err == nil && fi.Mode()&os.ModeCharDevice != 0
}

// runLibsDelete deletes library versions from the registry.
//
//	dservctl libs delete [--dry-run] [--yes] [--dir DIR] [--match GLOB]... [NAME | NAME-VER.tm]...
//
// The registry keeps no library history, so the delete is permanent; run
// `dservctl backup create` first. Without --yes it lists what will go and
// asks; with no terminal to ask on, it refuses. --dir drops the deleted
// files from that directory's .sync_base.json so `libs status` stops
// listing them. Local files are never touched (libs sync never removes
// them either), so rigs keep their copies until someone deletes them.
func runLibsDelete(cfg *Config, args []string) int {
	var names, globs []string
	dir := ""
	dryRun, yes := false, false
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--match", "-m":
			if i+1 >= len(args) {
				PrintError("--match needs a pattern")
				return 2
			}
			globs = append(globs, args[i+1])
			i++
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
			if strings.HasPrefix(args[i], "-") {
				PrintError("unknown option %q", args[i])
				return 2
			}
			names = append(names, args[i])
		}
	}
	if len(names) == 0 && len(globs) == 0 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl libs delete [--dry-run] [--yes] [--dir DIR] [--match GLOB]... [NAME | NAME-VER.tm]...\n")
		fmt.Fprintf(os.Stderr, "  Deletes library versions from the registry (permanent: run `dservctl backup create` first).\n")
		fmt.Fprintf(os.Stderr, "  e.g. dservctl libs delete --dry-run --match 'planko_bank_*'\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}
	if dir != "" && refuseForeignTree("libs delete", dir,
		filterForeign(foreignWorkgroup(dir, cfg.Workgroup)), cfg.Workgroup) {
		return 1
	}

	client := NewRegistryClient(cfg)
	libs, err := client.ListLibs(cfg.Workgroup)
	if err != nil {
		PrintError("fetching libs: %v", err)
		return 1
	}
	targets, err := resolveLibTargets(libs, names, globs)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	verb := "Deleting"
	if dryRun {
		verb = "Would delete"
	}
	fmt.Printf("%s %d lib(s) from workgroup %q:\n", verb, len(targets), cfg.Workgroup)
	for _, t := range targets {
		fmt.Printf("  - %s\n", t.filename)
	}
	if dryRun {
		return 0
	}

	if !yes {
		if !isTerminal() {
			PrintError("refusing to delete without confirmation; pass --yes (or --dry-run to preview)")
			return 1
		}
		fmt.Printf("Permanently delete these %d lib(s) from the registry? [y/N] ", len(targets))
		line, _ := bufio.NewReader(confirmIn).ReadString('\n')
		if a := strings.ToLower(strings.TrimSpace(line)); a != "y" && a != "yes" {
			fmt.Println("Nothing deleted.")
			return 1
		}
	}

	deleted, errors := 0, 0
	var gone []string
	for _, t := range targets {
		if _, err := client.DeleteLib(cfg.Workgroup, t.name, t.version); err != nil {
			PrintError("%s: %v", t.filename, err)
			errors++
			continue
		}
		deleted++
		gone = append(gone, t.filename)
		if cfg.Verbose {
			fmt.Printf("  ✗ %s\n", t.filename)
		}
	}

	if dir != "" && len(gone) > 0 {
		m := readBaseManifest(dir, cfg.Workgroup)
		for _, f := range gone {
			m.unset(f)
		}
		if err := writeBaseManifest(dir, m); err != nil {
			PrintError("updating %s: %v", baseManifestPath(dir), err)
			errors++
		}
		var still []string
		for _, f := range gone {
			if _, err := os.Stat(filepath.Join(dir, f)); err == nil {
				still = append(still, f)
			}
		}
		if len(still) > 0 {
			fmt.Printf("Still present locally in %s (not removed): %s\n", dir, strings.Join(still, ", "))
		}
	}

	fmt.Printf("Deleted %d", deleted)
	if errors > 0 {
		fmt.Printf(", %d error(s)", errors)
	}
	fmt.Println()
	if errors > 0 {
		return 1
	}
	return 0
}
