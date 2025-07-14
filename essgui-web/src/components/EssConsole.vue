<template>
  <div class="console-container">
    <!-- Header Section -->
    <div class="console-header">
      <!-- Row 1: Title, Mode, and Primary Controls -->
      <div class="header-row-1">
        <div class="left-controls">
          <!-- Title -->
          <div class="title-section">
            <CodeOutlined style="font-size: 16px; color: #1890ff;" />
            <span style="font-weight: 500;">ESS Console</span>
          </div>

          <!-- Mode Toggle -->
          <a-segmented v-model:value="viewMode" size="small" :options="[
            { label: 'Errors', value: 'errors' },
            { label: 'All', value: 'all' },
            { label: 'Recent', value: 'recent' }
          ]" />

          <!-- Error tracing toggle -->
          <div class="tracing-toggle">
            <a-switch size="small" v-model:checked="isTracing" :loading="loading" :disabled="!connected"
              @change="toggleTracing">
              <template #checkedChildren>
                <EyeOutlined />
              </template>
              <template #unCheckedChildren>
                <EyeInvisibleOutlined />
              </template>
            </a-switch>
            <span class="tracing-text">
              {{ isTracing ? 'Tracing' : 'Off' }}
            </span>
          </div>

          <!-- Log counts -->
          <div v-if="logCounts.total > 0" class="log-counts">
            <a-badge :count="logCounts.error"
              :number-style="{ backgroundColor: '#ff4d4f', fontSize: '9px', height: '16px', minWidth: '16px', lineHeight: '16px' }" />
            <a-badge :count="logCounts.warning"
              :number-style="{ backgroundColor: '#faad14', fontSize: '9px', height: '16px', minWidth: '16px', lineHeight: '16px' }" />
            <a-badge :count="logCounts.info"
              :number-style="{ backgroundColor: '#1890ff', fontSize: '9px', height: '16px', minWidth: '16px', lineHeight: '16px' }" />
            <a-badge :count="logCounts.debug"
              :number-style="{ backgroundColor: '#722ed1', fontSize: '9px', height: '16px', minWidth: '16px', lineHeight: '16px' }" />
            <span class="total-count">{{ logCounts.total }}</span>
          </div>
        </div>

        <!-- System controls on right -->
        <div class="right-controls">
          <!-- Debug mode toggle -->
          <a-tooltip title="Enable debug logging in ESS backend">
            <a-switch size="small" v-model:checked="debugMode" @change="toggleDebugMode" :disabled="!connected">
              <template #checkedChildren>Debug</template>
              <template #unCheckedChildren>No Debug</template>
            </a-switch>
          </a-tooltip>

          <!-- Send test message -->
          <a-dropdown :trigger="['click']">
            <a-button size="small" :disabled="!connected">
              <SendOutlined /> Send
            </a-button>
            <template #overlay>
              <a-menu @click="handleSendMessage">
                <a-menu-item key="info">Send Info</a-menu-item>
                <a-menu-item key="warning">Send Warning</a-menu-item>
                <a-menu-item key="error">Send Error</a-menu-item>
                <a-menu-item key="debug">Send Debug</a-menu-item>
              </a-menu>
            </template>
          </a-dropdown>
        </div>
      </div>

      <!-- Row 2: Filtering and Action Controls -->
      <div class="header-row-2">
        <div class="filter-controls">
          <!-- Auto-scroll toggle -->
          <a-tooltip title="Auto-scroll to newest entries">
            <a-switch size="small" v-model:checked="autoScroll">
              <template #checkedChildren>Auto</template>
              <template #unCheckedChildren>Manual</template>
            </a-switch>
          </a-tooltip>

          <!-- Filter controls -->
          <a-select size="small" v-model:value="filterLevel" style="width: 90px;" placeholder="Filter">
            <a-select-option value="all">All</a-select-option>
            <a-select-option value="error">Errors</a-select-option>
            <a-select-option value="warning">Warnings</a-select-option>
            <a-select-option value="info">Info</a-select-option>
            <a-select-option value="debug">Debug</a-select-option>
            <a-select-option value="general">General</a-select-option>
          </a-select>

          <!-- Search -->
          <a-input size="small" placeholder="Search logs..." v-model:value="searchText" style="width: 140px;"
            allow-clear>
            <template #prefix>
              <SearchOutlined />
            </template>
          </a-input>
        </div>

        <!-- Action buttons -->
        <a-space size="small">
          <a-tooltip title="Clear all logs">
            <a-button size="small" :icon="h(ClearOutlined)" @click="clearLogs"
              :disabled="displayedEntries.length === 0" />
          </a-tooltip>

          <a-tooltip title="Export logs to CSV">
            <a-button size="small" :icon="h(DownloadOutlined)" @click="exportLogs"
              :disabled="displayedEntries.length === 0" />
          </a-tooltip>
        </a-space>
      </div>
    </div>

    <!-- Connection warning -->
    <div v-if="!connected" class="connection-warning">
      <WarningOutlined style="margin-right: 6px;" />
      Not connected to dserv - logging unavailable
    </div>

    <!-- Main table section -->
    <div class="table-section">
      <!-- Empty state -->
      <div v-if="displayedEntries.length === 0" class="empty-state">
        <template v-if="allEntries.length === 0">
          <CodeOutlined class="empty-icon" />
          <div class="empty-text">No log entries captured yet</div>
          <div class="empty-subtext">
            {{ isTracing ? 'Error tracing is active globally' : 'Enable error tracing to capture ESS logs' }}
          </div>
        </template>
        <template v-else>
          <FilterOutlined class="empty-icon" />
          <div class="empty-text">No entries match the current filter</div>
        </template>
      </div>

      <!-- Table container -->
      <div v-else class="table-container">
        <a-table ref="logTable" :columns="columns" :data-source="displayedEntries" size="small" row-key="id"
          :pagination="false" :scroll="{ y: true, x: 800 }" :row-class-name="getRowClassName"
          :expand-row-by-click="true" :expanded-row-keys="expandedRows" @expand="onRowExpand" class="console-table">
          <template #bodyCell="{ column, record }">
            <template v-if="column.key === 'time'">
              <span class="time-cell">{{ record.timeString }}</span>
            </template>

            <template v-else-if="column.key === 'level'">
              <a-tag :color="getLevelInfo(record.level).color" class="level-tag">
                <template #icon>
                  <component :is="getLevelInfo(record.level).icon" style="font-size: 10px;" />
                </template>
                {{ record.level.toUpperCase() }}
              </a-tag>
            </template>

            <template v-else-if="column.key === 'category'">
              <a-tag color="default" class="category-tag">
                {{ record.category }}
              </a-tag>
            </template>

            <template v-else-if="column.key === 'source'">
              <a-tag :color="getSourceColor(record.source)" class="source-tag">
                {{ record.source }}
              </a-tag>
            </template>

            <template v-else-if="column.key === 'context'">
              <div class="context-cell">
                <a-tooltip :title="`System: ${record.system}`">
                  <span class="context-system">{{ record.system }}</span>
                </a-tooltip>
                <span class="context-separator">/</span>
                <a-tooltip :title="`Protocol: ${record.protocol}`">
                  <span class="context-protocol">{{ record.protocol }}</span>
                </a-tooltip>
                <span class="context-separator">/</span>
                <a-tooltip :title="`Variant: ${record.variant}`">
                  <span class="context-variant">{{ record.variant }}</span>
                </a-tooltip>
              </div>
            </template>

            <template v-else-if="column.key === 'text'">
              <div class="text-cell">
                <div class="text-content" @click="showLogDetails(record)">
                  {{ getFirstLine(record.text) }}
                </div>

                <!-- Show expand/collapse button if multi-line -->
                <a-button v-if="isMultiLine(record.text)" size="small" type="text"
                  :icon="h(expandedRows.includes(record.id) ? UpOutlined : DownOutlined)"
                  @click.stop="toggleRowExpansion(record.id)" class="expand-button" />

                <!-- Detail button -->
                <a-tooltip title="View full details">
                  <a-button size="small" type="text" :icon="h(EllipsisOutlined)" @click.stop="showLogDetails(record)"
                    class="detail-button" />
                </a-tooltip>
              </div>
            </template>
          </template>

          <!-- Expandable row content for multi-line messages -->
          <template #expandedRowRender="{ record }">
            <div class="expanded-content">
              <div class="expanded-header">Full Message:</div>
              <div class="expanded-message">{{ record.text }}</div>

              <div v-if="record.stackTrace && record.stackTrace.length > 0" class="expanded-stack">
                <div class="expanded-header">Stack Trace:</div>
                <div class="stack-trace">
                  <div v-for="(trace, index) in record.stackTrace" :key="index" class="stack-line"
                    :class="{ 'is-file': trace.isFile }">
                    {{ trace.line }}
                  </div>
                </div>
              </div>
            </div>
          </template>
        </a-table>
      </div>
    </div>

    <!-- Footer section -->
    <div class="console-footer">
      <!-- Left side - entry counts -->
      <div class="footer-left">
        <span v-if="displayedEntries.length > 0" class="entry-info">
          Showing {{ displayedEntries.length }}{{ allEntries.length !== displayedEntries.length ? ` of
          ${allEntries.length}`
          : '' }} entries
          <span v-if="searchText" class="filter-indicator">(filtered)</span>
          <span v-if="filterLevel !== 'all'" class="filter-indicator">({{ filterLevel }})</span>
        </span>
      </div>

      <!-- Right side - status info -->
      <div class="footer-right">
        <span class="status-item">
          {{ viewMode === 'errors' ? 'Errors Only' : viewMode === 'recent' ? 'Recent' : 'All Logs' }}
        </span>
        <span class="status-item">
          {{ connected ? (isTracing ? 'Tracing' : 'Connected') : 'Disconnected' }}
        </span>
        <span v-if="allEntries.length > 0" class="status-item">
          {{ new Date(allEntries[allEntries.length - 1].timestamp).toLocaleTimeString() }}
        </span>
      </div>
    </div>

    <!-- Log details modal -->
    <a-modal v-model:open="showDetails" title="Log Entry Details" width="800px" :footer="null">
      <template #title>
        <div style="display: flex; align-items: center; gap: 8px;">
          <component v-if="selectedEntry" :is="getLevelInfo(selectedEntry.level).icon" />
          <span>Log Entry Details</span>
        </div>
      </template>

      <div v-if="selectedEntry">
        <a-card size="small" style="margin-bottom: 16px;">
          <div style="display: grid; grid-template-columns: auto 1fr; gap: 8px 16px; font-size: 12px;">
            <strong>Time:</strong><span>{{ selectedEntry.timeString }}</span>
            <strong>Level:</strong><span>{{ selectedEntry.level.toUpperCase() }}</span>
            <strong>Category:</strong><span>{{ selectedEntry.category }}</span>
            <strong>Source:</strong><span>{{ selectedEntry.source }}</span>
            <strong>System:</strong><span>{{ selectedEntry.system }}</span>
            <strong>Protocol:</strong><span>{{ selectedEntry.protocol }}</span>
            <strong>Variant:</strong><span>{{ selectedEntry.variant }}</span>
          </div>
        </a-card>

        <div style="margin-bottom: 16px;">
          <div style="font-weight: 500; margin-bottom: 8px;">Message:</div>
          <div
            style="background: #f5f5f5; padding: 12px; font-size: 12px; font-family: monospace; white-space: pre-wrap; max-height: 200px; overflow: auto; border-radius: 4px;">
            {{ selectedEntry.text }}
          </div>
        </div>

        <div v-if="selectedEntry.stackTrace && selectedEntry.stackTrace.length > 0">
          <div style="font-weight: 500; margin-bottom: 8px;">Stack Trace:</div>
          <div
            style="background: #f5f5f5; padding: 12px; font-size: 11px; font-family: monospace; max-height: 150px; overflow: auto; border-radius: 4px;">
            <div v-for="(trace, index) in selectedEntry.stackTrace" :key="index" style="margin-bottom: 4px;"
              :style="{ color: trace.isFile ? '#1890ff' : '#666' }">
              {{ trace.line }}
            </div>
          </div>
        </div>
      </div>

      <template #footer>
        <a-button @click="showDetails = false">Close</a-button>
      </template>
    </a-modal>

    <!-- Send message modal -->
    <a-modal v-model:open="showSendModal" title="Send Log Message" @ok="sendMessage" @cancel="showSendModal = false">
      <a-form layout="vertical">
        <a-form-item label="Level">
          <a-select v-model:value="sendForm.level">
            <a-select-option value="info">Info</a-select-option>
            <a-select-option value="warning">Warning</a-select-option>
            <a-select-option value="error">Error</a-select-option>
            <a-select-option value="debug">Debug</a-select-option>
          </a-select>
        </a-form-item>
        <a-form-item label="Category">
          <a-input v-model:value="sendForm.category" placeholder="e.g., frontend, user, test" />
        </a-form-item>
        <a-form-item label="Message">
          <a-textarea v-model:value="sendForm.message" :rows="3" placeholder="Enter log message..." />
        </a-form-item>
      </a-form>
    </a-modal>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, nextTick, h, watch } from 'vue'
import {
  ClearOutlined,
  DownloadOutlined,
  CodeOutlined,
  WarningOutlined,
  CloseCircleOutlined,
  InfoCircleOutlined,
  ExclamationCircleOutlined,
  EyeOutlined,
  EyeInvisibleOutlined,
  SearchOutlined,
  FilterOutlined,
  UpOutlined,
  DownOutlined,
  EllipsisOutlined,
  SendOutlined,
  UserOutlined
} from '@ant-design/icons-vue'
import { message } from 'ant-design-vue'
import { dserv } from '../services/dserv.js'
import { errorService } from '../services/errorService.js'

// Component state
const loading = ref(false)
const connected = ref(false)
const autoScroll = ref(true)
const filterLevel = ref('all')
const searchText = ref('')
const selectedEntry = ref(null)
const showDetails = ref(false)
const logTable = ref(null)
const expandedRows = ref([])
const viewMode = ref('all') // 'errors', 'all', 'recent'
const debugMode = ref(false)

// Send message modal
const showSendModal = ref(false)
const sendForm = ref({
  level: 'info',
  category: 'frontend',
  message: ''
})

// Reactive references to global service data
const allLogs = computed(() => errorService.getLogs())
const allErrors = computed(() => errorService.getErrors())
const logCounts = computed(() => errorService.getLogCounts())
const isTracing = computed(() => errorService.isTracingActive())

// Determine which entries to show based on view mode
const allEntries = computed(() => {
  switch (viewMode.value) {
    case 'errors':
      return allErrors.value
    case 'recent':
      return errorService.getRecentLogs(100)
    case 'all':
    default:
      return allLogs.value
  }
})

// Helper functions for text handling
const getFirstLine = (text) => {
  return text.split('\n')[0]
}

const isMultiLine = (text) => {
  return text.includes('\n')
}

// Row expansion handling
const toggleRowExpansion = (rowId) => {
  const index = expandedRows.value.indexOf(rowId)
  if (index === -1) {
    expandedRows.value.push(rowId)
  } else {
    expandedRows.value.splice(index, 1)
  }
}

const onRowExpand = (expanded, record) => {
  if (expanded) {
    if (!expandedRows.value.includes(record.id)) {
      expandedRows.value.push(record.id)
    }
  } else {
    const index = expandedRows.value.indexOf(record.id)
    if (index !== -1) {
      expandedRows.value.splice(index, 1)
    }
  }
}

// Service methods
const toggleTracing = async () => {
  if (loading.value) return

  loading.value = true
  try {
    await errorService.toggleTracing()
    message.success(`Error tracing ${isTracing.value ? 'enabled' : 'disabled'}`)
  } catch (error) {
    console.error('Failed to toggle error tracing:', error)
    message.error('Failed to toggle error tracing')
  } finally {
    loading.value = false
  }
}

const toggleDebugMode = async () => {
  try {
    await errorService.setDebugMode(debugMode.value)
    message.success(`Debug mode ${debugMode.value ? 'enabled' : 'disabled'}`)
  } catch (error) {
    console.error('Failed to toggle debug mode:', error)
    message.error('Failed to toggle debug mode')
    debugMode.value = !debugMode.value // Revert on error
  }
}

const clearLogs = () => {
  if (viewMode.value === 'errors') {
    errorService.clearErrors()
    message.success('Errors cleared')
  } else {
    errorService.clearLogs()
    message.success('All logs cleared')
  }
  expandedRows.value = []
}

const exportLogs = () => {
  if (viewMode.value === 'errors') {
    errorService.exportErrors()
    message.success('Errors exported')
  } else {
    errorService.exportLogs()
    message.success('Logs exported')
  }
}

// Send message handling
const handleSendMessage = ({ key }) => {
  sendForm.value.level = key
  showSendModal.value = true
}

const sendMessage = async () => {
  if (!sendForm.value.message.trim()) {
    message.error('Please enter a message')
    return
  }

  try {
    const success = await errorService.sendLogMessage(
      sendForm.value.level,
      sendForm.value.message,
      sendForm.value.category
    )

    if (success) {
      message.success('Message sent to ESS')
      showSendModal.value = false
      sendForm.value.message = ''
    } else {
      message.error('Failed to send message')
    }
  } catch (error) {
    console.error('Failed to send message:', error)
    message.error('Failed to send message')
  }
}

// Filter logs
const filteredEntries = computed(() => {
  let filtered = allEntries.value

  // Filter by level
  if (filterLevel.value !== 'all') {
    filtered = filtered.filter(entry => entry.level === filterLevel.value)
  }

  // Filter by search text
  if (searchText.value) {
    const search = searchText.value.toLowerCase()
    filtered = filtered.filter(entry =>
      entry.text.toLowerCase().includes(search) ||
      entry.category.toLowerCase().includes(search) ||
      entry.system.toLowerCase().includes(search) ||
      entry.protocol.toLowerCase().includes(search) ||
      entry.variant.toLowerCase().includes(search)
    )
  }

  return filtered
})

// Final displayed entries (for naming consistency)
const displayedEntries = computed(() => filteredEntries.value)

// Auto-scroll function using the unified approach
const scrollToBottom = () => {
  if (!autoScroll.value) return

  nextTick(() => {
    const table = logTable.value
    if (table?.querySelector) {
      const scrollContainer = table.querySelector('.ant-table-body')
      if (scrollContainer) {
        requestAnimationFrame(() => {
          scrollContainer.scrollTop = scrollContainer.scrollHeight
        })
      }
    }
  })
}

// Watch for new entries to auto-scroll
watch(allEntries, () => {
  if (autoScroll.value) {
    scrollToBottom()
  }
}, { deep: true })

// Get level icon and color
const getLevelInfo = (level) => {
  switch (level) {
    case 'error':
      return { icon: h(CloseCircleOutlined), color: '#ff4d4f' }
    case 'warning':
      return { icon: h(ExclamationCircleOutlined), color: '#faad14' }
    case 'info':
      return { icon: h(InfoCircleOutlined), color: '#1890ff' }
    case 'debug':
      return { icon: h(CodeOutlined), color: '#722ed1' }
    case 'general':
      return { icon: h(CodeOutlined), color: '#8c8c8c' }
    default:
      return { icon: h(CodeOutlined), color: '#8c8c8c' }
  }
}

// Get source color
const getSourceColor = (source) => {
  switch (source) {
    case 'ess_backend':
      return 'blue'
    case 'tcl_error':
      return 'red'
    case 'frontend':
      return 'green'
    default:
      return 'default'
  }
}

// Show log details modal
const showLogDetails = (entry) => {
  selectedEntry.value = entry
  showDetails.value = true
}

// Get row class name for styling
const getRowClassName = (record) => {
  switch (record.level) {
    case 'error': return 'error-row'
    case 'warning': return 'warning-row'
    case 'info': return 'info-row'
    case 'debug': return 'debug-row'
    default: return ''
  }
}

// Table columns - optimized for the new logging system
const columns = [
  {
    title: 'Time',
    dataIndex: 'timeString',
    key: 'time',
    width: 90,
    fixed: 'left'
  },
  {
    title: 'Level',
    dataIndex: 'level',
    key: 'level',
    width: 65
  },
  {
    title: 'Category',
    dataIndex: 'category',
    key: 'category',
    width: 70
  },
  {
    title: 'Source',
    dataIndex: 'source',
    key: 'source',
    width: 80
  },
  {
    title: 'Context',
    key: 'context',
    width: 180
  },
  {
    title: 'Message',
    dataIndex: 'text',
    key: 'text',
    ellipsis: true
  }
]

// Component lifecycle
onMounted(() => {
  console.log('ESS Console mounted - using global error/log service')

  // Listen for connection changes
  const cleanup = dserv.registerComponent('EssConsole')

  dserv.on('connection', ({ connected: isConnected }) => {
    connected.value = isConnected
  }, 'EssConsole')

  // Set initial connection state
  connected.value = dserv.state.connected

  onUnmounted(() => {
    cleanup()
  })
})
</script>

<style scoped>
/* UNIFIED CONSOLE LAYOUT */
.console-container {
  height: 100%;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}

.console-header {
  flex-shrink: 0;
  background: #fafafa;
  border-bottom: 1px solid #e8e8e8;
}

.header-row-1 {
  padding: 6px 12px;
  display: flex;
  justify-content: space-between;
  align-items: center;
  border-bottom: 1px solid #f0f0f0;
}

.left-controls {
  display: flex;
  align-items: center;
  gap: 16px;
}

.title-section {
  display: flex;
  align-items: center;
  gap: 8px;
}

.tracing-toggle {
  display: flex;
  align-items: center;
  gap: 6px;
}

.tracing-text {
  font-size: 11px;
}

.log-counts {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 10px;
}

.total-count {
  color: #666;
  font-size: 10px;
}

.right-controls {
  display: flex;
  align-items: center;
  gap: 8px;
}

.header-row-2 {
  padding: 4px 12px;
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.filter-controls {
  display: flex;
  align-items: center;
  gap: 12px;
}

.connection-warning {
  flex-shrink: 0;
  padding: 8px 12px;
  background: #fff2e6;
  border-bottom: 1px solid #ffd591;
  font-size: 12px;
  color: #d4680f;
}

.table-section {
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
  overflow: hidden;
  position: relative;
}

.empty-state {
  height: 100%;
  display: flex;
  align-items: center;
  justify-content: center;
  flex-direction: column;
  color: #999;
  font-size: 14px;
}

.empty-icon {
  font-size: 48px;
  margin-bottom: 16px;
  color: #d9d9d9;
}

.empty-text {
  margin-bottom: 8px;
}

.empty-subtext {
  font-size: 12px;
}

.table-container {
  flex: 1;
  min-height: 0;
  overflow: hidden;
}

.console-footer {
  flex-shrink: 0;
  padding: 4px 12px;
  border-top: 1px solid #e8e8e8;
  background: #fafafa;
  font-size: 11px;
  color: #666;
  display: flex;
  justify-content: space-between;
  align-items: center;
  white-space: nowrap;
  overflow: hidden;
}

.footer-left {
  flex: 1;
  min-width: 0;
  margin-right: 12px;
}

.entry-info {
  overflow: hidden;
  text-overflow: ellipsis;
}

.filter-indicator {
  margin-left: 4px;
}

.footer-right {
  display: flex;
  align-items: center;
  gap: 12px;
  flex-shrink: 0;
}

.status-item {
  white-space: nowrap;
}

/* TABLE SPECIFIC STYLES */
.console-table {
  height: 100%;
}

/* Apply unified table layout */
:deep(.console-table .ant-table) {
  height: 100%;
  display: flex;
  flex-direction: column;
}

:deep(.console-table .ant-table-container) {
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
}

:deep(.console-table .ant-table-header) {
  flex-shrink: 0;
}

:deep(.console-table .ant-table-body) {
  flex: 1;
  overflow-y: auto !important;
  overflow-x: auto !important;
  min-height: 0;
}

/* Cell styling */
.time-cell {
  font-size: 10px;
  font-family: monospace;
  color: #666;
}

.level-tag {
  font-size: 9px;
  padding: 1px 4px;
  line-height: 1.2;
}

.category-tag {
  font-size: 9px;
  padding: 1px 4px;
  line-height: 1.2;
}

.source-tag {
  font-size: 9px;
  padding: 1px 4px;
  line-height: 1.2;
}

.context-cell {
  font-size: 10px;
  font-family: monospace;
  display: flex;
  gap: 3px;
  align-items: center;
}

.context-system {
  color: #1890ff;
  cursor: help;
  font-weight: 500;
}

.context-protocol {
  color: #52c41a;
  cursor: help;
  font-weight: 500;
}

.context-variant {
  color: #fa8c16;
  cursor: help;
  font-weight: 500;
}

.context-separator {
  color: #ccc;
  font-size: 8px;
}

.text-cell {
  display: flex;
  align-items: center;
  gap: 6px;
  margin-left: 16px;
}

.text-content {
  font-family: monospace;
  font-size: 11px;
  max-width: 320px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  flex: 1;
  cursor: pointer;
}

.expand-button {
  width: 18px;
  height: 14px;
  padding: 0;
  font-size: 9px;
}

.detail-button {
  width: 18px;
  height: 14px;
  padding: 0;
  font-size: 9px;
}

/* Expanded row content */
.expanded-content {
  padding: 8px 16px;
  background: #f8f9fa;
  border-radius: 4px;
  margin: 4px 0;
}

.expanded-header {
  font-weight: 500;
  margin-bottom: 8px;
  font-size: 11px;
  color: #666;
}

.expanded-message {
  font-family: monospace;
  font-size: 11px;
  white-space: pre-wrap;
  max-height: 200px;
  overflow: auto;
  background: white;
  padding: 8px;
  border-radius: 2px;
  border: 1px solid #e8e8e8;
}

.expanded-stack {
  margin-top: 12px;
}

.stack-trace {
  font-family: monospace;
  font-size: 10px;
  background: white;
  padding: 8px;
  border-radius: 2px;
  border: 1px solid #e8e8e8;
  max-height: 120px;
  overflow: auto;
}

.stack-line {
  margin-bottom: 2px;
  color: #666;
}

.stack-line.is-file {
  color: #1890ff;
}

/* Row styling */
:deep(.error-row) {
  background-color: #fff2f0 !important;
}

:deep(.error-row:hover) {
  background-color: #ffebe6 !important;
}

:deep(.warning-row) {
  background-color: #fffbe6 !important;
}

:deep(.warning-row:hover) {
  background-color: #fff7db !important;
}

:deep(.info-row) {
  background-color: #e6f7ff !important;
}

:deep(.info-row:hover) {
  background-color: #d1f0ff !important;
}

:deep(.debug-row) {
  background-color: #f9f0ff !important;
}

:deep(.debug-row:hover) {
  background-color: #efdbff !important;
}

/* Table row and cell styling */
:deep(.console-table .ant-table-small .ant-table-tbody > tr > td) {
  padding: 3px 6px;
  font-size: 10px;
  vertical-align: top;
  line-height: 1.3;
}

:deep(.console-table .ant-table-small .ant-table-thead > tr > th) {
  padding: 3px 6px;
  font-size: 10px;
  font-weight: 500;
  background: #f5f5f5;
  line-height: 1.3;
}

/* Expanded row styling */
:deep(.console-table .ant-table-expanded-row > td) {
  padding: 0 !important;
}

/* Segmented control styling */
:deep(.ant-segmented) {
  background: #f5f5f5;
}

:deep(.ant-segmented-item) {
  font-size: 11px;
  padding: 2px 8px;
}

/* Table wrapper styling for proper height */
:deep(.console-table .ant-table-wrapper) {
  height: 100% !important;
  display: flex;
  flex-direction: column;
}

:deep(.console-table .ant-spin-nested-loading) {
  height: 100% !important;
}

:deep(.console-table .ant-spin-container) {
  height: 100% !important;
  display: flex;
  flex-direction: column;
}
</style>
