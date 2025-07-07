import { ref, reactive } from 'vue'

// Tcl list parser to handle space-separated lists with braces for items containing spaces
// Example: "item1 {item2 with space} item3" -> ["item1", "item2 with space", "item3"]
function parseTclList(str) {
  if (!str || typeof str !== 'string') return []
  
  const items = []
  let current = ''
  let braceLevel = 0
  let inBraces = false
  
  for (let i = 0; i < str.length; i++) {
    const char = str[i]
    
    if (char === '{' && !inBraces) {
      inBraces = true
      braceLevel = 1
    } else if (char === '{' && inBraces) {
      braceLevel++
      current += char
    } else if (char === '}' && inBraces) {
      braceLevel--
      if (braceLevel === 0) {
        inBraces = false
        // Don't add the closing brace to current
      } else {
        current += char
      }
    } else if (char === ' ' && !inBraces) {
      // Space outside braces - end current item
      if (current.trim()) {
        items.push(current.trim())
        current = ''
      }
    } else {
      current += char
    }
  }
  
  // Add final item
  if (current.trim()) {
    items.push(current.trim())
  }
  
  return items.filter(item => item.length > 0)
}

// ESS State Management
const essState = reactive({
  currentSystem: '',
  currentProtocol: '',
  currentVariant: '',
  currentSubject: '',
  status: '', // string: 'running', 'stopped', 'inactive'
  in_obs: false, // boolean: converted from 0/1
  obs_id: 0, // integer: current observation ID (starts at 0)
  obs_total: 0, // integer: total number of observations
  systems: [],
  protocols: [],
  variants: [],
  configComplete: false,
  variables: {}
})

const backendStateAvailable = ref(false)

export function useEssState() {
  
  const connectToBackendState = () => {
    // Don't create any EventSource here - use the existing connection from useConnection
    // Just listen for server updates via window events
    window.addEventListener('server-update', handleEssUpdate)
    
    // Add visibility change listener to detect wake from sleep
    document.addEventListener('visibilitychange', handleVisibilityChange)
    
    console.log('🔄 Connected to ESS state via existing connection')
    backendStateAvailable.value = true
    return true
  }

  const handleVisibilityChange = () => {
    if (document.visibilityState === 'visible') {
      console.log('🔄 useEssState - Page became visible, checking connection...')
      
      // Force a complete reconnection by testing the connection
      testConnectionAndReconnect()
    }
  }

  const testConnectionAndReconnect = async () => {
    try {
      // Test if we can actually reach the server
      const testResponse = await fetch('/api/status', { 
        method: 'HEAD',
        cache: 'no-cache'
      })
      
      if (!testResponse.ok) {
        throw new Error(`Server not responding: ${testResponse.status}`)
      }
      
      // Test if our variable endpoint works
      const variableTest = await fetch('/api/variables/prefix?prefix=ess/', {
        cache: 'no-cache'
      })
      
      if (variableTest.ok) {
        console.log('🔄 useEssState - Server is responding, reloading ESS state...')
        // Small delay to allow any connection recovery
        setTimeout(() => {
          loadInitialState()
        }, 1000)
      } else {
        throw new Error(`Variables endpoint not responding: ${variableTest.status}`)
      }
      
    } catch (error) {
      console.error('❌ useEssState - Server connection test failed:', error)
      
      // Force connection service to reconnect
      const connectionService = window.connectionService || 
        (typeof window !== 'undefined' && window.app?.config?.globalProperties?.$connectionService)
      
      if (connectionService && typeof connectionService.forceReconnect === 'function') {
        console.log('🔄 useEssState - Forcing connection service to reconnect...')
        await connectionService.forceReconnect()
        
        // Try loading ESS state again after reconnection
        setTimeout(() => {
          loadInitialState()
        }, 2000)
      } else {
        console.log('🔄 useEssState - Connection service not available, retrying in 5s...')
        // Retry after longer delay
        setTimeout(() => {
          testConnectionAndReconnect()
        }, 5000)
      }
    }
  }

  const handleEssUpdate = (event) => {
    const update = event.detail
    
    console.log('🔄 useEssState - Received server update:', update)
    
    if (update && update.name) {
      // Process ESS-related variable updates immediately in the order received
      if (update.name.startsWith('ess/')) {
        console.log('🔄 useEssState - Processing ESS update immediately:', update.name, '=', update.value)
        updateEssVariable(update.name, update.value)
      } else {
        console.log('🔄 useEssState - Ignoring non-ESS update:', update.name)
      }
    } else {
      console.log('🔄 useEssState - Invalid update format:', update)
    }
  }

  const updateEssVariable = (name, value) => {
    console.log('🔄 useEssState - updateEssVariable called:', name, '=', value)
    
    // Store in variables as an object with value and timestamp for consistency
    essState.variables[name] = {
      name: name,
      value: value,
      timestamp: new Date().toISOString()
    }
    console.log('🔄 useEssState - Updated variables store, now has:', Object.keys(essState.variables).length, 'items')
    
    // Update specific ESS state based on variable name
    switch (name) {
      case 'ess/system':
        const oldSystem = essState.currentSystem
        essState.currentSystem = value || ''
        console.log('🔄 useEssState - System updated:', oldSystem, '->', essState.currentSystem)
        
        // Reset observation counters when system changes to trigger empty label
        if (oldSystem !== essState.currentSystem) {
          essState.obs_id = 0
          essState.obs_total = 0
          console.log('🔄 useEssState - Reset observation counters due to system change')
        }
        break
      case 'ess/protocol':
        essState.currentProtocol = value || ''
        console.log('🔄 useEssState - Protocol updated:', essState.currentProtocol)
        break
      case 'ess/variant':
        essState.currentVariant = value || ''
        console.log('🔄 useEssState - Variant updated:', essState.currentVariant)
        break
      case 'ess/subject':
        essState.currentSubject = value || ''
        console.log('🔄 useEssState - Subject updated:', essState.currentSubject)
        break
      case 'ess/status':
          essState.status = value || ''
          console.log('🔄 useEssState - Status updated:', essState.status)
          break
      case 'ess/in_obs':
          // Convert 0/1 or "0"/"1" to boolean
          essState.in_obs = !!(value === 1 || value === '1' || value === true)
          console.log('🔄 useEssState - in_obs updated:', essState.in_obs)
          break
      case 'ess/obs_id':
          // Convert to integer
          essState.obs_id = parseInt(value) || 0
          console.log('🔄 useEssState - obs_id updated:', essState.obs_id)
          break
      case 'ess/obs_total':
          // Convert to integer
          essState.obs_total = parseInt(value) || 0
          console.log('🔄 useEssState - obs_total updated:', essState.obs_total)
          break
      case 'ess/reset':
          // Reset event received - reset obs_id to 0 but keep obs_total
          if (value) { // Only process if value is truthy (event occurred)
            essState.obs_id = 0
            console.log('🔄 useEssState - Reset event received, obs_id reset to 0, keeping obs_total:', essState.obs_total)
          }
          break
      case 'ess/systems':
        if (Array.isArray(value)) {
          essState.systems = value
        } else if (typeof value === 'string') {
          console.log('🔄 Parsing systems string:', value)
          essState.systems = parseTclList(value)
          console.log('🔄 Parsed systems result:', essState.systems)
        } else {
          essState.systems = []
        }
        console.log('📊 Systems list updated:', essState.systems)
        break
      case 'ess/protocols':
        if (Array.isArray(value)) {
          essState.protocols = value
        } else if (typeof value === 'string') {
          console.log('🔄 Parsing protocols string:', value)
          essState.protocols = parseTclList(value)
          console.log('🔄 Parsed protocols result:', essState.protocols)
        } else {
          essState.protocols = []
        }
        console.log('📊 Protocols list updated:', essState.protocols)
        break
      case 'ess/variants':
        if (Array.isArray(value)) {
          essState.variants = value
        } else if (typeof value === 'string') {
          console.log('🔄 Parsing variants string:', value)
          essState.variants = parseTclList(value)
          console.log('🔄 Parsed variants result:', essState.variants)
        } else {
          essState.variants = []
        }
        console.log('📊 Variants list updated:', essState.variants)
        break
    }
    
    // Update config complete status
    essState.configComplete = !!(
      essState.currentSystem && 
      essState.currentProtocol && 
      essState.currentVariant
    )
  }

  const loadInitialState = async () => {
    try {
      console.log('🔄 Loading initial ESS state...')
      
      // TODO: Implement when /api/ess/config endpoint is added
      // For now, get ESS configuration from variables
      const configResponse = await fetch('/api/variables/prefix?prefix=ess/')
      if (configResponse.ok) {
        const config = await configResponse.json()
        console.log('📊 ESS variables received:', config)
        
        // Process variables immediately in the order received from the API
        Object.entries(config.variables || {}).forEach(([name, variable]) => {
          console.log('🔄 loadInitialState - Processing variable immediately:', name, '=', variable.value)
          updateEssVariable(name, variable.value)
        })
        
        backendStateAvailable.value = true
        console.log('✅ Initial ESS state loaded successfully')
        return true
      } else {
        throw new Error(`HTTP ${configResponse.status}`)
      }
    } catch (error) {
      console.error('❌ Failed to load initial ESS state:', error)
      backendStateAvailable.value = false
      return false
    }
  }

  const setEssVariable = async (variableName, value) => {
    try {
      const response = await fetch('/api/ess/eval', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({
          script: `set ${variableName} ${JSON.stringify(value)}`
        })
      })

      if (response.ok) {
        const result = await response.json()
        console.log('✅ ESS variable set:', variableName, '=', value)
        
        // Update local state immediately for responsiveness
        updateEssVariable(variableName, value)
        
        return result.success
      } else {
        throw new Error(`HTTP ${response.status}`)
      }
    } catch (error) {
      console.error('❌ Failed to set ESS variable:', error)
      return false
    }
  }

  const touchSystemVariables = async () => {
    // No longer needed - dserv automatically sends updates when variables change
    console.log('ℹ️ Touch operations not needed - dserv sends automatic updates')
  }

  const getCurrentState = () => {
    return {
      system: essState.currentSystem,
      protocol: essState.currentProtocol,
      variant: essState.currentVariant,
      configComplete: essState.configComplete
    }
  }

  const initialize = async () => {
    console.log('🔄 Initializing ESS state...')
    
    // Load initial state first
    const loaded = await loadInitialState()
    
    if (loaded) {
      // Connect to live updates (just add event listener, don't create EventSource)
      connectToBackendState()
    }
    
    return loaded
  }

  const cleanup = () => {
    console.log('🔄 Cleaning up ESS state...')
    
    // Remove event listeners
    window.removeEventListener('server-update', handleEssUpdate)
    document.removeEventListener('visibilitychange', handleVisibilityChange)
    
    backendStateAvailable.value = false
  }

  return {
    // State
    essState,
    backendStateAvailable,
    
    // Methods
    updateEssVariable,
    setEssVariable,
    touchSystemVariables,
    getCurrentState,
    loadInitialState,
    connectToBackendState,
    initialize,
    cleanup
  }
}
