<template>
  <div class="desktop-app">
    <!-- Title Bar -->
    <TitleBar 
      :app-name="appName"
      :app-icon="appIcon"
      @minimize="handleMinimize"
      @maximize="handleMaximize" 
      @close="handleClose"
    />

    <!-- Menu Bar -->
    <MenuBar 
      :menu-items="menuItems"
      @menu-action="handleMenuAction"
    />

    <!-- Main Toolbar -->
    <MainToolbar 
      :connection-status="connectionStatus"
      :active-panel="activePanel"
      :is-connected="isConnected"
      @connect="connectToServer"
      @disconnect="disconnectFromServer"
      @panel-change="handlePanelChange"
    />

    <!-- Main Layout Container -->
    <div class="main-layout">
      <!-- Content Area with Sidebars -->
      <div class="content-area">
        <!-- Left Sidebar -->
        <div class="left-sidebar" :style="{ width: leftSidebarWidth + 'px' }">
          <div class="sidebar-resizer left" @mousedown="startLeftResize"></div>
          <SystemControlSidebar />
        </div>

        <!-- Main Panel Area -->
        <div class="main-panel">
          <PanelContainer 
            :active-panel="activePanel"
            :is-connected="isConnected"
            :connection-status="connectionStatus"
          />
        </div>

        <!-- Right Sidebar -->
        <div class="right-sidebar" :style="{ width: rightSidebarWidth + 'px' }">
          <div class="sidebar-resizer right" @mousedown="startRightResize"></div>
          <StatusSidebar 
            :server-status="serverStatus"
            :statistics="appStatistics"
            :is-connected="isConnected"
          />
        </div>
      </div>

      <!-- Bottom Terminal -->
      <div class="bottom-terminal" :style="{ height: terminalHeight + 'px' }">
        <div class="terminal-resizer" @mousedown="startTerminalResize"></div>
        <Terminal />
      </div>
    </div>

    <!-- Status Bar -->
    <StatusBar 
      :status-message="statusMessage"
      :current-time="currentTime"
      :connection-status="connectionStatus"
    />

    <!-- Modal Dialogs -->
    <AboutDialog 
      v-if="showAbout"
      @close="showAbout = false"
    />

    <SettingsDialog 
      v-if="showSettings"
      :settings="appSettings"
      @close="showSettings = false"
      @save="handleSettingsSave"
    />
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, provide, watch } from 'vue'

// Import all components
import TitleBar from './components/layout/TitleBar.vue'
import MenuBar from './components/layout/MenuBar.vue'
import MainToolbar from './components/layout/MainToolbar.vue'
import PanelContainer from './components/panels/PanelContainer.vue'
import SystemControlSidebar from './components/SystemControlSidebar.vue'
import SimpleSidebar from './components/SimpleSidebar.vue'
import StatusSidebar from './components/layout/StatusSidebar.vue'
import Terminal from './components/Terminal.vue'
import StatusBar from './components/layout/StatusBar.vue'
import AboutDialog from './components/dialogs/AboutDialog.vue'
import SettingsDialog from './components/dialogs/SettingsDialog.vue'

// Import composables
import { useEssState } from './composables/useEssState'
import { useConnection } from './composables/useConnection'
import { useAppState } from './composables/useAppState'
import { useResizer } from './composables/useResizer'

// Terminal mode setting
const serviceMode = ref("ess")
provide("serviceMode", serviceMode)


// App configuration
const appName = 'ESS DservGUI'
const appIcon = '📡'

// Initialize composables
console.log('🔄 Initializing composables...')
const essStateComposable = useEssState()
const {
  essState,
  updateEssVariable,
  initialize: initializeEss,
  cleanup: cleanupEss,
  backendStateAvailable
} = essStateComposable

const connectionComposable = useConnection()
const {
  isConnected,
  connectionStatus,
  serverStatus,
  connectToServer,
  disconnectFromServer,
  initialize: initializeConnection,
  cleanup: cleanupConnection
} = connectionComposable

console.log('🔄 Composables initialized successfully')

const {
  activePanel,
  currentTime,
  appStatistics,
  appSettings,
  updateCurrentTime,
  updateStatistics
} = useAppState()

const {
  leftSidebarWidth,
  rightSidebarWidth,
  terminalHeight,
  startLeftResize,
  startRightResize,
  startTerminalResize
} = useResizer()

// Menu configuration
const menuItems = ref([
  {
    label: 'File',
    items: [
      { label: 'New Session', action: 'new-session' },
      { label: 'Save Settings', action: 'save-settings' },
      { label: 'Export Data', action: 'export-data' },
      { label: 'Exit', action: 'exit' }
    ]
  },
  {
    label: 'View',
    items: [
      { label: 'Refresh All', action: 'refresh-all' },
      { label: 'Clear Log', action: 'clear-log' },
      { label: 'Full Screen', action: 'toggle-fullscreen' }
    ]
  },
  {
    label: 'Tools',
    items: [
      { label: 'Settings', action: 'show-settings' },
      { label: 'Debug Info', action: 'show-debug' },
      { label: 'System Health', action: 'system-health' }
    ]
  },
  {
    label: 'Help',
    items: [
      { label: 'User Guide', action: 'user-guide' },
      { label: 'Keyboard Shortcuts', action: 'shortcuts' },
      { label: 'About', action: 'show-about' }
    ]
  }
])

// Modal states
const showAbout = ref(false)
const showSettings = ref(false)

// Computed
const statusMessage = computed(() => {
  const connectedState = isConnected.value
  const connStatus = connectionStatus.value
  
  console.log('🔍 statusMessage computed - isConnected.value:', connectedState)
  console.log('🔍 statusMessage computed - connectionStatus.value:', connStatus)
  
  // Handle different connection states
  switch (connStatus) {
    case 'connected':
      return 'Connected'
    case 'connecting':
      return 'Connecting to server...'
    case 'disconnected':
      return 'Disconnected - Click Connect to start'
    case 'error':
      return 'Connection error - Check server status'
    default:
      return 'Ready - Click Connect to start'
  }
})

// Provide services to child components
provide('essState', essState)
provide('connectionService', { isConnected, connectionStatus, connectToServer, disconnectFromServer })
provide('appState', { activePanel, appStatistics })

// Event handlers
const handleMinimize = () => {
  console.log('Minimize window')
}

const handleMaximize = () => {
  console.log('Maximize window')
}

const handleClose = () => {
  console.log('Close application')
}

const handleMenuAction = (action) => {
  console.log('Menu action:', action)
  
  switch (action) {
    case 'show-about':
      showAbout.value = true
      break
    case 'show-settings':
      showSettings.value = true
      break
    case 'refresh-all':
      handleRefreshAll()
      break
    case 'clear-log':
      handleClearLog()
      break
    case 'export-data':
      handleExportData()
      break
    default:
      console.log('Unhandled menu action:', action)
  }
}

const handlePanelChange = (panelName) => {
  activePanel.value = panelName
}

const handleSettingsSave = (newSettings) => {
  appSettings.value = { ...appSettings.value, ...newSettings }
  localStorage.setItem('dserv-settings', JSON.stringify(appSettings.value))
  showSettings.value = false
}

const handleRefreshAll = async () => {
  // Refresh all data sources
  console.log('Refreshing all data...')
  // Implementation would call various refresh methods
}

const handleClearLog = () => {
  // Clear application logs
  console.log('Clearing logs...')
  // Implementation would clear log entries
}

const handleExportData = () => {
  // Export current data
  console.log('Exporting data...')
  // Implementation would export data to file
}

// Lifecycle
// Replace the onMounted section in your App.vue with this:

onMounted(async () => {
  console.log('🚀 ESS DservGUI starting...')
  
  // IMPORTANT: Initialize connection first (this establishes the EventSource)
  console.log('🔄 Initializing connection service...')
  await initializeConnection()
  
  // Wait a moment for connection to stabilize
  await new Promise(resolve => setTimeout(resolve, 1000))
  
  // Then initialize ESS state (this will use the existing connection)
  console.log('🔄 Initializing ESS state service...')
  await initializeEss()
  
  // Start periodic updates
  const timeInterval = setInterval(updateCurrentTime, 1000)
  const statsInterval = setInterval(updateStatistics, 5000)
  
  // Store intervals for cleanup
  window.timeInterval = timeInterval
  window.statsInterval = statsInterval
  
  console.log('✅ ESS DservGUI ready')
})

onUnmounted(() => {
  console.log('🔄 ESS DservGUI shutting down...')
  
  // Cleanup intervals
  if (window.timeInterval) clearInterval(window.timeInterval)
  if (window.statsInterval) clearInterval(window.statsInterval)
  
  // Cleanup services
  cleanupEss()
  cleanupConnection()
  
  console.log('✅ ESS DservGUI shutdown complete')
})

// Add debugging watchers
// Debug watchers to track state changes
watch(isConnected, (newVal, oldVal) => {
  console.log('🔍 WATCH isConnected changed from', oldVal, 'to', newVal)
}, { immediate: true })

watch(connectionStatus, (newVal, oldVal) => {
  console.log('🔍 WATCH connectionStatus changed from', oldVal, 'to', newVal)
}, { immediate: true })

watch(statusMessage, (newVal, oldVal) => {
  console.log('🔍 WATCH statusMessage changed from', oldVal, 'to', newVal)
}, { immediate: true })
</script>

<style scoped>
.desktop-app {
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', system-ui, sans-serif;
  font-size: 13px;
  line-height: 1.4;
  height: 100vh;
  display: flex;
  flex-direction: column;
  background: #f0f0f0;
  border: 1px solid #999;
  overflow: hidden;
}

.main-layout {
  flex: 1;
  display: flex;
  flex-direction: column;
  overflow: hidden;
  min-height: 0;
}

.content-area {
  flex: 1;
  display: flex;
  overflow: hidden;
  min-height: 0;
}

.left-sidebar {
  background: #f8f8f8;
  border-right: 1px solid #ccc;
  overflow-y: auto;
  flex-shrink: 0;
  min-width: 200px;
  max-width: 400px;
  position: relative;
}

.right-sidebar {
  background: #f8f8f8;
  border-left: 1px solid #ccc;
  overflow-y: auto;
  flex-shrink: 0;
  min-width: 150px;
  max-width: 300px;
  position: relative;
}

.main-panel {
  flex: 1;
  background: white;
  overflow: hidden;
  display: flex;
  flex-direction: column;
}

.bottom-terminal {
  position: relative;
  background: #f0f0f0;
  border-top: 2px solid #999;
  flex-shrink: 0;
  overflow: hidden;
  min-height: 150px;
  max-height: 600px;
}

.sidebar-resizer {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 4px;
  background: transparent;
  cursor: ew-resize;
  z-index: 10;
}

.sidebar-resizer.left {
  right: 0;
}

.sidebar-resizer.right {
  left: 0;
}

.sidebar-resizer:hover {
  background: #0078d4;
}

.terminal-resizer {
  position: absolute;
  top: 0;
  left: 0;
  right: 0;
  height: 4px;
  background: #d0d0d0;
  cursor: ns-resize;
  z-index: 10;
  border: 1px solid #999;
}

.terminal-resizer:hover {
  background: #0078d4;
}

/* Force Terminal component to use full height */
.bottom-terminal :deep(.terminal) {
  height: 100%;
  display: flex;
  flex-direction: column;
}

.bottom-terminal :deep(.terminal-output) {
  flex: 1;
  min-height: 0;
}

.bottom-terminal :deep(.terminal-header),
.bottom-terminal :deep(.terminal-input-form),
.bottom-terminal :deep(.terminal-status) {
  flex-shrink: 0;
}
</style>