package main

import (
	"fmt"
	"os"
	"regexp"
	"strings"
)

// pkgRequireRe matches `package require <name> [version]` lines, capturing the
// package name. Used to auto-detect which workgroup libs a system depends on.
var pkgRequireRe = regexp.MustCompile(`(?m)^\s*package\s+require\s+(?:-exact\s+)?([A-Za-z0-9_:.-]+)`)

// runClone copies a system — or a subset of its protocols — from one workgroup
// into another. The TARGET workgroup is the configured workgroup (-w / config);
// the SOURCE workgroup is given with --from-workgroup.
//
// Behavior depends on whether the target system already exists:
//   - missing  → create it (CloneSystem), optionally filtered to --protocol(s)
//   - exists   → add the named --protocol(s) into it (CloneProtocol)
//
// Usage:
//
//	dservctl clone <system> --from-workgroup SRC [--as NAME]
//	    [--from-system NAME] [--protocol P | --protocols P1,P2]
//	    [--with-libs] [--dry-run]
func runClone(cfg *Config, args []string) int {
	if len(args) < 1 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl clone <system> --from-workgroup SRC [--as NAME] [--protocol P | --protocols P1,P2] [--with-libs] [--dry-run]\n")
		return 2
	}
	if !requireWorkgroup(cfg) { // target workgroup
		return 2
	}
	if !requireUser(cfg) {
		return 2
	}

	sourceSystem := strings.TrimRight(args[0], "/")
	fromWG := ""
	asName := ""
	var protocols []string
	withLibs := false
	dryRun := false

	for i := 1; i < len(args); i++ {
		switch args[i] {
		case "--from-workgroup", "--from-wg":
			if i+1 < len(args) {
				fromWG = args[i+1]
				i++
			}
		case "--from-system":
			if i+1 < len(args) {
				sourceSystem = strings.TrimRight(args[i+1], "/")
				i++
			}
		case "--as":
			if i+1 < len(args) {
				asName = args[i+1]
				i++
			}
		case "--protocol", "--protocols":
			if i+1 < len(args) {
				for _, p := range strings.Split(args[i+1], ",") {
					if p = strings.TrimSpace(p); p != "" {
						protocols = append(protocols, p)
					}
				}
				i++
			}
		case "--with-libs":
			withLibs = true
		case "--dry-run", "-n":
			dryRun = true
		default:
			PrintError("unknown flag %q", args[i])
			return 2
		}
	}

	if fromWG == "" {
		PrintError("--from-workgroup is required")
		return 2
	}

	targetName := sourceSystem
	if asName != "" {
		targetName = asName
	}

	if fromWG == cfg.Workgroup && targetName == sourceSystem {
		PrintError("source and target are identical (%s/%s); use --as to clone under a new name or -w to target another workgroup", fromWG, sourceSystem)
		return 2
	}

	client := NewRegistryClient(cfg)

	// Does the target system already exist in the target workgroup? A
	// "not found" error is the normal create case; any other error (network,
	// auth) aborts. Note: the HTTP layer returns a non-nil map even on 404, so
	// we must key off the error, not the map.
	targetExists := true
	if _, err := client.GetManifest(cfg.Workgroup, targetName, ""); err != nil {
		if strings.Contains(strings.ToLower(err.Error()), "not found") {
			targetExists = false
		} else {
			PrintError("checking target system %q: %v", targetName, err)
			return 1
		}
	}

	rc := 0
	if !targetExists {
		rc = cloneNewSystem(cfg, client, fromWG, sourceSystem, targetName, protocols, dryRun)
	} else {
		rc = cloneIntoExisting(cfg, client, fromWG, sourceSystem, targetName, protocols, dryRun)
	}
	if rc != 0 {
		return rc
	}

	if withLibs {
		copyRequiredLibs(cfg, client, fromWG, sourceSystem, protocols, dryRun)
	}
	return 0
}

// cloneNewSystem creates the target system from the source, optionally
// restricted to a protocol subset.
func cloneNewSystem(cfg *Config, client *AgentClient, fromWG, sourceSystem, targetName string, protocols []string, dryRun bool) int {
	scope := "all protocols"
	if len(protocols) > 0 {
		scope = "protocol(s) " + strings.Join(protocols, ", ")
	}
	if dryRun {
		fmt.Printf("Would create system %q in %q from %s/%s (%s)\n",
			targetName, cfg.Workgroup, fromWG, sourceSystem, scope)
		return 0
	}

	result, err := client.CloneSystem(cfg.Workgroup, targetName, fromWG, sourceSystem, protocols, cfg.User)
	if err != nil {
		PrintError("cloning system: %v", err)
		return 1
	}
	r := scaffoldResult(result)
	fmt.Printf("Created %s in %s: %d script(s)", targetName, cfg.Workgroup, intVal(r, "scripts"))
	if ff := strVal(r, "forkedFrom"); ff != "" {
		fmt.Printf(" (forked from %s)", ff)
	}
	fmt.Println()
	return 0
}

// cloneIntoExisting adds protocol(s) from the source into an already-existing
// target system. Requires --protocol(s).
func cloneIntoExisting(cfg *Config, client *AgentClient, fromWG, sourceSystem, targetName string, protocols []string, dryRun bool) int {
	if len(protocols) == 0 {
		PrintError("system %q already exists in %q; pass --protocol to add protocol(s), or --as to clone under a new name", targetName, cfg.Workgroup)
		return 1
	}

	added := 0
	errors := 0
	for _, p := range protocols {
		if dryRun {
			fmt.Printf("Would add protocol %q to %s/%s from %s/%s\n", p, cfg.Workgroup, targetName, fromWG, sourceSystem)
			added++
			continue
		}
		_, err := client.CloneProtocol(cfg.Workgroup, targetName, p, p, fromWG, sourceSystem, cfg.User)
		if err != nil {
			PrintError("%s: %v", p, err)
			errors++
			continue
		}
		added++
		fmt.Printf("  + protocol %s\n", p)
	}
	if !dryRun {
		fmt.Printf("Added %d protocol(s) to %s", added, targetName)
		if errors > 0 {
			fmt.Printf(", %d error(s)", errors)
		}
		fmt.Println()
	}
	if errors > 0 {
		return 1
	}
	return 0
}

// copyRequiredLibs scans the source system's scripts for `package require`
// directives, matches them against the source workgroup's libs, and copies the
// matches into the target workgroup. Requires that don't correspond to a source
// workgroup lib (platform libs like ess/dlsh) are ignored.
func copyRequiredLibs(cfg *Config, client *AgentClient, fromWG, sourceSystem string, protocols []string, dryRun bool) {
	resp, err := client.GetScripts(fromWG, sourceSystem, "")
	if err != nil {
		PrintError("scanning libs: fetching source scripts: %v", err)
		return
	}
	grouped, _ := resp["scripts"].(map[string]interface{})

	// When cloning a protocol subset, only scan system-level + selected protocols.
	var want map[string]bool
	if len(protocols) > 0 {
		want = map[string]bool{"_system": true}
		for _, p := range protocols {
			want[p] = true
		}
	}

	required := map[string]bool{}
	for key, v := range grouped {
		if want != nil && !want[key] {
			continue
		}
		items, _ := v.([]interface{})
		for _, it := range items {
			s, _ := it.(map[string]interface{})
			content := strVal(s, "content")
			for _, m := range pkgRequireRe.FindAllStringSubmatch(content, -1) {
				required[m[1]] = true
			}
		}
	}
	if len(required) == 0 {
		if cfg.Verbose {
			fmt.Println("Libs: no 'package require' directives found.")
		}
		return
	}

	srcLibs, err := client.ListLibs(fromWG)
	if err != nil {
		PrintError("scanning libs: listing source libs: %v", err)
		return
	}
	tgtLibs, _ := client.ListLibs(cfg.Workgroup)
	tgtHave := map[string]string{} // name@version -> checksum
	for _, l := range tgtLibs {
		tgtHave[strVal(l, "name")+"@"+strVal(l, "version")] = strVal(l, "checksum")
	}

	copied, skipped, errors := 0, 0, 0
	matched := map[string]bool{}
	for _, l := range srcLibs {
		name := strVal(l, "name")
		if !required[name] {
			continue
		}
		matched[name] = true
		ver := strVal(l, "version")
		if tc, ok := tgtHave[name+"@"+ver]; ok && tc == strVal(l, "checksum") {
			skipped++
			continue
		}
		if dryRun {
			fmt.Printf("Would copy lib %s-%s from %s\n", name, ver, fromWG)
			copied++
			continue
		}
		libData, err := client.GetLib(fromWG, name, ver)
		if err != nil {
			PrintError("lib %s: %v", name, err)
			errors++
			continue
		}
		_, err = client.SaveLib(cfg.Workgroup, name, ver, map[string]interface{}{
			"content":   strVal(libData, "content"),
			"updatedBy": cfg.User,
		})
		if err != nil {
			PrintError("lib %s: %v", name, err)
			errors++
			continue
		}
		copied++
		fmt.Printf("  + lib %s-%s\n", name, ver)
	}

	fmt.Printf("Libs: %d copied, %d already present", copied, skipped)
	if errors > 0 {
		fmt.Printf(", %d error(s)", errors)
	}
	fmt.Println()

	// Surface requires that weren't workgroup libs — likely platform deps the
	// target must already provide. Informational only.
	if cfg.Verbose {
		var external []string
		for name := range required {
			if !matched[name] {
				external = append(external, name)
			}
		}
		if len(external) > 0 {
			fmt.Printf("  (not workgroup libs, assumed available on target: %s)\n", strings.Join(external, ", "))
		}
	}
}

// scaffoldResult unwraps the {"success":true,"result":{...}} envelope returned
// by the scaffold endpoints, falling back to the top-level map.
func scaffoldResult(result map[string]interface{}) map[string]interface{} {
	if r, ok := result["result"].(map[string]interface{}); ok {
		return r
	}
	return result
}
