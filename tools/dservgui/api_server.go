package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"time"

	"github.com/gorilla/websocket"
)

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
}

// Default channel routing - route high-frequency variables through WebSocket
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
	value, err := api.dservClient.Query(req.Variable)

	response := QueryResponse{
		Variable: req.Variable,
		Success:  err == nil,
	}

	if err != nil {
		response.Error = err.Error()
	} else {
		response.Value = value
	}

	api.sendJSON(w, response)
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

			// Send variable update
			data, _ := json.Marshal(variable)
			fmt.Fprintf(w, "data: %s\n\n", data)

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
