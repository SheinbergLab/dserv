<template>
  <div style="height: 100%; display: flex; flex-direction: row; padding: 8px; gap: 8px;">
    
    <!-- Left side: Table -->
    <div style="flex: 3; display: flex; flex-direction: column; min-width: 0;">
      <!-- Minimal header -->
      <div style="flex-shrink: 0; margin-bottom: 8px;">
        <div v-if="stimData && stimData.length > 0" style="font-size: 12px; color: #666;">
          <strong>{{ stimData.length }}</strong> trials
          <span v-if="arrayColumns.length > 0" style="margin-left: 12px;">
            <strong>Arrays:</strong> {{ arrayColumns.join(', ') }}
          </span>
        </div>
        <div v-else style="font-size: 12px; color: #666;">
          No stimulus data
        </div>
      </div>
      
      <!-- Table -->
      <div style="flex: 1; min-height: 0; overflow: hidden;">
        <template v-if="stimData && stimData.length > 0">
          <a-table
            :columns="tableColumns"
            :data-source="stimData"
            size="small"
            :pagination="false"
            :scroll="{ y: 280, x: 'max-content' }"
            sticky
            row-key="trial_index"
            :row-selection="{
              type: 'radio',
              selectedRowKeys: selectedRowKeys,
              onSelect: onRowSelect,
              hideSelectAll: true,
              columnWidth: 20
            }"
          >
          </a-table>
        </template>

        <!-- Simple empty state -->
        <div v-else style="height: 280px; display: flex; align-items: center; justify-content: center; color: #999;">
          <div style="text-align: center;">
            <div style="font-size: 16px; margin-bottom: 8px;">📊</div>
            <div>No stimulus data available</div>
          </div>
        </div>
      </div>
    </div>

    <!-- Right side: Tree view -->
    <div style="flex: 1; min-width: 150px; border-left: 1px solid #e8e8e8; padding-left: 6px;">
      <div style="font-size: 10px; color: #666; margin-bottom: 6px;">
        <strong>Trial {{ selectedTrial }} Details</strong>
      </div>
      
      <div v-if="selectedTrialData" style="height: 320px; overflow: auto; font-size: 9px; font-family: monospace;">
        <a-tree
          :tree-data="treeData"
          :show-line="true"
          :default-expand-all="false"
          :default-expanded-keys="defaultExpandedKeys"
        >
          <template #title="{ title, value, isLeaf }">
            <span v-if="isLeaf" style="color: #666;">
              {{ title }}: <span style="color: #1890ff;">{{ formatValue(value) }}</span>
            </span>
            <span v-else style="color: #333; font-weight: 500;">
              {{ title }}
            </span>
          </template>
        </a-tree>
      </div>
      
      <div v-else style="height: 320px; display: flex; align-items: center; justify-content: center; color: #999; text-align: center;">
        <div>
          <div style="font-size: 12px; margin-bottom: 4px;">👆</div>
          <div style="font-size: 9px;">Click a trial to view details</div>
        </div>
      </div>
    </div>

    <!-- Array detail modal -->
    <a-modal
      v-model:open="arrayModalVisible"
      :title="`${arrayModalTitle} - Trial ${arrayModalTrialIndex}`"
      width="80%"
      :footer="null"
    >
      <div style="max-height: 60vh; overflow: auto;">
        <template v-if="Array.isArray(arrayModalData)">
          <!-- Simple array display -->
          <div v-if="!isNestedArray(arrayModalData)" style="font-family: monospace; font-size: 12px;">
            <div v-for="(item, index) in arrayModalData" :key="index" style="margin-bottom: 2px;">
              [{{ index }}]: {{ item }}
            </div>
          </div>
          
          <!-- Nested array as table -->
          <a-table
            v-else
            :columns="nestedArrayColumns"
            :data-source="nestedArrayTableData"
            size="small"
            :pagination="false"
            :scroll="{ y: 400, x: 'max-content' }"
          />
        </template>
        <div v-else style="font-family: monospace; font-size: 12px;">
          {{ arrayModalData }}
        </div>
      </div>
    </a-modal>
  </div>
</template>

<script setup>
import { ref, computed, watch, onMounted, h } from 'vue'
import { dserv } from '../services/dserv.js'

// Reactive state
const rawStimData = ref(null)
const stimData = ref([])
const arrayColumns = ref([])
const arrayModalVisible = ref(false)
const arrayModalData = ref(null)
const arrayModalTitle = ref('')
const arrayModalTrialIndex = ref(0)

// Tree view state
const selectedRowKeys = ref([])
const selectedTrial = ref(0)
const selectedTrialData = ref(null)

// Tree functionality
function onRowSelect(record) {
  selectedRowKeys.value = [record.trial_index]
  selectedTrial.value = record.trial_index
  selectedTrialData.value = record
}

function formatValue(value) {
  if (typeof value === 'number') {
    return Number.isInteger(value) ? value.toString() : value.toFixed(3)
  }
  if (typeof value === 'string') {
    return `"${value}"`
  }
  return String(value)
}

function buildTreeData(data) {
  if (!data) return []
  
  const treeNodes = []
  let keyCounter = 0
  
  Object.entries(data).forEach(([key, value]) => {
    const nodeKey = `${keyCounter++}`
    
    if (Array.isArray(value) || (value && typeof value === 'object' && 'length' in value)) {
      // Array node
      const children = []
      for (let i = 0; i < value.length; i++) {
        children.push({
          title: `[${i}]`,
          key: `${nodeKey}-${i}`,
          value: value[i],
          isLeaf: true
        })
      }
      
      treeNodes.push({
        title: `${key} (${value.length})`,
        key: nodeKey,
        children: children
      })
    } else {
      // Simple value node
      treeNodes.push({
        title: key,
        key: nodeKey,
        value: value,
        isLeaf: true
      })
    }
  })
  
  return treeNodes
}

const treeData = computed(() => buildTreeData(selectedTrialData.value))
const defaultExpandedKeys = computed(() => {
  // Auto-expand array nodes that have few elements
  return treeData.value
    .filter(node => node.children && node.children.length <= 5)
    .map(node => node.key)
})

// Process hybrid JSON data
function processHybridData(hybridData) {
  if (!hybridData || !hybridData.rows || !hybridData.arrays) {
    return []
  }

  const { rows, arrays } = hybridData
  const arrayFields = Object.keys(arrays)
  
  return rows.map((row, index) => {
    const enhancedRow = { trial_index: index, ...row }
    
    // Replace array indices with actual array references
    arrayFields.forEach(fieldName => {
      if (fieldName in row && typeof row[fieldName] === 'number') {
        const arrayIndex = row[fieldName]
        const arrayData = arrays[fieldName][arrayIndex]
        enhancedRow[fieldName] = arrayData
      }
    })
    
    return enhancedRow
  })
}

// Generate table columns dynamically
const tableColumns = computed(() => {
  if (!stimData.value || stimData.value.length === 0) return []
  
  const sampleRow = stimData.value[0]
  const columns = []
  
  // Add trial index column first
  columns.push({
    title: 'Trial',
    dataIndex: 'trial_index',
    key: 'trial_index',
    width: 50,
    fixed: 'left'
  })
  
  // Add other columns
  Object.keys(sampleRow).forEach(key => {
    if (key === 'trial_index') return
    
    const sampleValue = sampleRow[key]
    const isArrayColumn = Array.isArray(sampleValue) || (sampleValue && typeof sampleValue === 'object' && 'length' in sampleValue)
    
    if (isArrayColumn && !arrayColumns.value.includes(key)) {
      arrayColumns.value.push(key)
    }
    
    columns.push({
      title: key, // Keep original JSON key name
      dataIndex: key,
      key: key,
      width: isArrayColumn ? 80 : 90,
      ellipsis: !isArrayColumn,
      customRender: isArrayColumn ? ({ text, record }) => {
        if (text && text.length !== undefined) {
          return h('span', {
            style: {
              color: '#666',
              fontSize: '11px',
              cursor: 'pointer',
              textDecoration: 'none'
            },
            onMouseenter: (e) => e.target.style.textDecoration = 'underline',
            onMouseleave: (e) => e.target.style.textDecoration = 'none',
            onClick: () => showArrayDetail(text, key, record.trial_index)
          }, `array[${text.length}]`)
        } else {
          return '—'
        }
      } : ({ text }) => {
        if (typeof text === 'number' && !Number.isInteger(text)) {
          return text.toFixed(3)
        }
        return text
      },
      sorter: !isArrayColumn ? (a, b) => {
        const aVal = a[key]
        const bVal = b[key]
        if (typeof aVal === 'number' && typeof bVal === 'number') {
          return aVal - bVal
        }
        return String(aVal).localeCompare(String(bVal))
      } : false
    })
  })
  
  return columns
})

// Data status indicator
const dataStatus = computed(() => {
  if (!dserv.state.connected) {
    return { color: 'red', text: 'Disconnected' }
  }
  if (!rawStimData.value) {
    return { color: 'orange', text: 'No Data' }
  }
  if (stimData.value && stimData.value.length > 0) {
    return { color: 'green', text: `${stimData.value.length} Trials` }
  }
  return { color: 'orange', text: 'Loading...' }
})

// Array detail modal functions
function showArrayDetail(arrayData, columnName, trialIndex) {
  arrayModalData.value = arrayData
  arrayModalTitle.value = columnName.replace(/_/g, ' ').replace(/\b\w/g, l => l.toUpperCase())
  arrayModalTrialIndex.value = trialIndex
  arrayModalVisible.value = true
}

function isNestedArray(arr) {
  return arr.length > 0 && Array.isArray(arr[0])
}

// For nested arrays, create table columns and data
const nestedArrayColumns = computed(() => {
  if (!Array.isArray(arrayModalData.value) || !isNestedArray(arrayModalData.value)) {
    return []
  }
  
  const firstRow = arrayModalData.value[0]
  if (!Array.isArray(firstRow)) return []
  
  const columns = [
    { title: 'Row', dataIndex: 'rowIndex', key: 'rowIndex', width: 60 }
  ]
  
  firstRow.forEach((_, colIndex) => {
    columns.push({
      title: `Col ${colIndex}`,
      dataIndex: `col_${colIndex}`,
      key: `col_${colIndex}`,
      width: 80
    })
  })
  
  return columns
})

const nestedArrayTableData = computed(() => {
  if (!Array.isArray(arrayModalData.value) || !isNestedArray(arrayModalData.value)) {
    return []
  }
  
  return arrayModalData.value.map((row, rowIndex) => {
    const rowData = { rowIndex, key: rowIndex }
    if (Array.isArray(row)) {
      row.forEach((cell, colIndex) => {
        rowData[`col_${colIndex}`] = cell
      })
    }
    return rowData
  })
})

// Handle stiminfo datapoint in dserv message handler
onMounted(async () => {
  // Add custom handler for stiminfo
  const originalHandleDatapoint = dserv.handleDatapoint.bind(dserv)
  
  dserv.handleDatapoint = function(data) {
    // Call original handler first
    originalHandleDatapoint(data)
    
    // Handle our custom datapoint
    if (data.name === 'ess/stiminfo') {
      try {
        rawStimData.value = JSON.parse(data.data)
        stimData.value = processHybridData(rawStimData.value)
        arrayColumns.value = [] // Reset array columns
        
        // Reset tree panel selection
        selectedRowKeys.value = []
        selectedTrial.value = 0
        selectedTrialData.value = null
        
        console.log('Processed stiminfo data:', stimData.value.length, 'trials')
      } catch (error) {
        console.error('Failed to parse stiminfo data:', error)
        rawStimData.value = null
        stimData.value = []
        
        // Reset on error too
        selectedRowKeys.value = []
        selectedTrial.value = 0
        selectedTrialData.value = null
      }
    }
  }
  
  // Query for existing stiminfo data if connected
  if (dserv.state.connected) {
    try {
      await dserv.essCommand('dservTouch ess/stiminfo')
      console.log('Requested current stiminfo data')
    } catch (error) {
      console.error('Failed to query stiminfo data:', error)
    }
  }
})

// Also query when connection is established
watch(() => dserv.state.connected, async (connected) => {
  if (connected) {
    try {
      await dserv.essCommand('dservTouch ess/stiminfo')
      console.log('Requested current stiminfo data on connect')
    } catch (error) {
      console.error('Failed to query stiminfo data on connect:', error)
    }
  }
})
</script>

<style scoped>
/* Ensure table cells don't wrap */
:deep(.ant-table-tbody > tr > td) {
  white-space: nowrap;
  padding: 2px 6px !important;
  line-height: 1.2;
}

:deep(.ant-table-thead > tr > th) {
  padding: 4px 6px !important;
  line-height: 1.2;
}

/* Style for array buttons */
:deep(.ant-btn-link) {
  color: #1890ff;
}

:deep(.ant-btn-link:hover) {
  color: #40a9ff;
}

/* Style for selected row highlight */
:deep(.selected-row) {
  background-color: #e6f7ff !important;
}

:deep(.selected-row:hover) {
  background-color: #bae7ff !important;
}

/* Tighter radio button column */
:deep(.ant-table-selection-column) {
  padding: 2px 4px !important;
}

/* Smaller radio buttons */
:deep(.ant-radio-wrapper) {
  font-size: 10px;
}

:deep(.ant-radio) {
  transform: scale(0.8);
}
</style>