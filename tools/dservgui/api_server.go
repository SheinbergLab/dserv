package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"time"

	"github.com/gorilla/websocket"
)

// CORS middleware to handle cross-origin requests
func corsMiddleware(next http.HandlerFunc) http.HandlerFunc {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Set CORS headers
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type, Authorization")
		w.Header().Set("Access-Control-Max-Age", "86400") // 24 hours

		// Handle preflight OPTIONS request
		if r.Method == "OPTIONS" {
			w.WriteHeader(http.StatusOK)
			return
		}

		// Call the next handler
		next.ServeHTTP(w, r)
	})
}

// APIServer handles all HTTP API endpoints
type APIServer struct {
	dservClient  *DservClient
	essClient    *EssClient
	stateManager *StateManager
	upgrader     websocket.Upgrader // WebSocket upgrader
}

// Channel routing configuration - determines which variables go through which channels
type ChannelConfig struct {
	HighFrequency []string // WebSocket channel for high-frequency updates
	LowFrequency  []string // SSE channel for low-frequency updates
	LargeData     []string // Variables that should use optimized large data handling
}

// Default channel routing - route variables based on their characteristics
var defaultChannelRouting = ChannelConfig{
	HighFrequency: []string{
		"ess/em_pos",      // Eye position data (high frequency)
		"ess/em_velocity", // Eye velocity (high frequency)
		"ess/cursor_pos",  // Real-time cursor position
	},
	LowFrequency: []string{
		"ess/systems", // Configuration variables (low frequency)
		"ess/protocols",
		"ess/variants",
		"ess/status", // Status updates
		"ess/trial_", // Trial-level data (prefix match)
	},
	LargeData: []string{
		"ess/stiminfo",  // Stimulus information (large columnar data)
		"ess/trialinfo", // Trial data tables (potentially large)
	},
}

// NewAPIServer creates a new API server
func NewAPIServer(dservClient *DservClient, essClient *EssClient, stateManager *StateManager) *APIServer {
	return &APIServer{
		dservClient:  dservClient,
		essClient:    essClient,
		stateManager: stateManager,
		upgrader: websocket.Upgrader{
			CheckOrigin: func(r *http.Request) bool {
				return true // Allow all origins for development
			},
		},
	}
}

// Request/Response structures

type QueryRequest struct {
	Variable string `json:"variable"`
}

type QueryResponse struct {
	Variable string `json:"variable"`
	Value    string `json:"value,omitempty"`
	Success  bool   `json:"success"`
	Error    string `json:"error,omitempty"`
}

type SubscribeRequest struct {
	Variable string `json:"variable"`
	Every    int    `json:"every"`
}

type SubscribeResponse struct {
	Variable string `json:"variable"`
	Every    int    `json:"every"`
	Success  bool   `json:"success"`
	Error    string `json:"error,omitempty"`
}

type TouchRequest struct {
	Variable string `json:"variable"`
}

type TouchResponse struct {
	Variable string `json:"variable"`
	Success  bool   `json:"success"`
	Error    string `json:"error,omitempty"`
}

type SetRequest struct {
	Variable string `json:"variable"`
	Value    string `json:"value"`
}

type SetResponse struct {
	Variable string `json:"variable"`
	Value    string `json:"value"`
	Success  bool   `json:"success"`
	Error    string `json:"error,omitempty"`
}

type EssEvalRequest struct {
	Script string `json:"script"`
}

type EssEvalResponse struct {
	Script  string `json:"script"`
	Result  string `json:"result,omitempty"`
	Success bool   `json:"success"`
	Error   string `json:"error,omitempty"`
}

type BatchQueryRequest struct {
	Variables []string `json:"variables"`
}

type BatchTouchRequest struct {
	Variables []string `json:"variables"`
}

// Core API Handlers

// HandleQuery handles variable query requests
func (api *APIServer) HandleQuery(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req QueryRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid JSON", http.StatusBadRequest)
		return
	}

	if req.Variable == "" {
		api.sendJSON(w, QueryResponse{
			Success: false,
			Error:   "Variable name cannot be empty",
		})
		return
	}

	// Query dserv
	rawValue, err := api.dservClient.Query(req.Variable)

	response := QueryResponse{
		Variable: req.Variable,
		Success:  err == nil,
	}

	if err != nil {
		response.Error = err.Error()
	} else {
		// Parse the space-separated dserv response format
		// Format: "1 variable_name 1 timestamp dtype {actual_value}"
		parsedValue := api.parseDservQueryResponse(rawValue)
		response.Value = parsedValue

		// Optionally update state manager with the parsed value for real-time access
		if parsedValue != rawValue && parsedValue != "" {
			api.stateManager.ProcessUpdate(req.Variable, parsedValue)
		}
	}

	api.sendJSON(w, response)
}

// parseDservQueryResponse parses space-separated dserv query response
// Format: "1 variable_name 1 timestamp dtype {actual_value}"
// Returns the actual value without braces, or original string if parsing fails
func (api *APIServer) parseDservQueryResponse(response string) string {
	// Handle braced content properly - the value might contain spaces
	// Format: "1 variable_name 1 timestamp dtype {actual_value with spaces}"

	// Look for the opening brace
	braceStart := strings.Index(response, "{")
	if braceStart == -1 {
		// No braces found, try simple field parsing
		parts := strings.Fields(response)
		if len(parts) >= 6 {
			return parts[len(parts)-1]
		}
		return response
	}

	// Look for the closing brace
	braceEnd := strings.LastIndex(response, "}")
	if braceEnd == -1 || braceEnd <= braceStart {
		// Malformed braces, return original
		return response
	}

	// Extract content within braces
	value := response[braceStart+1 : braceEnd]
	return value
}

// Utility method to send JSON responses
func (api *APIServer) sendJSON(w http.ResponseWriter, data interface{}) {
	w.Header().Set("Content-Type", "application/json")
	if err := json.NewEncoder(w).Encode(data); err != nil {
		http.Error(w, "Failed to encode JSON", http.StatusInternalServerError)
	}
}

// HandleSubscribe handles subscription requests
func (api *APIServer) HandleSubscribe(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req SubscribeRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid JSON", http.StatusBadRequest)
		return
	}

	if req.Variable == "" {
		api.sendJSON(w, SubscribeResponse{
			Success: false,
			Error:   "Variable name cannot be empty",
		})
		return
	}

	if req.Every <= 0 {
		req.Every = 1
	}

	// Subscribe via dserv
	err := api.dservClient.Subscribe(req.Variable, req.Every)

	response := SubscribeResponse{
		Variable: req.Variable,
		Every:    req.Every,
		Success:  err == nil,
	}

	if err != nil {
		response.Error = err.Error()
	}

	api.sendJSON(w, response)
}

// HandleTouch handles touch/refresh requests
func (api *APIServer) HandleTouch(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req TouchRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid JSON", http.StatusBadRequest)
		return
	}

	if req.Variable == "" {
		api.sendJSON(w, TouchResponse{
			Success: false,
			Error:   "Variable name cannot be empty",
		})
		return
	}

	// Touch via dserv
	err := api.dservClient.Touch(req.Variable)

	response := TouchResponse{
		Variable: req.Variable,
		Success:  err == nil,
	}

	if err != nil {
		response.Error = err.Error()
	}

	api.sendJSON(w, response)
}

// HandleSet handles variable set requests
func (api *APIServer) HandleSet(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req SetRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid JSON", http.StatusBadRequest)
		return
	}

	if req.Variable == "" {
		api.sendJSON(w, SetResponse{
			Success: false,
			Error:   "Variable name cannot be empty",
		})
		return
	}

	// Allow empty values for testing (value can be empty string)
	// if req.Value == "" {
	// 	api.sendJSON(w, SetResponse{
	// 		Variable: req.Variable,
	// 		Success:  false,
	// 		Error:    "Value cannot be empty",
	// 	})
	// 	return
	// }

	// Set via dserv - the subscription mechanism should update our state manager
	err := api.dservClient.Set(req.Variable, req.Value)

	response := SetResponse{
		Variable: req.Variable,
		Value:    req.Value,
		Success:  err == nil,
	}

	if err != nil {
		response.Error = err.Error()
		fmt.Printf("❌ SET failed for %s: %v\n", req.Variable, err)
	} else {
		fmt.Printf("🔧 SET %s = %s (waiting for subscription update...)\n", req.Variable, req.Value)
	}

	api.sendJSON(w, response)
}

// HandleEssEval handles ESS script execution
func (api *APIServer) HandleEssEval(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req EssEvalRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid JSON", http.StatusBadRequest)
		return
	}

	if req.Script == "" {
		api.sendJSON(w, EssEvalResponse{
			Success: false,
			Error:   "Script cannot be empty",
		})
		return
	}

	// Execute via ESS
	result, err := api.essClient.Eval(req.Script)

	response := EssEvalResponse{
		Script:  req.Script,
		Success: err == nil,
	}

	if err != nil {
		response.Error = err.Error()
	} else {
		response.Result = result
	}

	api.sendJSON(w, response)
}

// State Access Handlers

// HandleGetAllVariables returns all stored variables
func (api *APIServer) HandleGetAllVariables(w http.ResponseWriter, r *http.Request) {
	variables := api.stateManager.GetAllVariables()
	stats := api.stateManager.GetStats()

	response := map[string]interface{}{
		"variables": variables,
		"stats":     stats,
		"timestamp": time.Now(),
	}

	api.sendJSON(w, response)
}

// HandleGetVariablesByPrefix returns variables by name prefix
func (api *APIServer) HandleGetVariablesByPrefix(w http.ResponseWriter, r *http.Request) {
	prefix := r.URL.Query().Get("prefix")
	if prefix == "" {
		http.Error(w, "Prefix parameter required", http.StatusBadRequest)
		return
	}

	variables := api.stateManager.GetVariablesByPrefix(prefix)

	response := map[string]interface{}{
		"prefix":    prefix,
		"variables": variables,
		"count":     len(variables),
		"timestamp": time.Now(),
	}

	api.sendJSON(w, response)
}

// HandleLiveUpdates provides Server-Sent Events stream of variable updates
func (api *APIServer) HandleLiveUpdates(w http.ResponseWriter, r *http.Request) {
	// Set SSE headers
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("Access-Control-Allow-Origin", "*")

	// Optional prefix filter
	prefix := r.URL.Query().Get("prefix")

	// Subscribe to state manager updates
	updateChan := api.stateManager.Subscribe()
	defer api.stateManager.Unsubscribe(updateChan)

	// Send initial connection confirmation
	fmt.Fprintf(w, "data: {\"type\":\"connected\",\"timestamp\":\"%s\"}\n\n", time.Now().Format(time.RFC3339))
	if flusher, ok := w.(http.Flusher); ok {
		flusher.Flush()
	}

	for {
		select {
		case variable := <-updateChan:
			// Skip high-frequency variables - they go through WebSocket
			if api.isHighFrequencyVariable(variable.Name) {
				continue
			}

			// Apply prefix filter if specified
			if prefix != "" && !strings.HasPrefix(variable.Name, prefix) {
				continue
			}

			// Check if this is a large data variable that should be optimized
			if api.shouldOptimizeVariable(variable) {
				// Send optimized metadata notification instead of full data
				optimizedUpdate := api.createOptimizedUpdate(variable)
				data, _ := json.Marshal(optimizedUpdate)
				fmt.Fprintf(w, "data: %s\n\n", data)

				fmt.Printf("📊 Sent optimized update for large variable: %s (%d bytes -> %d bytes)\n",
					variable.Name, len(variable.Value), len(data))
			} else {
				// Send normal variable update
				data, _ := json.Marshal(variable)
				fmt.Fprintf(w, "data: %s\n\n", data)
			}

			if flusher, ok := w.(http.Flusher); ok {
				flusher.Flush()
			}

		case <-r.Context().Done():
			return
		}
	}
}

// isHighFrequencyVariable determines if a variable should use WebSocket channel
func (api *APIServer) isHighFrequencyVariable(varName string) bool {
	// Check exact matches first
	for _, highFreqVar := range defaultChannelRouting.HighFrequency {
		if varName == highFreqVar {
			return true
		}
	}

	// Check prefix matches for patterns like "ess/sensor_*"
	for _, highFreqVar := range defaultChannelRouting.HighFrequency {
		if strings.HasSuffix(highFreqVar, "_") && strings.HasPrefix(varName, highFreqVar) {
			return true
		}
	}

	return false
}

// HandleWebSocket provides WebSocket stream for high-frequency variable updates
func (api *APIServer) HandleWebSocket(w http.ResponseWriter, r *http.Request) {
	// Upgrade HTTP connection to WebSocket
	conn, err := api.upgrader.Upgrade(w, r, nil)
	if err != nil {
		fmt.Printf("❌ WebSocket upgrade failed: %v\n", err)
		return
	}
	defer conn.Close()

	fmt.Printf("✅ WebSocket connection established\n")

	// Subscribe to state manager updates
	updateChan := api.stateManager.Subscribe()
	defer api.stateManager.Unsubscribe(updateChan)

	// Send initial connection confirmation
	initMessage := map[string]interface{}{
		"type":      "connected",
		"timestamp": time.Now().Format(time.RFC3339),
		"channel":   "websocket",
	}
	if err := conn.WriteJSON(initMessage); err != nil {
		fmt.Printf("❌ Failed to send WebSocket init message: %v\n", err)
		return
	}

	// Handle incoming messages (for future bidirectional communication)
	go func() {
		for {
			_, _, err := conn.ReadMessage()
			if err != nil {
				fmt.Printf("📡 WebSocket read error (client disconnected): %v\n", err)
				break
			}
		}
	}()

	// Send variable updates for high-frequency variables only
	for {
		select {
		case variable := <-updateChan:
			// Only send high-frequency variables through WebSocket
			if !api.isHighFrequencyVariable(variable.Name) {
				continue
			}

			// Send variable update via WebSocket
			if err := conn.WriteJSON(variable); err != nil {
				fmt.Printf("❌ Failed to send WebSocket message: %v\n", err)
				return
			}

		case <-r.Context().Done():
			fmt.Printf("📡 WebSocket context cancelled\n")
			return
		}
	}
}

// Batch Operation Handlers

// HandleBatchQuery handles querying multiple variables
func (api *APIServer) HandleBatchQuery(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req BatchQueryRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid JSON", http.StatusBadRequest)
		return
	}

	results := make(map[string]interface{})

	for _, variable := range req.Variables {
		value, err := api.dservClient.Query(variable)

		if err != nil {
			results[variable] = map[string]interface{}{
				"success": false,
				"error":   err.Error(),
			}
		} else {
			results[variable] = map[string]interface{}{
				"success": true,
				"value":   value,
			}
		}
	}

	response := map[string]interface{}{
		"results":   results,
		"count":     len(results),
		"timestamp": time.Now(),
	}

	api.sendJSON(w, response)
}

// HandleBatchTouch handles touching multiple variables
func (api *APIServer) HandleBatchTouch(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req BatchTouchRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid JSON", http.StatusBadRequest)
		return
	}

	results := make(map[string]interface{})

	for _, variable := range req.Variables {
		err := api.dservClient.Touch(variable)

		if err != nil {
			results[variable] = map[string]interface{}{
				"success": false,
				"error":   err.Error(),
			}
		} else {
			results[variable] = map[string]interface{}{
				"success": true,
			}
		}
	}

	response := map[string]interface{}{
		"results":   results,
		"count":     len(results),
		"timestamp": time.Now(),
	}

	api.sendJSON(w, response)
}

// Status and Debug Handlers

// HandleStatus returns system status
func (api *APIServer) HandleStatus(w http.ResponseWriter, r *http.Request) {
	dservStats := api.dservClient.GetStats()
	stateStats := api.stateManager.GetStats()

	response := map[string]interface{}{
		"dserv_client":  dservStats,
		"state_manager": stateStats,
		"ess_client": map[string]interface{}{
			"address": api.essClient.address,
			"healthy": api.essClient.IsHealthy(),
		},
		"system_health": map[string]interface{}{
			"dserv_healthy": api.dservClient.IsHealthy(),
			"state_healthy": api.stateManager.IsHealthy(),
			"ess_healthy":   api.essClient.IsHealthy(),
		},
		"timestamp": time.Now(),
	}

	api.sendJSON(w, response)
}

// Simple frontend helpers that work with raw variables

// HandleGetEssVariables returns ESS-related variables
func (api *APIServer) HandleGetEssVariables(w http.ResponseWriter, r *http.Request) {
	variables := api.stateManager.GetVariablesByPrefix("ess/")

	response := map[string]interface{}{
		"variables": variables,
		"count":     len(variables),
		"timestamp": time.Now(),
	}

	api.sendJSON(w, response)
}

// HandleGetSystemVariables returns system-related variables
func (api *APIServer) HandleGetSystemVariables(w http.ResponseWriter, r *http.Request) {
	variables := api.stateManager.GetVariablesByPrefix("system/")

	response := map[string]interface{}{
		"variables": variables,
		"count":     len(variables),
		"timestamp": time.Now(),
	}

	api.sendJSON(w, response)
}

// HandleGetDataStreams returns information about data streams
func (api *APIServer) HandleGetDataStreams(w http.ResponseWriter, r *http.Request) {
	streams := make(map[string]interface{})

	// Check for stimdg
	if stimVar, exists := api.stateManager.GetVariable("stimdg"); exists {
		streams["stimdg"] = map[string]interface{}{
			"available": true,
			"size":      len(stimVar.Value),
			"timestamp": stimVar.Timestamp,
			"hasData":   len(stimVar.Value) > 0,
		}
	} else {
		streams["stimdg"] = map[string]interface{}{
			"available": false,
			"hasData":   false,
		}
	}

	// Check for trialdg
	if trialVar, exists := api.stateManager.GetVariable("trialdg"); exists {
		streams["trialdg"] = map[string]interface{}{
			"available": true,
			"size":      len(trialVar.Value),
			"timestamp": trialVar.Timestamp,
			"hasData":   len(trialVar.Value) > 0,
		}
	} else {
		streams["trialdg"] = map[string]interface{}{
			"available": false,
			"hasData":   false,
		}
	}

	response := map[string]interface{}{
		"streams":   streams,
		"timestamp": time.Now(),
	}

	api.sendJSON(w, response)
}

// HandleGetSingleVariable returns a single variable by name
func (api *APIServer) HandleGetSingleVariable(w http.ResponseWriter, r *http.Request) {
	name := r.URL.Query().Get("name")
	if name == "" {
		http.Error(w, "Name parameter required", http.StatusBadRequest)
		return
	}

	variable, exists := api.stateManager.GetVariable(name)
	if !exists {
		http.Error(w, fmt.Sprintf("Variable '%s' not found", name), http.StatusNotFound)
		return
	}

	response := map[string]interface{}{
		"variable":  variable,
		"timestamp": time.Now(),
	}

	api.sendJSON(w, response)
}

// HandleVariableRoutes routes variable-related requests
func (api *APIServer) HandleVariableRoutes(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Path

	// Remove the /api/variables/ prefix
	subPath := strings.TrimPrefix(path, "/api/variables/")

	switch subPath {
	case "get":
		api.HandleGetSingleVariable(w, r)
	case "prefix":
		api.HandleGetVariablesByPrefix(w, r)
	case "large":
		api.HandleLargeData(w, r)
	case "stimulus":
		api.HandleStimulusData(w, r) // Legacy endpoint - forwards to large endpoint
	default:
		// If no subpath matches, it might be the root /api/variables/ request
		// But since we handle /api/variables separately, this is an error
		http.Error(w, "Invalid variables endpoint", http.StatusNotFound)
	}
}

// LargeDataRequest represents request parameters for large JSON data
type LargeDataRequest struct {
	Variable string   `json:"variable"`          // Variable name to fetch
	Columns  []string `json:"columns,omitempty"` // Specific columns to return
	Limit    int      `json:"limit,omitempty"`   // Max rows to return (0 = all)
	Offset   int      `json:"offset,omitempty"`  // Row offset for pagination
	Preview  bool     `json:"preview,omitempty"` // Just metadata and sample
}

// LargeDataResponse represents the response for large JSON data
type LargeDataResponse struct {
	Variable string                 `json:"variable"`
	Metadata LargeDataMetadata      `json:"metadata"`
	Data     map[string]interface{} `json:"data,omitempty"`
	Preview  map[string]interface{} `json:"preview,omitempty"`
	Error    string                 `json:"error,omitempty"`
}

// LargeDataMetadata provides information about the dataset
type LargeDataMetadata struct {
	Variable     string                 `json:"variable"`
	Columns      []string               `json:"columns"`
	RowCount     int                    `json:"rowCount"`
	ColumnTypes  map[string]string      `json:"columnTypes"`
	DataSize     int                    `json:"dataSize"`
	LastUpdated  time.Time              `json:"lastUpdated"`
	HasData      bool                   `json:"hasData"`
	SampleValues map[string]interface{} `json:"sampleValues"`
	IsColumnar   bool                   `json:"isColumnar"`
}

// HandleLargeData provides optimized access to large JSON variables
func (api *APIServer) HandleLargeData(w http.ResponseWriter, r *http.Request) {
	// Parse query parameters
	variableName := r.URL.Query().Get("name")
	if variableName == "" {
		http.Error(w, "Variable name parameter required", http.StatusBadRequest)
		return
	}

	preview := r.URL.Query().Get("preview") == "true"
	limitStr := r.URL.Query().Get("limit")
	offsetStr := r.URL.Query().Get("offset")
	columnsStr := r.URL.Query().Get("columns")

	var limit, offset int
	var requestedColumns []string

	if limitStr != "" {
		fmt.Sscanf(limitStr, "%d", &limit)
	}
	if offsetStr != "" {
		fmt.Sscanf(offsetStr, "%d", &offset)
	}
	if columnsStr != "" {
		requestedColumns = strings.Split(columnsStr, ",")
	}

	// Get the variable from state manager
	variable, exists := api.stateManager.GetVariable(variableName)
	if !exists {
		response := LargeDataResponse{
			Variable: variableName,
			Metadata: LargeDataMetadata{Variable: variableName, HasData: false},
			Error:    fmt.Sprintf("Variable '%s' not found", variableName),
		}
		api.sendJSON(w, response)
		return
	}

	// Try to parse as JSON
	var jsonData interface{}
	if err := json.Unmarshal([]byte(variable.Value), &jsonData); err != nil {
		response := LargeDataResponse{
			Variable: variableName,
			Metadata: LargeDataMetadata{Variable: variableName, HasData: false},
			Error:    fmt.Sprintf("Variable '%s' is not valid JSON: %v", variableName, err),
		}
		api.sendJSON(w, response)
		return
	}

	// Check if it's columnar data (object with array values)
	dataMap, isObject := jsonData.(map[string]interface{})
	if !isObject {
		response := LargeDataResponse{
			Variable: variableName,
			Metadata: LargeDataMetadata{Variable: variableName, HasData: false, IsColumnar: false},
			Error:    fmt.Sprintf("Variable '%s' is not a columnar JSON object", variableName),
		}
		api.sendJSON(w, response)
		return
	}

	// Calculate metadata
	metadata := api.calculateLargeDataMetadata(dataMap, variable)

	response := LargeDataResponse{
		Variable: variableName,
		Metadata: metadata,
	}

	if preview {
		// For preview, just return metadata and sample values
		response.Preview = api.createLargeDataPreview(dataMap, 3) // 3 sample rows
	} else {
		// Return actual data with potential filtering/pagination
		response.Data = api.filterLargeData(dataMap, requestedColumns, limit, offset)
	}

	api.sendJSON(w, response)
}

// calculateLargeDataMetadata analyzes the JSON data and returns metadata
func (api *APIServer) calculateLargeDataMetadata(dataMap map[string]interface{}, variable Variable) LargeDataMetadata {
	columns := make([]string, 0, len(dataMap))
	columnTypes := make(map[string]string)
	sampleValues := make(map[string]interface{})
	rowCount := 0
	isColumnar := true

	for colName, colData := range dataMap {
		columns = append(columns, colName)

		if arr, ok := colData.([]interface{}); ok {
			if len(arr) > rowCount {
				rowCount = len(arr)
			}

			// Determine column type from first non-nil value
			columnType := "unknown"
			var sampleValue interface{}

			for _, val := range arr {
				if val != nil {
					sampleValue = val
					switch val.(type) {
					case float64, int64, int:
						columnType = "numeric"
					case string:
						columnType = "text"
					case bool:
						columnType = "boolean"
					default:
						columnType = "mixed"
					}
					break
				}
			}

			columnTypes[colName] = columnType
			sampleValues[colName] = sampleValue
		} else {
			// Not an array - this might not be columnar data
			isColumnar = false
			columnTypes[colName] = "non-array"
			sampleValues[colName] = colData
		}
	}

	return LargeDataMetadata{
		Variable:     variable.Name,
		Columns:      columns,
		RowCount:     rowCount,
		ColumnTypes:  columnTypes,
		DataSize:     len(variable.Value),
		LastUpdated:  variable.Timestamp,
		HasData:      len(columns) > 0 && (rowCount > 0 || !isColumnar),
		SampleValues: sampleValues,
		IsColumnar:   isColumnar,
	}
}

// createLargeDataPreview creates a small sample of the data for preview
func (api *APIServer) createLargeDataPreview(dataMap map[string]interface{}, sampleRows int) map[string]interface{} {
	preview := make(map[string]interface{})

	for colName, colData := range dataMap {
		if arr, ok := colData.([]interface{}); ok {
			maxRows := sampleRows
			if len(arr) < maxRows {
				maxRows = len(arr)
			}

			if maxRows > 0 {
				preview[colName] = arr[:maxRows]
			} else {
				preview[colName] = []interface{}{}
			}
		} else {
			// Non-array data - just include as-is
			preview[colName] = colData
		}
	}

	return preview
}

// filterLargeData applies column filtering and pagination to the data
func (api *APIServer) filterLargeData(dataMap map[string]interface{}, columns []string, limit, offset int) map[string]interface{} {
	filtered := make(map[string]interface{})

	// Determine which columns to include
	columnsToInclude := columns
	if len(columnsToInclude) == 0 {
		// Include all columns if none specified
		for colName := range dataMap {
			columnsToInclude = append(columnsToInclude, colName)
		}
	}

	// Apply column filtering and pagination
	for _, colName := range columnsToInclude {
		if colData, exists := dataMap[colName]; exists {
			if arr, ok := colData.([]interface{}); ok {
				// Apply pagination for array data
				start := offset
				end := len(arr)

				if start >= len(arr) {
					// Offset beyond data
					filtered[colName] = []interface{}{}
					continue
				}

				if limit > 0 && start+limit < len(arr) {
					end = start + limit
				}

				filtered[colName] = arr[start:end]
			} else {
				// Non-array data - include as-is
				filtered[colName] = colData
			}
		}
	}

	return filtered
}

// isLargeDataVariable checks if a variable should use optimized large data handling
func (api *APIServer) isLargeDataVariable(variableName string) bool {
	for _, pattern := range defaultChannelRouting.LargeData {
		if strings.HasSuffix(pattern, "/") {
			// Prefix match (e.g., "data/" matches "data/experiment1")
			if strings.HasPrefix(variableName, pattern) {
				return true
			}
		} else if strings.HasSuffix(pattern, "_") {
			// Prefix match with underscore (e.g., "ess/trial_" matches "ess/trial_001")
			if strings.HasPrefix(variableName, pattern) {
				return true
			}
		} else {
			// Exact match
			if variableName == pattern {
				return true
			}
		}
	}
	return false
}

// shouldOptimizeVariable determines if a variable should be automatically optimized
// based on size thresholds and content analysis
func (api *APIServer) shouldOptimizeVariable(variable Variable) bool {
	// Check if it's explicitly marked as large data
	if api.isLargeDataVariable(variable.Name) {
		return true
	}

	// Auto-detect based on size (>100KB)
	if len(variable.Value) > 100*1024 {
		// Try to parse as JSON to see if it's structured data
		var jsonData interface{}
		if err := json.Unmarshal([]byte(variable.Value), &jsonData); err == nil {
			// It's valid JSON and large - candidate for optimization
			if dataMap, ok := jsonData.(map[string]interface{}); ok {
				// Check if it looks like columnar data
				hasArrays := false
				for _, value := range dataMap {
					if _, isArray := value.([]interface{}); isArray {
						hasArrays = true
						break
					}
				}
				return hasArrays
			}
		}
	}

	return false
}

// createOptimizedUpdate creates a lightweight notification for large data variables
func (api *APIServer) createOptimizedUpdate(variable Variable) map[string]interface{} {
	// Try to parse the JSON and get metadata
	var jsonData interface{}
	if err := json.Unmarshal([]byte(variable.Value), &jsonData); err != nil {
		// If parsing fails, send basic notification
		return map[string]interface{}{
			"name":      variable.Name,
			"timestamp": variable.Timestamp,
			"type":      "large_data_update",
			"size":      len(variable.Value),
			"error":     "Failed to parse JSON",
		}
	}

	if dataMap, ok := jsonData.(map[string]interface{}); ok {
		// Calculate basic metadata
		metadata := api.calculateLargeDataMetadata(dataMap, variable)

		return map[string]interface{}{
			"name":      variable.Name,
			"timestamp": variable.Timestamp,
			"type":      "large_data_update",
			"metadata": map[string]interface{}{
				"columns":    metadata.Columns,
				"rowCount":   metadata.RowCount,
				"dataSize":   metadata.DataSize,
				"isColumnar": metadata.IsColumnar,
				"hasData":    metadata.HasData,
			},
			// Include just the column names and types, not the data
			"preview": "Use /api/variables/large?name=" + variable.Name + "&preview=true for data",
		}
	}

	// Non-object JSON
	return map[string]interface{}{
		"name":      variable.Name,
		"timestamp": variable.Timestamp,
		"type":      "large_data_update",
		"size":      len(variable.Value),
		"preview":   "Use /api/variables/large?name=" + variable.Name + " for data",
	}
}

// Legacy stimulus endpoint - now just calls the general endpoint
func (api *APIServer) HandleStimulusData(w http.ResponseWriter, r *http.Request) {
	// Add the stimulus variable name and forward to general handler
	q := r.URL.Query()
	q.Set("name", "ess/stiminfo")
	r.URL.RawQuery = q.Encode()

	api.HandleLargeData(w, r)
}
