package main

import (
	"log"
	"net/http"
	"os"
	"time"
)

func main() {
	// Parse command line flags
	verbose := false
	for _, arg := range os.Args[1:] {
		if arg == "-v" || arg == "--verbose" {
			verbose = true
		}
	}

	log.Printf("🚀 Starting ESS WebGUI")

	// Create core components - no registry needed!
	dservClient := NewDservClient("localhost:4620", verbose)
	essClient := NewEssClient("localhost:2570")
	stateManager := NewStateManager() // Much simpler
	apiServer := NewAPIServer(dservClient, essClient, stateManager)

	// Initialize dserv connection
	log.Printf("📡 Initializing dserv connection...")
	if err := dservClient.Initialize(); err != nil {
		log.Printf("⚠️  Warning: Failed to initialize dserv: %v", err)
	}

	// Start the update processing pipeline
	log.Printf("🔄 Starting update processing pipeline...")
	go startUpdatePipeline(dservClient, stateManager)

	// Subscribe to variables we need for this interface
	log.Printf("📋 Subscribing to key variables...")
	subscribeToDatapoints(dservClient)
	touchInterfaceVariables(dservClient)

	// Setup HTTP routes
	log.Printf("🌐 Setting up HTTP server...")
	setupRoutes(apiServer)
	setupStaticFiles()

	// Start health monitoring
	if verbose {
		go startHealthMonitor(dservClient, stateManager)
	}

	// Print status
	printStartupInfo(verbose)

	// Start HTTP server
	log.Printf("✅ Server ready on http://localhost:8080")
	log.Fatal(http.ListenAndServe(":8080", nil))
}

// startUpdatePipeline processes all incoming dserv updates
func startUpdatePipeline(client *DservClient, manager *StateManager) {
	updateChan := client.GetUpdateChannel()

	for update := range updateChan {
		// Simply forward everything to the state manager
		manager.ProcessUpdate(update.Name, update.Value)
	}

	log.Printf("❌ Update pipeline stopped")
}

// subscribeToDatapoints
func subscribeToDatapoints(client *DservClient) {
	// Wildcard subscriptions - match the C++ add_match calls
	wildcards := []string{
		"ess/*",             // All ESS variables
		"system/*",          // All system variables
		"stimdg",            // Stimulus data
		"trialdg",           // Trial data
		"openiris/settings", // OpenIris settings
		"print",             // Print messages
	}

	subscribed := 0
	for _, pattern := range wildcards {
		if err := client.Subscribe(pattern, 1); err != nil {
			log.Printf("⚠️  Failed to subscribe to %s: %v", pattern, err)
		} else {
			subscribed++
		}
	}

	log.Printf("📊 Subscribed to %d patterns", subscribed)
}

// touchInterfaceVariables touches specific variables to update the interface
func touchInterfaceVariables(client *DservClient) {
	// Touch specific variables - matches the C++ foreach loop
	touchVariables := []string{
		// Core ESS configuration
		"ess/systems", "ess/protocols", "ess/variants",
		"ess/system", "ess/protocol", "ess/variant",

		// Experiment state
		"ess/subject", "ess/state", "ess/em_pos",
		"ess/obs_id", "ess/obs_total",

		// Progress tracking
		"ess/block_pct_complete", "ess/block_pct_correct",

		// Extended ESS variables
		"ess/variant_info", "ess/screen_w", "ess/screen_h",
		"ess/screen_halfx", "ess/screen_halfy",

		// Scripts and configuration
		"ess/state_table", "ess/rmt_cmds",
		"ess/system_script", "ess/protocol_script",
		"ess/variants_script", "ess/loaders_script",
		"ess/stim_script", "ess/param_settings",
		"ess/params",

		// Data streams
		"stimdg", "trialdg",

		// Git information
		"ess/git/branches", "ess/git/branch",

		// System information
		"system/hostname", "system/os",

		// OpenIris settings
		"openiris/settings",
	}

	touched := 0
	failed := 0

	log.Printf("🔄 Touching %d interface variables...", len(touchVariables))

	for _, variable := range touchVariables {
		if err := client.Touch(variable); err != nil {
			log.Printf("⚠️  Failed to touch %s: %v", variable, err)
			failed++
		} else {
			touched++
		}
	}

	log.Printf("✅ Successfully touched %d/%d variables (%d failed)",
		touched, len(touchVariables), failed)
}

// setupRoutes configures all HTTP endpoints
func setupRoutes(api *APIServer) {
	// Core variable operations
	http.HandleFunc("/api/query", api.HandleQuery)
	http.HandleFunc("/api/subscribe", api.HandleSubscribe)
	http.HandleFunc("/api/touch", api.HandleTouch)

	// ESS operations
	http.HandleFunc("/api/ess/eval", api.HandleEssEval)

	// State access
	http.HandleFunc("/api/variables", api.HandleGetAllVariables)
	http.HandleFunc("/api/variables/prefix", api.HandleGetVariablesByPrefix)

	// Live updates - dual channel approach!
	http.HandleFunc("/api/updates", api.HandleLiveUpdates) // SSE for low-frequency
	http.HandleFunc("/api/ws", api.HandleWebSocket)        // WebSocket for high-frequency

	// Batch operations
	http.HandleFunc("/api/batch/query", api.HandleBatchQuery)
	http.HandleFunc("/api/batch/touch", api.HandleBatchTouch)

	// Status and debugging
	http.HandleFunc("/api/status", api.HandleStatus)

	log.Printf("✅ Registered all API endpoints")
}

// setupStaticFiles configures static file serving
func setupStaticFiles() {
	// Check for built frontend files
	if hasDistFiles("./dist") {
		log.Printf("📦 Serving built frontend from ./dist")
		fileServer := http.FileServer(http.Dir("./dist"))
		http.Handle("/", createSPAHandler(fileServer))
	} else {
		log.Printf("🔧 Development mode - serving fallback page")
		http.HandleFunc("/", serveDevelopmentPage)
	}
}

// startHealthMonitor runs periodic health checks
func startHealthMonitor(client *DservClient, manager *StateManager) {
	ticker := time.NewTicker(30 * time.Second)
	defer ticker.Stop()

	for range ticker.C {
		clientStats := client.GetStats()
		managerStats := manager.GetStats()

		log.Printf("🔍 Health Check:")
		log.Printf("   Dserv: Connected=%v, Updates=%v",
			clientStats["connected"], clientStats["total_updates"])
		log.Printf("   State: Variables=%v, Subscribers=%v",
			managerStats["total_variables"], managerStats["subscribers"])
	}
}

// printStartupInfo displays startup information
func printStartupInfo(verbose bool) {
	log.Printf("📋 ESS WebGUI Configuration:")
	log.Printf("   Dserv: localhost:4620")
	log.Printf("   ESS: localhost:2570")
	log.Printf("   HTTP: localhost:8080")
	log.Printf("   Mode: Simple passthrough (no registry)")
	log.Printf("   Verbose: %v", verbose)

	if verbose {
		log.Printf("📚 Available API endpoints:")
		endpoints := []string{
			"GET  /api/status - System status",
			"GET  /api/variables - All variables",
			"GET  /api/variables/prefix?prefix=X - Variables by prefix",
			"GET  /api/updates - Live variable updates (SSE)",
			"POST /api/query - Query single variable",
			"POST /api/subscribe - Subscribe to variable",
			"POST /api/touch - Touch/refresh variable",
			"POST /api/ess/eval - Execute ESS command",
		}
		for _, endpoint := range endpoints {
			log.Printf("   %s", endpoint)
		}
	}
}

// Utility functions

func hasDistFiles(path string) bool {
	entries, err := os.ReadDir(path)
	if err != nil {
		return false
	}
	for _, entry := range entries {
		if !entry.IsDir() {
			return true
		}
	}
	return false
}

func createSPAHandler(fileServer http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Don't serve API routes as files
		if len(r.URL.Path) >= 4 && r.URL.Path[:4] == "/api" {
			http.NotFound(w, r)
			return
		}
		fileServer.ServeHTTP(w, r)
	})
}

func serveDevelopmentPage(w http.ResponseWriter, r *http.Request) {
	// Don't serve API routes
	if len(r.URL.Path) >= 4 && r.URL.Path[:4] == "/api" {
		http.NotFound(w, r)
		return
	}

	w.Header().Set("Content-Type", "text/html")
	w.Write([]byte(`<!DOCTYPE html>
<html>
<head>
    <title>ESS WebGUI - Simple Mode</title>
    <style>
        body { font-family: system-ui; max-width: 800px; margin: 50px auto; padding: 20px; }
        .status { background: #d4edda; padding: 15px; border-radius: 5px; margin: 20px 0; }
        .endpoints { background: #f8f9fa; padding: 15px; border-radius: 5px; margin: 20px 0; }
        code { background: #e9ecef; padding: 2px 4px; border-radius: 3px; }
        ul { margin: 10px 0; }
        li { margin: 5px 0; }
    </style>
</head>
<body>
    <h1>ESS DservGUI - Simple Mode</h1>
    
    <div class="status">
        <h3>Simple Passthrough Backend</h3>
        <p>Your Go backend stores and forwards all dserv variables without pre-configuration.</p>
        <p>All parsing and categorization happens in the JavaScript frontend.</p>
    </div>
    
    <div class="endpoints">
        <h3>Test Endpoints</h3>
        <ul>
            <li><a href="/api/status">System Status</a></li>
            <li><a href="/api/variables">All Variables</a></li>
            <li><a href="/api/variables/prefix?prefix=ess/">ESS Variables</a></li>
            <li><a href="/api/variables/prefix?prefix=system/">System Variables</a></li>
        </ul>
    </div>
    
    <div class="endpoints">
        <h3>Live Data Stream</h3>
        <p>Connect to: <code>/api/updates</code></p>
        <p>All variable updates are forwarded in real-time via Server-Sent Events.</p>
        <p>Your frontend JavaScript handles all parsing and categorization.</p>
    </div>

    <div class="endpoints">
        <h3>System Info</h3>
        <ul>
            <li>dserv: localhost:4620</li>
            <li>ESS: localhost:2570</li>
            <li>HTTP: localhost:8080</li>
            <li>Mode: Simple passthrough (no registry)</li>
            <li>All data forwarded as raw strings</li>
        </ul>
    </div>
</body>
</html>`))
}
