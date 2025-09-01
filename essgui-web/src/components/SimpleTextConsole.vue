<template>
  <div class="text-console">
    <!-- Simple header -->
    <div class="console-header">
      <div class="header-left">
        <span class="console-title">ESS Console</span>

        <!-- Error tracing toggle -->
        <a-switch
          size="small"
          v-model:checked="isTracing"
          :loading="loading"
          :disabled="!connected"
          @change="toggleTracing"
        >
          <template #checkedChildren>ON</template>
          <template #unCheckedChildren>OFF</template>
        </a-switch>
        <span class="tracing-label">{{ isTracing ? 'Tracing' : 'Off' }}</span>

        <!-- Simple counts -->
        <span v-if="logCounts.error > 0" class="error-count">{{ logCounts.error }} errors</span>
        <span v-if="logCounts.warning > 0" class="warning-count">{{ logCounts.warning }} warnings</span>
      </div>

      <div class="header-right">
        <!-- Simple filter -->
        <a-select v-model:value="filterLevel" size="small" style="width: 80px;">
          <a-select-option value="all">All</a-select-option>
          <a-select-option value="error">Errors</a-select-option>
          <a-select-option value="warning">Warnings</a-select-option>
          <a-select-option value="info">Info</a-select-option>
        </a-select>

        <!-- Clear button -->
        <a-button size="small" @click="clearLogs" :disabled="entries.length === 0">
          Clear
        </a-button>
      </div>
    </div>

    <!-- Connection warning -->
    <div v-if="!connected" class="connection-warning">
      Not connected to dserv - logging unavailable
    </div>

    <!-- Console output -->
    <div class="console-output" ref="outputRef" @scroll="handleScroll">
      <div v-if="entries.length === 0" class="empty-message">
        {{ isTracing ? 'No log entries captured yet' : 'Enable error tracing to capture ESS logs' }}
      </div>

      <div
        v-for="entry in visibleEntries"
        :key="entry.id"
        class="log-line"
        :class="`log-${entry.level}`"
        @click="showDetails(entry)"
      >
        {{ formatLogLine(entry) }}
      </div>

      <!-- Load more indicator -->
      <div v-if="hasMore" class="load-more" @click="loadMore">
        Load more entries... ({{ totalEntries - visibleEntries.length }} remaining)
      </div>
    </div>

    <!-- Simple status bar -->
    <div class="console-footer">
      <span>{{ entries.length }} entries</span>
      <span v-if="filterLevel !== 'all'">({{ filterLevel }} filter)</span>
      <span class="connection-status">{{ connected ? (isTracing ? 'Tracing' : 'Connected') : 'Disconnected' }}</span>
    </div>

    <!-- Simple details modal -->
    <a-modal v-model:open="showDetailsModal" title="Log Details" width="600px" :footer="null">
      <div v-if="selectedEntry" class="log-details">
        <div class="detail-row">
          <strong>Time:</strong> {{ selectedEntry.timeString }}
        </div>
        <div class="detail-row">
          <strong>Level:</strong> {{ selectedEntry.level.toUpperCase() }}
        </div>
        <div class="detail-row">
          <strong>Source:</strong> {{ selectedEntry.source }}
        </div>
        <div class="detail-row">
          <strong>Category:</strong> {{ selectedEntry.category }}
        </div>
        <div class="detail-row">
          <strong>Context:</strong> {{ selectedEntry.system }}/{{ selectedEntry.protocol }}/{{ selectedEntry.variant }}
        </div>
        <div class="detail-message">
          <strong>Message:</strong>
          <pre>{{ selectedEntry.text }}</pre>
        </div>
      </div>
    </a-modal>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, nextTick } from 'vue'
import { message } from 'ant-design-vue'
import { dserv } from '../services/dserv.js'
import { errorService } from '../services/errorService.js'

// Component state
const loading = ref(false)
const connected = ref(false)
const filterLevel = ref('all')
const outputRef = ref(null)
const showDetailsModal = ref(false)
const selectedEntry = ref(null)

// Pagination for performance
const pageSize = 100
const visibleCount = ref(pageSize)

// Get data from error service (but limit reactivity)
const allEntries = ref([])
const logCounts = ref({ error: 0, warning: 0, info: 0, debug: 0, total: 0 })
const isTracing = ref(false)

// Update data periodically instead of reactively
let updateInterval = null

const updateData = () => {
  // Get fresh data but don't make it reactive
  const logs = errorService.getLogs()
  const counts = errorService.getLogCounts()
  const tracing = errorService.isTracingActive()

  // Only update if changed to avoid unnecessary re-renders
  if (logs.length !== allEntries.value.length) {
    allEntries.value = logs

    // Auto-scroll to bottom if we were already at bottom
    nextTick(() => {
      if (shouldAutoScroll.value) {
        scrollToBottom()
      }
    })
  }

  logCounts.value = counts
  isTracing.value = tracing
}

// Filtered entries (simple filtering)
const entries = computed(() => {
  if (filterLevel.value === 'all') {
    return allEntries.value
  }
  return allEntries.value.filter(entry => entry.level === filterLevel.value)
})

// Visible entries with pagination
const visibleEntries = computed(() => {
  return entries.value.slice(-visibleCount.value)
})

const totalEntries = computed(() => entries.value.length)
const hasMore = computed(() => visibleCount.value < totalEntries.value)

// Auto-scroll detection
const shouldAutoScroll = ref(true)

const handleScroll = () => {
  if (!outputRef.value) return

  const { scrollTop, scrollHeight, clientHeight } = outputRef.value
  const isAtBottom = scrollHeight - scrollTop <= clientHeight + 10

  shouldAutoScroll.value = isAtBottom
}

const scrollToBottom = () => {
  if (outputRef.value) {
    outputRef.value.scrollTop = outputRef.value.scrollHeight
  }
}

const loadMore = () => {
  visibleCount.value = Math.min(visibleCount.value + pageSize, totalEntries.value)
}

// Format log line for display
const formatLogLine = (entry) => {
  const time = entry.timeString || new Date(entry.timestamp).toLocaleTimeString()
  const level = entry.level.toUpperCase().padEnd(5)
  const source = entry.source.padEnd(12)
  const category = entry.category.padEnd(10)

  // Get first line of message
  const message = entry.text.split('\n')[0]

  return `${time} [${level}] ${source} ${category} ${message}`
}

// Show details modal
const showDetails = (entry) => {
  selectedEntry.value = entry
  showDetailsModal.value = true
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

const clearLogs = () => {
  errorService.clearLogs()
  message.success('Logs cleared')
  visibleCount.value = pageSize
}

// Component lifecycle
onMounted(() => {
  console.log('Simple Text Console mounted')

  // Register with dserv
  const cleanup = dserv.registerComponent('SimpleTextConsole')

  // Listen for connection changes
  dserv.on('connection', ({ connected: isConnected }) => {
    connected.value = isConnected
  }, 'SimpleTextConsole')

  // Set initial connection state
  connected.value = dserv.state.connected

  // Update data every 500ms instead of reactive updates
  updateData()
  updateInterval = setInterval(updateData, 500)

  onUnmounted(() => {
    cleanup()
    if (updateInterval) {
      clearInterval(updateInterval)
    }
  })
})
</script>

<style scoped>
.text-console {
  height: 100%;
  display: flex;
  flex-direction: column;
  font-family: 'Monaco', 'Menlo', 'Consolas', monospace;
  font-size: 12px;
  background: #1e1e1e;
  color: #d4d4d4;
}

.console-header {
  flex-shrink: 0;
  padding: 8px 12px;
  background: #2d2d30;
  border-bottom: 1px solid #3c3c3c;
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.header-left {
  display: flex;
  align-items: center;
  gap: 12px;
}

.console-title {
  font-weight: 500;
  color: #ffffff;
}

.tracing-label {
  font-size: 11px;
  color: #cccccc;
}

.error-count {
  color: #f48771;
  font-size: 11px;
}

.warning-count {
  color: #dcdcaa;
  font-size: 11px;
}

.header-right {
  display: flex;
  align-items: center;
  gap: 8px;
}

.connection-warning {
  flex-shrink: 0;
  padding: 6px 12px;
  background: #5a1e00;
  color: #ff8c67;
  font-size: 11px;
  border-bottom: 1px solid #8b2d00;
}

.console-output {
  flex: 1;
  overflow-y: auto;
  padding: 8px;
  line-height: 1.4;
  scroll-behavior: smooth;
}

.empty-message {
  color: #6a6a6a;
  font-style: italic;
  text-align: center;
  padding: 20px;
}

.log-line {
  margin-bottom: 1px;
  padding: 2px 4px;
  white-space: pre;
  cursor: pointer;
  border-radius: 2px;
}

.log-line:hover {
  background: #264f78;
}

.log-error {
  color: #f48771;
  background: rgba(244, 135, 113, 0.1);
}

.log-error:hover {
  background: rgba(244, 135, 113, 0.2);
}

.log-warning {
  color: #dcdcaa;
  background: rgba(220, 220, 170, 0.1);
}

.log-warning:hover {
  background: rgba(220, 220, 170, 0.2);
}

.log-info {
  color: #9cdcfe;
  background: rgba(156, 220, 254, 0.1);
}

.log-info:hover {
  background: rgba(156, 220, 254, 0.2);
}

.log-debug {
  color: #c586c0;
  background: rgba(197, 134, 192, 0.1);
}

.log-debug:hover {
  background: rgba(197, 134, 192, 0.2);
}

.load-more {
  text-align: center;
  padding: 8px;
  color: #569cd6;
  cursor: pointer;
  border: 1px dashed #3c3c3c;
  margin: 4px 0;
  border-radius: 2px;
}

.load-more:hover {
  background: rgba(86, 156, 214, 0.1);
}

.console-footer {
  flex-shrink: 0;
  padding: 4px 12px;
  background: #2d2d30;
  border-top: 1px solid #3c3c3c;
  font-size: 10px;
  color: #cccccc;
  display: flex;
  justify-content: space-between;
}

.connection-status {
  color: #4ec9b0;
}

.log-details {
  font-family: monospace;
  font-size: 12px;
}

.detail-row {
  margin-bottom: 8px;
  display: flex;
  gap: 8px;
}

.detail-row strong {
  min-width: 80px;
  color: #569cd6;
}

.detail-message {
  margin-top: 12px;
}

.detail-message strong {
  display: block;
  margin-bottom: 8px;
  color: #569cd6;
}

.detail-message pre {
  background: #f5f5f5;
  padding: 12px;
  border-radius: 4px;
  max-height: 200px;
  overflow: auto;
  white-space: pre-wrap;
  font-size: 11px;
  color: #333;
}

/* Override Ant Design styles for dark theme */
:deep(.ant-select-small .ant-select-selector) {
  background: #3c3c3c;
  border-color: #5a5a5a;
  color: #d4d4d4;
}

:deep(.ant-select-small .ant-select-arrow) {
  color: #d4d4d4;
}

:deep(.ant-btn-sm) {
  background: #3c3c3c;
  border-color: #5a5a5a;
  color: #d4d4d4;
}

:deep(.ant-btn-sm:hover) {
  background: #4a4a4a;
  border-color: #6a6a6a;
}

:deep(.ant-switch-small) {
  background: #5a5a5a;
}

:deep(.ant-switch-small.ant-switch-checked) {
  background: #1890ff;
}
</style>
