package main

import (
	"fmt"
	"io"
	"os"
	"strings"
	"time"
)

// runSystems lists ESS systems in a workgroup, or deletes a system.
func runSystems(cfg *Config, args []string) int {
	if len(args) > 0 && args[0] == "delete" {
		return runSystemDelete(cfg, args[1:])
	}

	wg := cfg.Workgroup
	if len(args) > 0 {
		wg = args[0]
	}
	if wg == "" {
		PrintError("workgroup required (use --workgroup or provide as argument)")
		return 2
	}

	client := NewRegistryClient(cfg)
	systems, err := client.ListSystems(wg)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(systems)
		return 0
	}

	if len(systems) == 0 {
		fmt.Printf("No systems found in workgroup %q.\n", wg)
		return 0
	}

	headers := []string{"NAME", "VERSION", "PROTOCOLS", "SCRIPTS", "UPDATED BY", "UPDATED"}
	var rows [][]string
	for _, sys := range systems {
		protos := ""
		if p, ok := sys["protocols"].([]interface{}); ok {
			names := make([]string, len(p))
			for i, v := range p {
				names[i] = fmt.Sprintf("%v", v)
			}
			protos = strings.Join(names, ", ")
		}
		rows = append(rows, []string{
			strVal(sys, "name"),
			strVal(sys, "version"),
			protos,
			fmt.Sprintf("%d", intVal(sys, "scriptCount")),
			strVal(sys, "updatedBy"),
			formatTime(strVal(sys, "updatedAt")),
		})
	}

	PrintTable(headers, rows)
	return 0
}

func runSystemDelete(cfg *Config, args []string) int {
	if len(args) == 0 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl systems delete <system> [--workgroup WG]\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}

	system := args[0]

	client := NewRegistryClient(cfg)
	result, err := client.DeleteSystem(cfg.Workgroup, system)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	scriptCount := intVal(result, "deletedScripts")
	fmt.Printf("Deleted system %q from workgroup %q (%d scripts removed)\n", system, cfg.Workgroup, scriptCount)
	return 0
}

// runScripts lists scripts for a system grouped by protocol.
func runScripts(cfg *Config, args []string) int {
	if len(args) == 0 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl scripts <system> [--version V]\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}

	system := args[0]
	version := ""
	for i := 1; i < len(args); i++ {
		if args[i] == "--version" && i+1 < len(args) {
			version = args[i+1]
			i++
		}
	}

	client := NewRegistryClient(cfg)
	result, err := client.GetScripts(cfg.Workgroup, system, version)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	// Scripts are grouped by protocol key
	headers := []string{"PROTOCOL", "TYPE", "FILENAME", "CHECKSUM", "UPDATED BY"}
	var rows [][]string
	for key, val := range result {
		if key == "system" || key == "version" || key == "workgroup" {
			continue
		}
		scripts, ok := val.([]interface{})
		if !ok {
			continue
		}
		proto := key
		if proto == "_system" {
			proto = "(system)"
		}
		for _, s := range scripts {
			script, ok := s.(map[string]interface{})
			if !ok {
				continue
			}
			checksum := strVal(script, "checksum")
			if len(checksum) > 8 {
				checksum = checksum[:8]
			}
			rows = append(rows, []string{
				proto,
				strVal(script, "type"),
				strVal(script, "filename"),
				checksum,
				strVal(script, "updatedBy"),
			})
		}
	}

	if len(rows) == 0 {
		fmt.Printf("No scripts found for system %q.\n", system)
		return 0
	}

	PrintTable(headers, rows)
	return 0
}

// runScript handles get/save/delete for a single script.
func runScript(cfg *Config, args []string) int {
	if len(args) == 0 {
		fmt.Fprintf(os.Stderr, "Usage:\n")
		fmt.Fprintf(os.Stderr, "  dservctl script get <system> <protocol> <type> [--version V] [--output FILE]\n")
		fmt.Fprintf(os.Stderr, "  dservctl script save <system> <protocol> <type> [--file FILE] [--comment MSG] [--version V]\n")
		fmt.Fprintf(os.Stderr, "  dservctl script delete <system> <protocol> <type>\n")
		return 2
	}

	switch args[0] {
	case "get":
		return runScriptGet(cfg, args[1:])
	case "save":
		return runScriptSave(cfg, args[1:])
	case "delete":
		return runScriptDelete(cfg, args[1:])
	default:
		PrintError("unknown script subcommand %q (use get, save, or delete)", args[0])
		return 2
	}
}

func runScriptGet(cfg *Config, args []string) int {
	if len(args) < 3 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl script get <system> <protocol> <type> [--version V] [--output FILE]\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}

	system, protocol, scriptType := args[0], args[1], args[2]
	version := ""
	output := ""
	for i := 3; i < len(args); i++ {
		switch args[i] {
		case "--version":
			if i+1 < len(args) {
				version = args[i+1]
				i++
			}
		case "--output", "-o":
			if i+1 < len(args) {
				output = args[i+1]
				i++
			}
		}
	}

	client := NewRegistryClient(cfg)
	result, err := client.GetScript(cfg.Workgroup, system, protocol, scriptType, version)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	content := strVal(result, "content")

	if output != "" {
		if err := os.WriteFile(output, []byte(content), 0644); err != nil {
			PrintError("writing file: %v", err)
			return 1
		}
		fmt.Fprintf(os.Stderr, "Written to %s\n", output)
	} else {
		fmt.Print(content)
	}

	return 0
}

func runScriptSave(cfg *Config, args []string) int {
	if len(args) < 3 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl script save <system> <protocol> <type> [--file FILE] [--comment MSG] [--version V]\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}
	if !requireUser(cfg) {
		return 2
	}

	system, protocol, scriptType := args[0], args[1], args[2]
	version := ""
	file := ""
	comment := ""
	for i := 3; i < len(args); i++ {
		switch args[i] {
		case "--version":
			if i+1 < len(args) {
				version = args[i+1]
				i++
			}
		case "--file", "-f":
			if i+1 < len(args) {
				file = args[i+1]
				i++
			}
		case "--comment", "-m":
			if i+1 < len(args) {
				comment = args[i+1]
				i++
			}
		}
	}

	// Read content from file or stdin
	var content []byte
	var err error
	if file != "" {
		content, err = os.ReadFile(file)
		if err != nil {
			PrintError("reading file: %v", err)
			return 1
		}
	} else {
		content, err = io.ReadAll(os.Stdin)
		if err != nil {
			PrintError("reading stdin: %v", err)
			return 1
		}
	}

	if len(content) == 0 {
		PrintError("no content to save (provide --file or pipe content to stdin)")
		return 2
	}

	client := NewRegistryClient(cfg)

	// Get current checksum for conflict detection
	expectedChecksum := ""
	existing, err := client.GetScript(cfg.Workgroup, system, protocol, scriptType, version)
	if err == nil && existing != nil {
		expectedChecksum = strVal(existing, "checksum")
	}

	// Save
	req := map[string]interface{}{
		"content":          string(content),
		"expectedChecksum": expectedChecksum,
		"updatedBy":        cfg.User,
		"comment":          comment,
	}

	result, err := client.SaveScript(cfg.Workgroup, system, protocol, scriptType, version, req)
	if err != nil {
		if strings.Contains(err.Error(), "conflict") {
			PrintError("conflict: script was modified by someone else. Fetch the latest version and retry.")
		} else {
			PrintError("%v", err)
		}
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	checksum := ""
	if result != nil {
		checksum = strVal(result, "checksum")
		if len(checksum) > 8 {
			checksum = checksum[:8]
		}
	}
	fmt.Printf("Saved %s/%s/%s (checksum: %s)\n", system, protocol, scriptType, checksum)
	return 0
}

func runScriptDelete(cfg *Config, args []string) int {
	if len(args) < 3 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl script delete <system> <protocol> <type>\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}

	system, protocol, scriptType := args[0], args[1], args[2]

	client := NewRegistryClient(cfg)
	_, err := client.DeleteScript(cfg.Workgroup, system, protocol, scriptType)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	fmt.Printf("Deleted %s/%s/%s\n", system, protocol, scriptType)
	return 0
}

// runHistory shows version history for a script.
func runHistory(cfg *Config, args []string) int {
	if len(args) < 3 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl history <system> <protocol> <type>\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}

	system, protocol, scriptType := args[0], args[1], args[2]

	client := NewRegistryClient(cfg)
	result, err := client.GetHistory(cfg.Workgroup, system, protocol, scriptType)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	history := extractList(result, "history")
	if len(history) == 0 {
		fmt.Println("No history found.")
		return 0
	}

	headers := []string{"#", "CHECKSUM", "SAVED BY", "SAVED AT", "COMMENT"}
	var rows [][]string
	for i, entry := range history {
		checksum := strVal(entry, "checksum")
		if len(checksum) > 8 {
			checksum = checksum[:8]
		}
		rows = append(rows, []string{
			fmt.Sprintf("%d", i+1),
			checksum,
			strVal(entry, "savedBy"),
			formatTime(strVal(entry, "savedAt")),
			strVal(entry, "comment"),
		})
	}

	PrintTable(headers, rows)
	return 0
}

// runTemplates lists, adds, or seeds templates.
func runTemplates(cfg *Config, args []string) int {
	if len(args) > 0 && args[0] == "add" {
		return runTemplateAdd(cfg, args[1:])
	}
	if len(args) > 0 && args[0] == "seed" {
		return runTemplateSeed(cfg, args[1:])
	}

	client := NewRegistryClient(cfg)
	templates, err := client.ListTemplates()
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(templates)
		return 0
	}

	if len(templates) == 0 {
		fmt.Println("No templates available.")
		return 0
	}

	headers := []string{"NAME", "VERSION", "DESCRIPTION", "PROTOCOLS", "SCRIPTS"}
	var rows [][]string
	for _, tmpl := range templates {
		protos := ""
		if p, ok := tmpl["protocols"].([]interface{}); ok {
			names := make([]string, len(p))
			for i, v := range p {
				names[i] = fmt.Sprintf("%v", v)
			}
			protos = strings.Join(names, ", ")
		}
		rows = append(rows, []string{
			strVal(tmpl, "name"),
			strVal(tmpl, "version"),
			strVal(tmpl, "description"),
			protos,
			fmt.Sprintf("%d", intVal(tmpl, "scriptCount")),
		})
	}

	PrintTable(headers, rows)
	return 0
}

func runTemplateAdd(cfg *Config, args []string) int {
	if len(args) == 0 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl templates add <template-name> [--workgroup WG]\n")
		return 2
	}
	if !requireWorkgroup(cfg) {
		return 2
	}
	if !requireUser(cfg) {
		return 2
	}

	templateName := args[0]

	client := NewRegistryClient(cfg)
	result, err := client.AddToWorkgroup(templateName, cfg.Workgroup, cfg.User)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	fmt.Printf("Added template %q to workgroup %q\n", templateName, cfg.Workgroup)
	return 0
}

func runTemplateSeed(cfg *Config, args []string) int {
	if len(args) == 0 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl templates seed <path> [system...]\n")
		fmt.Fprintf(os.Stderr, "\nSeeds templates from a filesystem directory into the _templates workgroup.\n")
		fmt.Fprintf(os.Stderr, "If no systems are specified, all found systems are imported.\n")
		return 2
	}

	sourcePath := args[0]
	systems := args[1:]

	client := NewRegistryClient(cfg)
	result, err := client.SeedTemplates(sourcePath, systems)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	if seeded, ok := result["systems"].([]interface{}); ok && len(seeded) > 0 {
		names := make([]string, len(seeded))
		for i, v := range seeded {
			names[i] = fmt.Sprintf("%v", v)
		}
		fmt.Printf("Seeded %d template(s): %s\n", len(names), strings.Join(names, ", "))
	} else {
		fmt.Println("No templates seeded.")
	}
	if libs, ok := result["libraries"].([]interface{}); ok && len(libs) > 0 {
		fmt.Printf("Imported %d library/libraries.\n", len(libs))
	}
	return 0
}

// runExport exports a workgroup or system as ZIP.
func runExport(cfg *Config, args []string) int {
	wg := cfg.Workgroup
	system := ""
	output := ""

	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "-o", "--output":
			if i+1 < len(args) {
				output = args[i+1]
				i++
			}
		default:
			if wg == "" {
				wg = args[i]
			} else if system == "" {
				system = args[i]
			}
		}
	}

	if wg == "" {
		PrintError("workgroup required")
		return 2
	}

	if output == "" {
		if system != "" {
			output = system + ".zip"
		} else {
			output = wg + ".zip"
		}
	}

	client := NewRegistryClient(cfg)
	data, err := client.ExportZip(wg, system)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if err := os.WriteFile(output, data, 0644); err != nil {
		PrintError("writing file: %v", err)
		return 1
	}

	fmt.Printf("Exported to %s (%d bytes)\n", output, len(data))
	return 0
}

// --- helpers ---

func formatTime(s string) string {
	if s == "" {
		return ""
	}
	t, err := time.Parse(time.RFC3339Nano, s)
	if err != nil {
		// Try other formats
		t, err = time.Parse(time.RFC3339, s)
		if err != nil {
			return s
		}
	}
	return t.Local().Format("2006-01-02 15:04")
}
