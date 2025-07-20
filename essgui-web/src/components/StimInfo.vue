<template>
  <div style="height: 100%; display: flex; flex-direction: row; padding: 8px; gap: 8px; overflow: hidden;">
    
    <!-- Left side: Table -->
    <div style="flex: 3; display: flex; flex-direction: column; min-width: 0; height: 100%; overflow: hidden;">
      <!-- Header with load button -->
      <div style="flex-shrink: 0; margin-bottom: 8px; display: flex; justify-content: space-between; align-items: center;">
        <div style="font-size: 12px; color: #666;">
          <template v-if="isLoading">
            <a-spin size="small" style="margin-right: 8px;" />
            Loading stimulus data...
          </template>
          <template v-else-if="stimData && stimData.length > 0">
            <strong>{{ stimData.length }}</strong> trials
            <span v-if="arrayColumns.length > 0" style="margin-left: 12px;">
              <strong>Arrays:</strong> {{ arrayColumns.join(', ') }}
            </span>
          </template>
          <template v-else-if="hasRequestedData">
            No stimulus data
          </template>
          <template v-else>
            Click "Show Stim Info" to view stimulus information
          </template>
        </div>
        
        <div style="display: flex; align-items: center; gap: 8px;">
          <a-checkbox 
            v-model:checked="autoLoad" 
            size="small"
            style="font-size: 11px;"
          >
            Auto
          </a-checkbox>
          
          <a-button 
            v-if="!hasRequestedData || stimData.length > 0"
            type="primary" 
            size="small" 
            @click="loadStimData"
            :loading="isLoading"
          >
            {{ hasRequestedData ? 'Refresh' : 'Show Stim Info' }}
          </a-button>
        </div>
      </div>
      
      <!-- Table -->
      <div style="flex: 1; min-height: 0; overflow: hidden; display: flex; flex-direction: column;">
        <template v-if="isLoading">
          <div style="flex: 1; display: flex; align-items: center; justify-content: center;">
            <a-spin size="large" />
          </div>
        </template>
        
        <template v-else-if="stimData && stimData.length > 0">
          <div ref="tableContainerRef" style="flex: 1; overflow: hidden; position: relative;">
            <a-table
              :columns="tableColumns"
              :data-source="stimData"
              size="small"
              :pagination="false"
              :scroll="{ y: tableScrollHeight, x: 'max-content' }"
              sticky
              row-key="trial_index"
              :row-selection="{
                type: 'radio',
                selectedRowKeys: selectedRowKeys,
                onSelect: onRowSelect,
                hideSelectAll: true,
                columnWidth: 20
              }"
              style="height: 100%;"
            >
            </a-table>
          </div>
        </template>

        <template v-else>
          <div style="flex: 1; display: flex; align-items: center; justify-content: center; color: #999;">
            <div style="text-align: center;">
              <div style="font-size: 24px; margin-bottom: 8px;">📋</div>
              <div style="margin-bottom: 12px;">
                {{ hasRequestedData ? 'No stimulus data available' : 'Click "Show Stim Info" to view trial information' }}
              </div>
              <a-button 
                v-if="!hasRequestedData"
                type="primary" 
                @click="loadStimData" 
                :loading="isLoading"
              >
                Show Stim Info
              </a-button>
            </div>
          </div>
        </template>
      </div>
    </div>

    <!-- Right side: Tree view -->
    <div style="flex: 1; min-width: 150px; border-left: 1px solid #e8e8e8; padding-left: 6px; height: 100%; display: flex; flex-direction: column; overflow: hidden;">
      <div style="flex-shrink: 0; font-size: 10px; color: #666; margin-bottom: 6px;">
        <strong>Trial {{ selectedTrial }} Details</strong>
      </div>
      
      <div v-if="selectedTrialData" style="flex: 1; overflow: auto; font-size: 9px; font-family: monospace;">
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
      
      <div v-else style="flex: 1; display: flex; align-items: center; justify-content: center; color: #999; text-align: center;">
        <div>
          <div style="font-size: 12px; margin-bottom: 4px;">👆</div>
          <div style="font-size: 9px;">
            {{ hasRequestedData ? 'Click a trial to view details' : 'Load data to view trial details' }}
          </div>
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
          <div v-if="!isNestedArray(arrayModalData)" style="font-family: monospace; font-size: 12px;">
            <div v-for="(item, index) in arrayModalData" :key="index" style="margin-bottom: 2px;">
              [{{ index }}]: {{ item }}
            </div>
          </div>
          
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
import { ref, computed, onMounted, onUnmounted, h, nextTick } from 'vue'
import { dserv } from '../services/dserv.js'

// ============================================================================
// REACTIVE STATE (Enhanced with caching)
// ============================================================================

const rawStimData = ref(null)
const tableContainerRef = ref(null)
const tableScrollHeight = ref(400)
const stimData = ref([])
const arrayColumns = ref([])

// NEW: Caching state for two-tier approach
const expandedRowData = ref(new Map())
const expandedCache = ref(new Map())
const arrayMetadata = ref(new Map())

// Existing modal state
const arrayModalVisible = ref(false)
const arrayModalData = ref(null)
const arrayModalTitle = ref('')
const arrayModalTrialIndex = ref(0)

// Loading state (unchanged)
const isLoading = ref(false)
const hasRequestedData = ref(false)
const autoLoad = ref(true) // CHANGED: Default to true for "always show"
const autoLoadTimeoutId = ref(null)
const resetTimeoutId = ref(null)

// Tree view state (unchanged)
const selectedRowKeys = ref([])
const selectedTrial = ref(0)
const selectedTrialData = ref(null)

// ============================================================================
// CORE DATA PROCESSING (New two-tier approach)
// ============================================================================

// TIER 1: Ultra-fast summary processing - minimal metadata only
function processHybridDataSummary(hybridData) {
  console.time('Ultra-fast processing');
  
  if (!hybridData || !hybridData.rows || !hybridData.arrays) {
    console.error('Invalid hybrid data format');
    return [];
  }

  const { rows, arrays } = hybridData;
  const arrayFields = Object.keys(arrays);
  
  // Pre-compute array sizes ONCE (not reactive for performance)
  const arraySizeMaps = {};
  arrayFields.forEach(fieldName => {
    const arrayCollection = arrays[fieldName];
    arraySizeMaps[fieldName] = arrayCollection.map(arr => 
      Array.isArray(arr) ? arr.length : 0
    );
    
    // Store minimal metadata (non-reactive)
    arrayMetadata.value.set(fieldName, {
      totalArrays: arrayCollection.length,
      hasNestedArrays: arrayCollection.length > 0 && 
                     Array.isArray(arrayCollection[0]) && 
                     Array.isArray(arrayCollection[0][0])
    });
  });

  // Create ultra-lightweight summary rows
  const summaryRows = rows.map((row, index) => {
    // Start with minimal object
    const summaryRow = { trial_index: index };
    
    // Copy non-array fields directly
    Object.keys(row).forEach(key => {
      if (!arrayFields.includes(key)) {
        summaryRow[key] = row[key];
      } else if (typeof row[key] === 'number') {
        // For array fields, store just the index and size as simple properties
        const arrayIndex = row[key];
        const size = arraySizeMaps[key][arrayIndex] || 0;
        
        // Use simple object (not reactive metadata object)
        summaryRow[key] = `ARRAY_REF:${arrayIndex}:${size}:${key}`;
      }
    });
    
    return summaryRow;
  });

  console.timeEnd('Ultra-fast processing');
  console.log(`Ultra-fast processed: ${summaryRows.length} trials, ${arrayFields.length} array fields`);
  
  return summaryRows;
}

// TIER 2: On-demand expansion for tree view (entire row) - Throttled
function expandRowData(trialIndex) {
  if (expandedRowData.value.has(trialIndex)) {
    return expandedRowData.value.get(trialIndex);
  }

  console.time(`Expand row ${trialIndex}`);
  
  const row = rawStimData.value.rows[trialIndex];
  const { arrays } = rawStimData.value;
  const expandedRow = { trial_index: trialIndex, ...row };
  
  // Expand array fields but limit depth for performance
  Object.keys(arrays).forEach(fieldName => {
    if (fieldName in row && typeof row[fieldName] === 'number') {
      const arrayIndex = row[fieldName];
      const arrayData = arrays[fieldName][arrayIndex];
      
      // For very large arrays, just store reference for tree view
      // CONFIGURABLE: Threshold for "large" arrays
      const LARGE_ARRAY_THRESHOLD = 100; // Was 1000, now 100
      const PREVIEW_SIZE = 5; // Was 10, now 5
      
      if (Array.isArray(arrayData) && arrayData.length > LARGE_ARRAY_THRESHOLD) {
        expandedRow[fieldName] = {
          _isLargeArray: true,
          _size: arrayData.length,
          _preview: arrayData.slice(0, PREVIEW_SIZE), // Just first 5 items
          _fullData: arrayData // Keep reference for modal
        };
      } else {
        expandedRow[fieldName] = arrayData;
      }
    }
  });
  
  expandedRowData.value.set(trialIndex, expandedRow);
  console.timeEnd(`Expand row ${trialIndex}`);
  
  return expandedRow;
}

// TIER 2: On-demand expansion for specific arrays (modal view)
function expandSingleArray(trialIndex, fieldName) {
  const cacheKey = `${trialIndex}:${fieldName}`;
  
  if (expandedCache.value.has(cacheKey)) {
    return expandedCache.value.get(cacheKey);
  }

  const row = rawStimData.value.rows[trialIndex];
  const { arrays } = rawStimData.value;
  
  if (fieldName in row && typeof row[fieldName] === 'number') {
    const arrayIndex = row[fieldName];
    const arrayData = arrays[fieldName][arrayIndex];
    expandedCache.value.set(cacheKey, arrayData);
    return arrayData;
  }
  
  return null;
}

// Clear all caches when new data loads
function clearCaches() {
  expandedRowData.value.clear();
  expandedCache.value.clear();
  arrayMetadata.value.clear();
}

// ============================================================================
// TREE FUNCTIONALITY (Updated to use expanded data)
// ============================================================================

function onRowSelect(record) {
  selectedRowKeys.value = [record.trial_index]
  selectedTrial.value = record.trial_index
  
  // Expand the row data for tree view (async to avoid blocking)
  nextTick(() => {
    selectedTrialData.value = expandRowData(record.trial_index);
  });
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
  
  // CONFIGURABLE: Tree view limits for performance
  const MAX_TREE_ITEMS = 50; // Back to 50 for better single-trial exploration
  
  Object.entries(data).forEach(([key, value]) => {
    const nodeKey = `${keyCounter++}`
    
    // Handle large arrays specially
    if (value && value._isLargeArray) {
      treeNodes.push({
        title: `${key} (${value._size} items - showing first ${value._preview.length})`,
        key: nodeKey,
        children: value._preview.map((item, i) => ({
          title: `[${i}]`,
          key: `${nodeKey}-${i}`,
          value: item,
          isLeaf: true
        }))
      });
    } else if (Array.isArray(value) || (value && typeof value === 'object' && 'length' in value)) {
      // Limit tree display for performance
      const actualLength = value.length;
      const itemsToShow = Math.min(actualLength, MAX_TREE_ITEMS);
      
      const children = []
      for (let i = 0; i < itemsToShow; i++) {
        children.push({
          title: `[${i}]`,
          key: `${nodeKey}-${i}`,
          value: value[i],
          isLeaf: true
        })
      }
      
      if (actualLength > MAX_TREE_ITEMS) {
        children.push({
          title: `... and ${actualLength - MAX_TREE_ITEMS} more items`,
          key: `${nodeKey}-more`,
          value: `(${actualLength - MAX_TREE_ITEMS} additional items)`,
          isLeaf: true
        });
      }
      
      treeNodes.push({
        title: `${key} (${actualLength})`,
        key: nodeKey,
        children: children
      })
    } else {
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
  return treeData.value
    .filter(node => node.children && node.children.length <= 5)
    .map(node => node.key)
})

// ============================================================================
// TABLE COLUMNS (Updated for ultra-fast summary view)
// ============================================================================

const tableColumns = computed(() => {
  if (!stimData.value || stimData.value.length === 0) return []
  
  const sampleRow = stimData.value[0]
  const columns = []
  
  columns.push({
    title: 'Trial',
    dataIndex: 'trial_index',
    key: 'trial_index',
    width: 50,
    fixed: 'left'
  })
  
  Object.keys(sampleRow).forEach(key => {
    if (key === 'trial_index') return
    
    const sampleValue = sampleRow[key]
    const isArrayRef = typeof sampleValue === 'string' && sampleValue.startsWith('ARRAY_REF:')
    
    if (isArrayRef) {
      if (!arrayColumns.value.includes(key)) {
        arrayColumns.value.push(key)
      }
      
      columns.push({
        title: key,
        dataIndex: key,
        key: key,
        width: 100,
        customRender: ({ text, record }) => {
          if (!text || !text.startsWith('ARRAY_REF:')) return '—'
          
          // Parse: "ARRAY_REF:arrayIndex:size:fieldName"
          const [, arrayIndex, size, fieldName] = text.split(':')
          const metadata = arrayMetadata.value.get(fieldName)
          const typeIndicator = metadata?.hasNestedArrays ? 'nested' : 'array'
          
          return h('span', {
            style: {
              color: '#666',
              fontSize: '11px',
              cursor: 'pointer',
              textDecoration: 'none',
              transition: 'color 0.2s ease'
            },
            onMouseenter: (e) => {
              e.target.style.color = '#1890ff'
              e.target.style.textDecoration = 'underline'
            },
            onMouseleave: (e) => {
              e.target.style.color = '#666'
              e.target.style.textDecoration = 'none'
            },
            onClick: () => handleArrayExpand(record.trial_index, fieldName)
          }, `${typeIndicator}[${size}] →`)
        }
      })
    } else {
      // Regular columns (unchanged)
      columns.push({
        title: key,
        dataIndex: key,
        key: key,
        width: 90,
        ellipsis: true,
        customRender: ({ text }) => {
          if (typeof text === 'number' && !Number.isInteger(text)) {
            return text.toFixed(3)
          }
          return text
        },
        sorter: (a, b) => {
          const aVal = a[key]
          const bVal = b[key]
          if (typeof aVal === 'number' && typeof bVal === 'number') {
            return aVal - bVal
          }
          return String(aVal).localeCompare(String(bVal))
        }
      })
    }
  })
  
  return columns
})

// ============================================================================
// ARRAY EXPANSION HANDLERS
// ============================================================================

function handleArrayExpand(trialIndex, fieldName) {
  console.log(`Expanding array: trial ${trialIndex}, field ${fieldName}`);
  
  try {
    const arrayData = expandSingleArray(trialIndex, fieldName);
    if (arrayData) {
      showArrayDetail(arrayData, fieldName, trialIndex);
    } else {
      console.warn(`No array data found for trial ${trialIndex}, field ${fieldName}`);
    }
  } catch (error) {
    console.error('Failed to expand array:', error);
  }
}

// Array detail modal functions (unchanged)
function showArrayDetail(arrayData, columnName, trialIndex) {
  arrayModalData.value = arrayData
  arrayModalTitle.value = columnName.replace(/_/g, ' ').replace(/\b\w/g, l => l.toUpperCase())
  arrayModalTrialIndex.value = trialIndex
  arrayModalVisible.value = true
}

function isNestedArray(arr) {
  return arr.length > 0 && Array.isArray(arr[0])
}

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

// ============================================================================
// DATA LOADING (Updated to use summary processing)
// ============================================================================

async function loadStimData() {
  if (isLoading.value) return
  
  console.log('Loading stimulus data...')
  isLoading.value = true
  hasRequestedData.value = true
  
  try {
    await dserv.essCommand('dservTouch ess/stiminfo')
    console.log('Requested stimulus data')
  } catch (error) {
    console.error('Failed to request stimulus data:', error)
    isLoading.value = false
  }
}

// ============================================================================
// UI MANAGEMENT (unchanged)
// ============================================================================

function updateTableHeight() {
  if (tableContainerRef.value) {
    const containerHeight = tableContainerRef.value.clientHeight
    tableScrollHeight.value = Math.max(200, containerHeight - 40)
    console.log('Updated table scroll height:', tableScrollHeight.value)
  }
}

let resizeObserver = null
function setupResizeObserver() {
  if (tableContainerRef.value && window.ResizeObserver) {
    resizeObserver = new ResizeObserver(() => {
      updateTableHeight()
    })
    resizeObserver.observe(tableContainerRef.value)
  }
}

// Helper function to reset stimulus data state with debouncing
function resetStimDataState() {
  // IMMEDIATELY reset hasRequestedData in manual mode to prevent processing unexpected data
  if (!autoLoad.value) {
    console.log('Manual mode: immediately resetting request state to prevent processing unexpected data')
    hasRequestedData.value = false
  }
  
  // Clear any existing reset timeout
  if (resetTimeoutId.value) {
    clearTimeout(resetTimeoutId.value)
  }
  
  // Debounce the reset to handle rapid system/protocol/variant changes
  resetTimeoutId.value = setTimeout(() => {
    console.log('Executing debounced reset of stimulus data state')
    
    selectedRowKeys.value = []
    selectedTrial.value = 0
    selectedTrialData.value = null
    
    // NEW: Clear caches
    clearCaches()
    
    // Clear any existing auto-load timeout
    if (autoLoadTimeoutId.value) {
      clearTimeout(autoLoadTimeoutId.value)
    }
    
    if (autoLoad.value && dserv.state.connected) {
      // Auto-load mode: keep existing data visible, start loading new data
      console.log('Scheduling auto-load of stimulus data')
      autoLoadTimeoutId.value = setTimeout(() => {
        console.log('Auto-loading stimulus data after configuration change')
        loadStimData()
        autoLoadTimeoutId.value = null
      }, 200) // Shorter delay since we already debounced the reset
    } else {
      // Manual mode: clear data (hasRequestedData already reset above)
      console.log('Manual mode: clearing data')
      isLoading.value = false
      stimData.value = []
      rawStimData.value = null
      arrayColumns.value = []
    }
    
    resetTimeoutId.value = null
  }, 300) // Wait 300ms for all related changes to settle
}

// ============================================================================
// COMPONENT LIFECYCLE (Updated data handler)
// ============================================================================

onMounted(() => {
  console.log('StimInfo component mounted')
  
  // Register component with clean lifecycle management
  const cleanup = dserv.registerComponent('StimInfo', {
    subscriptions: [
      { pattern: 'ess/stiminfo', every: 1 }
    ]
  })
  
  // UPDATED: Event-based data handling with summary processing
  dserv.on('datapoint:ess/stiminfo', (data) => {
    console.log('Received stiminfo data (autoLoad:', autoLoad.value, 'hasRequestedData:', hasRequestedData.value, 'isLoading:', isLoading.value, ')')
    
    // Only process if we're expecting data OR if autoLoad is enabled
    if (isLoading.value || hasRequestedData.value || autoLoad.value) {
      try {
        rawStimData.value = JSON.parse(data.data)
        
        // NEW: Use summary processing instead of full processing
        stimData.value = processHybridDataSummary(rawStimData.value)
        
        // Clear state
        arrayColumns.value = []
        selectedRowKeys.value = []
        selectedTrial.value = 0
        selectedTrialData.value = null
        
        // Mark as requested since we received and processed data
        hasRequestedData.value = true
        
        console.log('Processed stiminfo summary:', stimData.value.length, 'trials')
        
        // Update table height after data loads
        nextTick(() => {
          updateTableHeight()
          setupResizeObserver()
        })
      } catch (error) {
        console.error('Failed to parse stiminfo data:', error)
        stimData.value = []
      } finally {
        isLoading.value = false
      }
    } else {
      console.log('Ignoring unexpected stiminfo data (not loading, not requested, autoLoad disabled)')
    }
  })
  
  // Listen for system changes - reset when loading finishes (unchanged)
  dserv.on('systemState', ({ loading }) => {
    if (!loading) {
      console.log('System loading completed, checking if reset needed')
      setTimeout(() => {
        resetStimDataState()
      }, 100)
    }
  })
  
  // Handle reconnection scenarios (unchanged)
  dserv.on('connection', ({ connected }) => {
    if (connected && isLoading.value && hasRequestedData.value) {
      console.log('Reconnected while loading, re-requesting stimulus data')
      loadStimData()
    } else if (connected) {
      console.log('Connected but not auto-loading (isLoading:', isLoading.value, 'hasRequestedData:', hasRequestedData.value, ')')
    }
  })
  
  // Set up window resize listener
  window.addEventListener('resize', updateTableHeight)
  
  // Cleanup on unmount
  onUnmounted(() => {
    cleanup()
    window.removeEventListener('resize', updateTableHeight)
    if (resizeObserver) {
      resizeObserver.disconnect()
    }
  })
})
</script>

<style scoped>
/* Ensure the table wrapper respects its container */
:deep(.ant-table-wrapper) {
  height: 100%;
}

/* Existing styles */
:deep(.ant-table-tbody > tr > td) {
  white-space: nowrap;
  padding: 2px 6px !important;
  line-height: 1.2;
}

:deep(.ant-table-thead > tr > th) {
  padding: 4px 6px !important;
  line-height: 1.2;
}

:deep(.ant-btn-link) {
  color: #1890ff;
}

:deep(.ant-btn-link:hover) {
  color: #40a9ff;
}

:deep(.selected-row) {
  background-color: #e6f7ff !important;
}

:deep(.selected-row:hover) {
  background-color: #bae7ff !important;
}

:deep(.ant-table-selection-column) {
  padding: 2px 4px !important;
}

:deep(.ant-radio-wrapper) {
  font-size: 10px;
}

:deep(.ant-radio) {
  transform: scale(0.8);
}
</style>