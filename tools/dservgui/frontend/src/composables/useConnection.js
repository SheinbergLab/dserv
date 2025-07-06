import { ref, reactive, computed } from 'vue'

// Global connection state
const isConnected = ref(false)
const connectionStatus = ref('disconnected') // 'disconnected', 'connecting', 'connected', 'error'
const serverStatus = reactive({
  callbackPort: null,
  registrationID: null,
  address: 'localhost:4620',
  lastUpdate: null
})

let eventSource = null
let webSocket = null
let wsReconnectTimer = null

export function useConnection() {
  
  const connectToServer = async () => {
    console.log('🔄 connectToServer called - current isConnected:', isConnected.value)
    
    if (isConnected.value) {
      console.log('Already connected')
      return
    }

    connectionStatus.value = 'connecting'
    console.log('🔄 Set connection status to connecting')
    
    try {
      // First, get server status
      console.log('🔄 Fetching server status from /api/status...')
      const response = await fetch('/api/status')
      console.log('🔄 Status response:', response.status, response.statusText)
      
      if (response.ok) {
        const status = await response.json()
        console.log('🔄 Server status received:', status)
        Object.assign(serverStatus, status.dserv_client || {})
      } else {
        throw new Error(`HTTP ${response.status}: ${response.statusText}`)
      }

      // Start event stream for live updates
      console.log('🔄 Starting event stream...')
      startEventStream()
      
      // Start WebSocket for high-frequency updates
      console.log('🔄 Starting WebSocket connection...')
      startWebSocket()
      
      // Note: isConnected will be set to true in the EventSource onopen handler
      console.log('✅ connectToServer completed, waiting for EventSource to open...')
      
    } catch (error) {
      console.error('❌ Connection failed:', error)
      connectionStatus.value = 'error'
      isConnected.value = false
      
      // Retry after delay
      setTimeout(() => {
        console.log('🔄 Retrying connection in 5 seconds...')
        if (!isConnected.value) {
          connectToServer()
        }
      }, 5000)
    }
  }

  const disconnectFromServer = () => {
    // Close SSE connection
    if (eventSource) {
      eventSource.close()
      eventSource = null
    }
    
    // Close WebSocket connection
    if (webSocket) {
      webSocket.close()
      webSocket = null
    }
    
    // Clear any reconnect timers
    if (wsReconnectTimer) {
      clearTimeout(wsReconnectTimer)
      wsReconnectTimer = null
    }
    
    isConnected.value = false
    connectionStatus.value = 'disconnected'
    console.log('🔌 Disconnected from server (both SSE and WebSocket)')
  }

  const startEventStream = () => {
    console.log('🔄 startEventStream called')
    
    if (eventSource) {
      console.log('🔄 Closing existing EventSource')
      eventSource.close()
    }
    
    console.log('🔄 Creating new EventSource for /api/updates')
    eventSource = new EventSource('/api/updates')
    
    // Add a timeout to detect if EventSource never opens
    const openTimeout = setTimeout(() => {
      if (eventSource && eventSource.readyState === EventSource.CONNECTING) {
        console.error('❌ EventSource connection timeout - still connecting after 10 seconds')
        console.error('❌ EventSource readyState:', eventSource.readyState)
        console.error('❌ EventSource url:', eventSource.url)
        
        isConnected.value = false
        connectionStatus.value = 'error'
        
        // Close and retry
        eventSource.close()
        setTimeout(() => {
          console.log('🔄 Retrying EventSource after timeout...')
          startEventStream()
        }, 5000)
      }
    }, 10000)
    
    eventSource.onopen = () => {
      console.log('✅ EventSource opened successfully')
      console.log('🔍 About to set isConnected to true...')
      clearTimeout(openTimeout)
      isConnected.value = true
      connectionStatus.value = 'connected'
      console.log('📡 Event stream connected - isConnected now:', isConnected.value)
      console.log('📡 Event stream connected - connectionStatus now:', connectionStatus.value)
    }
    
    eventSource.onmessage = (event) => {
      try {
        const update = JSON.parse(event.data)
        handleServerUpdate(update)
      } catch (error) {
        console.error('Error parsing server update:', error)
      }
    }

    eventSource.onerror = (error) => {
      console.error('❌ Event stream error:', error)
      console.log('❌ EventSource readyState:', eventSource?.readyState)
      console.log('❌ EventSource url:', eventSource?.url)
      
      clearTimeout(openTimeout)
      isConnected.value = false
      connectionStatus.value = 'error'
      
      // Attempt to reconnect
      setTimeout(() => {
        if (eventSource && eventSource.readyState === EventSource.CLOSED) {
          console.log('🔄 Attempting to reconnect EventSource...')
          startEventStream()
        }
      }, 3000)
    }
  }

  const startWebSocket = () => {
    console.log('🔄 startWebSocket called')
    
    // Close existing WebSocket if any
    if (webSocket) {
      console.log('🔄 Closing existing WebSocket')
      webSocket.close()
    }
    
    // Clear any existing reconnect timer
    if (wsReconnectTimer) {
      clearTimeout(wsReconnectTimer)
      wsReconnectTimer = null
    }
    
    try {
      // Create WebSocket connection
      const wsUrl = `ws://${window.location.hostname}:8080/api/ws`
      console.log('🔄 Creating WebSocket connection to:', wsUrl)
      webSocket = new WebSocket(wsUrl)
      
      webSocket.onopen = () => {
        console.log('✅ WebSocket connection opened successfully')
        console.log('📡 WebSocket ready for high-frequency data')
      }
      
      webSocket.onmessage = (event) => {
        try {
          const update = JSON.parse(event.data)
          console.log('📡 WebSocket received high-freq update:', update.name || update.type)
          handleServerUpdate(update)
        } catch (error) {
          console.error('❌ Error parsing WebSocket message:', error)
        }
      }
      
      webSocket.onclose = (event) => {
        console.log('📡 WebSocket connection closed:', event.code, event.reason)
        
        // Only attempt reconnect if we're still supposed to be connected
        if (isConnected.value && !event.wasClean) {
          console.log('🔄 WebSocket closed unexpectedly, attempting reconnect in 3 seconds...')
          wsReconnectTimer = setTimeout(() => {
            if (isConnected.value) {
              startWebSocket()
            }
          }, 3000)
        }
      }
      
      webSocket.onerror = (error) => {
        console.error('❌ WebSocket error:', error)
        // Don't set connection status to error - SSE is still primary connection
        // WebSocket is supplementary for high-frequency data
      }
      
    } catch (error) {
      console.error('❌ Failed to create WebSocket:', error)
      // Don't treat WebSocket failure as connection failure - SSE is primary
    }
  }

  const handleServerUpdate = (update) => {
    // Handle different types of updates
    serverStatus.lastUpdate = new Date().toISOString()
    
    // Log different types of updates
    if (update.type === 'connected') {
      console.log('🔄 Connection confirmation received via', update.channel || 'SSE')
    } else if (update.name) {
      // Variable update - log high-frequency vs low-frequency
      const isHighFreq = ['ess/em_pos', 'ess/em_velocity', 'ess/cursor_pos'].includes(update.name)
      console.log(`📡 ${isHighFreq ? 'High-freq' : 'Low-freq'} update:`, update.name, '=', update.value)
    }
    
    console.log('🔄 useConnection - Dispatching server-update event...')
    
    // Emit update to other composables/components as needed
    window.dispatchEvent(new CustomEvent('server-update', { 
      detail: update 
    }))
    
    console.log('🔄 useConnection - server-update event dispatched')
  }

  const getServerStatus = async () => {
    try {
      const response = await fetch('/api/status')
      if (response.ok) {
        const status = await response.json()
        Object.assign(serverStatus, status.dserv_client || {})
        return status
      }
    } catch (error) {
      console.error('Failed to get server status:', error)
    }
    return null
  }

  const initialize = async () => {
    console.log('🔄 Initializing connection service...')
    
    try {
      // Check if server is available
      console.log('🔄 Testing server availability...')
      const status = await getServerStatus()
      console.log('🔄 Server status result:', status)
      
      if (status) {
        console.log('✅ Server is available, attempting auto-connect...')
        // Auto-connect if server is available
        await connectToServer()
      } else {
        console.log('❌ Server not available, skipping auto-connect')
        connectionStatus.value = 'error'
      }
    } catch (error) {
      console.error('❌ Failed to initialize connection:', error)
      connectionStatus.value = 'error'
    }
  }

  const forceReconnect = async () => {
    console.log('🔄 Force reconnect requested - disconnecting and reconnecting...')
    
    // Disconnect first
    disconnectFromServer()
    
    // Wait a moment for cleanup
    await new Promise(resolve => setTimeout(resolve, 1000))
    
    // Reconnect
    await connectToServer()
  }

  const cleanup = () => {
    console.log('🔄 Cleaning up connection service...')
    disconnectFromServer()
  }

  return {
    // State
    isConnected,
    connectionStatus,
    serverStatus,
    
    // Connection info
    webSocketConnected: computed(() => webSocket?.readyState === WebSocket.OPEN),
    sseConnected: computed(() => eventSource?.readyState === EventSource.OPEN),
    
    // Methods
    connectToServer,
    disconnectFromServer,
    forceReconnect,
    getServerStatus,
    initialize,
    cleanup
  }
}
