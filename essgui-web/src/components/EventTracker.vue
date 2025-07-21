<template>
  <div class="event-tracker-container">
    <!-- Header Section -->
    <div class="event-header">
      <a-row :gutter="12" align="middle">
        <a-col :span="4">
          <a-statistic :title="viewingObsIndex === -1 ? 'Current Obs' : 'Viewing Obs'"
            :value="viewingObsIndex === -1 ? (obsInfo.obsCount >= 0 ? obsInfo.obsCount : 'None') : allObservations[viewingObsIndex]?.obsNumber ?? 'N/A'" />
        </a-col>
        <a-col :span="4">
          <a-statistic title="Events" :value="currentObsEventCount" />
        </a-col>
        <a-col :span="4">
          <a-statistic title="Total Obs" :value="totalObservations" />
        </a-col>
        <a-col :span="6">
          <!-- Observation navigation -->
          <div class="obs-navigation">
            <span class="nav-label">View:</span>
            <a-button-group size="small">
              <a-button @click="previousObservation" :disabled="!canNavigateBack" :icon="h(LeftOutlined)" />
              <a-button @click="nextObservation" :disabled="!canNavigateForward" :icon="h(RightOutlined)" />
            </a-button-group>
            <span class="view-status" :class="{ 'live': viewingObsIndex === -1 }">
              {{ viewingObsText }}
            </span>
          </div>
        </a-col>
        <a-col :span="6" style="text-align: right;">
          <div class="header-actions">
            <a-space size="small">
              <a-button size="small" @click="clearEvents" :icon="h(ClearOutlined)">
                Clear
              </a-button>
              <a-button size="small" @click="exportEvents" :icon="h(DownloadOutlined)">
                Export
              </a-button>
            </a-space>
            <a-button v-if="viewingObsIndex !== -1" size="small" @click="jumpToLive" type="primary"
              :icon="h(FastForwardOutlined)" class="live-button">
              Live
            </a-button>
          </div>
        </a-col>
      </a-row>
    </div>

    <!-- Table Section -->
    <div class="table-section">
      <div class="table-container">
        <a-table ref="eventTable" :columns="columns" :data-source="displayedEvents" :pagination="false"
          :scroll="{ y: true, x: 800 }" size="small" :row-class-name="getRowClassName" :loading="loading"
          class="event-table">
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
    </div>

    <!-- Footer Section -->
    <div class="event-footer">
      <a-row align="middle">
        <a-col :span="12">
          <a-space size="small">
            <span>Connection:</span>
            <a-badge :status="connected ? 'processing' : 'error'" :text="connected ? 'Connected' : 'Disconnected'" />
            <span v-if="lastEventTime" class="last-event">
              Last event: {{ formatLastEventTime() }}
            </span>
            <!-- NEW: Background tracking indicator -->
            <a-badge v-if="connected" status="success" text="Background tracking active" />
          </a-space>
        </a-col>
        <a-col :span="12" style="text-align: right;">
          <a-space size="small">
            <span>Filter:</span>
            <a-select v-model:value="eventTypeFilter" size="small" style="width: 120px;" placeholder="All types"
              :options="typeFilterOptions" allow-clear />
            <a-input v-model:value="searchText" size="small" placeholder="Search events..." style="width: 150px;"
              :suffix="h(SearchOutlined)" allow-clear />
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
  FastForwardOutlined,
  SearchOutlined,
  LeftOutlined,
  RightOutlined,
  DownOutlined
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
const lastEventTime = ref(null)
const obsInfo = ref({ obsCount: -1, obsStart: 0, events: [] })
const eventTable = ref(null)

// Filtering and search
const eventTypeFilter = ref(null)
const searchText = ref('')

// NEW: Track if we need to sync with background events
const needsSync = ref(false)

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

// NEW: Sync with background events when component becomes visible
function syncWithBackgroundEvents() {
  console.log('Syncing with background events...')
  
  // Get the current observation events from the event service
  const serviceObsInfo = eventService.getObsInfo()
  const serviceEvents = eventService.getCurrentObsEvents()
  
  // Update our local state
  obsInfo.value = serviceObsInfo
  
  // Convert service events to display format
  events.value = serviceEvents.map((event, index) => {
    // Calculate elapsed time from previous event
    let elapsedTime = '0.0'
    if (index > 0) {
      const timeDiff = (event.timestamp - serviceEvents[index - 1].timestamp) / 1000
      elapsedTime = timeDiff.toFixed(1)
    }
    
    return {
      key: `sync_${event.timestamp}_${event.type}_${event.subtype}_${index}_${Math.random().toString(36).substr(2, 6)}`, // Guaranteed unique key
      ...event,
      decodedParams: eventService.decodeParams(event),
      obsIndex: serviceObsInfo.obsCount,
      elapsedTime: elapsedTime
    }
  })
  
  // NEW: Sync historical observations from the service
  const historicalObs = eventService.getHistoricalObservations()
  allObservations.value = historicalObs.map(obs => ({
    obsNumber: obs.obsNumber,
    events: obs.events.map((event, index) => ({
      key: `hist_${obs.obsNumber}_${event.timestamp}_${event.type}_${event.subtype}_${index}_${Math.random().toString(36).substr(2, 6)}`,
      ...event,
      decodedParams: event.decodedParams || eventService.decodeParams(event)
    })),
    startTime: obs.startTime,
    endTime: obs.endTime
  }))
  
  lastEventTime.value = events.value.length > 0 ? Date.now() : null
  needsSync.value = false
  
  console.log(`Synced ${events.value.length} current events and ${allObservations.value.length} historical observations`)
  
  // Auto-scroll to bottom if viewing live
  if (viewingObsIndex.value === -1 && events.value.length > 0) {
    scrollToBottom()
  }
}

// Enhanced event handling - now works with service events and historical data
function handleEventServiceUpdate(eventOrInfo) {
  if (eventOrInfo.type === 'obs_reset') {
    obsInfo.value = eventService.getObsInfo()
    // Clear all observations and events
    allObservations.value = []
    events.value = []
    viewingObsIndex.value = -1
  } else if (eventOrInfo.type === 'obs_start') {
    obsInfo.value = eventService.getObsInfo()
    
    // NEW: Historical observations are now managed by the service
    // Just sync with the service's historical data
    const historicalObs = eventService.getHistoricalObservations()
    allObservations.value = historicalObs.map(obs => ({
      obsNumber: obs.obsNumber,
      events: obs.events.map((event, index) => ({
        key: `hist_start_${obs.obsNumber}_${event.timestamp}_${event.type}_${event.subtype}_${index}_${Math.random().toString(36).substr(2, 6)}`,
        ...event,
        decodedParams: event.decodedParams || eventService.decodeParams(event)
      })),
      startTime: obs.startTime,
      endTime: obs.endTime
    }))
    
    // Clear current events for new observation
    events.value = []
    viewingObsIndex.value = -1
  } else if (eventOrInfo.type === 'obs_end') {
    obsInfo.value = eventService.getObsInfo()
  } else if (eventOrInfo.type === 'name_update' || eventOrInfo.type === 'subtype_names_update' || eventOrInfo.type === 'names_reset') {
    // Force re-render when names update
    events.value = [...events.value]
    allObservations.value = [...allObservations.value]
  } else if (eventOrInfo.timestamp) {
    // This is an actual event, not a control message
    addEventFromService(eventOrInfo)
  }
}

// NEW: Add event from service (works whether component is mounted or not)
function addEventFromService(event) {
  // Skip system events that shouldn't be displayed
  if (event.type === 7 || event.type === 1 || event.type === 6) {
    return
  }
  
  // Only display if in observation or it's a BEGINOBS/ENDOBS event
  if (!(obsInfo.value.obsCount >= 0 || event.type === 19 || event.type === 20)) {
    return
  }
  
  const enhancedEvent = {
    key: `live_${event.timestamp}_${event.type}_${event.subtype}_${Date.now()}_${Math.random().toString(36).substr(2, 6)}`, // Guaranteed unique
    ...event,
    decodedParams: eventService.decodeParams(event),
    obsIndex: obsInfo.value.obsCount,
    elapsedTime: '0.0'
  }
  
  // Calculate elapsed time from previous event
  if (events.value.length > 0) {
    const previousEvent = events.value[events.value.length - 1]
    const timeDiff = (event.timestamp - previousEvent.timestamp) / 1000
    enhancedEvent.elapsedTime = timeDiff.toFixed(1)
  }
  
  events.value.push(enhancedEvent)
  lastEventTime.value = Date.now()
  
  // Limit events per observation
  if (events.value.length > 2000) {
    events.value.splice(0, 500)
  }
  
  // Auto-scroll if viewing live
  if (viewingObsIndex.value === -1) {
    scrollToBottom()
  }
}

// Keep the original datapoint handler for direct dserv events (backup)
function handleDatapoint(data) {
  if (data.name === 'eventlog/events') {
    const event = eventService.parseEvent(data)
    if (event) {
      // The service will handle this and call our handler
      // This is just a backup in case service isn't working
    }
  }
}

// Computed properties (unchanged)
const displayedEvents = computed(() => {
  let sourceEvents

  if (viewingObsIndex.value === -1) {
    sourceEvents = events.value
  } else {
    const obs = allObservations.value[viewingObsIndex.value]
    sourceEvents = obs ? obs.events : []
  }

  let filtered = sourceEvents

  if (eventTypeFilter.value !== null) {
    filtered = filtered.filter(event => event.type === eventTypeFilter.value)
  }

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
    return events.value.length
  } else {
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

// Auto-scroll function (unchanged)
const scrollToBottom = () => {
  nextTick(() => {
    const table = eventTable.value
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

// Utility functions (unchanged)
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
  if (type < 16) return 'blue'
  if (type < 32) return 'green'
  if (type < 64) return 'orange'
  if (type < 128) return 'purple'
  return 'red'
}

function getSubtypeColor(type, subtype) {
  return 'default'
}

function getRowClassName(record) {
  const classes = []

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

// Navigation functions (unchanged)
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

function jumpToLive() {
  viewingObsIndex.value = -1
  if (events.value.length > 0) {
    scrollToBottom()
  }
}

// Actions with enhanced export and clear
function clearEvents() {
  events.value = []
  allObservations.value = []
  viewingObsIndex.value = -1
  
  // NEW: Also clear the service's historical data
  eventService.obsReset()
  message.success('All events and historical data cleared')
}

// NEW: Enhanced export with options
function exportEvents() {
  try {
    // Export all data including historical observations
    const data = eventService.exportAllData('csv')
    const blob = new Blob([data], { type: 'text/csv' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = `all_events_${new Date().toISOString().slice(0, 19).replace(/:/g, '-')}.csv`
    document.body.appendChild(a)
    a.click()
    document.body.removeChild(a)
    URL.revokeObjectURL(url)
    
    // Show export statistics
    const stats = eventService.getEventStatistics()
    console.log('Events exported:', {
      currentObs: stats.currentObservation,
      historical: stats.historical,
      session: stats.session
    })
    
    message.success(`Exported ${stats.session.totalEvents} events from ${stats.session.totalObservations} observations`)
  } catch (error) {
    console.error('Export failed:', error)
    message.error('Export failed')
  }
}

// NEW: Export just current view
function exportCurrentView() {
  try {
    const viewEvents = displayedEvents.value
    if (viewEvents.length === 0) {
      message.warning('No events to export in current view')
      return
    }
    
    const headers = ['Timestamp', 'ElapsedTime', 'Type', 'Subtype', 'TypeName', 'SubtypeName', 'Parameters']
    const rows = [headers]
    
    viewEvents.forEach(event => {
      rows.push([
        formatTimestamp(event.timestamp),
        event.elapsedTime || '0.0',
        event.type,
        event.subtype,
        getEventTypeName(event.type),
        getEventSubtypeName(event.type, event.subtype),
        event.decodedParams || eventService.decodeParams(event)
      ])
    })
    
    const csvData = rows.map(row => row.map(cell => `"${cell}"`).join(',')).join('\n')
    const blob = new Blob([csvData], { type: 'text/csv' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    
    const viewType = viewingObsIndex.value === -1 ? 'live' : `obs_${allObservations.value[viewingObsIndex.value]?.obsNumber}`
    a.download = `events_${viewType}_${new Date().toISOString().slice(0, 19).replace(/:/g, '-')}.csv`
    
    document.body.appendChild(a)
    a.click()
    document.body.removeChild(a)
    URL.revokeObjectURL(url)
    
    message.success(`Exported ${viewEvents.length} events from current view`)
  } catch (error) {
    console.error('Export current view failed:', error)
    message.error('Export failed')
  }
}

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
  console.log('EventTracker mounted - syncing with background events')
  
  cleanupDserv = dserv.registerComponent('EventTracker')

  // Listen to datapoints as backup
  dserv.on('datapoint:eventlog/events', handleDatapoint)
  dserv.on('connection', (data) => {
    connected.value = data.connected
  })

  // Set up event service handlers - this is the primary event source
  cleanupEventService = eventService.addHandler(handleEventServiceUpdate)

  // Initialize connection state
  connected.value = dserv.state.connected

  // NEW: Sync with any events that happened while component was unmounted
  syncWithBackgroundEvents()
  
  console.log('EventTracker fully initialized and synced')
})

onUnmounted(() => {
  console.log('EventTracker unmounted - events will continue tracking in background')
  
  if (cleanupDserv) cleanupDserv()
  if (cleanupEventService) cleanupEventService()
  dserv.off('datapoint:eventlog/events', handleDatapoint)
})

// Watch for connection changes to update UI
watch(() => dserv.state.connected, (newConnected) => {
  connected.value = newConnected
})

// NEW: Watch for when component becomes visible again (if using visibility API)
watch(() => document.visibilityState, (newState) => {
  if (newState === 'visible' && needsSync.value) {
    syncWithBackgroundEvents()
  }
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
  getAllObservations: () => allObservations.value,
  syncWithBackgroundEvents // NEW: Allow manual sync
})
</script>

<style scoped>
/* Same styles as before - keeping all the existing CSS */
.event-tracker-container {
  height: 100%;
  display: flex;
  flex-direction: column;
  overflow: hidden;
  font-family: 'Monaco', 'Menlo', 'Ubuntu Mono', monospace;
}

.event-header {
  flex-shrink: 0;
  padding: 12px;
  border-bottom: 1px solid #d9d9d9;
  background: #fafafa;
}

.obs-navigation {
  display: flex;
  align-items: center;
  gap: 8px;
}

.nav-label {
  font-size: 12px;
}

.view-status {
  font-size: 12px;
  font-weight: 500;
  color: #1890ff;
}

.view-status.live {
  color: #52c41a;
}

.header-actions {
  display: flex;
  flex-direction: column;
  gap: 4px;
  align-items: flex-end;
}

.live-button {
  width: 80px;
}

.table-section {
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}

.table-container {
  flex: 1;
  min-height: 0;
  overflow: hidden;
}

.event-footer {
  flex-shrink: 0;
  padding: 8px 12px;
  border-top: 1px solid #d9d9d9;
  background: #fafafa;
  font-size: 12px;
}

.last-event {
  font-size: 11px;
  color: #666;
}

.event-table {
  height: 100%;
}

:deep(.event-table .ant-table) {
  height: 100%;
  display: flex;
  flex-direction: column;
}

:deep(.event-table .ant-table-container) {
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
}

:deep(.event-table .ant-table-header) {
  flex-shrink: 0;
}

:deep(.event-table .ant-table-body) {
  flex: 1;
  overflow-y: auto !important;
  overflow-x: auto !important;
  min-height: 0;
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

:deep(.obs-start-row td) {
  background-color: #e6f7ff !important;
}

:deep(.obs-start-row:hover td) {
  background-color: #bae7ff !important;
}

:deep(.obs-end-row td) {
  background-color: #fff2e6 !important;
}

:deep(.obs-end-row:hover td) {
  background-color: #ffd591 !important;
}

:deep(.user-event-row td) {
  background-color: #f6ffed !important;
}

:deep(.user-event-row:hover td) {
  background-color: #d9f7be !important;
}

:deep(.event-table .ant-table-small .ant-table-tbody > tr > td) {
  padding: 4px 8px;
}

:deep(.event-table .ant-table-thead > tr > th) {
  font-weight: 600;
  background: #fafafa;
}

:deep(.event-table .ant-table-wrapper) {
  height: 100% !important;
  display: flex;
  flex-direction: column;
}

:deep(.event-table .ant-spin-nested-loading) {
  height: 100% !important;
}

:deep(.event-table .ant-spin-container) {
  height: 100% !important;
  display: flex;
  flex-direction: column;
}

@media (max-width: 1200px) {
  .params-cell {
    max-width: 150px;
  }
}

@media (max-width: 768px) {
  .event-header :deep(.ant-col) {
    margin-bottom: 8px;
  }

  .event-footer :deep(.ant-col) {
    margin-bottom: 4px;
  }
}
</style>