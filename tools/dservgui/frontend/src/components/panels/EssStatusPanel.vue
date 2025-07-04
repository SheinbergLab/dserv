<template>
  <div class="ess-status-panel">
    <div class="panel-header">
      <h3>ESS Status</h3>
      <div class="status-indicator" :class="essConfig.complete ? 'complete' : 'incomplete'">
        {{ essConfig.complete ? '✅' : '⚠️' }}
      </div>
    </div>
    
    <div class="status-grid">
      <div class="status-item">
        <label>Current System:</label>
        <span class="value">{{ essConfig.system || 'Not set' }}</span>
      </div>
      
      <div class="status-item">
        <label>Current Protocol:</label>
        <span class="value">{{ essConfig.protocol || 'Not set' }}</span>
      </div>
      
      <div class="status-item">
        <label>Current Variant:</label>
        <span class="value">{{ essConfig.variant || 'Not set' }}</span>
      </div>
      
      <div class="status-item">
        <label>Available Systems:</label>
        <span class="value">{{ essConfig.systems.length }} systems</span>
      </div>
      
      <div class="status-item">
        <label>Available Protocols:</label>
        <span class="value">{{ essConfig.protocols.length }} protocols</span>
      </div>
      
      <div class="status-item">
        <label>Available Variants:</label>
        <span class="value">{{ essConfig.variants.length }} variants</span>
      </div>
    </div>
    
    <div class="variables-section">
      <h4>ESS Variables</h4>
      <div class="variables-list">
        <div v-for="(value, name) in essVariables" :key="name" class="variable-item">
          <span class="var-name">{{ name.replace('ess/', '') }}:</span>
          <span class="var-value">{{ formatValue(value) }}</span>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { computed, inject, reactive } from 'vue'

// 🎯 Simply inject the centralized ESS state with fallback
const essState = inject('essState', () => reactive({
  currentSystem: '',
  currentProtocol: '',
  currentVariant: '',
  systems: [],
  protocols: [],
  variants: [],
  configComplete: false,
  variables: {}
}), true)

// 🎯 All the reactive goodness happens automatically via Vue's reactivity
const essConfig = computed(() => ({
  system: essState.currentSystem,
  protocol: essState.currentProtocol,
  variant: essState.currentVariant,
  systems: essState.systems,
  protocols: essState.protocols,
  variants: essState.variants,
  complete: essState.configComplete
}))

const essVariables = computed(() => essState.variables)

const formatValue = (value) => {
  if (Array.isArray(value)) {
    return `[${value.length} items]`
  }
  if (typeof value === 'string' && value.length > 30) {
    return value.substring(0, 30) + '...'
  }
  return String(value)
}

console.log('🚀 EssStatusPanel mounted - automatically reactive to ESS state!')
</script>

<style scoped>
.ess-status-panel {
  padding: 16px;
  background: white;
  border-radius: 8px;
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.1);
}

.panel-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 16px;
}

.panel-header h3 {
  margin: 0;
  color: #333;
}

.status-indicator {
  font-size: 18px;
}

.status-indicator.complete {
  color: #4caf50;
}

.status-indicator.incomplete {
  color: #ff9800;
}

.status-grid {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 12px;
  margin-bottom: 24px;
}

.status-item {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.status-item label {
  font-weight: 500;
  color: #666;
  font-size: 12px;
}

.status-item .value {
  font-weight: 500;
  color: #333;
}

.variables-section h4 {
  margin: 0 0 12px 0;
  color: #333;
}

.variables-list {
  max-height: 200px;
  overflow-y: auto;
  border: 1px solid #ddd;
  border-radius: 4px;
  padding: 8px;
  background: #f9f9f9;
}

.variable-item {
  display: flex;
  justify-content: space-between;
  padding: 4px 0;
  border-bottom: 1px solid #eee;
  font-size: 12px;
}

.variable-item:last-child {
  border-bottom: none;
}

.var-name {
  font-weight: 500;
  color: #333;
}

.var-value {
  color: #666;
  text-align: right;
}
</style>
