<template>
  <div class="dev-tools" v-if="isDevelopment">
    <div class="dev-tools-toggle" @click="expanded = !expanded">
      <span class="toggle-icon">{{ expanded ? '▼' : '▶' }}</span>
      <span>Dev Tools</span>
      <span class="status-dot" :class="{ 'online': isOnline, 'offline': !isOnline }">●</span>
    </div>
    
    <div class="dev-tools-panel" v-if="expanded">
      <!-- Connection Status -->
      <div class="dev-section">
        <h4>Connection Status</h4>
        <div class="status-grid">
          <div class="status-item">
            <span class="label">Backend:</span>
            <span class="value" :class="connectionStatus.backend">{{ connectionStatus.backend }}</span>
          </div>
          <div class="status-item">
            <span class="label">ESS:</span>
            <span class="value" :class="essStatus">{{ essStatus }}</span>
          </div>
          <div class="status-item">
            <span class="label">WebSocket:</span>
            <span class="value" :class="connectionStatus.websocket">{{ connectionStatus.websocket }}</span>
          </div>
        </div>
      </div>

      <!-- ESS State -->
      <div class="dev-section">
        <h4>ESS State</h4>
        <div class="state-grid">
          <div class="state-item">
            <span class="label">System:</span>
            <span class="value">{{ essData.currentSystem || 'none' }}</span>
          </div>
          <div class="state-item">
            <span class="label">Protocol:</span>
            <span class="value">{{ essData.currentProtocol || 'none' }}</span>
          </div>
          <div class="state-item">
            <span class="label">Variant:</span>
            <span class="value">{{ essData.currentVariant || 'none' }}</span>
          </div>
          <div class="state-item">
            <span class="label">Variables:</span>
            <span class="value">{{ Object.keys(essData.variables || {}).length }}</span>
          </div>
        </div>
      </div>

      <!-- Performance Metrics -->
      <div class="dev-section">
        <h4>Performance</h4>
        <div class="perf-grid">
          <div class="perf-item">
            <span class="label">Last API Call:</span>
            <span class="value">{{ lastApiCall || 'none' }}</span>
          </div>
          <div class="perf-item">
            <span class="label">API Response Time:</span>
            <span class="value">{{ apiResponseTime }}ms</span>
          </div>
          <div class="perf-item">
            <span class="label">Update Frequency:</span>
            <span class="value">{{ updateFrequency }}/sec</span>
          </div>
        </div>
      </div>

      <!-- Quick Actions -->
      <div class="dev-section">
        <h4>Quick Actions</h4>
        <div class="action-buttons">
          <button @click="testConnection" class="dev-btn">Test Connection</button>
          <button @click="refreshEssState" class="dev-btn">Refresh ESS</button>
          <button @click="clearLogs" class="dev-btn">Clear Logs</button>
          <button @click="exportState" class="dev-btn">Export State</button>
        </div>
      </div>

      <!-- Debug Controls -->
      <div class="dev-section">
        <h4>Debug Settings</h4>
        <div class="debug-controls">
          <label>
            Log Level:
            <select v-model="debugLevel" @change="updateDebugLevel">
              <option value="error">Error</option>
              <option value="warn">Warning</option>
              <option value="info">Info</option>
              <option value="debug">Debug</option>
              <option value="trace">Trace</option>
            </select>
          </label>
          <label>
            <input type="checkbox" v-model="showApiCalls" @change="toggleApiLogging">
            Log API Calls
          </label>
          <label>
            <input type="checkbox" v-model="showStateChanges" @change="toggleStateLogging">
            Log State Changes
          </label>
        </div>
      </div>

      <!-- Recent Logs -->
      <div class="dev-section">
        <h4>Recent Logs</h4>
        <div class="log-viewer">
          <div 
            v-for="(log, index) in recentLogs" 
            :key="index" 
            class="log-entry" 
            :class="log.level"
          >
            <span class="log-time">{{ log.time }}</span>
            <span class="log-level">{{ log.level.toUpperCase() }}</span>
            <span class="log-message">{{ log.message }}</span>
          </div>
          <div v-if="recentLogs.length === 0" class="no-logs">
            No recent logs
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, inject, onMounted, onUnmounted, watch } from 'vue'

// Only show in development
const isDevelopment = import.meta.env.DEV

// Component state
const expanded = ref(false)
const isOnline = ref(navigator.onLine)
const lastApiCall = ref('')
const apiResponseTime = ref(0)
const updateFrequency = ref(0)
const debugLevel = ref(localStorage.getItem('dservgui-debug-level') || 'info')
const showApiCalls = ref(localStorage.getItem('dservgui-log-api') === 'true')
const showStateChanges = ref(localStorage.getItem('dservgui-log-state') === 'true')
const recentLogs = ref([])

// Inject services
const connectionService = inject('connectionService', null)
const essState = inject('essState', null)

// Computed properties
const connectionStatus = computed(() => {
  try {
    return {
      backend: connectionService?.isConnected?.value ? 'connected' : 'disconnected',
      websocket: connectionService?.wsConnected?.value ? 'connected' : 'disconnected'
    }
  } catch (error) {
    return {
      backend: 'unknown',
      websocket: 'unknown'
    }
  }
})

const essStatus = computed(() => {
  try {
    return essState?.variables?.['ess/status']?.value || 'unknown'
  } catch (error) {
    return 'unknown'
  }
})

const essData = computed(() => {
  try {
    return {
      currentSystem: essState?.currentSystem,
      currentProtocol: essState?.currentProtocol,
      currentVariant: essState?.currentVariant,
      variables: essState?.variables
    }
  } catch (error) {
    return {
      currentSystem: null,
      currentProtocol: null,
      currentVariant: null,
      variables: {}
    }
  }
})

// Performance tracking
let updateCount = 0
let updateInterval = null

onMounted(() => {
  // Track online status
  window.addEventListener('online', () => { isOnline.value = true })
  window.addEventListener('offline', () => { isOnline.value = false })
  
  // Track update frequency
  updateInterval = setInterval(() => {
    updateFrequency.value = updateCount
    updateCount = 0
  }, 1000)
  
  // Intercept fetch for API timing
  if (isDevelopment) {
    const originalFetch = window.fetch
    window.fetch = async (...args) => {
      const start = performance.now()
      const [url] = args
      
      try {
        const response = await originalFetch(...args)
        const end = performance.now()
        
        if (url.includes('/api/')) {
          lastApiCall.value = new URL(url, window.location.origin).pathname
          apiResponseTime.value = Math.round(end - start)
          
          if (showApiCalls.value) {
            addLog('debug', `API: ${lastApiCall.value} (${apiResponseTime.value}ms)`)
          }
        }
        
        return response
      } catch (error) {
        const end = performance.now()
        apiResponseTime.value = Math.round(end - start)
        
        if (showApiCalls.value) {
          addLog('error', `API Error: ${url} - ${error.message}`)
        }
        
        throw error
      }
    }
  }
})

onUnmounted(() => {
  if (updateInterval) {
    clearInterval(updateInterval)
  }
})

// Watch for state changes
watch(() => essState?.variables, () => {
  updateCount++
  if (showStateChanges.value) {
    const varCount = Object.keys(essState?.variables || {}).length
    addLog('debug', `ESS state updated: ${varCount} variables`)
  }
}, { deep: true })

// Methods
const addLog = (level, message) => {
  const log = {
    time: new Date().toLocaleTimeString(),
    level,
    message
  }
  
  recentLogs.value.unshift(log)
  
  // Keep only last 20 logs
  if (recentLogs.value.length > 20) {
    recentLogs.value = recentLogs.value.slice(0, 20)
  }
}

const testConnection = async () => {
  addLog('info', 'Testing connection...')
  try {
    const response = await fetch('/api/status')
    if (response.ok) {
      addLog('info', 'Connection test successful')
    } else {
      addLog('error', `Connection test failed: ${response.status}`)
    }
  } catch (error) {
    addLog('error', `Connection test error: ${error.message}`)
  }
}

const refreshEssState = async () => {
  addLog('info', 'Refreshing ESS state...')
  try {
    const response = await fetch('/api/ess/variables')
    if (response.ok) {
      addLog('info', 'ESS state refreshed')
    } else {
      addLog('error', `ESS refresh failed: ${response.status}`)
    }
  } catch (error) {
    addLog('error', `ESS refresh error: ${error.message}`)
  }
}

const clearLogs = () => {
  recentLogs.value = []
  addLog('info', 'Logs cleared')
}

const exportState = () => {
  const state = {
    timestamp: new Date().toISOString(),
    connection: connectionStatus.value,
    ess: essData.value,
    performance: {
      lastApiCall: lastApiCall.value,
      apiResponseTime: apiResponseTime.value,
      updateFrequency: updateFrequency.value
    },
    logs: recentLogs.value
  }
  
  const blob = new Blob([JSON.stringify(state, null, 2)], { 
    type: 'application/json' 
  })
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url
  a.download = `dservgui-state-${Date.now()}.json`
  a.click()
  URL.revokeObjectURL(url)
  
  addLog('info', 'State exported')
}

const updateDebugLevel = () => {
  localStorage.setItem('dservgui-debug-level', debugLevel.value)
  addLog('info', `Debug level set to: ${debugLevel.value}`)
}

const toggleApiLogging = () => {
  localStorage.setItem('dservgui-log-api', showApiCalls.value.toString())
  addLog('info', `API logging ${showApiCalls.value ? 'enabled' : 'disabled'}`)
}

const toggleStateLogging = () => {
  localStorage.setItem('dservgui-log-state', showStateChanges.value.toString())
  addLog('info', `State logging ${showStateChanges.value ? 'enabled' : 'disabled'}`)
}
</script>

<style scoped>
.dev-tools {
  position: fixed;
  bottom: 10px;
  right: 10px;
  background: rgba(0, 0, 0, 0.9);
  color: #fff;
  border-radius: 4px;
  font-family: 'Courier New', monospace;
  font-size: 12px;
  z-index: 10000;
  max-width: 400px;
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.3);
}

.dev-tools-toggle {
  padding: 8px 12px;
  cursor: pointer;
  display: flex;
  align-items: center;
  gap: 8px;
  background: rgba(255, 255, 255, 0.1);
  border-radius: 4px 4px 0 0;
}

.dev-tools-toggle:hover {
  background: rgba(255, 255, 255, 0.2);
}

.toggle-icon {
  font-size: 10px;
  width: 12px;
}

.status-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  font-size: 8px;
}

.status-dot.online {
  color: #4caf50;
}

.status-dot.offline {
  color: #f44336;
}

.dev-tools-panel {
  max-height: 400px;
  overflow-y: auto;
  border-radius: 0 0 4px 4px;
}

.dev-section {
  padding: 8px 12px;
  border-bottom: 1px solid rgba(255, 255, 255, 0.1);
}

.dev-section:last-child {
  border-bottom: none;
}

.dev-section h4 {
  margin: 0 0 6px 0;
  font-size: 11px;
  color: #ffd700;
  text-transform: uppercase;
  font-weight: bold;
}

.status-grid, .state-grid, .perf-grid {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 4px;
  font-size: 10px;
}

.status-item, .state-item, .perf-item {
  display: flex;
  justify-content: space-between;
}

.label {
  color: #ccc;
}

.value {
  font-weight: bold;
}

.value.connected {
  color: #4caf50;
}

.value.disconnected {
  color: #f44336;
}

.value.unknown {
  color: #ff9800;
}

.action-buttons {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 4px;
}

.dev-btn {
  padding: 4px 6px;
  background: rgba(255, 255, 255, 0.1);
  border: 1px solid rgba(255, 255, 255, 0.2);
  color: #fff;
  border-radius: 2px;
  font-size: 10px;
  cursor: pointer;
}

.dev-btn:hover {
  background: rgba(255, 255, 255, 0.2);
}

.debug-controls {
  display: flex;
  flex-direction: column;
  gap: 4px;
  font-size: 10px;
}

.debug-controls label {
  display: flex;
  align-items: center;
  gap: 6px;
}

.debug-controls select {
  background: rgba(255, 255, 255, 0.1);
  border: 1px solid rgba(255, 255, 255, 0.2);
  color: #fff;
  padding: 2px 4px;
  border-radius: 2px;
  font-size: 10px;
}

.debug-controls input[type="checkbox"] {
  margin: 0;
}

.log-viewer {
  max-height: 120px;
  overflow-y: auto;
  background: rgba(0, 0, 0, 0.3);
  border-radius: 2px;
  padding: 4px;
}

.log-entry {
  display: flex;
  gap: 8px;
  padding: 2px 0;
  font-size: 9px;
  border-bottom: 1px solid rgba(255, 255, 255, 0.05);
}

.log-entry:last-child {
  border-bottom: none;
}

.log-time {
  color: #888;
  width: 50px;
  flex-shrink: 0;
}

.log-level {
  width: 40px;
  flex-shrink: 0;
  font-weight: bold;
}

.log-entry.error .log-level {
  color: #f44336;
}

.log-entry.warn .log-level {
  color: #ff9800;
}

.log-entry.info .log-level {
  color: #2196f3;
}

.log-entry.debug .log-level {
  color: #4caf50;
}

.log-message {
  flex: 1;
  word-break: break-word;
}

.no-logs {
  color: #666;
  font-style: italic;
  text-align: center;
  padding: 8px;
}

/* Scrollbar styling */
::-webkit-scrollbar {
  width: 6px;
}

::-webkit-scrollbar-track {
  background: rgba(255, 255, 255, 0.1);
}

::-webkit-scrollbar-thumb {
  background: rgba(255, 255, 255, 0.3);
  border-radius: 3px;
}

::-webkit-scrollbar-thumb:hover {
  background: rgba(255, 255, 255, 0.5);
}
</style>
