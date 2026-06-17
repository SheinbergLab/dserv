package main

import (
	"fmt"
	"os"
	"os/exec"
	"sort"
)

// Content (line-level) diffs of local scripts against the registry.
//
//	dservctl script  diff <system> <protocol> <type>   — one script
//	dservctl scripts diff <system>                      — every changed script
//
// Default comparison target is the registry's current published version
// (HEAD). With --base, the target is the recorded ancestor from
// .sync_base.json (fetched from history) — "what I changed since I last
// synced", ignoring changes others pushed.

// runScriptDiff handles `dservctl script diff <system> <protocol> <type>`.
func runScriptDiff(cfg *Config, args []string) int {
	if len(args) < 3 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl script diff <system> <protocol> <type> [--dir DIR] [--version V] [--base]\n")
		fmt.Fprintf(os.Stderr, "  (protocol \"_\" for system-level scripts; --base diffs vs the recorded ancestor)\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}
	system, protocol, stype := args[0], args[1], args[2]
	dir, version, useBase := ".", "", false
	for i := 3; i < len(args); i++ {
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
		case "--base":
			useBase = true
		}
	}

	client := NewRegistryClient(cfg)
	status, err := diffScriptContent(cfg, client, system, protocol, stype, dir, version, useBase)
	if err != nil {
		PrintError("%v", err)
		return 1
	}
	switch status {
	case "same":
		fmt.Println("No differences.")
	case "localmissing":
		fmt.Println("Local file not found (registry-only).")
	}
	return 0
}

// runScriptsDiff handles `dservctl scripts diff <system>` — a content diff of
// every script in the system, showing only the ones that differ.
func runScriptsDiff(cfg *Config, args []string) int {
	if len(args) < 1 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl scripts diff <system> [--dir DIR] [--version V] [--base]\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}
	system := args[0]
	dir, version, useBase := ".", "", false
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
		case "--base":
			useBase = true
		}
	}

	client := NewRegistryClient(cfg)
	manifest, err := client.GetManifest(cfg.Workgroup, system, version)
	if err != nil {
		PrintError("fetching manifest for %s: %v", system, err)
		return 1
	}
	if manifest == nil {
		PrintError("system %q not found", system)
		return 1
	}
	scripts := extractList(manifest, "scripts")
	sort.Slice(scripts, func(i, j int) bool {
		return localRelPath(strVal(scripts[i], "protocol"), fileOrType(scripts[i])) <
			localRelPath(strVal(scripts[j], "protocol"), fileOrType(scripts[j]))
	})

	differ, same, miss, errs := 0, 0, 0, 0
	for _, s := range scripts {
		protocol := strVal(s, "protocol")
		stype := strVal(s, "type")
		status, err := diffScriptContent(cfg, client, system, protocol, stype, dir, version, useBase)
		if err != nil {
			PrintError("%s/%s: %v", protocol, stype, err)
			errs++
			continue
		}
		switch status {
		case "diff":
			differ++
		case "same":
			same++
		case "localmissing":
			miss++
		}
	}

	fmt.Printf("\n%d changed, %d unchanged", differ, same)
	if miss > 0 {
		fmt.Printf(", %d registry-only", miss)
	}
	if errs > 0 {
		fmt.Printf(", %d error(s)", errs)
	}
	fmt.Println()
	if errs > 0 {
		return 1
	}
	return 0
}

// diffScriptContent fetches the registry content for one script and renders a
// unified diff against the local file. It prints the diff itself when they
// differ. Returns "diff", "same", or "localmissing".
func diffScriptContent(cfg *Config, client *AgentClient, system, protocol, stype, dir, version string, useBase bool) (string, error) {
	// HEAD also yields the filename (to locate the local file) and checksum.
	reg, err := client.GetScript(cfg.Workgroup, system, protocol, stype, version)
	if err != nil {
		return "", fmt.Errorf("fetching %s/%s from registry: %w", protocol, stype, err)
	}
	filename := strVal(reg, "filename")
	if filename == "" {
		filename = stype
	}
	headContent := strVal(reg, "content")
	headChecksum := strVal(reg, "checksum")

	relPath := localRelPath(protocol, filename)
	localPath := localFilePath(dir, protocol, filename)
	localBytes, err := os.ReadFile(localPath)
	if err != nil {
		return "localmissing", nil
	}

	leftContent := headContent
	leftLabel := fmt.Sprintf("registry:%s/%s (HEAD)", system, relPath)

	if useBase {
		base := readBaseManifest(dir, cfg.Workgroup)
		baseChecksum := base.get(relPath)
		if baseChecksum == "" {
			return "", fmt.Errorf("%s: no recorded base (sync or push it first)", relPath)
		}
		if baseChecksum != headChecksum {
			c, ok := historyContent(client, cfg.Workgroup, system, protocol, stype, baseChecksum)
			if !ok {
				return "", fmt.Errorf("%s: base version %s not found in registry history", relPath, shortSum(baseChecksum))
			}
			leftContent = c
		}
		leftLabel = fmt.Sprintf("registry:%s/%s (base %s)", system, relPath, shortSum(baseChecksum))
	}

	if hashBytes([]byte(leftContent)) == hashBytes(localBytes) {
		return "same", nil
	}

	renderUnifiedDiff(leftContent, string(localBytes), leftLabel, "local:"+localPath)
	return "diff", nil
}

// historyContent locates the content for a specific checksum among a script's
// current version and its history entries.
func historyContent(client *AgentClient, workgroup, system, protocol, stype, checksum string) (string, bool) {
	result, err := client.GetHistory(workgroup, system, protocol, stype)
	if err != nil {
		return "", false
	}
	if s, ok := result["script"].(map[string]interface{}); ok {
		if strVal(s, "checksum") == checksum {
			return strVal(s, "content"), true
		}
	}
	for _, h := range extractList(result, "history") {
		if strVal(h, "checksum") == checksum {
			return strVal(h, "content"), true
		}
	}
	return "", false
}

// renderUnifiedDiff writes both sides to temp files and shells out to the
// system `diff -u`. Falls back to a checksum summary if `diff` is absent
// (e.g. a Windows box without it).
func renderUnifiedDiff(left, right, leftLabel, rightLabel string) {
	lf, err1 := os.CreateTemp("", "dservctl-reg-*.txt")
	rf, err2 := os.CreateTemp("", "dservctl-local-*.txt")
	if err1 != nil || err2 != nil {
		PrintError("creating temp files for diff")
		return
	}
	defer os.Remove(lf.Name())
	defer os.Remove(rf.Name())
	lf.WriteString(left)
	lf.Close()
	rf.WriteString(right)
	rf.Close()

	diffBin, err := exec.LookPath("diff")
	if err != nil {
		fmt.Printf("(system 'diff' not found; showing checksums)\n")
		fmt.Printf("  %s: %s\n", leftLabel, shortSum(hashBytes([]byte(left))))
		fmt.Printf("  %s: %s\n", rightLabel, shortSum(hashBytes([]byte(right))))
		return
	}

	// -L labels work on both BSD (macOS) and GNU diff.
	cmd := exec.Command(diffBin, "-u", "-L", leftLabel, "-L", rightLabel, lf.Name(), rf.Name())
	out, _ := cmd.Output() // diff exits 1 when files differ — expected, not an error
	fmt.Print(string(out))
}

func shortSum(cs string) string {
	if len(cs) > 8 {
		return cs[:8]
	}
	return cs
}
