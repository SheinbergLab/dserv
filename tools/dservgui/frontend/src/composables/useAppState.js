import { ref, reactive } from 'vue'

// Global app state
const activePanel = ref('query')
const currentTime = ref('')
const appStatistics = reactive({
  logEntries: 0,
  subscriptions: 0,
  variables: 0,
  updatesPerMinute: 0,
  memoryUsage: 0
})

const appSettings = reactive({
  theme: 'light',
  autoConnect: true,
  logLevel: 'info',
  maxLogEntries: 1000,
  refreshInterval: 5000,
  fontSize: 13,
  compactMode: false
})

export function useAppState() {
  
  const updateCurrentTime = () => {
    currentTime.value = new Date().toLocaleTimeString()
  }

  const updateStatistics = async () => {
    try {
      // Get statistics from various sources
      const [statusResponse, variablesResponse] = await Promise.all([
        fetch('/api/status'),
        fetch('/api/variables')
      ])

      if (statusResponse.ok) {
        const status = await statusResponse.json()
        
        // Update statistics from server response
        if (status.state_manager) {
          appStatistics.variables = status.state_manager.total_variables || 0
          appStatistics.subscriptions = status.state_manager.subscribers || 0
        }
        
        if (status.dserv_client) {
          // Calculate updates per minute
          const totalUpdates = status.dserv_client.total_updates || 0
          // This is a simplified calculation
          appStatistics.updatesPerMinute = totalUpdates > 0 ? Math.round(totalUpdates / 60) : 0
        }
      }

      if (variablesResponse.ok) {
        const variables = await variablesResponse.json()
        if (variables.stats) {
          appStatistics.variables = variables.stats.total_variables || 0
        }
      }

    } catch (error) {
      console.error('Failed to update statistics:', error)
    }
  }

  const loadSettings = () => {
    try {
      const saved = localStorage.getItem('dserv-app-settings')
      if (saved) {
        const savedSettings = JSON.parse(saved)
        Object.assign(appSettings, savedSettings)
      }
    } catch (error) {
      console.warn('Failed to load settings:', error)
    }
  }

  const saveSettings = () => {
    try {
      localStorage.setItem('dserv-app-settings', JSON.stringify(appSettings))
    } catch (error) {
      console.warn('Failed to save settings:', error)
    }
  }

  const resetSettings = () => {
    Object.assign(appSettings, {
      theme: 'light',
      autoConnect: true,
      logLevel: 'info',
      maxLogEntries: 1000,
      refreshInterval: 5000,
      fontSize: 13,
      compactMode: false
    })
    saveSettings()
  }

  const incrementLogEntries = () => {
    appStatistics.logEntries++
    
    // Trim if over limit
    if (appStatistics.logEntries > appSettings.maxLogEntries) {
      appStatistics.logEntries = appSettings.maxLogEntries
    }
  }

  const clearLogEntries = () => {
    appStatistics.logEntries = 0
  }

  const setActivePanel = (panelName) => {
    if (['query', 'subscribe', 'dataframe', 'monitor', 'log'].includes(panelName)) {
      activePanel.value = panelName
      
      // Save last active panel
      localStorage.setItem('dserv-active-panel', panelName)
    }
  }

  const getMemoryUsage = () => {
    // Estimate memory usage (simplified)
    if (performance && performance.memory) {
      const used = performance.memory.usedJSHeapSize
      const total = performance.memory.totalJSHeapSize
      appStatistics.memoryUsage = Math.round((used / total) * 100)
    }
  }

  const initialize = () => {
    console.log('🔄 Initializing app state...')
    
    // Load saved settings
    loadSettings()
    
    // Load last active panel
    const savedPanel = localStorage.getItem('dserv-active-panel')
    if (savedPanel) {
      activePanel.value = savedPanel
    }
    
    // Start periodic updates
    updateCurrentTime()
    updateStatistics()
    getMemoryUsage()
    
    // Set up intervals
    const timeInterval = setInterval(updateCurrentTime, 1000)
    const statsInterval = setInterval(updateStatistics, appSettings.refreshInterval)
    const memoryInterval = setInterval(getMemoryUsage, 10000)
    
    // Store intervals for cleanup
    window.appStateIntervals = {
      time: timeInterval,
      stats: statsInterval,
      memory: memoryInterval
    }
  }

  const cleanup = () => {
    console.log('🔄 Cleaning up app state...')
    
    // Save settings
    saveSettings()
    
    // Clear intervals
    if (window.appStateIntervals) {
      Object.values(window.appStateIntervals).forEach(clearInterval)
      delete window.appStateIntervals
    }
  }

  return {
    // State
    activePanel,
    currentTime,
    appStatistics,
    appSettings,
    
    // Methods
    updateCurrentTime,
    updateStatistics,
    loadSettings,
    saveSettings,
    resetSettings,
    incrementLogEntries,
    clearLogEntries,
    setActivePanel,
    getMemoryUsage,
    initialize,
    cleanup
  }
}
