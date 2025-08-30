<template>
  <a-layout style="height: 100vh; font-size: 13px;">

    <!-- Top Bar with Development Mode Toggle - FULL WIDTH -->
    <div
      style="flex-shrink: 0; height: 32px; background: #fafafa; border-bottom: 1px solid #d9d9d9; display: flex; justify-content: space-between; align-items: center; padding: 0 12px;">
      <div style="display: flex; align-items: center; gap: 8px;">

        <!-- Existing system status (hide in dev mode) -->
        <template v-if="!developmentMode">
          <span style="font-size: 11px; color: #666;">System:</span>
          <a-badge :status="systemHealthStatus" :text="systemHealthText" style="font-size: 11px;" />

          <!-- Stim Connection Status -->
          <span style="font-size: 11px; color: #666; margin-left: 8px;">Stim:</span>
          <a-tag :color="stimConnectionColor" size="small" style="margin: 0; font-size: 10px;">
            {{ stimConnectionStatus }}
          </a-tag>
        </template>

        <!-- Development Mode Toggle -->
        <a-divider type="vertical" style="margin: 0 8px;" />
        <a-switch
          v-model:checked="developmentMode"
          size="small"
          checked-children="Dev"
          un-checked-children="Exp"
        />
        <span style="font-size: 11px; color: #666;">
          {{ developmentMode ? 'Development' : 'Experiment' }} Mode
        </span>

        <!-- Development mode indicator -->
        <template v-if="developmentMode">
          <a-tag color="processing" size="small" style="margin: 0; font-size: 10px;">
            🛠️ Development Active
          </a-tag>
        </template>
      </div>

      <div style="display: flex; align-items: center; gap: 4px;">
        <!-- System monitor toggle (hide in dev mode) -->
        <a-button
          v-if="!developmentMode"
          size="small"
          type="text"
          :icon="h(isStatusSidebarVisible ? EyeInvisibleOutlined : EyeOutlined)"
          @click="toggleStatusSidebar"
          style="font-size: 11px; height: 24px; padding: 0 8px;"
          :title="isStatusSidebarVisible ? 'Hide System Monitor' : 'Show System Monitor'"
        >
          {{ isStatusSidebarVisible ? 'Hide Monitor' : 'Show Monitor' }}
        </a-button>

        <!-- Development mode help -->
        <a-button
          v-if="developmentMode"
          size="small"
          type="text"
          :icon="h(QuestionCircleOutlined)"
          @click="showDevHelp = true"
          style="font-size: 11px; height: 24px; padding: 0 8px;"
          title="Development Mode Help"
        >
          Help
        </a-button>
      </div>
    </div>

    <!-- Main Content Area Below Header -->
    <a-layout style="height: calc(100vh - 56px);"> <!-- 56px = 32px header + 24px status bar -->

      <!-- Control Panel - Left Sidebar (Hide in dev mode) -->
      <a-layout-sider
        v-if="!developmentMode"
        width="290"
        theme="light"
        style="border-right: 1px solid #d9d9d9;"
      >
        <experiment-control ref="experimentControlRef" />
      </a-layout-sider>

      <!-- Middle Column - Eye/Touch Viewer + Performance (Hide in dev mode) -->
      <a-layout-sider
        v-if="!developmentMode"
        width="320"
        theme="light"
        style="border-right: 1px solid #d9d9d9; background: white;"
      >
        <div style="height: 100%; display: flex; flex-direction: column;">
          <!-- Eye/Touch Visualizer Section -->
          <div
            style="flex: 0 0 auto; height: 450px; border-bottom: 1px solid #d9d9d9; display: flex; flex-direction: column;">
            <!-- Simple Header -->
            <div style="flex-shrink: 0; padding: 8px; border-bottom: 1px solid #e8e8e8; background: #fafafa;">
              <span style="font-weight: 500; font-size: 12px;">Eye & Touch Tracking</span>
            </div>

            <!-- Eye/Touch Visualizer -->
            <div style="flex: 1; min-height: 0; padding: 8px;">
              <eye-touch-visualizer style="height: 100%;" />
            </div>
          </div>

          <!-- Performance Section -->
          <div style="flex: 1; min-height: 0; display: flex; flex-direction: column;">
            <!-- Performance Header -->
            <div style="flex-shrink: 0; padding: 8px; border-bottom: 1px solid #e8e8e8; background: #fafafa;">
              <span style="font-weight: 500; font-size: 12px;">Performance Monitor</span>
            </div>

            <!-- Performance Content -->
            <div style="flex: 1; padding: 8px; overflow: auto;">
              <!-- Performance Table -->
              <a-table :columns="performanceColumns" :data-source="performanceData" size="small" :pagination="false"
                :scroll="{ y: 120 }" style="margin-bottom: 8px;" />

              <!-- Quick Stats -->
              <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin-top: 8px;">
                <div style="padding: 4px 8px; background: #f5f5f5; border-radius: 4px; text-align: center;">
                  <div style="font-size: 10px; color: #666;">Overall Accuracy</div>
                  <div style="font-size: 14px; font-weight: 500;">86.5%</div>
                </div>
                <div style="padding: 4px 8px; background: #f5f5f5; border-radius: 4px; text-align: center;">
                  <div style="font-size: 10px; color: #666;">Avg RT</div>
                  <div style="font-size: 14px; font-weight: 500;">550ms</div>
                </div>
              </div>
            </div>
          </div>
        </div>
      </a-layout-sider>

      <!-- Main Content Area with Tabs -->
      <a-layout style="display: flex; flex-direction: column; height: 100%; overflow: hidden;">

        <!-- Main Content with Optional Right Sidebar -->
        <div style="flex: 1; display: flex; overflow: hidden;">

          <!-- Main Content Area -->
          <a-layout-content
            style="background: white; flex: 1; display: flex; flex-direction: column; overflow: hidden;">

            <!-- Development Workspace (Full Screen in Dev Mode) -->
            <div v-if="developmentMode" style="height: 100%; overflow: hidden;">
              <development-workspace ref="developmentWorkspaceRef" />
            </div>

            <!-- Regular Tabs (Experiment Mode) -->
            <div v-else style="flex: 1; display: flex; flex-direction: column; overflow: hidden;">
              <a-tabs size="small" style="height: 100%; display: flex; flex-direction: column;" tab-position="top">

                <a-tab-pane key="graphics" tab="Graphics" style="height: 100%; overflow: hidden;">
                <div style="height: 100%; overflow: hidden;">
                  <graphics-viewer
                    stream-id="graphics/stimulus"
                    title="Stimulus Graphics"
                  />
                </div>
                </a-tab-pane>

                <a-tab-pane key="scripts" tab="Scripts" style="height: 100%; overflow: hidden;">
                  <div style="height: 100%; overflow: hidden;">
                    <scripts />
                  </div>
                </a-tab-pane>

                <a-tab-pane key="stim" tab="Stim" style="height: 100%; overflow: hidden;">
                  <div style="height: 100%; overflow: hidden;">
                    <stim-info />
                  </div>
                </a-tab-pane>

                <a-tab-pane key="system" tab="System" style="height: 100%; overflow: hidden;">
                  <div style="height: 100%; overflow: hidden;">
                    <state-system-diagram />
                  </div>
                </a-tab-pane>

                <a-tab-pane key="events" tab="Events" style="height: 100%; overflow: hidden;">
                  <div style="height: 100%; overflow: hidden;">
                    <event-tracker />
                  </div>
                </a-tab-pane>

                <a-tab-pane key="console" tab="Console" style="height: 100%; overflow: hidden;">
                  <div style="height: 100%; overflow: hidden;">
                    <ess-console />
                  </div>
                </a-tab-pane>

              </a-tabs>
            </div>
          </a-layout-content>

          <!-- Resizable Right Sidebar for System Status (Hide in dev mode) -->
          <template v-if="!developmentMode && isStatusSidebarVisible">
            <!-- Vertical Resizer -->
            <div class="vertical-resizer" @mousedown="startVerticalResize"></div>

            <!-- System Status Sidebar -->
            <div :style="{
              width: `${statusSidebarWidth}px`,
              flexShrink: 0,
              borderLeft: '1px solid #d9d9d9',
              background: '#fafafa',
              display: 'flex',
              flexDirection: 'column',
              overflow: 'hidden'
            }">
              <!-- Sidebar Header -->
              <div
                style="flex-shrink: 0; height: 40px; padding: 8px; border-bottom: 1px solid #d9d9d9; display: flex; justify-content: space-between; align-items: center; background: white;">
                <div style="display: flex; align-items: center; gap: 8px;">
                  <a-badge :status="systemHealthStatus" />
                  <span style="font-weight: 500; font-size: 12px;">System Monitor</span>
                  <span style="font-size: 10px; color: #999;">
                    {{ connectionUptime }} • {{ messagesPerSecond }}/s
                  </span>
                </div>
                <a-button size="small" type="text" :icon="h(CloseOutlined)" @click="isStatusSidebarVisible = false"
                  style="font-size: 10px; width: 20px; height: 20px; padding: 0;" />
              </div>

              <!-- System Status Content -->
              <div style="flex: 1; overflow: hidden;">
                <system-status />
              </div>
            </div>
          </template>
        </div>

        <!-- Terminal at Bottom (Hide in dev mode - dev workspace has its own) -->
        <template v-if="!developmentMode && isTerminalVisible">
          <div class="resizer" @mousedown="startResize"></div>
          <div :style="{ height: `${terminalHeight}px`, flexShrink: 0 }" class="terminal-wrapper">
            <ess-terminal ref="bottomTerminalRef" />
          </div>
        </template>

        <!-- Note: Development mode has its own integrated terminal in DevelopmentWorkspace -->
      </a-layout>
    </a-layout>

    <!-- Status Bar at Bottom - FULL WIDTH -->
    <status-bar :status-message="statusMessage" />

    <!-- Development Help Modal -->
    <a-modal
      v-model:open="showDevHelp"
      title="Development Mode Help"
      width="600px"
      :footer="null"
    >
      <div class="dev-help-content">
        <h4>🛠️ Development Mode Features</h4>
        <ul>
          <li><strong>Script Editor:</strong> Write and execute TCL scripts with syntax highlighting</li>
          <li><strong>Templates:</strong> Use the dropdown to load common script patterns</li>
          <li><strong>Terminal:</strong> View script output and system messages</li>
          <li><strong>Data Groups:</strong> Load and visualize dynamic groups (DGs)</li>
        </ul>

        <h4>⌨️ Keyboard Shortcuts</h4>
        <ul>
          <li><kbd>Ctrl+Enter</kbd> - Run script</li>
          <li><kbd>Ctrl+S</kbd> - Save script</li>
        </ul>

        <h4>📊 Working with Data Groups</h4>
        <ul>
          <li>Enter DG name (e.g., "stimdg") in the Data Groups tab</li>
          <li>Use script templates like "Load StimDG" for quick access</li>
          <li>Click array cells to view detailed content</li>
        </ul>

        <h4>🔄 Switching Modes</h4>
        <p>Toggle the <strong>Dev/Exp</strong> switch to return to experiment mode. Your development work is preserved.</p>
      </div>
    </a-modal>
  </a-layout>
</template>

<script setup>
import { ref, computed, onMounted, watch } from 'vue'
import { EyeOutlined, EyeInvisibleOutlined, CloseOutlined, QuestionCircleOutlined } from '@ant-design/icons-vue'
import { h } from 'vue'

import ExperimentControl from '../components/ExperimentControl.vue'
import EssTerminal from '../components/EssTerminal.vue'
import StatusBar from '../components/StatusBar.vue'
import StimInfo from '../components/StimInfo.vue'
import Scripts from '../components/Scripts.vue'
import StateSystemDiagram from '../components/StateSystemDiagram.vue'
import EventTracker from '../components/EventTracker.vue'
import EyeTouchVisualizer from '../components/EyeTouchVisualizer.vue'
import SystemStatus from '../components/SystemStatus.vue'
import EssConsole from '../components/EssConsole.vue'
import DevelopmentWorkspace from '../components/DevelopmentWorkspace.vue'

// Import dserv for system health monitoring
import { useDservMonitoring, dserv } from '../services/dserv.js'

// Status message
const statusMessage = ref('Ready')

// Development mode state
const developmentMode = ref(false)
const showDevHelp = ref(false)

// Terminal refs and state
const bottomTerminalRef = ref(null)
const experimentControlRef = ref(null)
const developmentWorkspaceRef = ref(null)
const isTerminalVisible = ref(true)
const terminalHeight = ref(180) // Initial height in px

// System Status Sidebar state
const isStatusSidebarVisible = ref(false) // Start hidden
const statusSidebarWidth = ref(350) // Initial width in px
const minSidebarWidth = 250
const maxSidebarWidth = 600

// System health monitoring - using the same approach as SystemStatus
const systemStatusData = ref(null)
const stimConnected = ref(false) // Track stim connection status

// Update system status periodically
function updateSystemStatus() {
  try {
    const { getSystemStatus } = useDservMonitoring()
    systemStatusData.value = getSystemStatus()
  } catch (error) {
    console.error('MainLayout - failed to get system status:', error)
    systemStatusData.value = null
  }
}

const systemHealthStatus = computed(() => {
  const status = systemStatusData.value

  if (!status?.connection?.connected) {
    return 'error'
  }

  // Check if we have any high-frequency datapoints that are stalled
  const datapoints = status?.datapoints || []
  const hasStalled = datapoints.some(dp => {
    if (dp?.config?.priority === 'high' && dp?.config?.expectedHz > 10) {
      const ratio = dp.currentHz / (dp.config.expectedHz || 1)
      return ratio < 0.3
    }
    return false
  })

  if (hasStalled) {
    return 'warning'
  }

  return 'success'
})

const systemHealthText = computed(() => {
  const status = systemStatusData.value

  if (!status?.connection?.connected) {
    return 'Disconnected'
  }

  const datapoints = status?.datapoints || []
  const hasStalled = datapoints.some(dp => {
    if (dp?.config?.priority === 'high' && dp?.config?.expectedHz > 10) {
      const ratio = dp.currentHz / (dp.config.expectedHz || 1)
      return ratio < 0.3
    }
    return false
  })

  if (hasStalled) {
    return 'Data Stalled'
  }

  return 'Healthy'
})

const connectionUptime = computed(() => {
  const status = systemStatusData.value
  const uptime = status?.connection?.uptime || 0

  if (!uptime) return '0s'

  const seconds = Math.floor(uptime / 1000)
  const minutes = Math.floor(seconds / 60)
  const hours = Math.floor(minutes / 60)

  if (hours > 0) return `${hours}h ${minutes % 60}m`
  if (minutes > 0) return `${minutes}m ${seconds % 60}s`
  return `${seconds}s`
})

const messagesPerSecond = computed(() => {
  const status = systemStatusData.value
  return status?.messages?.messagesPerSecond || 0
})

const stimConnectionColor = computed(() => {
  return stimConnected.value ? 'orange' : 'default'
})

const stimConnectionStatus = computed(() => {
  return stimConnected.value ? 'Connected' : 'Not Connected'
})

// Performance table data
const performanceColumns = [
  { title: 'Condition', dataIndex: 'condition', key: 'condition', width: 100 },
  { title: '% Correct', dataIndex: 'correct', key: 'correct', width: 80 },
  { title: 'RT (ms)', dataIndex: 'rt', key: 'rt', width: 80 },
  { title: 'N', dataIndex: 'n', key: 'n', width: 60 }
]

const performanceData = ref([
  { key: '1', condition: 'Easy', correct: 95, rt: 450, n: 50 },
  { key: '2', condition: 'Hard', correct: 78, rt: 650, n: 50 }
])

// Terminal resizing (existing)
function startResize(event) {
  event.preventDefault();
  window.addEventListener('mousemove', doResize);
  window.addEventListener('mouseup', stopResize);
}

function doResize(event) {
  const newHeight = window.innerHeight - event.clientY - 24; // 24px for status bar
  if (newHeight > 40 && newHeight < window.innerHeight * 0.9) {
    terminalHeight.value = newHeight;
  }
}

function stopResize() {
  window.removeEventListener('mousemove', doResize);
  window.removeEventListener('mouseup', stopResize);
}

// Vertical sidebar resizing (new)
function startVerticalResize(event) {
  event.preventDefault();
  window.addEventListener('mousemove', doVerticalResize);
  window.addEventListener('mouseup', stopVerticalResize);
}

function doVerticalResize(event) {
  const newWidth = window.innerWidth - event.clientX;
  if (newWidth >= minSidebarWidth && newWidth <= maxSidebarWidth) {
    statusSidebarWidth.value = newWidth;
  }
}

function stopVerticalResize() {
  window.removeEventListener('mousemove', doVerticalResize);
  window.removeEventListener('mouseup', stopVerticalResize);
}

// Toggle sidebar
function toggleStatusSidebar() {
  isStatusSidebarVisible.value = !isStatusSidebarVisible.value;
}

// Handle terminal logging from experiment control
function handleStatusUpdate({ message, type }) {
  statusMessage.value = message
  if (bottomTerminalRef.value) {
    bottomTerminalRef.value.addLine(message, type)
  }
}

// Save state to localStorage
watch(statusSidebarWidth, (newWidth) => {
  localStorage.setItem('essgui-sidebar-width', newWidth.toString())
})

watch(isStatusSidebarVisible, (visible) => {
  localStorage.setItem('essgui-sidebar-visible', visible.toString())
})

watch(developmentMode, (newValue) => {
  localStorage.setItem('essgui-development-mode', newValue.toString())

  // When entering dev mode, hide system sidebar to give more space
  if (newValue) {
    isStatusSidebarVisible.value = false
  }
})

onMounted(() => {
  console.log('MainLayout mounted with development mode support')

  // Start updating system status
  updateSystemStatus()
  setInterval(updateSystemStatus, 1000) // Update every second

  // Listen for stim connection status
  dserv.on('datapoint:ess/rmt_connected', (data) => {
    stimConnected.value = data.data === '1' || data.data === 1
  }, 'MainLayout')

  // Restore state from localStorage
  const savedWidth = localStorage.getItem('essgui-sidebar-width')
  if (savedWidth) {
    const width = parseInt(savedWidth, 10)
    if (width >= minSidebarWidth && width <= maxSidebarWidth) {
      statusSidebarWidth.value = width
    }
  }

  const savedVisible = localStorage.getItem('essgui-sidebar-visible')
  if (savedVisible) {
    isStatusSidebarVisible.value = savedVisible === 'true'
  }

  const savedMode = localStorage.getItem('essgui-development-mode')
  if (savedMode) {
    developmentMode.value = savedMode === 'true'
  }
})
</script>

<style scoped>
.resizer {
  height: 5px;
  background: #f0f0f0;
  cursor: ns-resize;
  flex-shrink: 0;
}

.vertical-resizer {
  width: 5px;
  background: #f0f0f0;
  cursor: ew-resize;
  flex-shrink: 0;
  transition: background 0.2s;
}

.vertical-resizer:hover {
  background: #d9d9d9;
}

.terminal-wrapper {
  overflow: hidden;
  background: white;
  border-top: 1px solid #d9d9d9;
}

.dev-help-content {
  line-height: 1.6;
}

.dev-help-content h4 {
  margin-top: 16px;
  margin-bottom: 8px;
  color: #333;
}

.dev-help-content ul {
  margin-bottom: 16px;
}

.dev-help-content li {
  margin-bottom: 4px;
}

.dev-help-content kbd {
  background: #f5f5f5;
  border: 1px solid #ddd;
  border-radius: 3px;
  padding: 2px 4px;
  font-family: monospace;
  font-size: 11px;
}

/* Fix tab content height */
:deep(.ant-tabs) {
  display: flex;
  flex-direction: column;
  height: 100%;
}

:deep(.ant-tabs-nav) {
  margin-left: 14px;
  flex-shrink: 0;
}

:deep(.ant-tabs-content) {
  height: 100%;
  overflow: hidden;
}

:deep(.ant-tabs-content-holder) {
  height: 100%;
}

:deep(.ant-tabs-tabpane) {
  height: 100%;
}

/* Ensure proper cursor during resize */
.vertical-resizer:active,
.resizer:active {
  user-select: none;
}
</style>
