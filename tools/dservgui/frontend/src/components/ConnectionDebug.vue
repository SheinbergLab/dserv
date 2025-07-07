<template>
  <div class="connection-debug">
    <h3>Connection Debug</h3>
    <div>
      <button @click="testDirectEventSource">Test Direct EventSource</button>
      <button @click="testFetch">Test Fetch</button>
      <button @click="forceConnect">Force Connect</button>
      <button @click="forceConnectedState">Force Connected State</button>
    </div>
    <div>
      <p>EventSource State: {{ eventSourceState }}</p>
      <p>Connection Status: {{ connectionStatus }}</p>
      <p>Is Connected: {{ isConnected }}</p>
      <p>Last Message: {{ lastMessage }}</p>
    </div>
    <div class="log">
      <h4>Debug Log:</h4>
      <pre>{{ debugLog }}</pre>
    </div>
  </div>
</template>

<script setup>
import { ref, inject } from 'vue'

const connectionService = inject('connectionService', null)
const isConnected = connectionService?.isConnected || ref(false)
const connectionStatus = connectionService?.connectionStatus || ref('unknown')

const eventSourceState = ref('not started')
const lastMessage = ref('none')
const debugLog = ref('')
const testEventSource = ref(null)

const log = (message) => {
  const timestamp = new Date().toLocaleTimeString()
  debugLog.value += `[${timestamp}] ${message}\n`
  console.log(`🔍 ConnectionDebug: ${message}`)
}

const testDirectEventSource = () => {
  log('Testing direct EventSource...')
  
  if (testEventSource.value) {
    testEventSource.value.close()
  }
  
  testEventSource.value = new EventSource('/api/updates')
  eventSourceState.value = 'connecting'
  
  testEventSource.value.onopen = () => {
    log('✅ Direct EventSource opened successfully!')
    eventSourceState.value = 'connected'
  }
  
  testEventSource.value.onmessage = (event) => {
    log(`📨 Received message: ${event.data}`)
    lastMessage.value = event.data
  }
  
  testEventSource.value.onerror = (error) => {
    log(`❌ Direct EventSource error: ${error}`)
    log(`❌ ReadyState: ${testEventSource.value?.readyState}`)
    eventSourceState.value = 'error'
  }
}

const testFetch = async () => {
  log('Testing fetch to /api/status...')
  
  try {
    const response = await fetch('/api/status')
    log(`✅ Fetch response: ${response.status} ${response.statusText}`)
    
    if (response.ok) {
      const data = await response.json()
      log(`✅ Status data received: ${JSON.stringify(data.system_health || {})}`)
    }
  } catch (error) {
    log(`❌ Fetch error: ${error.message}`)
  }
}

const forceConnect = () => {
  log('Forcing connection via connectionService...')
  
  if (connectionService?.connectToServer) {
    connectionService.connectToServer()
  } else {
    log('❌ No connectionService available')
  }
}

const forceConnectedState = () => {
  log('Forcing connected state manually...')
  
  if (connectionService?.isConnected) {
    connectionService.isConnected.value = true
  }
  
  if (connectionService?.connectionStatus) {
    connectionService.connectionStatus.value = 'connected'
  }
  
  log(`✅ Forced state - isConnected: ${connectionService?.isConnected?.value}, status: ${connectionService?.connectionStatus?.value}`)
}
</script>

<style scoped>
.connection-debug {
  padding: 16px;
  border: 2px solid #f00;
  margin: 16px;
  background: #fff;
}

.log {
  margin-top: 16px;
  max-height: 300px;
  overflow-y: auto;
}

.log pre {
  font-size: 11px;
  background: #f5f5f5;
  padding: 8px;
  white-space: pre-wrap;
}

button {
  margin: 4px;
  padding: 8px 12px;
  background: #007bff;
  color: white;
  border: none;
  cursor: pointer;
}

button:hover {
  background: #0056b3;
}
</style>
