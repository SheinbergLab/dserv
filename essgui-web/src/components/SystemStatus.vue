<template>
  <div class="compact-system-status" style="height: 100%; display: flex; flex-direction: column; font-size: 11px; overflow: hidden;">
    <!-- Ultra-Compact Header -->
    <div style="flex-shrink: 0; padding: 4px 6px; border-bottom: 1px solid #e8e8e8; display: flex; justify-content: space-between; align-items: center; background: #fafafa;">
      <div style="display: flex; align-items: center; gap: 4px;">
        <a-switch 
          v-model:checked="autoRefresh" 
          size="small"
          style="transform: scale(0.8);"
        />
        <span style="font-size: 9px; color: #666;">Auto</span>
      </div>
      <a-button 
        size="small" 
        @click="refreshData"
        :loading="refreshing && !autoRefresh"
        type="text"
        style="height: 18px; width: 18px; padding: 0; font-size: 10px;"
        :icon="h(ReloadOutlined)"
      />
    </div>

    <!-- Micro Status Cards -->
    <div style="flex-shrink: 0; display: grid; grid-template-columns: repeat(2, 1fr); gap: 3px; padding: 4px; background: #f8f8f8;">
      <!-- Connection -->
      <div style="background: white; padding: 3px 4px; border-radius: 3px; text-align: center; border: 1px solid #e8e8e8;">
        <div :style="{ 
          width: '6px', 
          height: '6px', 
          borderRadius: '50%', 
          backgroundColor: connectionColor,
          margin: '0 auto 1px'
        }"></div>
        <div style="font-size: 8px; color: #666;">{{ uptimeText }}</div>
      </div>

      <!-- Throughput -->
      <div style="background: white; padding: 3px 4px; border-radius: 3px; text-align: center; border: 1px solid #e8e8e8;">
        <div style="font-weight: 500; font-size: 10px;">{{ messagesPerSecond }}/s</div>
        <div style="font-size: 8px; color: #666;">{{ bytesText }}</div>
      </div>

      <!-- Bandwidth -->
      <div style="background: white; padding: 3px 4px; border-radius: 3px; text-align: center; border: 1px solid #e8e8e8;">
        <div style="font-weight: 500; font-size: 10px;">{{ bandwidthText }}</div>
        <div style="font-size: 8px; color: #666;">{{ peakBandwidthText }}</div>
      </div>

      <!-- Processing -->
      <div style="background: white; padding: 3px 4px; border-radius: 3px; text-align: center; border: 1px solid #e8e8e8;">
        <div style="font-weight: 500; font-size: 10px;">{{ avgProcessingText }}</div>
        <div style="font-size: 8px; color: #666;">{{ maxProcessingText }}</div>
      </div>
    </div>

    <!-- Compact Tabs -->
    <a-tabs 
      v-model:activeKey="activeTab" 
      size="small" 
      style="flex: 1; overflow: hidden;"
      :tab-bar-style="{ 
        margin: '0', 
        padding: '0 4px', 
        background: '#fafafa', 
        borderBottom: '1px solid #e8e8e8',
        minHeight: '24px'
      }"
    >
      <!-- Datapoints Tab -->
      <a-tab-pane key="datapoints" style="height: 100%; overflow: hidden;">
        <template #tab>
          <span style="font-size: 9px; padding: 2px 0;">
            Datapoints ({{ filteredDatapoints.length }})
          </span>
        </template>
        
        <div style="height: 100%; display: flex; flex-direction: column; overflow: hidden;">
          <!-- Ultra-compact filters -->
          <div style="flex-shrink: 0; padding: 3px 4px; display: flex; gap: 3px; background: #f8f8f8; border-bottom: 1px solid #e8e8e8;">
            <a-select 
              v-model:value="datapointFilter" 
              size="small" 
              style="width: 80px; font-size: 9px;"
            >
              <a-select-option value="all">All</a-select-option>
              <a-select-option 
                v-for="category in availableCategories" 
                :key="category" 
                :value="category"
              >
                {{ category.substring(0, 8) }}{{ category.length > 8 ? '...' : '' }}
              </a-select-option>
            </a-select>
            
            <a-select 
              v-model:value="priorityFilter" 
              size="small" 
              style="width: 60px; font-size: 9px;"
            >
              <a-select-option value="all">All</a-select-option>
              <a-select-option value="high">Hi</a-select-option>
              <a-select-option value="medium">Med</a-select-option>
              <a-select-option value="low">Lo</a-select-option>
            </a-select>
          </div>

          <!-- Compact datapoint list -->
          <div style="flex: 1; overflow-y: auto; padding: 2px;">
            <div v-if="filteredDatapoints.length === 0" style="padding: 8px; text-align: center; color: #666; font-size: 9px;">
              No datapoints available
            </div>
            <div 
              v-for="record in filteredDatapoints" 
              :key="record.name"
              style="padding: 2px 4px; margin-bottom: 1px; background: white; border: 1px solid #f0f0f0; border-radius: 2px; font-size: 9px;"
            >
              <!-- Datapoint name -->
              <div style="font-family: monospace; font-weight: 500; margin-bottom: 1px; color: #333;">
                {{ record.displayName }}
              </div>
              
              <!-- Stats row -->
              <div style="display: flex; justify-content: space-between; align-items: center;">
                <div style="display: flex; gap: 4px; align-items: center;">
                  <span style="font-weight: 500;">{{ record.frequency }}</span>
                  <span style="color: #666; font-size: 8px;">/ {{ record.expected }}</span>
                  <div 
                    :style="{
                      width: '6px',
                      height: '6px',
                      borderRadius: '50%',
                      backgroundColor: record.healthColor
                    }"
                  ></div>
                </div>
                
                <div style="display: flex; gap: 2px;">
                  <span 
                    :style="{
                      fontSize: '7px',
                      padding: '1px 3px',
                      borderRadius: '2px',
                      backgroundColor: record.categoryColor,
                      color: 'white'
                    }"
                  >
                    {{ record.categoryCode }}
                  </span>
                  <span 
                    :style="{
                      fontSize: '7px',
                      padding: '1px 3px',
                      borderRadius: '2px',
                      backgroundColor: record.priorityColor,
                      color: 'white'
                    }"
                  >
                    {{ record.priorityCode }}
                  </span>
                </div>
              </div>
              
              <!-- Detail stats -->
              <div style="font-size: 8px; color: #666; margin-top: 1px;">
                Avg: {{ record.avgHz }} • Max: {{ record.maxHz }} • Count: {{ record.count }}
              </div>
            </div>
          </div>
        </div>
      </a-tab-pane>

      <!-- Components Tab -->
      <a-tab-pane key="components" style="height: 100%; overflow: hidden;">
        <template #tab>
          <span style="font-size: 9px; padding: 2px 0;">
            Components ({{ componentCount }})
          </span>
        </template>
        
        <div style="height: 100%; overflow-y: auto; padding: 4px;">
          <!-- Category breakdown -->
          <div style="margin-bottom: 6px;">
            <div style="font-size: 9px; font-weight: 500; margin-bottom: 2px; color: #666;">By Category</div>
            <div style="display: flex; flex-wrap: wrap; gap: 2px;">
              <div 
                v-for="cat in categoryBreakdown" 
                :key="cat.name"
                style="font-size: 8px; padding: 1px 4px; background: #f0f0f0; border-radius: 2px; display: flex; align-items: center; gap: 2px;"
              >
                <span>{{ cat.name }}</span>
                <span style="font-weight: 500; color: #333;">{{ cat.count }}</span>
              </div>
            </div>
          </div>

          <!-- Priority breakdown -->
          <div style="margin-bottom: 6px;">
            <div style="font-size: 9px; font-weight: 500; margin-bottom: 2px; color: #666;">By Priority</div>
            <div style="display: flex; gap: 2px;">
              <div 
                v-for="pri in priorityBreakdown" 
                :key="pri.name"
                :style="{
                  fontSize: '8px',
                  padding: '1px 4px',
                  borderRadius: '2px',
                  backgroundColor: pri.color,
                  color: 'white',
                  display: 'flex',
                  alignItems: 'center',
                  gap: '2px'
                }"
              >
                <span>{{ pri.name }}</span>
                <span style="font-weight: 500;">{{ pri.count }}</span>
              </div>
            </div>
          </div>

          <!-- Active components -->
          <div>
            <div style="font-size: 9px; font-weight: 500; margin-bottom: 2px; color: #666;">Active Components</div>
            <div style="display: flex; flex-wrap: wrap; gap: 1px;">
              <div 
                v-for="component in activeComponents" 
                :key="component"
                style="font-size: 8px; padding: 1px 3px; background: #e6f7ff; border: 1px solid #91d5ff; border-radius: 2px; color: #1890ff;"
              >
                {{ component }}
              </div>
            </div>
          </div>
        </div>
      </a-tab-pane>

      <!-- Performance Tab -->
      <a-tab-pane key="performance" style="height: 100%; overflow: hidden;">
        <template #tab>
          <span style="font-size: 9px; padding: 2px 0;">
            Performance
          </span>
        </template>
        
        <div style="height: 100%; overflow-y: auto; padding: 4px;">
          <!-- Processing metrics -->
          <div style="margin-bottom: 6px; padding: 4px; background: white; border: 1px solid #e8e8e8; border-radius: 3px;">
            <div style="font-size: 9px; font-weight: 500; margin-bottom: 3px; color: #666;">Processing Times</div>
            <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 4px; font-size: 8px;">
              <div>
                <div style="color: #666;">Avg:</div>
                <div style="font-weight: 500;">{{ avgProcessingText }}</div>
              </div>
              <div>
                <div style="color: #666;">Max:</div>
                <div style="font-weight: 500;">{{ maxProcessingText }}</div>
              </div>
            </div>
          </div>

          <!-- Message stats -->
          <div style="margin-bottom: 6px; padding: 4px; background: white; border: 1px solid #e8e8e8; border-radius: 3px;">
            <div style="font-size: 9px; font-weight: 500; margin-bottom: 3px; color: #666;">Message Stats</div>
            <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 4px; font-size: 8px;">
              <div>
                <div style="color: #666;">Received:</div>
                <div style="font-weight: 500;">{{ totalReceived }}</div>
              </div>
              <div>
                <div style="color: #666;">Sent:</div>
                <div style="font-weight: 500;">{{ totalSent }}</div>
              </div>
            </div>
          </div>

          <!-- Bandwidth details -->
          <div style="margin-bottom: 6px; padding: 4px; background: white; border: 1px solid #e8e8e8; border-radius: 3px;">
            <div style="font-size: 9px; font-weight: 500; margin-bottom: 3px; color: #666;">Bandwidth</div>
            <div style="font-size: 8px;">
              <div style="display: flex; justify-content: space-between; margin-bottom: 1px;">
                <span style="color: #666;">Current:</span>
                <span style="font-weight: 500;">{{ bandwidthText }}</span>
              </div>
              <div style="display: flex; justify-content: space-between; margin-bottom: 1px;">
                <span style="color: #666;">Average:</span>
                <span style="font-weight: 500;">{{ avgBandwidthText }}</span>
              </div>
              <div style="display: flex; justify-content: space-between;">
                <span style="color: #666;">Peak:</span>
                <span style="font-weight: 500;">{{ peakBandwidthText }}</span>
              </div>
            </div>
          </div>

          <!-- Connection details -->
          <div style="padding: 4px; background: white; border: 1px solid #e8e8e8; border-radius: 3px;">
            <div style="font-size: 9px; font-weight: 500; margin-bottom: 3px; color: #666;">Connection</div>
            <div style="font-size: 8px;">
              <div style="display: flex; justify-content: space-between; margin-bottom: 1px;">
                <span style="color: #666;">Uptime:</span>
                <span style="font-weight: 500;">{{ uptimeText }}</span>
              </div>
              <div style="display: flex; justify-content: space-between;">
                <span style="color: #666;">Reconnects:</span>
                <span style="font-weight: 500;">{{ reconnectCount }}</span>
              </div>
            </div>
          </div>
        </div>
      </a-tab-pane>
    </a-tabs>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, watch } from 'vue'
import { ReloadOutlined } from '@ant-design/icons-vue'
import { h } from 'vue'
import { useDservMonitoring } from '../services/dserv.js'

// Component state
const activeTab = ref('datapoints')
const autoRefresh = ref(true)
const refreshing = ref(false)
const datapointFilter = ref('all')
const priorityFilter = ref('all')

// Raw data storage - single source of truth
const systemStatus = ref(null)

// Refresh interval
let refreshInterval = null

// Computed properties using proper Vue patterns
const connectionColor = computed(() => {
  return systemStatus.value?.connection?.connected ? '#52c41a' : '#f5222d'
})

const uptimeText = computed(() => {
  const uptime = systemStatus.value?.connection?.uptime || 0
  if (!uptime) return '0s'
  
  const seconds = Math.floor(uptime / 1000)
  const minutes = Math.floor(seconds / 60)
  const hours = Math.floor(minutes / 60)
  
  if (hours > 0) return `${hours}h ${minutes % 60}m`
  if (minutes > 0) return `${minutes}m`
  return `${seconds}s`
})

const messagesPerSecond = computed(() => {
  return systemStatus.value?.messages?.messagesPerSecond || 0
})

const bytesText = computed(() => {
  const bytes = systemStatus.value?.messages?.bytesPerSecond || 0
  if (bytes < 1024) return `${bytes}B`
  return `${(bytes / 1024).toFixed(0)}KB`
})

const bandwidthText = computed(() => {
  const kbps = systemStatus.value?.bandwidth?.currentKbps || 0
  if (kbps < 1024) return `${kbps.toFixed(0)}K`
  return `${(kbps / 1024).toFixed(1)}M`
})

const peakBandwidthText = computed(() => {
  const kbps = systemStatus.value?.bandwidth?.peakKbps || 0
  if (kbps < 1024) return `${kbps.toFixed(0)}K peak`
  return `${(kbps / 1024).toFixed(1)}M peak`
})

const avgBandwidthText = computed(() => {
  const kbps = systemStatus.value?.bandwidth?.averageKbps || 0
  if (kbps < 1024) return `${kbps.toFixed(0)}K`
  return `${(kbps / 1024).toFixed(1)}M`
})

const avgProcessingText = computed(() => {
  const ms = systemStatus.value?.performance?.avgProcessingTime || 0
  if (ms < 1) return `${(ms * 1000).toFixed(0)}μs`
  if (ms < 1000) return `${ms.toFixed(1)}ms`
  return `${(ms / 1000).toFixed(1)}s`
})

const maxProcessingText = computed(() => {
  const ms = systemStatus.value?.performance?.maxProcessingTime || 0
  if (ms < 1) return `${(ms * 1000).toFixed(0)}μs max`
  if (ms < 1000) return `${ms.toFixed(1)}ms max`
  return `${(ms / 1000).toFixed(1)}s max`
})

const totalReceived = computed(() => {
  const total = systemStatus.value?.messages?.totalReceived || 0
  return total.toLocaleString()
})

const totalSent = computed(() => {
  const total = systemStatus.value?.messages?.totalSent || 0
  return total.toLocaleString()
})

const reconnectCount = computed(() => {
  return systemStatus.value?.connection?.reconnectAttempts || 0
})

// Available categories for filter
const availableCategories = computed(() => {
  const cats = new Set()
  const datapoints = systemStatus.value?.datapoints || []
  datapoints.forEach(dp => {
    if (dp?.config?.category) cats.add(dp.config.category)
  })
  return Array.from(cats).sort()
})

// Processed datapoints for display
const filteredDatapoints = computed(() => {
  let datapoints = systemStatus.value?.datapoints || []
  
  // Filter out invalid datapoints and apply filters
  datapoints = datapoints.filter(dp => dp && dp.name && dp.config)
  
  if (datapointFilter.value !== 'all') {
    datapoints = datapoints.filter(dp => dp.config?.category === datapointFilter.value)
  }
  
  if (priorityFilter.value !== 'all') {
    datapoints = datapoints.filter(dp => dp.config?.priority === priorityFilter.value)
  }
  
  // Sort by frequency and process for display
  return datapoints
    .sort((a, b) => (b.currentHz || 0) - (a.currentHz || 0))
    .map(dp => ({
      name: dp.name,
      displayName: (dp.name || 'unknown').replace('ess/', ''),
      frequency: (dp.currentHz || 0).toFixed(1) + 'Hz',
      expected: dp.config?.expectedHz || '?',
      avgHz: (dp.avgHz || 0).toFixed(1),
      maxHz: dp.maxHz || 0,
      count: dp.count || 0,
      healthColor: getHealthColor(dp.health),
      categoryColor: getCategoryColor(dp.config?.category),
      categoryCode: (dp.config?.category?.substring(0, 3).toUpperCase()) || 'UNK',
      priorityColor: getPriorityColor(dp.config?.priority),
      priorityCode: (dp.config?.priority?.substring(0, 1).toUpperCase()) || '?'
    }))
})

// Component breakdown data
const categoryBreakdown = computed(() => {
  const subscriptions = systemStatus.value?.subscriptions?.byCategory
  if (!subscriptions || typeof subscriptions.entries !== 'function') return []
  
  return Array.from(subscriptions.entries()).map(([name, count]) => ({
    name,
    count
  }))
})

const priorityBreakdown = computed(() => {
  const subscriptions = systemStatus.value?.subscriptions?.byPriority
  if (!subscriptions || typeof subscriptions.entries !== 'function') return []
  
  return Array.from(subscriptions.entries()).map(([name, count]) => ({
    name,
    count,
    color: getPriorityColor(name)
  }))
})

const componentCount = computed(() => {
  return categoryBreakdown.value.reduce((total, cat) => total + cat.count, 0)
})

const activeComponents = computed(() => {
  // Extract component names from available data
  // This would need to be exposed from the monitoring API
  return ['ExperimentControl', 'EyeTouchVisualizer', 'SystemStatus']
})

// Methods
function refreshData() {
  refreshing.value = true
  
  try {
    const { getSystemStatus } = useDservMonitoring()
    systemStatus.value = getSystemStatus()
  } catch (error) {
    console.error('Failed to refresh system status:', error)
  } finally {
    setTimeout(() => {
      refreshing.value = false
    }, 200)
  }
}

function startAutoRefresh() {
  if (refreshInterval) clearInterval(refreshInterval)
  refreshInterval = setInterval(refreshData, 1000)
}

function stopAutoRefresh() {
  if (refreshInterval) {
    clearInterval(refreshInterval)
    refreshInterval = null
  }
}

// Helper functions
function getHealthColor(health) {
  switch (health) {
    case 'healthy': return '#52c41a'
    case 'slow': return '#faad14'
    case 'stalled': return '#f5222d'
    case 'too_fast': return '#722ed1'
    default: return '#d9d9d9'
  }
}

function getCategoryColor(category) {
  if (!category) return '#d9d9d9'
  const colors = {
    'eye_tracking': '#1890ff',
    'performance': '#52c41a',
    'experiment': '#faad14',
    'configuration': '#722ed1',
    'state': '#13c2c2',
    'scripts': '#eb2f96',
    'sensors': '#f5222d',
    'neural': '#fa541c'
  }
  return colors[category] || '#d9d9d9'
}

function getPriorityColor(priority) {
  if (!priority) return '#d9d9d9'
  switch (priority) {
    case 'high': return '#f5222d'
    case 'medium': return '#faad14'
    case 'low': return '#1890ff'
    default: return '#d9d9d9'
  }
}

// Lifecycle
onMounted(() => {
  console.log('Clean Compact SystemStatus mounted')
  refreshData()
  
  // Watch auto-refresh setting
  const stopWatching = watch(autoRefresh, (enabled) => {
    if (enabled) {
      startAutoRefresh()
    } else {
      stopAutoRefresh()
    }
  }, { immediate: true })
  
  onUnmounted(() => {
    stopAutoRefresh()
    stopWatching()
  })
})
</script>

<style scoped>
.compact-system-status {
  background: #fafafa;
}

.compact-system-status :deep(.ant-card-body) {
  padding: 4px;
}

.compact-system-status :deep(.ant-tabs-nav) {
  margin: 0;
}

.compact-system-status :deep(.ant-tabs-tab) {
  padding: 2px 6px;
  margin: 0;
  font-size: 9px;
  line-height: 1.2;
}

.compact-system-status :deep(.ant-tabs-content) {
  height: calc(100% - 24px);
  overflow: hidden;
}

.compact-system-status :deep(.ant-tabs-tabpane) {
  height: 100%;
  overflow: hidden;
}

.compact-system-status :deep(.ant-select-small .ant-select-selector) {
  height: 20px;
  line-height: 18px;
  font-size: 9px;
}

.compact-system-status :deep(.ant-select-small .ant-select-selection-item) {
  line-height: 18px;
  font-size: 9px;
}

.compact-system-status :deep(.ant-switch) {
  min-width: 28px;
  height: 16px;
  line-height: 16px;
}

.compact-system-status :deep(.ant-switch-handle) {
  width: 12px;
  height: 12px;
  top: 2px;
}

/* Ensure scrollbars are visible */
.compact-system-status :deep(*) {
  scrollbar-width: thin;
  scrollbar-color: #d9d9d9 transparent;
}

.compact-system-status :deep(*::-webkit-scrollbar) {
  width: 6px;
  height: 6px;
}

.compact-system-status :deep(*::-webkit-scrollbar-track) {
  background: transparent;
}

.compact-system-status :deep(*::-webkit-scrollbar-thumb) {
  background-color: #d9d9d9;
  border-radius: 3px;
}

.compact-system-status :deep(*::-webkit-scrollbar-thumb:hover) {
  background-color: #bfbfbf;
}
</style>