<template>
  <div class="simple-sidebar">
    <div class="header">Simple ESS Control</div>
    <div class="content">
      <p>ConnonMounted(() => {
  console.log('🚀 SimpleSidebar mounted')
  window.addEventListener('server-update', handleServerUpdate)
  
  // Auto-connect on mount
  setTimeout(() => {
    connectToBackend()
  }, 1000)
  
  window.addEventListener('beforeunload', () => {
    window.removeEventListener('server-update', handleServerUpdate)
  })
}){{ connectionStatus }}</p>
      <p>ESS State Available: {{ essStateAvailable }}</p>
      
      <div class="ess-state-info">
        <h4>ESS State:</h4>
        <pre>{{ JSON.stringify(essStateContent, null, 2) }}</pre>
      </div>
      
      <button @click="testFunction">Test Button</button>
      <button @click="connectToBackend">Connect</button>
      <button @click="loadEssVariables">Load ESS Variables</button>
      <button @click="testDirectAPI">Test API</button>
      
      <div class="event-log">
        <h4>Event Log:</h4>
        <div class="log-entries">
          <div v-for="entry in eventLog.slice(-5)" :key="entry.time" class="log-entry">
            <span class="log-time">{{ entry.time }}</span>: {{ entry.message }}
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, inject, watch, onMounted } from 'vue'

// Simple injection test
const connectionService = inject('connectionService', null)
const essState = inject('essState', null)

const eventLog = ref([])

const connectionStatus = computed(() => {
  if (!connectionService) return 'No Service'
  return connectionService.isConnected?.value ? 'Connected' : 'Disconnected'
})

const essStateAvailable = computed(() => {
  return essState ? 'Yes' : 'No'
})

const essStateContent = computed(() => {
  if (!essState) return 'No ESS State'
  return {
    system: essState.currentSystem,
    protocol: essState.currentProtocol,
    variant: essState.currentVariant,
    systems: essState.systems?.length || 0,
    protocols: essState.protocols?.length || 0,
    variants: essState.variants?.length || 0,
    variables: Object.keys(essState.variables || {}).length
  }
})

const testFunction = () => {
  console.log('🔍 Test - connectionService:', connectionService)
  console.log('🔍 Test - essState:', essState)
  console.log('🔍 Test - essState content:', essStateContent.value)
  
  eventLog.value.push({
    time: new Date().toLocaleTimeString(),
    message: 'Test button clicked',
    essState: JSON.stringify(essStateContent.value, null, 2)
  })
}

const connectToBackend = async () => {
  if (connectionService?.connectToServer) {
    console.log('🔄 SimpleSidebar - Connecting to backend...')
    await connectionService.connectToServer()
    eventLog.value.push({
      time: new Date().toLocaleTimeString(),
      message: 'Connection attempted'
    })
  }
}

const loadEssVariables = async () => {
  try {
    console.log('🔄 SimpleSidebar - Loading ESS variables...')
    const response = await fetch('/api/variables/prefix?prefix=ess/')
    if (response.ok) {
      const data = await response.json()
      console.log('🔄 SimpleSidebar - Got ESS variables:', data)
      eventLog.value.push({
        time: new Date().toLocaleTimeString(),
        message: `Loaded ${Object.keys(data.variables || {}).length} ESS variables`
      })
      
      // Show the actual data we got
      eventLog.value.push({
        time: new Date().toLocaleTimeString(),
        message: `System: ${data.variables['ess/system']?.value}, Protocol: ${data.variables['ess/protocol']?.value}, Variant: ${data.variables['ess/variant']?.value}`
      })
    }
  } catch (error) {
    console.error('Failed to load ESS variables:', error)
    eventLog.value.push({
      time: new Date().toLocaleTimeString(),
      message: `Failed to load ESS variables: ${error.message}`
    })
  }
}

const testDirectAPI = async () => {
  try {
    console.log('🔄 SimpleSidebar - Testing direct API...')
    const response = await fetch('/api/status')
    if (response.ok) {
      const data = await response.json()
      console.log('🔄 SimpleSidebar - API Status:', data)
      eventLog.value.push({
        time: new Date().toLocaleTimeString(),
        message: `API working - Connected: ${data.dserv_client?.connected}, Updates: ${data.dserv_client?.total_updates}`
      })
    }
  } catch (error) {
    console.error('Failed to test API:', error)
    eventLog.value.push({
      time: new Date().toLocaleTimeString(),
      message: `API test failed: ${error.message}`
    })
  }
}

// Watch for ESS state changes
watch(() => essState?.currentSystem, (newVal, oldVal) => {
  console.log('� SimpleSidebar - ESS System changed:', oldVal, '->', newVal)
  eventLog.value.push({
    time: new Date().toLocaleTimeString(),
    message: `System changed: ${oldVal} -> ${newVal}`
  })
}, { immediate: true })

watch(() => essState?.variables, (newVars) => {
  console.log('🔄 SimpleSidebar - ESS Variables changed:', Object.keys(newVars || {}))
  eventLog.value.push({
    time: new Date().toLocaleTimeString(),
    message: `Variables updated: ${Object.keys(newVars || {}).length} items`
  })
}, { deep: true, immediate: true })

// Listen for raw server updates
const handleServerUpdate = (event) => {
  const update = event.detail
  console.log('🔄 SimpleSidebar - Raw server update:', update)
  eventLog.value.push({
    time: new Date().toLocaleTimeString(),
    message: `Server update: ${update.name} = ${update.value}`
  })
}

onMounted(() => {
  console.log('�🚀 SimpleSidebar mounted')
  window.addEventListener('server-update', handleServerUpdate)
  
  window.addEventListener('beforeunload', () => {
    window.removeEventListener('server-update', handleServerUpdate)
  })
})
</script>

<style scoped>
.simple-sidebar {
  padding: 16px;
  background: #f0f0f0;
  border: 1px solid #ccc;
  max-height: 100vh;
  overflow-y: auto;
}

.header {
  font-weight: bold;
  margin-bottom: 12px;
}

.content p {
  margin: 8px 0;
}

.ess-state-info {
  margin: 12px 0;
  background: white;
  padding: 8px;
  border-radius: 4px;
  border: 1px solid #ddd;
}

.ess-state-info h4 {
  margin: 0 0 8px 0;
  font-size: 12px;
}

.ess-state-info pre {
  font-size: 10px;
  margin: 0;
  white-space: pre-wrap;
}

.event-log {
  margin: 12px 0;
  background: white;
  padding: 8px;
  border-radius: 4px;
  border: 1px solid #ddd;
}

.event-log h4 {
  margin: 0 0 8px 0;
  font-size: 12px;
}

.log-entries {
  max-height: 150px;
  overflow-y: auto;
}

.log-entry {
  font-size: 10px;
  margin: 2px 0;
  padding: 2px;
  border-bottom: 1px solid #eee;
}

.log-time {
  font-weight: bold;
  color: #666;
}

button {
  padding: 6px 12px;
  background: #007cba;
  color: white;
  border: none;
  border-radius: 4px;
  cursor: pointer;
  margin: 2px;
  font-size: 11px;
}
</style>
