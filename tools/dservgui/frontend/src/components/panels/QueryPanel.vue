<template>
  <div class="panel">
    <div class="panel-header">
      <span>Variable Query & Set</span>
      <div class="panel-controls">
        <button @click="toggleMode" class="panel-btn" :title="currentMode === 'query' ? 'Switch to Set Mode' : 'Switch to Query Mode'">
          {{ currentMode === 'query' ? '🔍' : '✏️' }}
        </button>
        <button @click="clearQueryHistory" class="panel-btn" title="Clear History">🗑️</button>
        <button @click="refreshQuery" class="panel-btn" title="Refresh">🔄</button>
      </div>
    </div>
    
    <div class="panel-content">
      <!-- Mode Tabs -->
      <div class="mode-tabs">
        <button 
          @click="switchToMode('query')" 
          class="mode-tab" 
          :class="{ active: currentMode === 'query' }"
        >
          Query Variable
        </button>
        <button 
          @click="switchToMode('set')" 
          class="mode-tab" 
          :class="{ active: currentMode === 'set' }"
        >
          Set Variable
        </button>
      </div>

      <!-- Query Form -->
      <div v-if="currentMode === 'query'" class="query-form">
        <div class="form-group">
          <label>Variable Name:</label>
          <input 
            v-model="queryVariable" 
            type="text" 
            class="text-input"
            placeholder="Enter variable name (e.g., ess/em_pos)"
            @keyup.enter="executeQuery"
            :disabled="queryLoading"
            list="variable-suggestions"
          />
          <datalist id="variable-suggestions">
            <option v-for="variable in recentVariables" :key="variable" :value="variable" />
          </datalist>
        </div>
        
        <div class="form-actions">
          <button 
            @click="executeQuery" 
            class="primary-btn" 
            :disabled="!queryVariable.trim() || queryLoading"
          >
            {{ queryLoading ? 'Querying...' : 'Query' }}
          </button>
          <button @click="clearQuery" class="secondary-btn">
            Clear
          </button>
        </div>
      </div>

      <!-- Set Form -->
      <div v-if="currentMode === 'set'" class="set-form">
        <div class="form-group">
          <label>Variable Name:</label>
          <input 
            v-model="setVariable" 
            type="text" 
            class="text-input"
            placeholder="Enter variable name (e.g., ess/status)"
            :disabled="setLoading"
            list="variable-suggestions"
          />
        </div>
        
        <div class="form-group">
          <label>Value:</label>
          <textarea 
            v-model="setValue" 
            class="text-area"
            placeholder="Enter value as string (e.g., hello, 123, true, stopped)"
            rows="3"
            :disabled="setLoading"
          ></textarea>
          <div class="value-hint">
            <small>Value will be sent as string to dserv</small>
          </div>
        </div>
        
        <div class="form-actions">
          <button 
            @click="executeSet" 
            class="primary-btn set-btn" 
            :disabled="!setVariable.trim() || !setValue.trim() || setLoading"
          >
            {{ setLoading ? 'Setting...' : 'Set Variable' }}
          </button>
          <button @click="clearSet" class="secondary-btn">
            Clear
          </button>
          <button @click="setAndQuery" class="tertiary-btn" :disabled="!setVariable.trim() || !setValue.trim() || setLoading">
            Set & Query
          </button>
        </div>
      </div>

      <!-- Result Display -->
      <div v-if="queryResult || setResult" class="result-area">
        <div class="result-header">
          <span>{{ currentMode === 'query' ? 'Query' : 'Set' }} Result:</span>
          <span class="result-timestamp">{{ formatTimestamp((queryResult || setResult).timestamp) }}</span>
        </div>
        <div class="result-content" :class="{ error: (queryResult || setResult).error }">
          <div v-if="(queryResult || setResult).error" class="error-message">
            <strong>Error:</strong> {{ (queryResult || setResult).error }}
          </div>
          <div v-else class="success-message">
            <div class="result-variable">{{ (queryResult || setResult).variable }}</div>
            <div v-if="currentMode === 'query'" class="result-value">{{ queryResult.value }}</div>
            <div v-else class="result-status">✅ Variable set successfully</div>
          </div>
        </div>
      </div>

      <!-- Recent Operations -->
      <div v-if="operationHistory.length > 0" class="history-section">
        <div class="section-header">
          <span>Recent Operations:</span>
          <button @click="clearOperationHistory" class="clear-btn">Clear All</button>
        </div>
        <div class="history-list">
          <div 
            v-for="(operation, index) in operationHistory" 
            :key="index"
            class="history-item"
            @click="selectHistoryItem(operation)"
          >
            <div class="history-variable">
              <span class="operation-type">{{ operation.type.toUpperCase() }}</span>
              {{ operation.variable }}
            </div>
            <div class="history-value">{{ operation.value || operation.error }}</div>
            <div class="history-time">{{ formatTime(operation.timestamp) }}</div>
          </div>
        </div>
      </div>

      <!-- Quick Actions -->
      <div class="quick-actions">
        <div class="section-header">Quick {{ currentMode === 'query' ? 'Queries' : 'Sets' }}:</div>
        <div v-if="currentMode === 'query'" class="quick-buttons">
          <button 
            v-for="variable in quickVariables" 
            :key="variable"
            class="quick-btn"
            @click="quickQuery(variable)"
            :disabled="queryLoading"
          >
            {{ variable }}
          </button>
        </div>
        <div v-else class="quick-sets">
          <div class="quick-set-group">
            <span class="quick-set-label">ess/status:</span>
            <button @click="quickSet('ess/status', 'active')" class="quick-btn small">Active</button>
            <button @click="quickSet('ess/status', 'inactive')" class="quick-btn small">Inactive</button>
            <button @click="quickSet('ess/status', 'error')" class="quick-btn small">Error</button>
          </div>
          <div class="quick-set-group">
            <span class="quick-set-label">ess/em_pos:</span>
            <button @click="quickSet('ess/em_pos', '0')" class="quick-btn small">0</button>
            <button @click="quickSet('ess/em_pos', '1')" class="quick-btn small">1</button>
            <button @click="quickSet('ess/em_pos', '2')" class="quick-btn small">2</button>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted } from 'vue'

defineProps({
  isConnected: {
    type: Boolean,
    default: false
  }
})

// State
const currentMode = ref('query')
const queryVariable = ref('')
const queryResult = ref(null)
const queryLoading = ref(false)
const setVariable = ref('')
const setValue = ref('')
const setResult = ref(null)
const setLoading = ref(false)
const operationHistory = ref([])
const recentVariables = ref([])

// Quick access variables
const quickVariables = [
  'ess/system',
  'ess/protocol', 
  'ess/variant',
  'ess/status',
  'ess/em_pos',
  'ess/temperature',
  'ess/voltage'
]

// Methods
const toggleMode = () => {
  const newMode = currentMode.value === 'query' ? 'set' : 'query'
  switchToMode(newMode)
}

const switchToMode = (mode) => {
  console.log(`QueryPanel: Switching to mode: ${mode}`)
  currentMode.value = mode
  // Clear results from the other mode to avoid confusion
  if (mode === 'query') {
    setResult.value = null
  } else {
    queryResult.value = null
  }
}
const executeQuery = async () => {
  if (!queryVariable.value.trim() || queryLoading.value) return
  
  console.log(`QueryPanel: Executing query for ${queryVariable.value}`)
  queryLoading.value = true
  setResult.value = null // Clear set result when querying
  
  try {
    const response = await fetch('/api/query', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ variable: queryVariable.value.trim() })
    })
    
    const result = await response.json()
    result.timestamp = new Date().toISOString()
    result.type = 'query'
    queryResult.value = result
    
    console.log('QueryPanel: Query result:', result)
    
    // Add to history
    addToHistory(result)
    
    // Add to recent variables
    addToRecentVariables(queryVariable.value.trim())
    
  } catch (error) {
    const errorResult = {
      variable: queryVariable.value,
      error: `Network error: ${error.message}`,
      timestamp: new Date().toISOString(),
      type: 'query'
    }
    queryResult.value = errorResult
    addToHistory(errorResult)
    console.error('QueryPanel: Query error:', error)
  } finally {
    queryLoading.value = false
  }
}

const executeSet = async () => {
  if (!setVariable.value.trim() || !setValue.value.trim() || setLoading.value) return
  
  console.log(`QueryPanel: Executing set for ${setVariable.value} = ${setValue.value}`)
  setLoading.value = true
  queryResult.value = null // Clear query result when setting
  
  try {
    // For now, always send as string (as requested)
    const valueToSend = setValue.value.trim()
    
    const response = await fetch('/api/set', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ 
        variable: setVariable.value.trim(),
        value: valueToSend
      })
    })
    
    const result = await response.json()
    result.timestamp = new Date().toISOString()
    result.type = 'set'
    result.value = valueToSend // Store the value we set
    setResult.value = result
    
    console.log('QueryPanel: Set result:', result)
    
    // Add to history
    addToHistory(result)
    
    // Add to recent variables
    addToRecentVariables(setVariable.value.trim())
    
  } catch (error) {
    const errorResult = {
      variable: setVariable.value,
      error: `Network error: ${error.message}`,
      timestamp: new Date().toISOString(),
      type: 'set'
    }
    setResult.value = errorResult
    addToHistory(errorResult)
    console.error('QueryPanel: Set error:', error)
  } finally {
    setLoading.value = false
  }
}

const setAndQuery = async () => {
  console.log('QueryPanel: Executing set and query operation')
  await executeSet()
  if (setResult.value && !setResult.value.error) {
    // Wait a moment for the set to propagate, then query
    setTimeout(() => {
      console.log('QueryPanel: Switching to query mode after set')
      switchToMode('query')
      queryVariable.value = setVariable.value
      executeQuery()
    }, 100)
  }
}

const quickQuery = (variable) => {
  console.log(`QueryPanel: Quick query for ${variable}`)
  switchToMode('query')
  queryVariable.value = variable
  executeQuery()
}

const quickSet = (variable, value) => {
  console.log(`QueryPanel: Quick set for ${variable} = ${value}`)
  switchToMode('set')
  setVariable.value = variable
  setValue.value = value
  executeSet()
}

const clearQuery = () => {
  queryVariable.value = ''
  queryResult.value = null
}

const clearSet = () => {
  setVariable.value = ''
  setValue.value = ''
  setResult.value = null
}

const addToHistory = (result) => {
  operationHistory.value.unshift(result)
  if (operationHistory.value.length > 50) {
    operationHistory.value = operationHistory.value.slice(0, 50)
  }
  saveHistory()
}

const addToRecentVariables = (variable) => {
  if (!recentVariables.value.includes(variable)) {
    recentVariables.value.unshift(variable)
    if (recentVariables.value.length > 20) {
      recentVariables.value.pop()
    }
    saveRecentVariables()
  }
}

const selectHistoryItem = (operation) => {
  console.log(`QueryPanel: Selecting history item: ${operation.type} ${operation.variable}`)
  if (operation.type === 'query') {
    switchToMode('query')
    queryVariable.value = operation.variable
    queryResult.value = operation
  } else {
    switchToMode('set')
    setVariable.value = operation.variable
    setValue.value = typeof operation.value === 'string' ? operation.value : JSON.stringify(operation.value)
    setResult.value = operation
  }
}

const clearQueryHistory = () => {
  clearOperationHistory()
}

const clearOperationHistory = () => {
  operationHistory.value = []
  localStorage.removeItem('dserv-operation-history')
}

const refreshQuery = () => {
  if (currentMode.value === 'query' && queryVariable.value.trim()) {
    executeQuery()
  } else if (currentMode.value === 'set' && setVariable.value.trim() && setValue.value.trim()) {
    executeSet()
  }
}

const formatTimestamp = (timestamp) => {
  return new Date(timestamp).toLocaleString()
}

const formatTime = (timestamp) => {
  return new Date(timestamp).toLocaleTimeString()
}

const saveHistory = () => {
  localStorage.setItem('dserv-operation-history', JSON.stringify(operationHistory.value))
}

const loadHistory = () => {
  try {
    const saved = localStorage.getItem('dserv-operation-history')
    if (saved) {
      operationHistory.value = JSON.parse(saved)
    }
  } catch (error) {
    console.warn('Failed to load operation history:', error)
  }
}

const saveRecentVariables = () => {
  localStorage.setItem('dserv-recent-variables', JSON.stringify(recentVariables.value))
}

const loadRecentVariables = () => {
  try {
    const saved = localStorage.getItem('dserv-recent-variables')
    if (saved) {
      recentVariables.value = JSON.parse(saved)
    }
  } catch (error) {
    console.warn('Failed to load recent variables:', error)
  }
}

onMounted(() => {
  loadHistory()
  loadRecentVariables()
})
</script>

<style scoped>
.panel {
  height: 100%;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}

.panel-header {
  height: 28px;
  background: linear-gradient(to bottom, #f0f0f0, #e0e0e0);
  border-bottom: 1px solid #ccc;
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 8px;
  font-weight: 500;
  font-size: 12px;
  flex-shrink: 0;
}

.panel-controls {
  display: flex;
  gap: 4px;
}

.panel-btn {
  width: 20px;
  height: 20px;
  border: 1px solid #999;
  background: linear-gradient(to bottom, #fff, #e0e0e0);
  font-size: 12px;
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: 2px;
}

.panel-btn:hover {
  background: linear-gradient(to bottom, #fff, #d0d0d0);
}

.panel-content {
  flex: 1;
  padding: 12px;
  overflow-y: auto;
}

.mode-tabs {
  display: flex;
  margin-bottom: 16px;
  border-bottom: 1px solid #ddd;
}

.mode-tab {
  flex: 1;
  padding: 8px 12px;
  background: #f5f5f5;
  border: 1px solid #ddd;
  border-bottom: none;
  cursor: pointer;
  font-size: 12px;
  transition: background-color 0.2s;
}

.mode-tab.active {
  background: white;
  border-bottom: 1px solid white;
  margin-bottom: -1px;
  font-weight: 500;
}

.mode-tab:not(.active):hover {
  background: #ebebeb;
}

.query-form, .set-form {
  margin-bottom: 20px;
}

.form-group {
  margin-bottom: 12px;
}

.form-group label {
  display: block;
  margin-bottom: 4px;
  font-weight: 500;
  font-size: 12px;
}

.text-input, .text-area {
  width: 100%;
  padding: 4px 6px;
  border: 2px inset #f0f0f0;
  font-size: 12px;
  font-family: inherit;
}

.text-area {
  resize: vertical;
  min-height: 60px;
}

.value-hint {
  margin-top: 4px;
  color: #666;
}

.form-actions {
  display: flex;
  gap: 6px;
}

.primary-btn, .secondary-btn, .tertiary-btn {
  padding: 4px 12px;
  font-size: 12px;
  border: 1px solid #999;
  cursor: pointer;
  border-radius: 2px;
}

.primary-btn {
  background: linear-gradient(to bottom, #4CAF50, #45a049);
  color: white;
}

.primary-btn.set-btn {
  background: linear-gradient(to bottom, #2196F3, #1976D2);
}

.secondary-btn {
  background: linear-gradient(to bottom, #fff, #e0e0e0);
}

.tertiary-btn {
  background: linear-gradient(to bottom, #FF9800, #F57C00);
  color: white;
}

.primary-btn:hover:not(:disabled) {
  background: linear-gradient(to bottom, #45a049, #3e8e41);
}

.primary-btn.set-btn:hover:not(:disabled) {
  background: linear-gradient(to bottom, #1976D2, #1565C0);
}

.secondary-btn:hover:not(:disabled) {
  background: linear-gradient(to bottom, #fff, #d0d0d0);
}

.tertiary-btn:hover:not(:disabled) {
  background: linear-gradient(to bottom, #F57C00, #E65100);
}

.primary-btn:disabled, .tertiary-btn:disabled {
  opacity: 0.6;
  cursor: not-allowed;
}

.result-status {
  background: #e8f5e8;
  color: #2e7d32;
  padding: 4px;
  border-radius: 2px;
  border: 1px solid #4caf50;
  font-weight: 500;
}

.result-area {
  margin-bottom: 20px;
  border: 1px solid #ccc;
  border-radius: 2px;
}

.result-header {
  padding: 4px 8px;
  background: #f0f0f0;
  font-weight: 500;
  font-size: 11px;
  border-bottom: 1px solid #ccc;
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.result-timestamp {
  color: #666;
  font-size: 10px;
}

.result-content {
  padding: 8px;
  font-family: monospace;
  font-size: 12px;
}

.result-content.error {
  background: #ffebee;
}

.error-message {
  color: #d32f2f;
}

.success-message {
  color: #2e7d32;
}

.result-variable {
  font-weight: bold;
  margin-bottom: 4px;
}

.result-value {
  background: #f8f9fa;
  padding: 4px;
  border-radius: 2px;
  border: 1px solid #e9ecef;
}

.section-header {
  font-weight: 500;
  font-size: 12px;
  margin-bottom: 8px;
  color: #333;
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.clear-btn {
  font-size: 10px;
  padding: 2px 6px;
  border: 1px solid #999;
  background: #fff;
  cursor: pointer;
  border-radius: 2px;
}

.clear-btn:hover {
  background: #f0f0f0;
}

.history-list {
  max-height: 200px;
  overflow-y: auto;
  border: 1px solid #ddd;
  border-radius: 2px;
}

.history-item {
  padding: 6px 8px;
  border-bottom: 1px solid #f0f0f0;
  cursor: pointer;
  font-size: 11px;
}

.history-item:hover {
  background: #f8f9fa;
}

.history-item:last-child {
  border-bottom: none;
}

.history-variable {
  font-weight: 500;
  color: #2196F3;
  display: flex;
  align-items: center;
  gap: 4px;
}

.operation-type {
  font-size: 9px;
  padding: 1px 4px;
  border-radius: 2px;
  background: #e3f2fd;
  color: #1976d2;
  font-weight: 600;
}

.history-value {
  color: #333;
  margin: 2px 0;
  font-family: monospace;
}

.history-time {
  color: #666;
  font-size: 10px;
}

.quick-actions {
  margin-top: 20px;
}

.quick-buttons {
  display: flex;
  flex-wrap: wrap;
  gap: 4px;
}

.quick-sets {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.quick-set-group {
  display: flex;
  align-items: center;
  gap: 6px;
  flex-wrap: wrap;
}

.quick-set-label {
  font-size: 11px;
  font-weight: 500;
  color: #666;
  min-width: 80px;
}

.quick-btn {
  padding: 4px 8px;
  font-size: 10px;
  border: 1px solid #999;
  background: linear-gradient(to bottom, #fff, #e0e0e0);
  cursor: pointer;
  border-radius: 2px;
}

.quick-btn.small {
  padding: 2px 6px;
  font-size: 9px;
}

.quick-btn:hover:not(:disabled) {
  background: linear-gradient(to bottom, #fff, #d0d0d0);
}

.quick-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}
</style>