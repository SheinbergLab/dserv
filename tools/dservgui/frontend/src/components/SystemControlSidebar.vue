<template>
  <div class="system-control-sidebar">

    <!-- Experiment Control Section -->
    <div class="sidebar-section">
      <div class="sidebar-header">Experiment Control</div>
      <div class="section-content">
        <!-- Subject Section -->
        <div class="form-group horizontal">
          <label class="field-label">Subject:</label>
          <div class="input-group">
            <select v-model="subject" class="text-input">
              <option value="human">human</option>
              <option value="momo">momo</option>
              <option value="sally">sally</option>
              <option value="riker">riker</option>
            </select>
          </div>
        </div>
      </div>

      <div class="section-content">
        <div class="control-buttons">
          <button @click="handleGo" class="control-btn" :class="{ active: isRunning }" :disabled="isRunning">
            <span class="icon">▶</span> Go
          </button>
          <button @click="handleStop" class="control-btn" :disabled="!isRunning">
            <span class="icon">■</span> Stop
          </button>
          <button @click="handleReset" class="control-btn">
            <span class="icon">↺</span> Reset
          </button>
        </div>

        <div class="status-display">
          <div class="status-item">
            <span class="label">Status:</span>
            <span class="value" :class="statusClass">{{ status }}</span>
          </div>
          <div class="status-item">
            <span class="label">Obs:</span>
            <span class="value">{{ obsCount }}</span>
          </div>
        </div>
      </div>
    </div>

    <!-- System Configuration Section -->
    <div class="sidebar-section">
      <div class="sidebar-header">ESS Configuration</div>
      <div class="section-content">
        <div class="form-group horizontal">
          <label class="field-label">System:</label>
          <div class="input-group">
            <select v-model="selectedSystem" @change="onSystemChange" class="text-input"
              :disabled="!isConnected">
              <option value="">{{ getDropdownPlaceholder('systems') }}</option>
              <option v-for="sys in essConfig.systems" :key="sys" :value="sys">{{ sys }}</option>
            </select>
            <button @click="refreshConfig" class="icon-btn" title="Refresh">
              <span class="icon">🔄</span>
            </button>
          </div>
        </div>

        <div class="form-group horizontal">
          <label class="field-label">Protocol:</label>
          <div class="input-group">
            <select v-model="selectedProtocol" @change="onProtocolChange" class="text-input"
              :disabled="!isConnected">
              <option value="">{{ getDropdownPlaceholder('protocols') }}</option>
              <option v-for="proto in essConfig.protocols" :key="proto" :value="proto">{{ proto }}</option>
            </select>
            <button @click="refreshConfig" class="icon-btn" title="Refresh">
              <span class="icon">🔄</span>
            </button>
          </div>
        </div>

        <div class="form-group horizontal">
          <label class="field-label">Variant:</label>
          <div class="input-group">
            <select v-model="selectedVariant" @change="onVariantChange" class="text-input"
              :class="{ 'variant-active': selectedVariant }" :disabled="!isConnected">
              <option value="">{{ getDropdownPlaceholder('variants') }}</option>
              <option v-for="variant in essConfig.variants" :key="variant" :value="variant">{{ variant }}</option>
            </select>
            <button @click="refreshConfig" class="icon-btn" title="Refresh">
              <span class="icon">🔄</span>
            </button>
          </div>
        </div>

        <div class="button-group">
          <button @click="saveSettings" class="primary-btn">Save Settings</button>
          <button @click="resetSettings" class="secondary-btn">Reset Settings</button>
        </div>

        <div class="config-status">
          <span class="status-indicator" :class="essConfig.complete ? 'complete' : 'incomplete'">●</span>
          <span>{{ essConfig.complete ? 'Configuration Complete' : 'Configuration Incomplete' }}</span>
        </div>
      </div>
    </div>

    <!-- Variant Options Section -->
    <div class="sidebar-section" v-if="variantOptions.length > 0">
      <div class="sidebar-header">Variant Options</div>
      <div class="section-content">
        <div v-for="option in variantOptions" :key="option.name" class="option-item">
          <label>{{ option.name }}:</label>
          <input v-if="option.type === 'number'" 
                 type="number" 
                 v-model.number="option.value" 
                 class="text-input small"
                 @change="onVariantOptionChange(option)" />
          <select v-else-if="option.type === 'select'" 
                  v-model="option.value" 
                  class="text-input small"
                  @change="onVariantOptionChange(option)">
            <option v-for="opt in option.options" :key="opt.value" :value="opt.value">
              {{ opt.label }}
            </option>
          </select>
          <input v-else 
                 type="text" 
                 v-model="option.value" 
                 class="text-input small"
                 @change="onVariantOptionChange(option)" />
        </div>
      </div>
    </div>

    <!-- System Settings Section -->
    <div class="sidebar-section" v-if="systemSettings.length > 0">
      <div class="sidebar-header">System Settings</div>
      <div class="section-content settings-list">
        <div v-for="setting in systemSettings" :key="setting.name" class="setting-item">
          <label :class="{ 'time-param': setting.isTimeParameter }" :title="setting.isTimeParameter ? 'Time parameter' : 'Variable parameter'">
            {{ setting.name }}:
          </label>
          <input :type="setting.type"
                 v-model="setting.value" 
                 class="text-input small"
                 @change="onSystemSettingChange(setting)" />
        </div>
      </div>
    </div>

    <!-- Connection Status -->
    <div class="sidebar-section connection-section">
      <div class="connection-status">
        <span class="status-indicator" :class="isConnected ? 'connected' : 'disconnected'">●</span>
        <span>{{ isConnected ? 'Backend Connected' : 'Backend Disconnected' }}</span>
      </div>
      <div v-if="lastUpdate" class="last-update">
        Last update: {{ lastUpdate }}
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, reactive, computed, onMounted, inject, watch } from 'vue'

// Inject services with simple fallbacks
const connectionService = inject('connectionService', null)
const essState = inject('essState', null)

const isConnected = computed(() => {
  try {
    return connectionService?.isConnected?.value || false
  } catch (error) {
    console.warn('Error accessing connectionService.isConnected:', error)
    return false
  }
})

// Debug logging
console.log('🔍 SystemControlSidebar - connectionService:', connectionService)
console.log('🔍 SystemControlSidebar - essState:', essState)

// Local state
const subject = ref('human')
const isRunning = ref(false)
const status = ref('Stopped')
const obsCount = ref(0)
const lastUpdate = ref('')

// ESS Configuration - reactive refs for form inputs, computed for data
const selectedSystem = ref('')
const selectedProtocol = ref('')
const selectedVariant = ref('')

// Watch centralized state and update local selections - with null guards
watch(() => essState?.currentSystem, (newSystem) => {
  if (newSystem !== undefined) {
    selectedSystem.value = newSystem || ''
  }
}, { immediate: true })
watch(() => essState?.currentProtocol, (newProtocol) => {
  if (newProtocol !== undefined) {
    selectedProtocol.value = newProtocol || ''
  }
}, { immediate: true })
watch(() => essState?.currentVariant, (newVariant) => {
  if (newVariant !== undefined) {
    selectedVariant.value = newVariant || ''
  }
}, { immediate: true })

// Computed for accessing centralized state data - with comprehensive null guards
const essConfig = computed(() => {
  if (!essState) {
    return {
      system: '',
      protocol: '',
      variant: '',
      systems: [],
      protocols: [],
      variants: [],
      complete: false
    }
  }
  
  try {
    return {
      system: essState.currentSystem || '',
      protocol: essState.currentProtocol || '',
      variant: essState.currentVariant || '',
      systems: essState.systems || [],
      protocols: essState.protocols || [],
      variants: essState.variants || [],
      complete: essState.configComplete || false
    }
  } catch (error) {
    console.warn('Error accessing essState in computed:', error)
    return {
      system: '',
      protocol: '',
      variant: '',
      systems: [],
      protocols: [],
      variants: [],
      complete: false
    }
  }
})

// Dynamic variant options and system settings - simplified for now
const variantOptions = ref([])
const systemSettings = ref([])

// Computed
const statusClass = computed(() => {
  if (status.value === 'Running') return 'status-running'
  if (status.value === 'Stopped') return 'status-stopped'
  if (status.value === 'Error') return 'status-error'
  return ''
})

// Utility functions
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

// Event handlers
const handleGo = async () => {
  try {
    const response = await fetch('/api/ess/eval', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ script: 'ess::start_experiment' })
    })
    
    if (response.ok) {
      isRunning.value = true
      status.value = 'Running'
      console.log('✅ Experiment started')
    } else {
      throw new Error('Failed to start experiment')
    }
  } catch (error) {
    console.error('❌ Error starting experiment:', error)
    status.value = 'Error'
  }
}

const handleStop = async () => {
  try {
    const response = await fetch('/api/ess/eval', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ script: 'ess::stop_experiment' })
    })
    
    if (response.ok) {
      isRunning.value = false
      status.value = 'Stopped'
      console.log('✅ Experiment stopped')
    } else {
      throw new Error('Failed to stop experiment')
    }
  } catch (error) {
    console.error('❌ Error stopping experiment:', error)
  }
}

const handleReset = async () => {
  try {
    const response = await fetch('/api/ess/eval', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ script: 'ess::reset_experiment' })
    })
    
    if (response.ok) {
      isRunning.value = false
      status.value = 'Stopped'
      obsCount.value = 0
      console.log('✅ Experiment reset')
    } else {
      throw new Error('Failed to reset experiment')
    }
  } catch (error) {
    console.error('❌ Error resetting experiment:', error)
  }
}

const getDropdownPlaceholder = (type) => {
  if (!isConnected.value) {
    return 'Backend unavailable...'
  }
  const items = type === 'systems' ? essConfig.value.systems : 
                type === 'protocols' ? essConfig.value.protocols : 
                essConfig.value.variants
  return items.length ? `Select a ${type.slice(0, -1)}...` : `Loading ${type}...`
}

const onSystemChange = async () => {
  if (!selectedSystem.value || !isConnected.value) return
  
  console.log('🔄 System changed to:', selectedSystem.value)
  
  try {
    // Use proper ESS command with just the system parameter
    const response = await fetch('/api/ess/eval', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        script: `ess::load_system ${selectedSystem.value}`
      })
    })
    
    if (response.ok) {
      console.log('✅ System loaded via ess::load_system')
      
      // Clear dependent selections - the load_system command should trigger updates
      selectedProtocol.value = ''
      selectedVariant.value = ''
    } else {
      throw new Error(`HTTP ${response.status}`)
    }
  } catch (error) {
    console.error('❌ Error loading system:', error)
  }
}

const onProtocolChange = async () => {
  if (!selectedProtocol.value || !isConnected.value) return
  
  console.log('🔄 Protocol changed to:', selectedProtocol.value)
  
  try {
    // Use proper ESS command with system and protocol parameters
    const systemValue = selectedSystem.value || essState?.currentSystem || ''
    const response = await fetch('/api/ess/eval', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        script: `ess::load_system ${systemValue} ${selectedProtocol.value}`
      })
    })
    
    if (response.ok) {
      console.log('✅ Protocol loaded via ess::load_system with system and protocol')
      
      // Clear dependent selections - the load_system command should trigger updates
      selectedVariant.value = ''
    } else {
      throw new Error(`HTTP ${response.status}`)
    }
  } catch (error) {
    console.error('❌ Error loading protocol:', error)
  }
}

const onVariantChange = async () => {
  if (!selectedVariant.value || !isConnected.value) return
  
  console.log('🔄 Variant changed to:', selectedVariant.value)
  
  try {
    // Use proper ESS command with system, protocol, and variant parameters
    const systemValue = selectedSystem.value || essState?.currentSystem || ''
    const protocolValue = selectedProtocol.value || essState?.currentProtocol || ''
    const response = await fetch('/api/ess/eval', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        script: `ess::load_system ${systemValue} ${protocolValue} ${selectedVariant.value}`
      })
    })
    
    if (response.ok) {
      console.log('✅ Variant loaded via ess::load_system with full hierarchy')
    } else {
      throw new Error(`HTTP ${response.status}`)
    }
  } catch (error) {
    console.error('❌ Error loading variant:', error)
  }
}

const onVariantOptionChange = async (option) => {
  console.log('🔄 Variant option changed:', option.name, '=', option.value)
  // Implementation removed for now - will add back later
}

const onSystemSettingChange = async (setting) => {
  console.log('🔄 System setting changed:', setting.name, '=', setting.value)
  // Implementation removed for now - will add back later
}

const reloadVariant = async () => {
  try {
    console.log('🔄 Reloading variant...')
    
    const response = await fetch('/api/ess/eval', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ script: 'ess::reload_variant' })
    })
    
    if (response.ok) {
      console.log('✅ Variant reloaded')
    } else {
      throw new Error(`HTTP ${response.status}`)
    }
  } catch (error) {
    console.error('❌ Error reloading variant:', error)
  }
}

const refreshConfig = async () => {
  // No longer needed - the centralized useEssState handles all updates
  // Just update the last update time for UI feedback
  lastUpdate.value = new Date().toLocaleTimeString()
  console.log('✅ Using centralized ESS state - Systems:', essConfig.value.systems.length, 'Protocols:', essConfig.value.protocols.length, 'Variants:', essConfig.value.variants.length)
}

const saveSettings = async () => {
  console.log('💾 Saving settings...')
  // Implementation for saving settings
}

const resetSettings = async () => {
  console.log('🔄 Resetting settings...')
  // Implementation for resetting settings
}

// Watch for changes in centralized ESS state to update last update time
watch(() => essState?.variables, () => {
  if (essState?.variables) {
    lastUpdate.value = new Date().toLocaleTimeString()
  }
}, { deep: true, immediate: true })

onMounted(() => {
  console.log('🚀 SystemControlSidebar mounted - using centralized ESS state')
  console.log('🔍 Available essState:', essState)
  console.log('🔍 Available connectionService:', connectionService)
  
  // Initial load will happen via the watcher when essState.variables is available
  // No need for polling since we use reactive centralized state
})
</script>

<style scoped>
.system-control-sidebar {
  width: 100%;
  height: 100%;
  background: #f8f8f8;
  display: flex;
  flex-direction: column;
  overflow-y: auto;
}

.sidebar-section {
  border-bottom: 1px solid #ddd;
}

.sidebar-section:last-child {
  border-bottom: none;
}

.sidebar-header {
  padding: 6px 8px;
  background: #e8e8e8;
  font-weight: 500;
  font-size: 11px;
  text-transform: uppercase;
  color: #666;
}

.section-content {
  padding: 8px;
}

/* Form Elements */
.form-group {
  margin-bottom: 8px;
}

.form-group.horizontal {
  display: flex;
  align-items: center;
  gap: 8px;
}

.field-label {
  flex-shrink: 0;
  width: 60px;
  text-align: right;
  font-weight: 500;
  font-size: 12px;
  margin-bottom: 0;
}

.input-group {
  display: flex;
  gap: 4px;
  flex: 1;
}

.text-input {
  width: 100%;
  padding: 4px 6px;
  border: 2px inset #f0f0f0;
  font-size: 12px;
}

.text-input:disabled {
  background: #f5f5f5;
  color: #999;
  cursor: not-allowed;
}

.text-input.variant-active {
  background: #e3f2fd;
  border-color: #2196F3;
}

/* Control Buttons */
.control-buttons {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 4px;
  margin-bottom: 12px;
}

.control-btn {
  padding: 6px 8px;
  border: 1px solid #999;
  background: linear-gradient(to bottom, #fff, #e0e0e0);
  font-size: 12px;
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 4px;
  border-radius: 2px;
}

.control-btn:hover:not(:disabled) {
  background: linear-gradient(to bottom, #fff, #d0d0d0);
}

.control-btn:disabled {
  opacity: 0.6;
  cursor: not-allowed;
}

.control-btn.active {
  background: linear-gradient(to bottom, #d0d0d0, #b0b0b0);
  border-color: #777;
}

.control-btn .icon {
  font-size: 10px;
}

/* Icon Button */
.icon-btn {
  width: 24px;
  height: 24px;
  border: 1px solid #999;
  background: linear-gradient(to bottom, #fff, #e0e0e0);
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: 2px;
  flex-shrink: 0;
}

.icon-btn:hover {
  background: linear-gradient(to bottom, #fff, #d0d0d0);
}

.icon-btn .icon {
  font-size: 12px;
}

/* Status Display */
.status-display {
  background: white;
  border: 1px solid #ddd;
  border-radius: 2px;
  padding: 6px;
}

.status-item {
  display: flex;
  justify-content: space-between;
  font-size: 12px;
  margin-bottom: 4px;
}

.status-item:last-child {
  margin-bottom: 0;
}

.status-item .label {
  font-weight: 500;
  color: #666;
}

.status-item .value {
  font-weight: 500;
}

.status-stopped {
  color: #d32f2f;
}

.status-running {
  color: #388e3c;
}

.status-error {
  color: #f57c00;
}

/* Button Groups */
.button-group {
  display: flex;
  gap: 4px;
  margin-top: 8px;
}

.primary-btn, .secondary-btn {
  flex: 1;
  padding: 4px 8px;
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

.secondary-btn:hover {
  background: linear-gradient(to bottom, #fff, #d0d0d0);
}

/* Configuration Status */
.config-status {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 11px;
  margin-top: 8px;
  padding: 4px;
  background: white;
  border: 1px solid #ddd;
  border-radius: 2px;
}

.status-indicator {
  font-size: 10px;
}

.status-indicator.complete {
  color: #4caf50;
}

.status-indicator.incomplete {
  color: #ff9800;
}

.status-indicator.connected {
  color: #4caf50;
}

.status-indicator.disconnected {
  color: #f44336;
}

/* Options and Settings */
.option-item, .setting-item {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 6px;
  gap: 8px;
}

.option-item:last-child, .setting-item:last-child {
  margin-bottom: 0;
}

.text-input.small {
  width: 80px;
}

.settings-list {
  max-height: 180px;
  overflow-y: auto;
}

/* Time parameter styling (like in C++ code) */
.time-param {
  color: #3c321e !important; /* Brown color for time parameters */
}

.setting-item label {
  font-size: 11px;
  color: #333;
  flex: 1;
  margin-bottom: 0;
}

.setting-item label.time-param {
  color: #3c321e; /* Time parameters get special color */
}

.option-item label {
  font-size: 11px;
  color: #333;
  flex: 1;
  margin-bottom: 0;
}

/* Variables List */
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
  font-size: 11px;
  border-bottom: 1px solid #eee;
}

.variable-item:last-child {
  border-bottom: none;
}

.var-name {
  font-weight: 500;
  color: #333;
  flex-shrink: 0;
  margin-right: 8px;
}

.var-value {
  color: #666;
  text-align: right;
  word-break: break-all;
}

.var-value.var-invalid {
  color: #f44336;
  font-style: italic;
}

/* Connection Section */
.connection-section {
  margin-top: auto;
  background: #e0e0e0;
}

.connection-status {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 11px;
  margin-bottom: 4px;
}

.last-update {
  font-size: 10px;
  color: #666;
}

/* Scrollbar */
::-webkit-scrollbar {
  width: 12px;
}

::-webkit-scrollbar-track {
  background: #f0f0f0;
  border: 1px inset #f0f0f0;
}

::-webkit-scrollbar-thumb {
  background: linear-gradient(to bottom, #e0e0e0, #c0c0c0);
  border: 1px outset #d0d0d0;
}

::-webkit-scrollbar-thumb:hover {
  background: linear-gradient(to bottom, #d0d0d0, #b0b0b0);
}
</style>