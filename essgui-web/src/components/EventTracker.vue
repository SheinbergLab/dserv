<template>
  <div class="event-tracker">
    <!-- Header with controls -->
    <div class="event-header">
      <a-row :gutter="12" align="middle">
        <a-col :span="4">
          <a-statistic title="Current Obs" :value="obsInfo.obsCount >= 0 ? obsInfo.obsCount : 'None'" />
        </a-col>
        <a-col :span="4">
          <a-statistic title="Events" :value="currentObsEventCount" />
        </a-col>
        <a-col :span="4">
          <a-statistic title="Total Obs" :value="totalObservations" />
        </a-col>
        <a-col :span="6">
          <!-- Observation navigation -->
          <div style="display: flex; align-items: center; gap: 8px;">
            <span style="font-size: 12px;">View Obs:</span>
            <a-button-group size="small">
              <a-button 
                @click="previousObservation" 
                :disabled="!canNavigateBack"
                :icon="h(LeftOutlined)"
              />
              <a-button 
                @click="nextObservation" 
                :disabled="!canNavigateForward"
                :icon="h(RightOutlined)"
              />
            </a-button-group>
            <span style="font-size: 12px;">{{ viewingObsText }}</span>
          </div>
        </a-col>
        <a-col :span="6" style="text-align: right;">
          <a-space>
            <a-button size="small" @click="clearEvents" :icon="h(ClearOutlined)">
              Clear
            </a-button>
            <a-button size="small" @click="exportEvents" :icon="h(DownloadOutlined)">
              Export
            </a-button>
            <a-button 
              size="small" 
              @click="autoScroll = !autoScroll"
              :type="autoScroll ? 'primary' : 'default'"
              :icon="h(autoScroll ? PauseOutlined : CaretRightOutlined)"
            >
              {{ autoScroll ? 'Pause' : 'Scroll' }}
            </a-button>
          </a-space>
        </a-col>
      </a-row>
    </div>

    <!-- Event table -->
    <div class="table-container">
      <a-table
        ref="eventTable"
        :columns="columns"
        :data-source="displayedEvents"
        :pagination="false"
        :scroll="{ y: 'calc(100vh - 300px)', x: 800 }"
        size="small"
        :row-class-name="getRowClassName"
        :loading="loading"
      >
        <template #bodyCell="{ column, record }">
          <template v-if="column.key === 'timestamp'">
            <span class="timestamp-cell">{{ formatTimestamp(record.timestamp) }}</span>
          </template>
          <template v-else-if="column.key === 'elapsedTime'">
            <span class="elapsed-time-cell">{{ record.elapsedTime }}</span>
          </template>
          <template v-else-if="column.key === 'type'">
            <a-tag :color="getTypeColor(record.type)">
              {{ record.type }}
            </a-tag>
          </template>
          <template v-else-if="column.key === 'typeName'">
            <span class="type-name">{{ getEventTypeName(record.type) }}</span>
          </template>
          <template v-else-if="column.key === 'subtype'">
            <a-tag size="small" :color="getSubtypeColor(record.type, record.subtype)">
              {{ record.subtype }}
            </a-tag>
          </template>
          <template v-else-if="column.key === 'subtypeName'">
            <span class="subtype-name">{{ getEventSubtypeName(record.type, record.subtype) }}</span>
          </template>
          <template v-else-if="column.key === 'params'">
            <span class="params-cell" :title="record.decodedParams">
              {{ record.decodedParams }}
            </span>
          </template>
        </template>
      </a-table>
    </div>

    <!-- Status bar -->
    <div class="status-bar">
      <a-row align="middle">
        <a-col :span="12">
          <a-space size="small">
            <span>Connection:</span>
            <a-badge 
              :status="connected ? 'processing' : 'error'" 
              :text="connected ? 'Connected' : 'Disconnected'"
            />
            <span v-if="lastEventTime">Last event: {{ formatLastEventTime() }}</span>
          </a-space>
        </a-col>
        <a-col :span="12" style="text-align: right;">
          <a-space size="small">
            <span>Filter:</span>
            <a-select
              v-model:value="eventTypeFilter"
              size="small"
              style="width: 120px;"
              placeholder="All types"
              :options="typeFilterOptions"
              allow-clear
            />
            <a-input
              v-model:value="searchText"
              size="small"
              placeholder="Search events..."
              style="width: 150px;"
              :suffix="h(SearchOutlined)"
              allow-clear
            />
          </a-space>
        </a-col>
      </a-row>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, nextTick, watch } from 'vue'
import { 
  ClearOutlined, 
  DownloadOutlined, 
  PauseOutlined, 
  CaretRightOutlined,
  SearchOutlined,
  LeftOutlined,
  RightOutlined 
} from '@ant-design/icons-vue'
import { message } from 'ant-design-vue'
import { h } from 'vue'
import { dserv } from '../services/dserv.js'
import { eventService } from '../services/eventService.js'

// Component state
const events = ref([])
const allObservations = ref([]) // Store all observation periods
const viewingObsIndex = ref(-1) // Which observation we're currently viewing (-1 = live)
const loading = ref(false)
const connected = ref(false)
const autoScroll = ref(true)
const lastEventTime = ref(null)
const obsInfo = ref({ obsCount: -1, obsStart: 0, events: [] })
const eventTable = ref(null)

// Filtering and search
const eventTypeFilter = ref(null)
const searchText = ref('')

// Table columns
const columns = [
  {
    title: 'Time',
    dataIndex: 'timestamp',
    key: 'timestamp',
    width: 80,
    fixed: 'left'
  },
  {
    title: 'Δt',
    dataIndex: 'elapsedTime',
    key: 'elapsedTime',
    width: 60
  },
  {
    title: 'Type',
    dataIndex: 'type',
    key: 'type',
    width: 50
  },
  {
    title: 'Type Name',
    dataIndex: 'typeName',
    key: 'typeName',
    width: 120
  },
  {
    title: 'Sub',
    dataIndex: 'subtype',
    key: 'subtype',
    width: 40
  },
  {
    title: 'Subtype Name',
    dataIndex: 'subtypeName',
    key: 'subtypeName',
    width: 100
  },
  {
    title: 'Parameters',
    dataIndex: 'params',
    key: 'params',
    ellipsis: true,
    width: 200
  }
]

// Computed properties
const displayedEvents = computed(() => {
  let sourceEvents
  
  if (viewingObsIndex.value === -1) {
    // Viewing live events
    sourceEvents = events.value
  } else {
    // Viewing historical observation
    const obs = allObservations.value[viewingObsIndex.value]
    sourceEvents = obs ? obs.events : []
  }
  
  let filtered = sourceEvents

  // Filter by event type
  if (eventTypeFilter.value !== null) {
    filtered = filtered.filter(event => event.type === eventTypeFilter.value)
  }

  // Filter by search text
  if (searchText.value) {
    const search = searchText.value.toLowerCase()
    filtered = filtered.filter(event => 
      getEventTypeName(event.type).toLowerCase().includes(search) ||
      getEventSubtypeName(event.type, event.subtype).toLowerCase().includes(search) ||
      event.decodedParams.toLowerCase().includes(search)
    )
  }

  return filtered
})

const currentObsEventCount = computed(() => {
  if (viewingObsIndex.value === -1) {
    // Live view
    return events.value.length
  } else {
    // Historical view
    const obs = allObservations.value[viewingObsIndex.value]
    return obs ? obs.events.length : 0
  }
})

const totalObservations = computed(() => allObservations.value.length)

const typeFilterOptions = computed(() => {
  const sourceEvents = viewingObsIndex.value === -1 ? events.value : 
    (allObservations.value[viewingObsIndex.value]?.events || [])
  const types = new Set(sourceEvents.map(e => e.type))
  return Array.from(types).map(type => ({
    label: `${type} - ${getEventTypeName(type)}`,
    value: type
  })).sort((a, b) => a.value - b.value)
})

const canNavigateBack = computed(() => viewingObsIndex.value < allObservations.value.length - 1)
const canNavigateForward = computed(() => viewingObsIndex.value > -1)

const viewingObsText = computed(() => {
  if (viewingObsIndex.value === -1) {
    return 'LIVE'
  } else {
    const obs = allObservations.value[viewingObsIndex.value]
    if (obs) {
      return `${obs.obsNumber} (${obs.events.length} events)`
    }
    return 'N/A'
  }
})

// Event handling functions
function handleDatapoint(data) {
  if (data.name === 'eventlog/events') {
    // ALWAYS process the event to update names, state, etc.
    const event = eventService.processEvent(data)
    
    // Only ADD TO DISPLAY if it meets our criteria:
    // - During observation periods OR BEGINOBS/ENDOBS events
    // - Not SYSTEM_STATE events (type 7)
    // - Not NAME events (type 1) 
    // - Not SUBTYPES events (type 6)
    if (event && (obsInfo.value.obsCount >= 0 || event.type === 19 || event.type === 20) && 
        event.type !== 7 && event.type !== 1 && event.type !== 6) {
      addEvent(event)
    }
  }
}

function addEvent(event) {
  // Only add to live events if we're not viewing historical data
  if (viewingObsIndex.value !== -1) {
    // If we're viewing historical data and a new event comes in,
    // switch back to live view
    viewingObsIndex.value = -1
  }

  // Calculate elapsed time from previous event
  let elapsedTime = ''
  if (events.value.length > 0) {
    const previousEvent = events.value[events.value.length - 1]
    const timeDiff = (event.timestamp - previousEvent.timestamp) / 1000
    elapsedTime = timeDiff.toFixed(1)
  } else {
    elapsedTime = '0.0'
  }

  const enhancedEvent = {
    key: Date.now() + Math.random(), // Unique key for Vue
    ...event,
    decodedParams: eventService.decodeParams(event),
    obsIndex: obsInfo.value.obsCount,
    elapsedTime: elapsedTime
  }

  events.value.push(enhancedEvent)
  lastEventTime.value = Date.now()

  // Limit number of events per observation to prevent memory issues
  if (events.value.length > 2000) {
    events.value.splice(0, 500) // Remove oldest 500 events
  }

  // Auto-scroll to bottom if enabled and viewing live
  if (autoScroll.value && viewingObsIndex.value === -1) {
    nextTick(() => {
      scrollToBottom()
    })
  }
}

// Auto-scroll throttling
let scrollTimeout = null

function scrollToBottom() {
  // Throttle scroll operations to reduce ResizeObserver triggers
  if (scrollTimeout) return
  
  scrollTimeout = setTimeout(() => {
    const table = eventTable.value
    if (table && table.$el) {
      const scrollBody = table.$el.querySelector('.ant-table-body')
      if (scrollBody) {
        scrollBody.scrollTop = scrollBody.scrollHeight
      }
    }
    scrollTimeout = null
  }, 50) // Throttle to max 20 times per second
}

// Event service handlers
function handleEventServiceUpdate(eventOrInfo) {
  if (eventOrInfo.type === 'obs_reset') {
    obsInfo.value = eventService.getObsInfo()
    // Clear all observations and events
    allObservations.value = []
    events.value = []
    viewingObsIndex.value = -1
  } else if (eventOrInfo.type === 'obs_start') {
    obsInfo.value = eventService.getObsInfo()
    // Start new observation - keep previous ones in history
    if (events.value.length > 0) {
      // Calculate elapsed times for historical events before saving
      const eventsWithElapsed = calculateElapsedTimesForSavedEvents(events.value)
      
      // Save the previous observation
      const prevObs = {
        obsNumber: obsInfo.value.obsCount - 1,
        events: eventsWithElapsed,
        startTime: Date.now(),
        endTime: null
      }
      allObservations.value.unshift(prevObs) // Add to beginning
      
      // Limit history to last 20 observations
      if (allObservations.value.length > 20) {
        allObservations.value = allObservations.value.slice(0, 20)
      }
    }
    // Clear current events for new observation
    events.value = []
    viewingObsIndex.value = -1 // Return to live view
  } else if (eventOrInfo.type === 'obs_end') {
    obsInfo.value = eventService.getObsInfo()
  } else if (eventOrInfo.type === 'name_update' || eventOrInfo.type === 'subtype_names_update' || eventOrInfo.type === 'names_reset') {
    // Force re-render when names update
    events.value = [...events.value]
    allObservations.value = [...allObservations.value]
  }
}

// Helper function to calculate elapsed times for events being saved to history
function calculateElapsedTimesForSavedEvents(eventList) {
  return eventList.map((event, index) => {
    let elapsedTime = '0.0'
    if (index > 0) {
      const timeDiff = (event.timestamp - eventList[index - 1].timestamp) / 1000
      elapsedTime = timeDiff.toFixed(1)
    }
    return {
      ...event,
      elapsedTime: elapsedTime
    }
  })
}

// Utility functions
function getEventTypeName(type) {
  return eventService.getEventTypeName(type)
}

function getEventSubtypeName(type, subtype) {
  return eventService.getEventSubtypeName(type, subtype)
}

function formatTimestamp(timestamp) {
  return eventService.formatTimestamp(timestamp)
}

function formatLastEventTime() {
  if (!lastEventTime.value) return ''
  const seconds = Math.floor((Date.now() - lastEventTime.value) / 1000)
  if (seconds < 60) return `${seconds}s ago`
  const minutes = Math.floor(seconds / 60)
  return `${minutes}m ago`
}

function getTypeColor(type) {
  // Color coding based on event type ranges
  if (type < 16) return 'blue'      // Reserved/System
  if (type < 32) return 'green'     // Core events
  if (type < 64) return 'orange'    // Experiment events
  if (type < 128) return 'purple'   // System events
  return 'red'                      // User events
}

function getSubtypeColor(type, subtype) {
  return 'default'
}

function getRowClassName(record) {
  const classes = []
  
  // Highlight important events
  switch (record.type) {
    case 19: // BEGINOBS
      classes.push('obs-start-row')
      break
    case 20: // ENDOBS
      classes.push('obs-end-row')
      break
    case 3: // USER
      classes.push('user-event-row')
      break
  }
  
  return classes.join(' ')
}

// Navigation functions
function previousObservation() {
  if (canNavigateBack.value) {
    viewingObsIndex.value++
  }
}

function nextObservation() {
  if (canNavigateForward.value) {
    viewingObsIndex.value--
  }
}

// Actions
function clearEvents() {
  events.value = []
  allObservations.value = []
  viewingObsIndex.value = -1
  // No notification message
}

function exportEvents() {
  try {
    const data = eventService.exportEvents('csv')
    const blob = new Blob([data], { type: 'text/csv' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = `events_${new Date().toISOString().slice(0, 19).replace(/:/g, '-')}.csv`
    document.body.appendChild(a)
    a.click()
    document.body.removeChild(a)
    URL.revokeObjectURL(url)
    console.log('Events exported to CSV')
  } catch (error) {
    console.error('Export failed:', error)
  }
}

// Refresh event names from the system (useful when system changes)
async function refreshEventNames() {
  try {
    await eventService.requestEventNameRefresh(dserv)
    console.log('Event names refreshed from system')
  } catch (error) {
    console.error('Failed to refresh event names:', error)
  }
}

// Component lifecycle
let cleanupDserv = null
let cleanupEventService = null

onMounted(() => {
  // Suppress harmless ResizeObserver warnings
  const originalError = window.console.error
  window.console.error = (...args) => {
    if (args[0] && args[0].toString().includes('ResizeObserver loop completed')) {
      return // Suppress this specific error
    }
    originalError.apply(console, args)
  }
  
  // Subscribe to eventlog events from dserv
  cleanupDserv = dserv.registerComponent('EventTracker', {
    subscriptions: [
      { pattern: 'eventlog/events', every: 1 }
    ]
  })

  // Set up dserv event handlers
  dserv.on('datapoint:eventlog/events', handleDatapoint)
  dserv.on('connection', (data) => {
    connected.value = data.connected
  })

  // Set up event service handlers
  cleanupEventService = eventService.addHandler(handleEventServiceUpdate)

  // Initialize connection state
  connected.value = dserv.state.connected

  // Get initial observation info
  obsInfo.value = eventService.getObsInfo()
})

onUnmounted(() => {
  if (cleanupDserv) cleanupDserv()
  if (cleanupEventService) cleanupEventService()
  dserv.off('datapoint:eventlog/events', handleDatapoint)
})

// Watch for connection changes to update UI
watch(() => dserv.state.connected, (newConnected) => {
  connected.value = newConnected
})

// Expose methods for parent components
defineExpose({
  clearEvents,
  exportEvents,
  refreshEventNames,
  previousObservation,
  nextObservation,
  getCurrentEvents: () => events.value,
  getObsInfo: () => obsInfo.value,
  getAllObservations: () => allObservations.value
})
</script>

<style scoped>
.event-tracker {
  height: 100%;
  display: flex;
  flex-direction: column;
  font-family: 'Monaco', 'Menlo', 'Ubuntu Mono', monospace;
}

.event-header {
  padding: 12px;
  border-bottom: 1px solid #d9d9d9;
  background: #fafafa;
  flex-shrink: 0;
}

.table-container {
  flex: 1;
  overflow: hidden;
}

.status-bar {
  padding: 8px 12px;
  border-top: 1px solid #d9d9d9;
  background: #fafafa;
  font-size: 12px;
  flex-shrink: 0;
}

.timestamp-cell {
  font-family: monospace;
  font-size: 11px;
  color: #666;
}

.elapsed-time-cell {
  font-family: monospace;
  font-size: 11px;
  color: #888;
  text-align: right;
}

.type-name {
  font-weight: 500;
}

.subtype-name {
  font-size: 12px;
  color: #666;
}

.params-cell {
  font-family: monospace;
  font-size: 11px;
  color: #333;
  max-width: 200px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

/* Row highlighting for special events */
:deep(.obs-start-row) {
  background-color: #e6f7ff !important;
}

:deep(.obs-start-row:hover) {
  background-color: #bae7ff !important;
}

:deep(.obs-end-row) {
  background-color: #fff2e6 !important;
}

:deep(.obs-end-row:hover) {
  background-color: #ffd591 !important;
}

:deep(.user-event-row) {
  background-color: #f6ffed !important;
}

:deep(.user-event-row:hover) {
  background-color: #d9f7be !important;
}

/* Table styling */
:deep(.ant-table-small .ant-table-tbody > tr > td) {
  padding: 4px 8px;
}

:deep(.ant-table-thead > tr > th) {
  font-weight: 600;
  background: #fafafa;
}

/* Responsive adjustments */
@media (max-width: 1200px) {
  .params-cell {
    max-width: 150px;
  }
}

@media (max-width: 768px) {
  .event-header :deep(.ant-col) {
    margin-bottom: 8px;
  }
  
  .status-bar :deep(.ant-col) {
    margin-bottom: 4px;
  }
}
</style>