<template>
  <div style="height: 100%; display: flex; flex-direction: column; overflow: hidden;">
    <!-- Header with controls -->
    <div style="flex-shrink: 0; padding: 8px 12px; border-bottom: 1px solid #e8e8e8; background: #fafafa; display: flex; justify-content: space-between; align-items: center;">
      <div style="display: flex; align-items: center; gap: 12px;">
        <div style="display: flex; align-items: center; gap: 8px;">
          <BugOutlined style="font-size: 16px; color: #1890ff;" />
          <span style="font-weight: 500;">ESS Error Console</span>
        </div>
        
        <!-- Error tracing toggle -->
        <div style="display: flex; align-items: center; gap: 6px;">
          <a-switch
            size="small"
            v-model:checked="isTracing"
            :loading="loading"
            :disabled="!connected"
            @change="toggleTracing"
          >
            <template #checkedChildren><EyeOutlined /></template>
            <template #unCheckedChildren><EyeInvisibleOutlined /></template>
          </a-switch>
          <span style="font-size: 12px;">
            {{ isTracing ? 'Tracing' : 'Not Tracing' }}
          </span>
        </div>

        <!-- Error counts from global service -->
        <div v-if="errorCounts.total > 0" style="display: flex; align-items: center; gap: 8px; font-size: 11px;">
          <a-badge :count="errorCounts.error" :number-style="{ backgroundColor: '#ff4d4f' }" />
          <a-badge :count="errorCounts.warning" :number-style="{ backgroundColor: '#faad14' }" />
          <a-badge :count="errorCounts.info" :number-style="{ backgroundColor: '#1890ff' }" />
          <span style="color: #666;">Total: {{ errorCounts.total }}</span>
        </div>
      </div>

      <!-- Controls section -->
      <div style="display: flex; align-items: center; gap: 8px;">
        <a-tooltip title="Auto-scroll to newest errors">
          <a-switch
            size="small"
            v-model:checked="autoScroll"
          >
            <template #checkedChildren>Auto</template>
            <template #unCheckedChildren>Manual</template>
          </a-switch>
        </a-tooltip>

        <a-select
          size="small"
          v-model:value="filterLevel"
          style="width: 100px;"
          placeholder="Filter"
        >
          <a-select-option value="all">All Levels</a-select-option>
          <a-select-option value="error">Errors</a-select-option>
          <a-select-option value="warning">Warnings</a-select-option>
          <a-select-option value="info">Info</a-select-option>
        </a-select>

        <a-input
          size="small"
          placeholder="Search errors..."
          v-model:value="searchText"
          style="width: 150px;"
          allow-clear
        >
          <template #prefix><SearchOutlined /></template>
        </a-input>

        <a-space size="small">
          <a-tooltip title="Clear all errors">
            <a-button
              size="small"
              :icon="h(ClearOutlined)"
              @click="clearErrors"
              :disabled="errorCounts.total === 0"
            />
          </a-tooltip>
          
          <a-tooltip title="Export errors to CSV">
            <a-button
              size="small"
              :icon="h(DownloadOutlined)"
              @click="exportErrors"
              :disabled="errorCounts.total === 0"
            />
          </a-tooltip>
        </a-space>
      </div>
    </div>

    <!-- Connection status warning -->
    <div v-if="!connected" style="flex-shrink: 0; padding: 8px 12px; background: #fff2e6; border-bottom: 1px solid #ffd591; font-size: 12px; color: #d4680f;">
      <WarningOutlined style="margin-right: 6px;" />
      Not connected to dserv - error tracing unavailable
    </div>

    <!-- Main table area -->
    <div style="flex: 1; overflow: hidden;">
      <div v-if="filteredErrors.length === 0" style="height: 100%; display: flex; align-items: center; justify-content: center; flex-direction: column; color: #999; font-size: 14px;">
        <template v-if="globalErrors.length === 0">
          <BugOutlined style="font-size: 48px; margin-bottom: 16px; color: #d9d9d9;" />
          <div style="margin-bottom: 8px;">No errors captured yet</div>
          <div style="font-size: 12px;">
            {{ isTracing ? 'Error tracing is active globally' : 'Enable error tracing to capture Tcl errors' }}
          </div>
        </template>
        <template v-else>
          <FilterOutlined style="font-size: 48px; margin-bottom: 16px; color: #d9d9d9;" />
          <div>No errors match the current filter</div>
        </template>
      </div>

      <a-table
        v-else
        ref="errorTable"
        :columns="columns"
        :data-source="filteredErrors"
        size="small"
        row-key="id"
        :pagination="false"
        :scroll="{ y: '100%', x: 800 }"
        :row-class-name="getRowClassName"
        :expand-row-by-click="true"
        :expanded-row-keys="expandedRows"
        @expand="onRowExpand"
      >
        <template #bodyCell="{ column, record }">
          <template v-if="column.key === 'time'">
            <!-- Smaller font for time -->
            <span style="font-size: 10px; font-family: monospace; color: #666;">
              {{ record.timeString }}
            </span>
          </template>
          
          <template v-else-if="column.key === 'level'">
            <!-- Smaller tag for level -->
            <a-tag :color="getErrorLevelInfo(record.level).color" style="font-size: 9px; padding: 1px 4px; line-height: 1.2;">
              <template #icon>
                <component :is="getErrorLevelInfo(record.level).icon" style="font-size: 10px;" />
              </template>
              {{ record.level.toUpperCase() }}
            </a-tag>
          </template>
          
          <template v-else-if="column.key === 'category'">
            <!-- Smaller tag for category -->
            <a-tag color="default" style="font-size: 9px; padding: 1px 4px; line-height: 1.2;">
              {{ record.category }}
            </a-tag>
          </template>
          
          <template v-else-if="column.key === 'context'">
            <!-- More spacious context with better formatting -->
            <div style="font-size: 10px; font-family: monospace; display: flex; gap: 3px; align-items: center;">
              <a-tooltip :title="`System: ${record.system}`">
                <span style="color: #1890ff; cursor: help; font-weight: 500;">{{ record.system }}</span>
              </a-tooltip>
              <span style="color: #ccc; font-size: 8px;">/</span>
              <a-tooltip :title="`Protocol: ${record.protocol}`">
                <span style="color: #52c41a; cursor: help; font-weight: 500;">{{ record.protocol }}</span>
              </a-tooltip>
              <span style="color: #ccc; font-size: 8px;">/</span>
              <a-tooltip :title="`Variant: ${record.variant}`">
                <span style="color: #fa8c16; cursor: help; font-weight: 500;">{{ record.variant }}</span>
              </a-tooltip>
            </div>
          </template>
          
          <template v-else-if="column.key === 'text'">
            <div style="display: flex; align-items: center; gap: 6px; margin-left: 16px;">
              <!-- Error text with more left margin to give context even more space -->
              <div 
                style="font-family: monospace; font-size: 11px; max-width: 320px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; flex: 1; cursor: pointer;"
                @click="showErrorDetails(record)"
              >
                {{ getFirstLine(record.text) }}
              </div>
              
              <!-- Show expand/collapse button if multi-line -->
              <a-button
                v-if="isMultiLine(record.text)"
                size="small"
                type="text"
                :icon="h(expandedRows.includes(record.id) ? UpOutlined : DownOutlined)"
                @click.stop="toggleRowExpansion(record.id)"
                style="width: 18px; height: 14px; padding: 0; font-size: 9px;"
              />
              
              <!-- Detail button -->
              <a-tooltip title="View full details">
                <a-button
                  size="small"
                  type="text"
                  :icon="h(EllipsisOutlined)"
                  @click.stop="showErrorDetails(record)"
                  style="width: 18px; height: 14px; padding: 0; font-size: 9px;"
                />
              </a-tooltip>
            </div>
          </template>
        </template>

        <!-- Expandable row content for multi-line errors -->
        <template #expandedRowRender="{ record }">
          <div style="padding: 8px 16px; background: #f8f9fa; border-radius: 4px; margin: 4px 0;">
            <div style="font-weight: 500; margin-bottom: 8px; font-size: 11px; color: #666;">
              Full Error Message:
            </div>
            <div style="font-family: monospace; font-size: 11px; white-space: pre-wrap; max-height: 200px; overflow: auto; background: white; padding: 8px; border-radius: 2px; border: 1px solid #e8e8e8;">{{ record.text }}</div>
            
            <div v-if="record.stackTrace.length > 0" style="margin-top: 12px;">
              <div style="font-weight: 500; margin-bottom: 4px; font-size: 11px; color: #666;">
                Stack Trace:
              </div>
              <div style="font-family: monospace; font-size: 10px; background: white; padding: 8px; border-radius: 2px; border: 1px solid #e8e8e8; max-height: 120px; overflow: auto;">
                <div 
                  v-for="(trace, index) in record.stackTrace" 
                  :key="index" 
                  style="margin-bottom: 2px;"
                  :style="{ color: trace.isFile ? '#1890ff' : '#666' }"
                >
                  {{ trace.line }}
                </div>
              </div>
            </div>
          </div>
        </template>
      </a-table>
    </div>

    <!-- Status bar -->
    <div style="flex-shrink: 0; padding: 4px 12px; border-top: 1px solid #e8e8e8; background: #fafafa; font-size: 11px; color: #666; display: flex; justify-content: space-between; align-items: center;">
      <div>
        <span v-if="filteredErrors.length > 0">
          Showing {{ filteredErrors.length }} of {{ globalErrors.length }} errors
          <span v-if="searchText"> (filtered by "{{ searchText }}")</span>
        </span>
      </div>
      <div style="display: flex; align-items: center; gap: 12px;">
        <span>
          Status: {{ connected ? (isTracing ? 'Tracing Active Globally' : 'Connected') : 'Disconnected' }}
        </span>
        <span v-if="globalErrors.length > 0">
          Last error: {{ new Date(globalErrors[globalErrors.length - 1].timestamp).toLocaleTimeString() }}
        </span>
      </div>
    </div>

    <!-- Error details modal -->
    <a-modal
      v-model:open="showDetails"
      title="Error Details"
      width="800px"
      :footer="null"
    >
      <template #title>
        <div style="display: flex; align-items: center; gap: 8px;">
          <component v-if="selectedError" :is="getErrorLevelInfo(selectedError.level).icon" />
          <span>Error Details</span>
        </div>
      </template>

      <div v-if="selectedError">
        <a-card size="small" style="margin-bottom: 16px;">
          <div style="display: grid; grid-template-columns: auto 1fr; gap: 8px 16px; font-size: 12px;">
            <strong>Time:</strong><span>{{ selectedError.timeString }}</span>
            <strong>Level:</strong><span>{{ selectedError.level.toUpperCase() }}</span>
            <strong>Category:</strong><span>{{ selectedError.category }}</span>
            <strong>System:</strong><span>{{ selectedError.system }}</span>
            <strong>Protocol:</strong><span>{{ selectedError.protocol }}</span>
            <strong>Variant:</strong><span>{{ selectedError.variant }}</span>
          </div>
        </a-card>

        <div style="margin-bottom: 16px;">
          <div style="font-weight: 500; margin-bottom: 8px;">Error Message:</div>
          <div style="background: #f5f5f5; padding: 12px; font-size: 12px; font-family: monospace; white-space: pre-wrap; max-height: 200px; overflow: auto; border-radius: 4px;">
            {{ selectedError.text }}
          </div>
        </div>

        <div v-if="selectedError.stackTrace.length > 0">
          <div style="font-weight: 500; margin-bottom: 8px;">Stack Trace:</div>
          <div style="background: #f5f5f5; padding: 12px; font-size: 11px; font-family: monospace; max-height: 150px; overflow: auto; border-radius: 4px;">
            <div 
              v-for="(trace, index) in selectedError.stackTrace" 
              :key="index" 
              style="margin-bottom: 4px;"
              :style="{ color: trace.isFile ? '#1890ff' : '#666' }"
            >
              {{ trace.line }}
            </div>
          </div>
        </div>
      </div>

      <template #footer>
        <a-button @click="showDetails = false">Close</a-button>
      </template>
    </a-modal>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, nextTick, h, watch } from 'vue'
import { 
  ClearOutlined, 
  DownloadOutlined,
  BugOutlined,
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
  EllipsisOutlined
} from '@ant-design/icons-vue'
import { dserv } from '../services/dserv.js'
import { errorService } from '../services/errorService.js'

// Component state
const loading = ref(false)
const connected = ref(false)
const autoScroll = ref(true)
const filterLevel = ref('all')
const searchText = ref('')
const selectedError = ref(null)
const showDetails = ref(false)
const errorTable = ref(null)
const expandedRows = ref([]) // Track which rows are expanded

// Reactive references to global service data
const globalErrors = computed(() => errorService.getErrors())
const errorCounts = computed(() => errorService.getErrorCounts())
const isTracing = computed(() => errorService.isTracingActive())

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
  } catch (error) {
    console.error('Failed to toggle error tracing:', error)
  } finally {
    loading.value = false
  }
}

const clearErrors = () => {
  errorService.clearErrors()
  expandedRows.value = [] // Clear expanded rows when clearing errors
}

const exportErrors = () => {
  errorService.exportErrors()
}

// Filter errors
const filteredErrors = computed(() => {
  let filtered = globalErrors.value
  
  // Filter by level
  if (filterLevel.value !== 'all') {
    filtered = filtered.filter(error => error.level === filterLevel.value)
  }
  
  // Filter by search text
  if (searchText.value) {
    const search = searchText.value.toLowerCase()
    filtered = filtered.filter(error =>
      error.text.toLowerCase().includes(search) ||
      error.category.toLowerCase().includes(search) ||
      error.system.toLowerCase().includes(search) ||
      error.protocol.toLowerCase().includes(search) ||
      error.variant.toLowerCase().includes(search)
    )
  }
  
  return filtered
})

// Watch for new errors to auto-scroll
watch(globalErrors, () => {
  if (autoScroll.value) {
    nextTick(() => {
      scrollToBottom()
    })
  }
}, { deep: true })

// Scroll to bottom of table
const scrollToBottom = () => {
  if (errorTable.value) {
    const tableBody = errorTable.value.$el.querySelector('.ant-table-body')
    if (tableBody) {
      tableBody.scrollTop = tableBody.scrollHeight
    }
  }
}

// Get error level icon and color
const getErrorLevelInfo = (level) => {
  switch (level) {
    case 'error':
      return { icon: h(CloseCircleOutlined), color: '#ff4d4f' }
    case 'warning':
      return { icon: h(ExclamationCircleOutlined), color: '#faad14' }
    case 'info':
      return { icon: h(InfoCircleOutlined), color: '#1890ff' }
    default:
      return { icon: h(BugOutlined), color: '#8c8c8c' }
  }
}

// Show error details modal
const showErrorDetails = (error) => {
  selectedError.value = error
  showDetails.value = true
}

// Get row class name for styling
const getRowClassName = (record) => {
  switch (record.level) {
    case 'error': return 'error-row'
    case 'warning': return 'warning-row'  
    case 'info': return 'info-row'
    default: return ''
  }
}

// Table columns - optimized for compact display
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
    title: 'Context',
    key: 'context',
    width: 180
  },
  {
    title: 'Error Message',
    dataIndex: 'text',
    key: 'text',
    ellipsis: true
  }
]

// Component lifecycle
onMounted(() => {
  console.log('EssErrorConsole UI component mounted - using global error service')
  
  // Listen for connection changes
  const cleanup = dserv.registerComponent('EssErrorConsoleUI')
  
  dserv.on('connection', ({ connected: isConnected }) => {
    connected.value = isConnected
  }, 'EssErrorConsoleUI')

  // Set initial connection state
  connected.value = dserv.state.connected

  onUnmounted(() => {
    cleanup()
  })
})
</script>

<style scoped>
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

/* Table styling with size optimizations */
:deep(.ant-table-small .ant-table-tbody > tr > td) {
  padding: 3px 6px; /* Reduced padding */
  font-size: 10px; /* Smaller default font */
  vertical-align: top;
  line-height: 1.3;
}

:deep(.ant-table-small .ant-table-thead > tr > th) {
  padding: 3px 6px; /* Reduced padding */
  font-size: 10px; /* Smaller header font */
  font-weight: 500;
  background: #f5f5f5;
  line-height: 1.3;
}

/* Compact row height */
:deep(.ant-table-small .ant-table-tbody > tr) {
  height: 28px; /* Reduced from 32px */
}

/* Smaller table headers */
:deep(.ant-table-thead > tr > th) {
  font-size: 10px !important;
}

/* Compact tags */
:deep(.ant-tag) {
  margin: 0;
  border-radius: 2px;
}

/* Expandable row styling */
:deep(.ant-table-expanded-row > td) {
  padding: 0 !important;
}

/* Expand icon styling */
:deep(.ant-table-row-expand-icon) {
  width: 14px;
  height: 14px;
  line-height: 14px;
  font-size: 9px;
}
</style>