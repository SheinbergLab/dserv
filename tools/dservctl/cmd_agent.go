package main

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"time"
)

// runStatus shows agent and dserv status.
func runStatus(cfg *Config, args []string) int {
	client := NewAgentClient(cfg)
	result, err := client.Get("/api/status")
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	// Agent info
	if agent, ok := result["agent"].(map[string]interface{}); ok {
		fmt.Printf("Agent:    v%s (up %s)\n", strVal(agent, "version"), strVal(agent, "uptime"))
	}

	// Dserv info
	if dserv, ok := result["dserv"].(map[string]interface{}); ok {
		status := strVal(dserv, "status")
		line := fmt.Sprintf("Dserv:    %s", status)
		if v := strVal(dserv, "version"); v != "" {
			line += fmt.Sprintf(" v%s", v)
		}
		if pid := intVal(dserv, "pid"); pid > 0 {
			line += fmt.Sprintf(" (PID %d)", pid)
		}
		fmt.Println(line)
	}

	// System info
	if sys, ok := result["system"].(map[string]interface{}); ok {
		fmt.Printf("Host:     %s (%s/%s)\n", strVal(sys, "hostname"), strVal(sys, "os"), strVal(sys, "arch"))
		if uptime := strVal(sys, "uptime"); uptime != "" {
			fmt.Printf("Uptime:   %s\n", uptime)
		}
		if load := strVal(sys, "loadAvg"); load != "" {
			fmt.Printf("Load:     %s\n", load)
		}
	}

	// Extra services
	if services, ok := result["services"].(map[string]interface{}); ok && len(services) > 0 {
		fmt.Println("Services:")
		for name, status := range services {
			fmt.Printf("  %-20s %s\n", name, status)
		}
	}

	return 0
}

// runMesh lists mesh nodes.
func runMesh(cfg *Config, args []string) int {
	client := NewAgentClient(cfg)

	// Determine path — use workgroup filter if available
	path := "/api/v1/mesh"
	wg := cfg.Workgroup
	if len(args) > 0 {
		wg = args[0]
	}
	if wg != "" {
		path += "?workgroup=" + wg
	}

	result, err := client.Get(path)
	if err != nil {
		// Fall back to /api/mesh/peers (client mode)
		result, err = client.Get("/api/mesh/peers")
		if err != nil {
			PrintError("%v", err)
			return 1
		}
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	// Extract nodes
	nodes := extractNodes(result)
	if len(nodes) == 0 {
		fmt.Println("No mesh nodes found.")
		return 0
	}

	// Table output
	headers := []string{"HOSTNAME", "IP", "PORT", "WORKGROUP", "STATUS", "STATE", "LAST SEEN"}
	var rows [][]string
	for _, node := range nodes {
		rows = append(rows, []string{
			strVal(node, "hostname"),
			strVal(node, "ip"),
			fmt.Sprintf("%d", intVal(node, "port")),
			strVal(node, "workgroup"),
			strVal(node, "status"),
			strVal(node, "state"),
			formatLastSeen(intVal(node, "lastSeenAgo")),
		})
	}

	PrintTable(headers, rows)
	return 0
}

// runService controls dserv or other systemd services.
func runService(cfg *Config, args []string) int {
	if len(args) == 0 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl service <start|stop|restart|status> [service-name]\n")
		return 2
	}

	action := args[0]
	validActions := map[string]bool{"start": true, "stop": true, "restart": true, "status": true}
	if !validActions[action] {
		PrintError("unknown action %q (use start, stop, restart, or status)", action)
		return 2
	}

	client := NewAgentClient(cfg)

	var path string
	if len(args) > 1 {
		// Specific service
		serviceName := args[1]
		path = fmt.Sprintf("/api/service/%s/%s", serviceName, action)
	} else {
		// Default to dserv
		path = fmt.Sprintf("/api/dserv/%s", action)
	}

	result, err := client.Post(path, nil)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	// Display result
	if action == "status" {
		status := strVal(result, "status")
		fmt.Printf("Status: %s\n", status)
		if v := strVal(result, "version"); v != "" {
			fmt.Printf("Version: %s\n", v)
		}
		if pid := intVal(result, "pid"); pid > 0 {
			fmt.Printf("PID: %d\n", pid)
		}
	} else {
		if result["success"] == true {
			fmt.Printf("Service %s: OK\n", action)
		} else if errMsg := strVal(result, "error"); errMsg != "" {
			PrintError("%s", errMsg)
			return 1
		}
	}

	return 0
}

// runComponents lists or installs components.
func runComponents(cfg *Config, args []string) int {
	client := NewAgentClient(cfg)

	// Check for subcommands
	if len(args) > 0 {
		switch args[0] {
		case "install":
			return runComponentInstall(cfg, client, args[1:])
		case "check":
			return runComponentCheck(cfg, client)
		case "update":
			return runComponentUpdate(cfg, client)
		}
	}

	result, err := client.Get("/api/components")
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	// Components come back as an array
	items, ok := result["items"].([]interface{})
	if !ok || len(items) == 0 {
		fmt.Println("No components found.")
		return 0
	}

	headers := []string{"ID", "NAME", "INSTALLED", "CURRENT", "LATEST", "UPDATE"}
	var rows [][]string
	for _, item := range items {
		cs, ok := item.(map[string]interface{})
		if !ok {
			continue
		}
		comp, _ := cs["component"].(map[string]interface{})
		installed := "no"
		if cs["installed"] == true {
			installed = "yes"
		}
		update := ""
		if cs["updateAvailable"] == true {
			update = "available"
		}
		rows = append(rows, []string{
			strVal(comp, "id"),
			strVal(comp, "name"),
			installed,
			strVal(cs, "currentVersion"),
			strVal(cs, "latestVersion"),
			update,
		})
	}

	PrintTable(headers, rows)
	return 0
}

// runComponentCheck shows only components with available updates.
func runComponentCheck(cfg *Config, client *AgentClient) int {
	result, err := client.Get("/api/components")
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	items, ok := result["items"].([]interface{})
	if !ok {
		fmt.Println("No components found.")
		return 0
	}

	var rows [][]string
	for _, item := range items {
		cs, ok := item.(map[string]interface{})
		if !ok {
			continue
		}
		if cs["updateAvailable"] != true {
			continue
		}
		comp, _ := cs["component"].(map[string]interface{})
		rows = append(rows, []string{
			strVal(comp, "id"),
			strVal(cs, "currentVersion"),
			strVal(cs, "latestVersion"),
		})
	}

	if len(rows) == 0 {
		fmt.Println("All components are up to date.")
		return 0
	}

	PrintTable([]string{"ID", "CURRENT", "LATEST"}, rows)
	return 0
}

// runComponentUpdate updates all components that have available updates,
// installing dependencies first.
func runComponentUpdate(cfg *Config, client *AgentClient) int {
	result, err := client.Get("/api/components")
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	items, ok := result["items"].([]interface{})
	if !ok {
		fmt.Println("No components found.")
		return 0
	}

	// Collect components with updates, separating dependencies from dependents
	var deps []string    // components others depend on (install first)
	var regular []string // everything else
	depSet := make(map[string]bool)

	// First pass: identify which components are dependencies
	for _, item := range items {
		cs, ok := item.(map[string]interface{})
		if !ok {
			continue
		}
		comp, _ := cs["component"].(map[string]interface{})
		if comp == nil {
			continue
		}
		if d, ok := comp["depends"].([]interface{}); ok {
			for _, dep := range d {
				if ds, ok := dep.(string); ok {
					depSet[ds] = true
				}
			}
		}
	}

	// Second pass: sort updatable components into deps-first order
	for _, item := range items {
		cs, ok := item.(map[string]interface{})
		if !ok {
			continue
		}
		if cs["updateAvailable"] != true {
			continue
		}
		comp, _ := cs["component"].(map[string]interface{})
		if comp == nil {
			continue
		}
		id := strVal(comp, "id")
		if depSet[id] {
			deps = append(deps, id)
		} else {
			regular = append(regular, id)
		}
	}

	updateOrder := append(deps, regular...)
	if len(updateOrder) == 0 {
		fmt.Println("All components are up to date.")
		return 0
	}

	fmt.Printf("Components to update: %s\n\n", strings.Join(updateOrder, ", "))
	for _, id := range updateOrder {
		rc := runComponentInstall(cfg, client, []string{id})
		if rc != 0 {
			return rc
		}
		fmt.Println()
	}
	return 0
}

func runComponentInstall(cfg *Config, client *AgentClient, args []string) int {
	if len(args) == 0 {
		fmt.Fprintf(os.Stderr, "Usage: dservctl components install <component-id> [--asset ASSET]\n")
		return 2
	}

	componentID := args[0]

	// Parse optional --asset flag
	var asset string
	for i := 1; i < len(args); i++ {
		if args[i] == "--asset" && i+1 < len(args) {
			asset = args[i+1]
			i++
		}
	}

	// Fetch component status to get available assets and dependency info
	statusResult, err := client.Get(fmt.Sprintf("/api/components/%s", componentID))
	if err != nil {
		PrintError("failed to get component status: %v", err)
		return 1
	}

	// Check if update is available
	currentVer := strVal(statusResult, "currentVersion")
	latestVer := strVal(statusResult, "latestVersion")

	if currentVer != "" && currentVer == latestVer {
		fmt.Printf("%s is already up to date (%s)\n", componentID, currentVer)
		return 0
	}

	// Auto-select asset if not specified
	if asset == "" {
		assets := extractStringArray(statusResult, "assets")
		if len(assets) == 0 {
			PrintError("no compatible assets found for %s", componentID)
			return 1
		}
		if len(assets) == 1 {
			asset = assets[0]
		} else {
			fmt.Fprintf(os.Stderr, "Multiple assets available for %s. Specify one with --asset:\n", componentID)
			for _, a := range assets {
				fmt.Fprintf(os.Stderr, "  %s\n", a)
			}
			return 2
		}
	}

	// Build stopServices list from dependency info
	allComponents, err := client.Get("/api/components")
	var stopServices []string
	if err == nil {
		stopServices = getServicesToStop(componentID, allComponents)
	}

	// Show what we're about to do
	if currentVer != "" {
		fmt.Printf("Updating %s: %s -> %s\n", componentID, currentVer, latestVer)
	} else {
		fmt.Printf("Installing %s %s\n", componentID, latestVer)
	}
	fmt.Printf("Asset: %s\n", asset)
	if len(stopServices) > 0 {
		fmt.Printf("Services to restart: %s\n", strings.Join(stopServices, ", "))
	}

	// Send install request with asset and stopServices
	body := map[string]interface{}{
		"asset":        asset,
		"stopServices": stopServices,
	}
	path := fmt.Sprintf("/api/components/%s/install", componentID)
	result, err := client.Post(path, body)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	// Poll for completion by watching version change
	fmt.Printf("Installing...")
	for i := 0; i < 60; i++ {
		time.Sleep(2 * time.Second)
		fmt.Printf(".")
		check, err := client.Get(fmt.Sprintf("/api/components/%s", componentID))
		if err != nil {
			continue
		}
		newVer := strVal(check, "currentVersion")
		if newVer != "" && newVer != currentVer {
			fmt.Printf(" done!\n")
			fmt.Printf("Installed %s %s\n", componentID, newVer)
			return 0
		}
	}

	fmt.Printf(" timeout waiting for version change\n")
	fmt.Println("The install may still be in progress. Run 'dservctl components' to check.")
	return 1
}

// extractStringArray extracts a []string from a JSON result field.
func extractStringArray(m map[string]interface{}, key string) []string {
	arr, ok := m[key].([]interface{})
	if !ok {
		return nil
	}
	var result []string
	for _, v := range arr {
		if s, ok := v.(string); ok {
			result = append(result, s)
		}
	}
	return result
}

// getServicesToStop computes which services need to be stopped when installing
// a component, based on dependency relationships.
func getServicesToStop(componentID string, allComponents map[string]interface{}) []string {
	items, ok := allComponents["items"].([]interface{})
	if !ok {
		return nil
	}

	services := make(map[string]bool)

	for _, item := range items {
		cs, ok := item.(map[string]interface{})
		if !ok {
			continue
		}
		comp, _ := cs["component"].(map[string]interface{})
		if comp == nil {
			continue
		}
		id := strVal(comp, "id")
		svc := strVal(comp, "service")

		// If this component itself has a service, stop it
		if id == componentID && svc != "" {
			services[svc] = true
		}

		// If this component depends on the one being installed, stop its service
		if deps, ok := comp["depends"].([]interface{}); ok {
			for _, d := range deps {
				if ds, ok := d.(string); ok && ds == componentID && svc != "" {
					services[svc] = true
				}
			}
		}
	}

	var result []string
	for svc := range services {
		result = append(result, svc)
	}
	return result
}

// runLogs shows service logs.
func runLogs(cfg *Config, args []string) int {
	service := ""
	lines := "50"

	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--service", "-s":
			if i+1 < len(args) {
				service = args[i+1]
				i++
			}
		case "--lines", "-n":
			if i+1 < len(args) {
				lines = args[i+1]
				i++
			}
		default:
			// First bare arg is service name
			if service == "" {
				service = args[i]
			}
		}
	}

	client := NewAgentClient(cfg)
	path := fmt.Sprintf("/api/logs?lines=%s", lines)
	if service != "" {
		path += "&service=" + service
	}

	result, err := client.Get(path)
	if err != nil {
		PrintError("%v", err)
		return 1
	}

	if cfg.JSON {
		printJSON(result)
		return 0
	}

	// Print raw logs
	if logs, ok := result["logs"].(string); ok {
		fmt.Print(logs)
	}
	return 0
}

// --- Helpers ---

func printJSON(data interface{}) {
	enc := json.NewEncoder(os.Stdout)
	enc.SetIndent("", "  ")
	enc.Encode(data)
}

func strVal(m map[string]interface{}, key string) string {
	if v, ok := m[key]; ok {
		if s, ok := v.(string); ok {
			return s
		}
		return fmt.Sprintf("%v", v)
	}
	return ""
}

func intVal(m map[string]interface{}, key string) int {
	if v, ok := m[key]; ok {
		switch n := v.(type) {
		case float64:
			return int(n)
		case int:
			return n
		case json.Number:
			if i, err := n.Int64(); err == nil {
				return int(i)
			}
		}
	}
	return 0
}

func extractNodes(result map[string]interface{}) []map[string]interface{} {
	// Try "nodes" key (MeshResponse format)
	if nodes, ok := result["nodes"].([]interface{}); ok {
		return toMapSlice(nodes)
	}
	// Try "items" key (array wrapper)
	if items, ok := result["items"].([]interface{}); ok {
		return toMapSlice(items)
	}
	return nil
}

func toMapSlice(items []interface{}) []map[string]interface{} {
	var result []map[string]interface{}
	for _, item := range items {
		if m, ok := item.(map[string]interface{}); ok {
			result = append(result, m)
		}
	}
	return result
}

func formatLastSeen(secondsAgo int) string {
	if secondsAgo <= 0 {
		return "now"
	}
	if secondsAgo < 60 {
		return fmt.Sprintf("%ds ago", secondsAgo)
	}
	if secondsAgo < 3600 {
		return fmt.Sprintf("%dm ago", secondsAgo/60)
	}
	return fmt.Sprintf("%dh ago", secondsAgo/3600)
}

// requireWorkgroup ensures a workgroup is configured, printing an error if not.
func requireWorkgroup(cfg *Config) bool {
	if cfg.Workgroup == "" {
		PrintError("workgroup required (use --workgroup or set DSERV_WORKGROUP)")
		return false
	}
	return true
}

// requireUser ensures a user is configured.
func requireUser(cfg *Config) bool {
	if cfg.User == "" {
		// Try to get from environment
		cfg.User = os.Getenv("USER")
		if cfg.User == "" {
			PrintError("user required (use --user or set DSERV_USER)")
			return false
		}
	}
	return true
}

// parseServiceFlag extracts a -s service flag from args, returning the service name and remaining args.
func parseServiceFlag(args []string) (string, []string) {
	var service string
	var remaining []string
	for i := 0; i < len(args); i++ {
		if (args[i] == "-s" || args[i] == "--service") && i+1 < len(args) {
			service = args[i+1]
			i++
		} else {
			remaining = append(remaining, args[i])
		}
	}
	return service, remaining
}

// joinPath builds a URL path from segments, ensuring proper encoding.
func joinPath(parts ...string) string {
	var cleaned []string
	for _, p := range parts {
		p = strings.TrimPrefix(p, "/")
		p = strings.TrimSuffix(p, "/")
		if p != "" {
			cleaned = append(cleaned, p)
		}
	}
	return "/" + strings.Join(cleaned, "/")
}
