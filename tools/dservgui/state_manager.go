package main

import (
	"log"
	"strings"
	"sync"
	"time"
)

// Variable represents a stored variable with minimal metadata
type Variable struct {
	Name      string    `json:"name"`
	Value     string    `json:"value"` // Store raw value as-is
	Timestamp time.Time `json:"timestamp"`
}

// StateManager handles storage and forwarding of all variables
type StateManager struct {
	// Thread-safe storage
	mutex     sync.RWMutex
	variables map[string]Variable

	// Update subscribers
	subscribersMutex sync.RWMutex
	subscribers      []chan Variable

	// Simple statistics
	totalUpdates int
}

// NewStateManager creates a new state manager
func NewStateManager() *StateManager {
	return &StateManager{
		variables:   make(map[string]Variable),
		subscribers: make([]chan Variable, 0),
	}
}

// ProcessUpdate processes a new variable update from dserv
func (sm *StateManager) ProcessUpdate(name, value string) {
	sm.mutex.Lock()
	sm.totalUpdates++

	// Create variable record - store everything as-is
	variable := Variable{
		Name:      name,
		Value:     value,
		Timestamp: time.Now(),
	}

	// Store the variable
	sm.variables[name] = variable
	sm.mutex.Unlock()

	// Log the update
	sm.logUpdate(variable)

	// Forward to all subscribers immediately
	sm.notifySubscribers(variable)
}

// logUpdate provides simple logging
func (sm *StateManager) logUpdate(variable Variable) {
	// Log large data streams by size only
	if variable.Name == "stimdg" || variable.Name == "trialdg" {
		//		dataSize := len(variable.Value)
		//		log.Printf("📊 %s: %d bytes", variable.Name, dataSize)
		return
	}

	// Log important variables that might be set via API
	if strings.Contains(variable.Name, "ess/status") ||
		strings.Contains(variable.Name, "ess/em_pos") ||
		strings.Contains(variable.Name, "ess/subject") ||
		strings.Contains(variable.Name, "test/") {
		log.Printf("📊 %s = %s", variable.Name, variable.Value)
	}

	// Log all other variables with their values (commented out for now)
	//	log.Printf("📊 %s = %s", variable.Name, variable.Value)
}

// notifySubscribers sends the variable update to all subscribers
func (sm *StateManager) notifySubscribers(variable Variable) {
	sm.subscribersMutex.RLock()
	defer sm.subscribersMutex.RUnlock()

	// Send to all subscribers (non-blocking)
	for i := len(sm.subscribers) - 1; i >= 0; i-- {
		select {
		case sm.subscribers[i] <- variable:
			// Successfully sent
		default:
			// Channel full or closed, remove it
			close(sm.subscribers[i])
			sm.subscribers = append(sm.subscribers[:i], sm.subscribers[i+1:]...)
			log.Printf("⚠️  Removed blocked subscriber channel")
		}
	}
}

// Subscribe returns a channel that receives variable updates
func (sm *StateManager) Subscribe() chan Variable {
	sm.subscribersMutex.Lock()
	defer sm.subscribersMutex.Unlock()

	ch := make(chan Variable, 100) // Buffered to prevent blocking
	sm.subscribers = append(sm.subscribers, ch)
	return ch
}

// Unsubscribe removes a subscriber channel
func (sm *StateManager) Unsubscribe(ch chan Variable) {
	sm.subscribersMutex.Lock()
	defer sm.subscribersMutex.Unlock()

	for i, subscriber := range sm.subscribers {
		if subscriber == ch {
			close(ch)
			sm.subscribers = append(sm.subscribers[:i], sm.subscribers[i+1:]...)
			break
		}
	}
}

// GetVariable retrieves a specific variable
func (sm *StateManager) GetVariable(name string) (Variable, bool) {
	sm.mutex.RLock()
	defer sm.mutex.RUnlock()

	variable, exists := sm.variables[name]
	return variable, exists
}

// GetAllVariables returns all stored variables
func (sm *StateManager) GetAllVariables() map[string]Variable {
	sm.mutex.RLock()
	defer sm.mutex.RUnlock()

	// Return a copy to prevent external modification
	result := make(map[string]Variable)
	for k, v := range sm.variables {
		result[k] = v
	}
	return result
}

// GetVariablesByPrefix returns variables with names starting with prefix
func (sm *StateManager) GetVariablesByPrefix(prefix string) map[string]Variable {
	sm.mutex.RLock()
	defer sm.mutex.RUnlock()

	result := make(map[string]Variable)
	for name, variable := range sm.variables {
		if len(name) >= len(prefix) && name[:len(prefix)] == prefix {
			result[name] = variable
		}
	}
	return result
}

// GetStats returns state manager statistics
func (sm *StateManager) GetStats() map[string]interface{} {
	sm.mutex.RLock()
	defer sm.mutex.RUnlock()

	return map[string]interface{}{
		"total_variables": len(sm.variables),
		"total_updates":   sm.totalUpdates,
		"subscribers":     len(sm.subscribers),
	}
}

// IsHealthy returns whether the state manager is functioning properly
func (sm *StateManager) IsHealthy() bool {
	sm.mutex.RLock()
	defer sm.mutex.RUnlock()

	// Check if we have recent updates
	recentUpdates := false
	cutoff := time.Now().Add(-5 * time.Minute)

	for _, variable := range sm.variables {
		if variable.Timestamp.After(cutoff) {
			recentUpdates = true
			break
		}
	}

	// Consider healthy if we have variables and recent updates
	return len(sm.variables) > 0 && recentUpdates
}
