<template>
  <div class="status-sidebar">
    <!-- Server Status Panel -->
    <div class="info-panel">
      <div class="info-header">
        <span>Server Status</span>
        <button 
          class="refresh-btn" 
          @click="refreshStatus"
          title="Refresh status"
        >
          🔄
        </button>
      </div>
      <div class="info-content">
        <div class="info-row">
          <span>Server:</span>
          <span>{{ serverAddress }}</span>
        </div>
        <div class="info-row">
          <span>Callback:</span>
          <span>{{ serverStatus.callbackPort || '---' }}</span>
        </div>
        <div class="info-row">
          <span>Reg ID:</span>
          <span>{{ serverStatus.registrationID || '---' }}</span>
        </div>
        <div class="info-row">
          <span>Status:</span>
          <span :class="connectionStatusClass">{{ connectionText }}</span>
        </div>
      </div>
    </div>

    <!-- ESS Variables Panel -->
    <div class="info-panel">
      <div class="info-header">
        <span>ESS Variables</span>
        <button 
          class="refresh-btn" 
          @click="refreshVariables"
          title="Refresh variables"
        >
          �
        </button>
      </div>
      <div class="info-content">
        <div class="variables-list" v-if="essVariables.length > 0">
          <div v-for="variable in essVariables" :key="variable.name" class="variable-item">
            <span class="var-name">{{ variable.displayName }}:</span>
            <span class="var-value" :class="{ 'var-invalid': !variable.valid }">
              {{ formatVariableValue(variable) }}
            </span>
          </div>
        </div>
        <div v-else class="no-variables">
          No variables available
        </div>
      </div>
    </div>

    <!-- System Health Panel -->
    <div class="info-panel">
      <div class="info-header">
        <span>System Health</span>
        <div class="health-indicator" :class="overallHealthClass">
          {{ overallHealthIcon }}
        </div>
      </div>
      <div class="info-content">
        <div class="health-item">
          <span class="health-service">Dserv:</span>
          <span class="health-status" :class="dservHealthClass">
            {{ dservHealthText }}
          </span>
        </div>
        <div class="health-item">
          <span class="health-service">ESS:</span>
          <span class="health-status" :class="essHealthClass">
            {{ essHealthText }}
          </span>
        </div>
        <div class="health-item">
          <span class="health-service">Backend:</span>
          <span class="health-status" :class="backendHealthClass">
            {{ backendHealthText }}
          </span>
        </div>
        <div class="health-item">
          <span class="health-service">Memory:</span>
          <span class="health-status" :class="memoryHealthClass">
            {{ memoryUsage }}
          </span>
        </div>
      </div>
    </div>

    <!-- Quick Actions Panel -->
    <div class="info-panel">
      <div class="info-header">
        <span>Quick Actions</span>
      </div>
      <div class="info-content">
        <button 
          class="action-btn primary"
          @click="$emit('refresh-all')"
          :disabled="!isConnected"
        >
          🔄 Refresh All
        </button>
        <button 
          class="action-btn secondary"
          @click="$emit('clear-logs')"
        >
          🗑️ Clear Logs
        </button>
        <button 
          class="action-btn secondary"
          @click="$emit('export-data')"
          :disabled="!isConnected"
        >
          💾 Export Data
        </button>
        <button 
          class="action-btn secondary"
          @click="$emit('show-debug')"
        >
          🐛 Debug Info
        </button>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, inject, watch } from 'vue'

// Inject ESS state service
const essState = inject('essState', null)

const props = defineProps({
  serverStatus: {
    type: Object,
    default: () => ({})
  },
  statistics: {
    type: Object,
    default: () => ({})
  },
  isConnected: {
    type: Boolean,
    default: false
  }
})

defineEmits(['refresh-all', 'clear-logs', 'export-data', 'show-debug'])

const startTime = ref(Date.now())
const serverAddress = 'localhost:4620'
const essVariables = ref([])

// Computed properties
const connectionText = computed(() => {
  return props.isConnected ? 'Connected' : 'Disconnected'
})

const connectionStatusClass = computed(() => {
  return props.isConnected ? 'connected' : 'disconnected'
})

const overallHealthClass = computed(() => {
  if (!props.isConnected) return 'health-error'
  // Add logic to determine overall health based on various factors
  return 'health-good'
})

const overallHealthIcon = computed(() => {
  switch (overallHealthClass.value) {
    case 'health-good': return '✅'
    case 'health-warning': return '⚠️'
    case 'health-error': return '❌'
    default: return '❓'
  }
})

const dservHealthClass = computed(() => {
  return props.isConnected ? 'health-good' : 'health-error'
})

const dservHealthText = computed(() => {
  return props.isConnected ? 'Online' : 'Offline'
})

const essHealthClass = computed(() => {
  // This would check ESS connection status
  return 'health-good' // Placeholder
})

const essHealthText = computed(() => {
  return 'Online' // Placeholder
})

const backendHealthClass = computed(() => {
  return 'health-good' // Placeholder
})

const backendHealthText = computed(() => {
  return 'Online' // Placeholder
})

const memoryHealthClass = computed(() => {
  const usage = props.statistics.memoryUsage || 0
  if (usage > 80) return 'health-error'
  if (usage > 60) return 'health-warning'
  return 'health-good'
})

const memoryUsage = computed(() => {
  const usage = props.statistics.memoryUsage || 0
  return `${usage}%`
})

// Methods
const refreshStatus = () => {
  console.log('Refreshing server status...')
  // Emit event to parent or call API directly
}

const refreshVariables = () => {
  console.log('Refreshing ESS variables...')
  loadEssVariables()
}

const loadEssVariables = () => {
  if (!essState) return
  
  try {
    // Convert centralized ESS state variables to display format
    const variables = Object.entries(essState.variables || {}).map(([name, value]) => ({
      name,
      displayName: name.replace('ess/', ''),
      value: value,
      valid: true, // Assume valid if in centralized state
      timestamp: new Date().toISOString()
    }))
    
    essVariables.value = variables
    console.log('✅ ESS variables updated in StatusSidebar:', variables.length)
  } catch (error) {
    console.error('❌ Failed to load ESS variables in StatusSidebar:', error)
  }
}

const formatVariableValue = (variable) => {
  if (!variable.valid) {
    return 'Invalid'
  }
  
  if (Array.isArray(variable.value)) {
    return `[${variable.value.length} items]`
  }
  
  if (typeof variable.value === 'string' && variable.value.length > 20) {
    return variable.value.substring(0, 20) + '...'
  }
  
  return String(variable.value)
}

const formatUptime = () => {
  const uptime = Date.now() - startTime.value
  const hours = Math.floor(uptime / (1000 * 60 * 60))
  const minutes = Math.floor((uptime % (1000 * 60 * 60)) / (1000 * 60))
  const seconds = Math.floor((uptime % (1000 * 60)) / 1000)
  return `${hours.toString().padStart(2, '0')}:${minutes.toString().padStart(2, '0')}:${seconds.toString().padStart(2, '0')}`
}

onMounted(() => {
  // Initialize component
  console.log('StatusSidebar mounted')
  loadEssVariables()
})

// Watch for changes in centralized ESS state to update variables display
watch(() => essState?.variables, () => {
  if (essState?.variables) {
    loadEssVariables()
  }
}, { deep: true, immediate: true })
</script>

<style scoped>
.status-sidebar {
  height: 100%;
  overflow-y: auto;
  padding: 8px;
}

.info-panel {
  margin-bottom: 16px;
  border: 1px solid #ddd;
  border-radius: 3px;
  background: white;
}

.info-header {
  padding: 6px 8px;
  background: #e8e8e8;
  font-weight: 500;
  font-size: 11px;
  text-transform: uppercase;
  color: #666;
  border-bottom: 1px solid #ddd;
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.refresh-btn {
  width: 16px;
  height: 16px;
  border: none;
  background: transparent;
  cursor: pointer;
  font-size: 10px;
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: 2px;
}

.refresh-btn:hover {
  background: rgba(0,0,0,0.1);
}

.info-content {
  padding: 8px;
}

.info-row {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin: 4px 0;
  font-size: 11px;
}

.info-row span:first-child {
  color: #666;
  font-weight: 500;
}

.connected {
  color: #4CAF50;
  font-weight: 500;
}

.disconnected {
  color: #f44336;
  font-weight: 500;
}

.health-indicator {
  font-size: 14px;
}

.health-item {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin: 6px 0;
  font-size: 11px;
}

.health-service {
  color: #666;
  font-weight: 500;
}

.health-status {
  font-weight: 500;
  font-size: 10px;
  padding: 1px 4px;
  border-radius: 2px;
}

.health-good {
  color: #4CAF50;
  background: rgba(76, 175, 80, 0.1);
}

.health-warning {
  color: #FF9800;
  background: rgba(255, 152, 0, 0.1);
}

.health-error {
  color: #f44336;
  background: rgba(244, 67, 54, 0.1);
}

.action-btn {
  width: 100%;
  padding: 6px 8px;
  margin: 3px 0;
  border: 1px solid #999;
  border-radius: 2px;
  font-size: 10px;
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 4px;
}

.action-btn.primary {
  background: linear-gradient(to bottom, #4CAF50, #45a049);
  color: white;
}

.action-btn.secondary {
  background: linear-gradient(to bottom, #fff, #e0e0e0);
  color: #333;
}

.action-btn:hover:not(:disabled) {
  transform: translateY(-1px);
}

.action-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.action-btn.primary:hover:not(:disabled) {
  background: linear-gradient(to bottom, #45a049, #3e8e41);
}

.action-btn.secondary:hover:not(:disabled) {
  background: linear-gradient(to bottom, #fff, #d0d0d0);
}

/* Variables List Styles */
.variables-list {
  max-height: 200px;
  overflow-y: auto;
  background: white;
  border: 1px solid #ddd;
  border-radius: 2px;
  padding: 4px;
}

.variable-item {
  display: flex;
  justify-content: space-between;
  padding: 2px 4px;
  font-size: 10px;
  border-bottom: 1px solid #eee;
  align-items: center;
}

.variable-item:last-child {
  border-bottom: none;
}

.var-name {
  font-weight: 500;
  color: #333;
  flex-shrink: 0;
  margin-right: 8px;
  min-width: 60px;
}

.var-value {
  color: #666;
  text-align: right;
  word-break: break-all;
  font-family: monospace;
  font-size: 9px;
}

.var-value.var-invalid {
  color: #f44336;
  font-style: italic;
}

.no-variables {
  text-align: center;
  color: #999;
  font-style: italic;
  padding: 20px;
  font-size: 11px;
}
</style>