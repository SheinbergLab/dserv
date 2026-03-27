package main

import (
	"encoding/json"
	"fmt"
	"os"
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
	case "status":
		return configsStatus(cfg, rest)
	case "history":
		return configsHistory(cfg, rest)
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

	client := NewAgentClient(cfg)
	cmd := fmt.Sprintf("send configs {registry_push %s}", project)
	result, err := client.Do("POST", "/api/v1/dserv/eval", map[string]string{"command": cmd})
	if err != nil {
		PrintError("push failed: %v", err)
		return 1
	}

	if cfg.JSON {
		data, _ := json.MarshalIndent(result, "", "  ")
		fmt.Println(string(data))
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

	client := NewAgentClient(cfg)
	owStr := "0"
	if overwrite {
		owStr = "1"
	}
	cmd := fmt.Sprintf("send configs {registry_pull %s -overwrite %s}", project, owStr)
	result, err := client.Do("POST", "/api/v1/dserv/eval", map[string]string{"command": cmd})
	if err != nil {
		PrintError("pull failed: %v", err)
		return 1
	}

	if cfg.JSON {
		data, _ := json.MarshalIndent(result, "", "  ")
		fmt.Println(string(data))
	} else {
		fmt.Printf("Pulled project '%s' from registry\n", project)
	}
	return 0
}
