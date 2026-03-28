package main

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"time"
)

// runConfigs handles the "configs" subcommand for managing project configs/queues.
//
// Usage:
//
//	dservctl configs list                    List projects on registry
//	dservctl configs status [project]        Show sync status for a project
//	dservctl configs history [project]       Show push history for a project
//	dservctl configs push [project]          Push project via dserv configs subprocess
//	dservctl configs pull [project]          Pull project via dserv configs subprocess
func runConfigs(cfg *Config, args []string) int {
	if len(args) < 1 {
		printConfigsUsage()
		return 2
	}

	subcmd := args[0]
	rest := args[1:]

	switch subcmd {
	case "list":
		return configsList(cfg)
	case "show":
		return configsShow(cfg, rest)
	case "get":
		return configsGet(cfg, rest)
	case "status":
		return configsStatus(cfg, rest)
	case "history":
		return configsHistory(cfg, rest)
	case "local-get":
		return configsLocalGet(cfg, rest)
	case "push":
		return configsPush(cfg, rest)
	case "pull":
		return configsPull(cfg, rest)
	default:
		PrintError("unknown configs subcommand: %s", subcmd)
		printConfigsUsage()
		return 2
	}
}

func printConfigsUsage() {
	fmt.Fprintf(os.Stderr, "Usage: dservctl configs <subcommand>\n\n")
	fmt.Fprintf(os.Stderr, "Subcommands:\n")
	fmt.Fprintf(os.Stderr, "  list                    List projects on registry\n")
	fmt.Fprintf(os.Stderr, "  show <project>          List configs in a project (from registry)\n")
	fmt.Fprintf(os.Stderr, "  get <project> <config>  Show full config details (from registry)\n")
	fmt.Fprintf(os.Stderr, "  local-get <config>      Show config from local dserv DB\n")
	fmt.Fprintf(os.Stderr, "  status [project]        Show sync status for a project\n")
	fmt.Fprintf(os.Stderr, "  history [project]       Show push history for a project\n")
	fmt.Fprintf(os.Stderr, "  push [project]          Push project to registry (via dserv)\n")
	fmt.Fprintf(os.Stderr, "  pull [project]          Pull project from registry (via dserv)\n")
}

// configsList lists projects on the registry.
func configsList(cfg *Config) int {
	if !requireWorkgroup(cfg) {
		return 2
	}

	client := NewRegistryClient(cfg)
	projects, err := client.ListProjectDefs(cfg.Workgroup)
	if err != nil {
		PrintError("failed to list projects: %v", err)
		return 1
	}

	if len(projects) == 0 {
		fmt.Println("No projects found")
		return 0
	}

	if cfg.JSON {
		data, _ := json.MarshalIndent(projects, "", "  ")
		fmt.Println(string(data))
		return 0
	}

	headers := []string{"Name", "Description", "Systems", "Updated"}
	var rows [][]string
	for _, p := range projects {
		name, _ := p["name"].(string)
		desc, _ := p["description"].(string)
		systems := ""
		if s, ok := p["systems"].([]interface{}); ok {
			for i, sys := range s {
				if i > 0 {
					systems += ", "
				}
				systems += fmt.Sprintf("%v", sys)
			}
		}
		updated := ""
		if ts, ok := p["updatedAt"].(float64); ok && ts > 0 {
			updated = time.Unix(int64(ts), 0).Format("2006-01-02 15:04")
		}
		rows = append(rows, []string{name, desc, systems, updated})
	}

	PrintTable(headers, rows)
	return 0
}

// configsShow lists configs in a project directly from the registry.
func configsShow(cfg *Config, args []string) int {
	if !requireWorkgroup(cfg) {
		return 2
	}

	project := ""
	if len(args) > 0 {
		project = args[0]
	}
	if project == "" {
		PrintError("project name required: dservctl configs show <project>")
		return 2
	}

	client := NewRegistryClient(cfg)
	configs, err := client.ListRegistryConfigs(cfg.Workgroup, project)
	if err != nil {
		PrintError("failed to list configs: %v", err)
		return 1
	}

	if len(configs) == 0 {
		fmt.Println("No configs found")
		return 0
	}

	if cfg.JSON {
		data, _ := json.MarshalIndent(configs, "", "  ")
		fmt.Println(string(data))
		return 0
	}

	headers := []string{"Name", "System", "Protocol", "Variant", "Subject", "VariantArgs", "Params"}
	var rows [][]string
	for _, c := range configs {
		name, _ := c["name"].(string)
		system, _ := c["system"].(string)
		protocol, _ := c["protocol"].(string)
		variant, _ := c["variant"].(string)
		subject, _ := c["subject"].(string)

		va := summarizeMap(c["variantArgs"])
		pa := summarizeMap(c["params"])

		rows = append(rows, []string{name, system, protocol, variant, subject, va, pa})
	}

	PrintTable(headers, rows)
	return 0
}

// configsGet shows full details of a single config from the registry.
func configsGet(cfg *Config, args []string) int {
	if !requireWorkgroup(cfg) {
		return 2
	}

	if len(args) < 2 {
		PrintError("usage: dservctl configs get <project> <config>")
		return 2
	}

	project := args[0]
	configName := args[1]

	client := NewRegistryClient(cfg)
	config, err := client.GetRegistryConfig(cfg.Workgroup, project, configName)
	if err != nil {
		PrintError("failed to get config: %v", err)
		return 1
	}

	if cfg.JSON {
		data, _ := json.MarshalIndent(config, "", "  ")
		fmt.Println(string(data))
		return 0
	}

	// Pretty-print key fields
	fmt.Printf("Config: %s/%s/%s\n", cfg.Workgroup, project, configName)
	fmt.Println()

	printField("System", config["system"])
	printField("Protocol", config["protocol"])
	printField("Variant", config["variant"])
	printField("Subject", config["subject"])
	printField("Script Source", config["scriptSource"])
	printField("File Template", config["fileTemplate"])
	printField("Description", config["description"])

	fmt.Println()
	fmt.Println("Variant Args:")
	printMapField(config["variantArgs"])

	fmt.Println()
	fmt.Println("Params:")
	printMapField(config["params"])

	if tags, ok := config["tags"].([]interface{}); ok && len(tags) > 0 {
		fmt.Println()
		fmt.Printf("Tags: ")
		for i, t := range tags {
			if i > 0 {
				fmt.Print(", ")
			}
			fmt.Print(t)
		}
		fmt.Println()
	}

	return 0
}

// summarizeMap returns a compact key=val summary of a JSON object or Tcl dict string.
func summarizeMap(v interface{}) string {
	switch val := v.(type) {
	case map[string]interface{}:
		if len(val) == 0 {
			return "{}"
		}
		result := ""
		for k, v := range val {
			if result != "" {
				result += ", "
			}
			result += fmt.Sprintf("%s=%v", k, v)
		}
		return result
	case string:
		pairs := parseTclDict(val)
		if len(pairs) == 0 {
			return "{}"
		}
		result := ""
		for _, kv := range pairs {
			if result != "" {
				result += ", "
			}
			result += fmt.Sprintf("%s=%s", kv[0], kv[1])
		}
		return result
	default:
		return "{}"
	}
}

func printField(label string, v interface{}) {
	s, _ := v.(string)
	if s != "" {
		fmt.Printf("  %-16s %s\n", label+":", s)
	}
}

func printMapField(v interface{}) {
	switch val := v.(type) {
	case map[string]interface{}:
		if len(val) == 0 {
			fmt.Println("  (none)")
			return
		}
		for k, v := range val {
			fmt.Printf("  %-20s %v\n", k+":", v)
		}
	case string:
		// Handle Tcl dict strings like "key1 val1 key2 val2"
		m := parseTclDict(val)
		if len(m) == 0 {
			fmt.Println("  (none)")
			return
		}
		for _, kv := range m {
			fmt.Printf("  %-20s %s\n", kv[0]+":", kv[1])
		}
	default:
		fmt.Println("  (none)")
	}
}

// parseTclDict parses a simple Tcl dict string "key1 val1 key2 val2" into ordered pairs.
// Handles braced values like "key {val with spaces}".
func parseTclDict(s string) [][2]string {
	s = strings.TrimSpace(s)
	if s == "" {
		return nil
	}

	var pairs [][2]string
	tokens := tclTokenize(s)
	for i := 0; i+1 < len(tokens); i += 2 {
		pairs = append(pairs, [2]string{tokens[i], tokens[i+1]})
	}
	return pairs
}

// tclTokenize splits a Tcl-style string into words, respecting braces.
func tclTokenize(s string) []string {
	var tokens []string
	i := 0
	for i < len(s) {
		// Skip whitespace
		for i < len(s) && (s[i] == ' ' || s[i] == '\t') {
			i++
		}
		if i >= len(s) {
			break
		}
		if s[i] == '{' {
			// Braced word
			depth := 1
			start := i + 1
			i++
			for i < len(s) && depth > 0 {
				if s[i] == '{' {
					depth++
				} else if s[i] == '}' {
					depth--
				}
				i++
			}
			tokens = append(tokens, s[start:i-1])
		} else {
			// Bare word
			start := i
			for i < len(s) && s[i] != ' ' && s[i] != '\t' {
				i++
			}
			tokens = append(tokens, s[start:i])
		}
	}
	return tokens
}

// configsStatus shows sync status for a project.
func configsStatus(cfg *Config, args []string) int {
	if !requireWorkgroup(cfg) {
		return 2
	}

	project := ""
	if len(args) > 0 {
		project = args[0]
	}
	if project == "" {
		PrintError("project name required: dservctl configs status <project>")
		return 2
	}

	client := NewRegistryClient(cfg)
	status, err := client.GetBundleStatus(cfg.Workgroup, project)
	if err != nil {
		PrintError("failed to get status: %v", err)
		return 1
	}

	if cfg.JSON {
		data, _ := json.MarshalIndent(status, "", "  ")
		fmt.Println(string(data))
		return 0
	}

	fmt.Printf("Project: %s/%s\n", cfg.Workgroup, project)

	if ts, ok := status["updatedAt"].(float64); ok && ts > 0 {
		fmt.Printf("  Updated:     %s\n", time.Unix(int64(ts), 0).Format("2006-01-02 15:04:05"))
	}
	if ts, ok := status["lastPushedAt"].(float64); ok && ts > 0 {
		fmt.Printf("  Last push:   %s\n", time.Unix(int64(ts), 0).Format("2006-01-02 15:04:05"))
		if by, ok := status["lastPushedBy"].(string); ok && by != "" {
			fmt.Printf("  Pushed by:   %s\n", by)
		}
		if rig, ok := status["sourceRig"].(string); ok && rig != "" {
			fmt.Printf("  Source rig:  %s\n", rig)
		}
	} else {
		fmt.Println("  Last push:   never")
	}

	return 0
}

// configsHistory shows push history for a project.
func configsHistory(cfg *Config, args []string) int {
	if !requireWorkgroup(cfg) {
		return 2
	}

	project := ""
	limit := 10
	if len(args) > 0 {
		project = args[0]
	}
	if project == "" {
		PrintError("project name required: dservctl configs history <project>")
		return 2
	}
	for i := 1; i < len(args); i++ {
		if args[i] == "--limit" && i+1 < len(args) {
			fmt.Sscanf(args[i+1], "%d", &limit)
			i++
		}
	}

	client := NewRegistryClient(cfg)
	// The endpoint returns a JSON array, so use GetRaw
	path := fmt.Sprintf("%s/bundle-history/%s/%s?limit=%d", registryBase, cfg.Workgroup, project, limit)
	data, _, err := client.GetRaw(path)
	if err != nil {
		PrintError("failed to get history: %v", err)
		return 1
	}

	var entries []map[string]interface{}
	if err := json.Unmarshal(data, &entries); err != nil {
		PrintError("failed to parse history: %v", err)
		return 1
	}

	if cfg.JSON {
		out, _ := json.MarshalIndent(entries, "", "  ")
		fmt.Println(string(out))
		return 0
	}

	if len(entries) == 0 {
		fmt.Println("No push history")
		return 0
	}

	headers := []string{"ID", "Pushed By", "Source Rig", "Pushed At"}
	var rows [][]string
	for _, e := range entries {
		id := fmt.Sprintf("%.0f", e["id"])
		by, _ := e["pushedBy"].(string)
		rig, _ := e["sourceRig"].(string)
		pushed := ""
		if ts, ok := e["pushedAt"].(float64); ok && ts > 0 {
			pushed = time.Unix(int64(ts), 0).Format("2006-01-02 15:04:05")
		}
		rows = append(rows, []string{id, by, rig, pushed})
	}

	PrintTable(headers, rows)
	return 0
}

// configsLocalGet queries a config from the local dserv configs DB via TCP.
func configsLocalGet(cfg *Config, args []string) int {
	if len(args) < 1 {
		PrintError("usage: dservctl configs local-get <config-name> [--project <project>]")
		return 2
	}

	configName := args[0]
	project := ""
	for i := 1; i < len(args); i++ {
		if (args[i] == "--project" || args[i] == "-p") && i+1 < len(args) {
			project = args[i+1]
			i++
		}
	}

	// Build the Tcl command for the configs subprocess
	var tclCmd string
	if project != "" {
		tclCmd = fmt.Sprintf("config_get_json %s -project %s", configName, project)
	} else {
		tclCmd = fmt.Sprintf("config_get_json %s", configName)
	}

	// Send directly to dserv via TCP, targeting the configs subprocess
	resp, err := SendToInterp(cfg.Host, "configs", tclCmd)
	if err != nil {
		PrintError("failed to get local config: %v", err)
		return 1
	}

	resp = strings.TrimSpace(resp)
	if resp == "" || resp == "{}" {
		PrintError("config not found: %s", configName)
		return 1
	}

	// Try to parse as JSON
	var config map[string]interface{}
	if err := json.Unmarshal([]byte(resp), &config); err != nil {
		// Not valid JSON — print raw response (might be Tcl dict or error)
		fmt.Println(resp)
		return 0
	}

	if cfg.JSON {
		data, _ := json.MarshalIndent(config, "", "  ")
		fmt.Println(string(data))
		return 0
	}

	fmt.Printf("Local Config: %s\n\n", configName)
	printField("System", config["system"])
	printField("Protocol", config["protocol"])
	printField("Variant", config["variant"])
	printField("Subject", config["subject"])
	printField("Script Source", config["script_source"])
	printField("File Template", config["file_template"])

	fmt.Println()
	fmt.Println("Variant Args:")
	printMapField(config["variant_args"])

	fmt.Println()
	fmt.Println("Params:")
	printMapField(config["params"])

	return 0
}

// configsPush triggers a push via the dserv configs subprocess.
func configsPush(cfg *Config, args []string) int {
	project := ""
	if len(args) > 0 {
		project = args[0]
	}
	if project == "" {
		PrintError("project name required: dservctl configs push <project>")
		return 2
	}

	tclCmd := fmt.Sprintf("registry_push %s", project)
	resp, err := SendToInterp(cfg.Host, "configs", tclCmd)
	if err != nil {
		PrintError("push failed: %v", err)
		return 1
	}

	if cfg.JSON {
		fmt.Println(strings.TrimSpace(resp))
	} else {
		fmt.Printf("Pushed project '%s' to registry\n", project)
	}
	return 0
}

// configsPull triggers a pull via the dserv configs subprocess.
func configsPull(cfg *Config, args []string) int {
	project := ""
	overwrite := false
	if len(args) > 0 {
		project = args[0]
	}
	for i := 1; i < len(args); i++ {
		if args[i] == "--overwrite" || args[i] == "-f" {
			overwrite = true
		}
	}
	if project == "" {
		PrintError("project name required: dservctl configs pull <project> [--overwrite]")
		return 2
	}

	owStr := "0"
	if overwrite {
		owStr = "1"
	}
	tclCmd := fmt.Sprintf("registry_pull %s -overwrite %s", project, owStr)
	resp, err := SendToInterp(cfg.Host, "configs", tclCmd)
	if err != nil {
		PrintError("pull failed: %v", err)
		return 1
	}

	if cfg.JSON {
		fmt.Println(strings.TrimSpace(resp))
	} else {
		fmt.Printf("Pulled project '%s' from registry\n", project)
	}
	return 0
}
