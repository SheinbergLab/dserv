<template>
  <div class="panel">
    <div class="panel-header">
      <span>Variable Query</span>
      <div class="panel-controls">
        <button @click="clearQueryHistory" class="panel-btn" title="Clear History">🗑️</button>
        <button @click="refreshQuery" class="panel-btn" title="Refresh">🔄</button>
      </div>
    </div>
    
    <div class="panel-content">
      <!-- Query Form -->
      <div class="query-form">
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

      <!-- Query Result -->
      <div v-if="queryResult" class="result-area">
        <div class="result-header">
          <span>Query Result:</span>
          <span class="result-timestamp">{{ formatTimestamp(queryResult.timestamp) }}</span>
        </div>
        <div class="result-content" :class="{ error: queryResult.error }">
          <div v-if="queryResult.error" class="error-message">
            <strong>Error:</strong> {{ queryResult.error }}
          </div>
          <div v-else class="success-message">
            <div class="result-variable">{{ queryResult.variable }}</div>
            <div class="result-value">{{ queryResult.value }}</div>
          </div>
        </div>
      </div>

      <!-- Recent Queries -->
      <div v-if="queryHistory.length > 0" class="history-section">
        <div class="section-header">
          <span>Recent Queries:</span>
          <button @click="clearQueryHistory" class="clear-btn">Clear All</button>
        </div>
        <div class="history-list">
          <div 
            v-for="(query, index) in queryHistory" 
            :key="index"
            class="history-item"
            @click="selectHistoryItem(query)"
          >
            <div class="history-variable">{{ query.variable }}</div>
            <div class="history-value">{{ query.value || query.error }}</div>
            <div class="history-time">{{ formatTime(query.timestamp) }}</div>
          </div>
        </div>
      </div>

      <!-- Quick Actions -->
      <div class="quick-actions">
        <div class="section-header">Quick Queries:</div>
        <div class="quick-buttons">
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
const queryVariable = ref('')
const queryResult = ref(null)
const queryLoading = ref(false)
const queryHistory = ref([])
const recentVariables = ref([])

// Quick access variables
const quickVariables = [
  'ess/system',
  'ess/protocol', 
  'ess/variant',
  'ess/em_pos',
  'ess/temperature',
  'ess/voltage'
]

// Methods
const executeQuery = async () => {
  if (!queryVariable.value.trim() || queryLoading.value) return
  
  queryLoading.value = true
  
  try {
    const response = await fetch('/api/query', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ variable: queryVariable.value.trim() })
    })
    
    const result = await response.json()
    result.timestamp = new Date().toISOString()
    queryResult.value = result
    
    // Add to history
    addToHistory(result)
    
    // Add to recent variables
    addToRecentVariables(queryVariable.value.trim())
    
  } catch (error) {
    queryResult.value = {
      variable: queryVariable.value,
      error: `Network error: ${error.message}`,
      timestamp: new Date().toISOString()
    }
  } finally {
    queryLoading.value = false
  }
}

const quickQuery = (variable) => {
  queryVariable.value = variable
  executeQuery()
}

const clearQuery = () => {
  queryVariable.value = ''
  queryResult.value = null
}

const addToHistory = (result) => {
  queryHistory.value.unshift(result)
  if (queryHistory.value.length > 50) {
    queryHistory.value = queryHistory.value.slice(0, 50)
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

const selectHistoryItem = (query) => {
  queryVariable.value = query.variable
  queryResult.value = query
}

const clearQueryHistory = () => {
  queryHistory.value = []
  localStorage.removeItem('dserv-query-history')
}

const refreshQuery = () => {
  if (queryVariable.value.trim()) {
    executeQuery()
  }
}

const formatTimestamp = (timestamp) => {
  return new Date(timestamp).toLocaleString()
}

const formatTime = (timestamp) => {
  return new Date(timestamp).toLocaleTimeString()
}

const saveHistory = () => {
  localStorage.setItem('dserv-query-history', JSON.stringify(queryHistory.value))
}

const loadHistory = () => {
  try {
    const saved = localStorage.getItem('dserv-query-history')
    if (saved) {
      queryHistory.value = JSON.parse(saved)
    }
  } catch (error) {
    console.warn('Failed to load query history:', error)
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

.query-form {
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

.text-input {
  width: 100%;
  padding: 4px 6px;
  border: 2px inset #f0f0f0;
  font-size: 12px;
}

.form-actions {
  display: flex;
  gap: 6px;
}

.primary-btn, .secondary-btn {
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

.secondary-btn {
  background: linear-gradient(to bottom, #fff, #e0e0e0);
}

.primary-btn:hover:not(:disabled) {
  background: linear-gradient(to bottom, #45a049, #3e8e41);
}

.secondary-btn:hover:not(:disabled) {
  background: linear-gradient(to bottom, #fff, #d0d0d0);
}

.primary-btn:disabled {
  opacity: 0.6;
  cursor: not-allowed;
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

.quick-btn {
  padding: 4px 8px;
  font-size: 10px;
  border: 1px solid #999;
  background: linear-gradient(to bottom, #fff, #e0e0e0);
  cursor: pointer;
  border-radius: 2px;
}

.quick-btn:hover:not(:disabled) {
  background: linear-gradient(to bottom, #fff, #d0d0d0);
}

.quick-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}
</style>