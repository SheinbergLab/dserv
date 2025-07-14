// Enhanced BackupManager.vue with comprehensive debugging
<template>
  <div>
    <!-- Debug Panel (togglable) -->
    <a-collapse v-if="showDebug" style="margin-bottom: 16px;" size="small">
      <a-collapse-panel key="debug" header="Debug Information">
        <div style="font-family: monospace; font-size: 11px; background: #f5f5f5; padding: 8px; border-radius: 4px;">
          <div><strong>Script Type:</strong> {{ scriptType }}</div>
          <div><strong>Raw Backend Response:</strong></div>
          <pre style="margin: 4px 0; white-space: pre-wrap;">{{ debugInfo.rawResponse || 'No response yet' }}</pre>
          <div><strong>Parsed Files Count:</strong> {{ backups.length }}</div>
          <div><strong>Last Error:</strong> {{ debugInfo.lastError || 'None' }}</div>
          <div><strong>Backend Test:</strong>
            <a-button size="small" @click="testBackendConnection" :loading="debugInfo.testing">
              Test Backend
            </a-button>
            {{ debugInfo.backendStatus }}
          </div>
        </div>
      </a-collapse-panel>
    </a-collapse>

    <div style="margin-bottom: 16px;">
      <div style="display: flex; justify-content: space-between; align-items: center;">
        <div>
          <div style="font-weight: 500; margin-bottom: 8px;">
            Available Backups for {{ scriptType.toUpperCase() }} Script
          </div>
          <div style="font-size: 12px; color: #666;">
            Backups are automatically created before each save. Click to restore.
          </div>
        </div>
        <div style="display: flex; gap: 8px;">
          <a-button size="small" @click="showDebug = !showDebug">
            {{ showDebug ? 'Hide' : 'Show' }} Debug
          </a-button>
          <a-button size="small" @click="refreshBackups" :loading="loading">
            <ReloadOutlined /> Refresh
          </a-button>
        </div>
      </div>
    </div>

    <div v-if="loading" style="text-align: center; padding: 20px;">
      <a-spin size="large" />
      <div style="margin-top: 8px; font-size: 12px;">Loading backups...</div>
    </div>

    <div v-else-if="backups.length === 0" style="text-align: center; padding: 20px; color: #666;">
      <div>No backups available for this script.</div>
      <div style="font-size: 11px; margin-top: 8px;">
        Debug: {{ debugInfo.noBackupsReason }}
      </div>
    </div>

    <div v-else>
      <a-list
        :data-source="backups"
        size="small"
        :pagination="backups.length > 10 ? { pageSize: 10 } : false"
      >
        <template #renderItem="{ item }">
          <a-list-item style="padding: 8px 0;">
            <div style="width: 100%; display: flex; justify-content: space-between; align-items: center;">
              <div style="flex: 1;">
                <div style="font-weight: 500; font-size: 13px;">
                  {{ item.displayName }}
                </div>
                <div style="font-size: 11px; color: #666; margin-top: 2px;">
                  {{ item.formattedDate }} • {{ item.size }}
                </div>
                <div v-if="showDebug" style="font-size: 10px; color: #999; margin-top: 2px; font-family: monospace;">
                  Raw: {{ item.timestamp }} | Path: {{ item.path }}
                </div>
              </div>
              
              <div style="display: flex; gap: 8px;">
                <a-button
                  size="small"
                  @click="previewBackup(item)"
                  :loading="operationStates.previewing.has(item.path)"
                >
                  Preview
                </a-button>
                
                <a-button
                  type="primary"
                  size="small"
                  @click="confirmRestore(item)"
                  :loading="operationStates.restoring.has(item.path)"
                >
                  Restore
                </a-button>
                
                <a-button
                  danger
                  size="small"
                  @click="confirmDelete(item)"
                  :loading="operationStates.deleting.has(item.path)"
                >
                  Delete
                </a-button>
              </div>
            </div>
          </a-list-item>
        </template>
      </a-list>
    </div>

    <!-- Enhanced Preview Modal -->
    <a-modal
      v-model:open="showPreview"
      :title="`Preview: ${selectedBackup?.displayName || 'Backup'}`"
      width="900px"
      :footer="null"
    >
      <div v-if="previewLoading" style="text-align: center; padding: 40px;">
        <a-spin size="large" />
        <div style="margin-top: 8px;">Loading preview...</div>
      </div>
      
      <div v-else style="max-height: 500px; overflow: auto; background: #1e1e1e; color: #e2e8f0; padding: 12px; border-radius: 4px; font-family: monospace; font-size: 11px; white-space: pre-wrap;">
        {{ previewContent }}
      </div>
      
      <div v-if="showDebug && selectedBackup" style="margin-top: 8px; font-size: 10px; color: #666; border-top: 1px solid #f0f0f0; padding-top: 8px;">
        <strong>Debug Info:</strong><br>
        File Path: {{ selectedBackup.path }}<br>
        Timestamp: {{ selectedBackup.timestamp }}<br>
        Raw Filename: {{ selectedBackup.name }}
      </div>
      
      <!-- Modal footer buttons -->
      <template #footer>
        <div style="display: flex; justify-content: space-between;">
          <a-button @click="showPreview = false">
            Close
          </a-button>
          <a-button 
            v-if="selectedBackup"
            type="primary" 
            @click="confirmRestore(selectedBackup)"
            :loading="selectedBackup ? operationStates.restoring.has(selectedBackup.path) : false"
          >
            Restore This Version
          </a-button>
        </div>
      </template>
    </a-modal>
  </div>
</template>

<script setup>
import { ref, onMounted, reactive } from 'vue'
import { Modal, message } from 'ant-design-vue'
import { ReloadOutlined } from '@ant-design/icons-vue'
import { dserv } from '../services/dserv.js'

const props = defineProps({
  scriptType: {
    type: String,
    required: true
  }
})

const emit = defineEmits(['restore', 'close'])

const loading = ref(false)
const backups = ref([])
const showPreview = ref(false)
const selectedBackup = ref(null)
const previewContent = ref('')
const previewLoading = ref(false)
const showDebug = ref(false)

// Separate loading states for individual operations
const operationStates = reactive({
  previewing: new Set(),
  restoring: new Set(),
  deleting: new Set()
})

// Enhanced debug information
const debugInfo = reactive({
  rawResponse: '',
  lastError: '',
  backendStatus: '',
  testing: false,
  noBackupsReason: ''
})

onMounted(() => {
  loadBackups()
})

async function testBackendConnection() {
  debugInfo.testing = true
  try {
    // Test basic dserv connection
    const testResult = await dserv.essCommand('expr {1 + 1}')
    debugInfo.backendStatus = `✓ Connected (test result: ${testResult})`
    
    // Test if backup functions exist
    try {
      await dserv.essCommand('info commands ess::get_script_backups')
      debugInfo.backendStatus += ' | ✓ Backup commands available'
    } catch (e) {
      debugInfo.backendStatus += ' | ✗ Backup commands not found'
    }
  } catch (error) {
    debugInfo.backendStatus = `✗ Backend error: ${error.message}`
  } finally {
    debugInfo.testing = false
  }
}

async function loadBackups() {
  loading.value = true
  debugInfo.lastError = ''
  debugInfo.noBackupsReason = ''
  
  try {
    console.log(`Loading backups for script type: ${props.scriptType}`)
    
    // Enhanced backend call with better error handling
    const backupCommand = `ess::get_script_backups ${props.scriptType}`
    console.log(`Executing command: ${backupCommand}`)
    
    const backupFiles = await dserv.essCommand(backupCommand)
    debugInfo.rawResponse = `Type: ${typeof backupFiles}, Value: ${JSON.stringify(backupFiles, null, 2)}`
    
    console.log('Raw backup response:', backupFiles)
    console.log('Response type:', typeof backupFiles)
    console.log('Response length:', backupFiles?.length)
    
    // Handle different response types
    if (!backupFiles) {
      debugInfo.noBackupsReason = 'Backend returned null/undefined'
      backups.value = []
      return
    }
    
    // Handle Tcl list vs string vs array
    let fileList = []
    
    if (Array.isArray(backupFiles)) {
      // Already an array
      fileList = backupFiles
      console.log('Response is array:', fileList)
    } else if (typeof backupFiles === 'string') {
      const trimmedFiles = backupFiles.trim()
      if (!trimmedFiles) {
        debugInfo.noBackupsReason = 'Backend returned empty string'
        backups.value = []
        return
      }
      
      // Try different parsing methods for Tcl lists
      // Method 1: Split by whitespace (standard Tcl list)
      if (trimmedFiles.includes(' ')) {
        fileList = trimmedFiles.split(/\s+/).filter(f => f.trim())
        console.log('Parsed as space-separated list:', fileList)
      } 
      // Method 2: Split by newlines
      else if (trimmedFiles.includes('\n')) {
        fileList = trimmedFiles.split('\n').filter(f => f.trim())
        console.log('Parsed as newline-separated list:', fileList)
      }
      // Method 3: Single file
      else {
        fileList = [trimmedFiles]
        console.log('Parsed as single file:', fileList)
      }
    } else {
      debugInfo.noBackupsReason = `Backend returned unexpected type: ${typeof backupFiles}`
      backups.value = []
      return
    }
    
    if (fileList.length === 0) {
      debugInfo.noBackupsReason = 'No files after parsing'
      backups.value = []
      return
    }
    
    backups.value = fileList.map((file, index) => {
      const fileName = file.split('/').pop()
      console.log(`Processing file ${index}: ${fileName} (full path: ${file})`)
      
      // Enhanced timestamp extraction with multiple patterns
      const patterns = [
        /backup_(\d{8}_\d{6})_/,  // Standard format: backup_20250713_133513_
        /backup_(\d{14})_/,       // Alternative format: backup_20250713133513_
        /(\d{8}_\d{6})/,          // Anywhere in filename: 20250713_133513
        /(\d{14})/                // 14-digit timestamp: 20250713133513
      ]
      
      let timestamp = ''
      let match = null
      
      for (const pattern of patterns) {
        match = fileName.match(pattern)
        if (match) {
          timestamp = match[1]
          console.log(`Extracted timestamp "${timestamp}" using pattern ${pattern}`)
          break
        }
      }
      
      if (!timestamp) {
        console.warn(`Could not extract timestamp from: ${fileName}`)
        timestamp = 'unknown'
      }
      
      // Create simple object (not reactive)
      const backupItem = {
        path: file,
        name: fileName,
        displayName: fileName.replace(/^backup_\d+_\d+_/, '').replace(/\.tcl$/, ''),
        formattedDate: formatTimestamp(timestamp),
        size: 'Unknown',
        timestamp: timestamp,
        key: `backup-${index}` // Add unique key for Vue
      }
      
      console.log(`Created backup item:`, backupItem)
      return backupItem
    }).sort((a, b) => {
      // Sort by timestamp, most recent first
      if (a.timestamp === 'unknown' && b.timestamp === 'unknown') return 0
      if (a.timestamp === 'unknown') return 1
      if (b.timestamp === 'unknown') return -1
      return b.timestamp.localeCompare(a.timestamp)
    })
    
    console.log('Final processed backups:', backups.value)
    
  } catch (error) {
    console.error('Failed to load backups:', error)
    debugInfo.lastError = error.message
    debugInfo.noBackupsReason = `Error: ${error.message}`
    message.error('Failed to load backups: ' + error.message)
    backups.value = []
  } finally {
    loading.value = false
  }
}

function formatTimestamp(timestamp) {
  if (!timestamp || timestamp === 'unknown') {
    return 'Unknown date'
  }
  
  try {
    let dateStr = timestamp
    
    // Handle different timestamp formats
    if (timestamp.includes('_')) {
      // Format: YYYYMMDD_HHMMSS or YYMMDD_HHMMSS
      const [datePart, timePart] = timestamp.split('_')
      
      if (datePart.length === 8 && timePart.length === 6) {
        // Full year format: YYYYMMDD_HHMMSS
        const year = datePart.slice(0, 4)
        const month = datePart.slice(4, 6)
        const day = datePart.slice(6, 8)
        const hour = timePart.slice(0, 2)
        const minute = timePart.slice(2, 4)
        const second = timePart.slice(4, 6)
        
        dateStr = `${year}-${month}-${day}T${hour}:${minute}:${second}`
      } else if (datePart.length === 6 && timePart.length === 6) {
        // Short year format: YYMMDD_HHMMSS
        const year = '20' + datePart.slice(0, 2)
        const month = datePart.slice(2, 4)
        const day = datePart.slice(4, 6)
        const hour = timePart.slice(0, 2)
        const minute = timePart.slice(2, 4)
        const second = timePart.slice(4, 6)
        
        dateStr = `${year}-${month}-${day}T${hour}:${minute}:${second}`
      }
    } else if (timestamp.length === 14) {
      // Format: YYYYMMDDHHMMSS
      const year = timestamp.slice(0, 4)
      const month = timestamp.slice(4, 6)
      const day = timestamp.slice(6, 8)
      const hour = timestamp.slice(8, 10)
      const minute = timestamp.slice(10, 12)
      const second = timestamp.slice(12, 14)
      
      dateStr = `${year}-${month}-${day}T${hour}:${minute}:${second}`
    }
    
    console.log(`Timestamp conversion: "${timestamp}" -> "${dateStr}"`)
    
    const date = new Date(dateStr)
    
    if (isNaN(date.getTime())) {
      console.warn(`Invalid date created from timestamp: ${timestamp} -> ${dateStr}`)
      return `Invalid date (${timestamp})`
    }
    
    const formatted = date.toLocaleString()
    console.log(`Final formatted date: "${formatted}"`)
    return formatted
  } catch (error) {
    console.error('Error formatting timestamp:', timestamp, error)
    return `Error formatting (${timestamp})`
  }
}

async function previewBackup(backup) {
  const backupKey = backup.path
  if (operationStates.previewing.has(backupKey)) return
  
  try {
    operationStates.previewing.add(backupKey)
    previewLoading.value = true
    selectedBackup.value = backup
    
    console.log(`Previewing backup: ${backup.path}`)
    
    // Enhanced file reading with better error handling
    const readCommand = `
      if {[file exists {${backup.path}}]} {
        set f [open {${backup.path}} r]
        set content [read $f]
        close $f
        return $content
      } else {
        error "File does not exist: ${backup.path}"
      }
    `
    
    const content = await dserv.essCommand(readCommand)
    previewContent.value = content || '(Empty file)'
    showPreview.value = true
    
    console.log(`Preview loaded successfully, content length: ${content?.length || 0}`)
    
  } catch (error) {
    console.error('Failed to preview backup:', error)
    debugInfo.lastError = `Preview error: ${error.message}`
    
    // Show error details to user
    previewContent.value = `Error loading preview:\n\n${error.message}\n\nFile path: ${backup.path}`
    showPreview.value = true
    
  } finally {
    operationStates.previewing.delete(backupKey)
    previewLoading.value = false
  }
}

async function confirmRestore(backup) {
  const backupKey = backup.path
  if (operationStates.restoring.has(backupKey)) return
  
  Modal.confirm({
    title: 'Restore Backup',
    content: `Are you sure you want to restore from "${backup.displayName}"? This will overwrite the current script and cannot be undone.`,
    okText: 'Restore',
    okType: 'primary',
    cancelText: 'Cancel',
    async onOk() {
      operationStates.restoring.add(backupKey)
      try {
        console.log(`Restoring backup: ${backup.path}`)
        
        const restoreCommand = `ess::restore_script_backup ${props.scriptType} {${backup.path}}`
        await dserv.essCommand(restoreCommand)
        
        message.success('Script restored successfully')
        emit('restore', backup)
        emit('close')
      } catch (error) {
        console.error('Failed to restore backup:', error)
        debugInfo.lastError = `Restore error: ${error.message}`
        message.error(`Failed to restore backup: ${error.message}`)
      } finally {
        operationStates.restoring.delete(backupKey)
      }
    }
  })
  
  // Close preview if open
  showPreview.value = false
}

async function confirmDelete(backup) {
  const backupKey = backup.path
  if (operationStates.deleting.has(backupKey)) return
  
  Modal.confirm({
    title: 'Delete Backup',
    content: `Are you sure you want to delete "${backup.displayName}"? This action cannot be undone.`,
    okText: 'Delete',
    okType: 'danger',
    cancelText: 'Cancel',
    async onOk() {
      operationStates.deleting.add(backupKey)
      try {
        console.log(`Deleting backup: ${backup.path}`)
        
        // Enhanced delete command with existence check
        const deleteCommand = `
          if {[file exists {${backup.path}}]} {
            file delete {${backup.path}}
            return "deleted"
          } else {
            error "File does not exist: ${backup.path}"
          }
        `
        
        await dserv.essCommand(deleteCommand)
        message.success('Backup deleted successfully')
        
        // Remove from local list
        const index = backups.value.findIndex(b => b.path === backup.path)
        if (index !== -1) {
          backups.value.splice(index, 1)
        }
        
      } catch (error) {
        console.error('Failed to delete backup:', error)
        debugInfo.lastError = `Delete error: ${error.message}`
        message.error(`Failed to delete backup: ${error.message}`)
      } finally {
        operationStates.deleting.delete(backupKey)
      }
    }
  })
}

function refreshBackups() {
  loadBackups()
}
</script>

<style scoped>
:deep(.ant-list-item) {
  border-bottom: 1px solid #f0f0f0;
}

:deep(.ant-list-item:last-child) {
  border-bottom: none;
}

:deep(.ant-collapse-header) {
  font-size: 11px !important;
  padding: 4px 8px !important;
}

:deep(.ant-collapse-content-box) {
  padding: 8px !important;
}
</style>