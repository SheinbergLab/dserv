<template>
  <div style="height: 100%; display: flex; flex-direction: column; padding: 8px; overflow: hidden;">
    <!-- Clean Header - Just the title -->
    <div style="flex-shrink: 0; display: flex; align-items: center; justify-content: center; margin-bottom: 12px; padding: 4px 0;">
      <div style="font-weight: 500; font-size: 14px;">ESS Control</div>
    </div>

    <!-- FIXED SECTION - Always visible essential controls with FIXED HEIGHTS -->
    <div style="flex-shrink: 0; margin-bottom: 12px; padding: 0 4px; display: flex; flex-direction: column; align-items: center;">
      <!-- Subject Selection - FIXED HEIGHT -->
      <div class="system-config subject-section" style="background: #f8f9fa; border: 1px solid #e8e8e8; border-radius: 6px; padding: 4px; margin-bottom: 8px; width: 100%; max-width: 100%; min-height: 40px;">
        <a-form-item label="Subject" style="margin-bottom: 0;" class="subject-item">
          <a-select
            v-model:value="dserv.state.subject"
            size="small"
            style="flex: 1; min-width: 140px;"
            :disabled="isSystemBusy"
            @change="setSubject"
						>
            <a-select-option v-for="subject in subjects" :key="subject" :value="subject">
              {{ subject }}
            </a-select-option>
          </a-select>
        </a-form-item>
      </div>

      <!-- Control Buttons and Status Display - FIXED HEIGHT -->
      <div style="background: #f8f9fa; border: 1px solid #e8e8e8; border-radius: 8px; padding: 8px; margin-bottom: 12px; width: 100%; max-width: 100%; min-height: 120px;">
        <!-- Control Buttons -->
        <div class="control-buttons" style="margin-bottom: 8px;">
          <a-space size="small">
            <a-button
              type="primary"
              size="small"
              :loading="loading.start"
              :disabled="isSystemBusy || dserv.state.status === 'Running' || !dserv.state.currentVariant"
              @click="startExperiment"
							>
              Go
            </a-button>
            <a-button
              danger
              size="small"
              :loading="loading.stop"
              :disabled="isSystemBusy || dserv.state.status !== 'Running'"
              @click="stopExperiment"
							>
              Stop
            </a-button>
            <a-button
              size="small"
              :loading="loading.reset"
              :disabled="isSystemBusy || dserv.state.status === 'Running'"
              @click="resetExperiment"
							>
              Reset
            </a-button>
          </a-space>
        </div>

        <!-- Status Display - FIXED LAYOUT -->
        <div style="font-size: 12px; min-height: 72px;">
          <div style="display: flex; align-items: center; gap: 8px; margin-bottom: 4px; height: 20px;">
            <div class="obs-indicator" :class="{ 'is-active': dserv.state.inObs }"></div>
            <div style="display: flex; align-items: center; gap: 8px;">
              <span style="width: 50px; text-align: right; white-space: nowrap;"><strong>Status:</strong></span>
              <a-tag
                :color="getStatusColor(dserv.state.status)"
                style="margin: 0; min-width: 60px; text-align: center;"
								>
                {{ dserv.state.status }}
              </a-tag>
            </div>
          </div>

          <!-- Additional status info aligned with Status line -->
          <div style="padding-left: 20px; min-height: 48px;">
            <!-- Show loading indicator when system is busy -->
						<div
							style="display: flex; align-items: center; gap: 8px; margin-bottom: 4px; height: 20px;"
							>
							<span style="width: 50px; text-align: right; white-space: nowrap;"><strong>Obs:</strong></span>
							<a-tag
								:color="dserv.state.inObs ? 'red' : 'default'"
								style="margin: 0; min-width: 60px; text-align: center; height: 22px; line-height: 1; display: flex; align-items: center; justify-content: center; padding: 0 4px; box-sizing: border-box; font-size: 12px;"
								>
								{{ dserv.state.obsCount || 'None' }}
							</a-tag>
						</div>


						<div style="display: flex; align-items: center; gap: 4px; min-height: 16px;">
							<span style="width: 50px; text-align: right; white-space: nowrap;"><strong>File:</strong></span>
							<div style="flex: 1; display: flex; align-items: center; gap: 2px;">
								<span v-if="currentDatafile" style="color: green; font-family: monospace; font-size: 10px; flex: 1; overflow: hidden; text-overflow: ellipsis;">
									{{ currentDatafile.split('/').pop().replace('.ess', '') }}
								</span>
								<span v-else style="color: #999; font-size: 10px; flex: 1;">No file</span>
								<a-button
									size="small"
									:icon="h(currentDatafile ? CloseOutlined : FolderOpenOutlined)"
									:type="currentDatafile ? 'default' : 'primary'"
									:danger="!!currentDatafile"
									:disabled="isSystemBusy"
									@click="currentDatafile ? closeDatafile() : openDatafile()"
									:loading="datafileLoading"
									style="width: 16px; height: 16px; font-size: 8px;"
									/>
							</div>
						</div>


          </div>
        </div>
      </div>

      <!-- System Configuration -->
      <div class="system-config" style="background: #f8f9fa; border: 1px solid #e8e8e8; border-radius: 6px; padding: 6px; margin-bottom: 8px; width: 100%; max-width: 100%; min-height: 140px;">
        <div style="font-weight: 500; margin-bottom: 6px; font-size: 12px; min-height: 18px; display: flex; align-items: center;">
          System Configuration
          <a-spin v-if="isSystemBusy" size="small" style="margin-left: 8px;" />
          <span v-if="isSystemBusy" style="margin-left: 8px; font-size: 10px; color: #666;">Loading...</span>
        </div>

        <!-- All form items with consistent heights -->
        <a-form-item label="System" style="margin-bottom: 1px; min-height: 32px;" class="system-config-item">
          <div style="display: flex; align-items: center; gap: 4px;">
            <a-select
              v-model:value="dserv.state.currentSystem"
              size="small"
              style="flex: 1; min-width: 140px;"
              :loading="loading.system || isSystemBusy"
              :disabled="isSystemBusy"
              @change="setSystem"
              placeholder="Select system..."
							>
              <a-select-option v-for="system in dserv.state.systems" :key="system" :value="system">
                {{ system }}
              </a-select-option>
            </a-select>
            <a-button
              size="small"
              :icon="h(ReloadOutlined)"
              :disabled="isSystemBusy"
              @click="reloadSystem"
              style="width: 24px; height: 24px;"
							/>
          </div>
        </a-form-item>

        <a-form-item label="Protocol" style="margin-bottom: 1px; min-height: 32px;" class="system-config-item">
          <div style="display: flex; align-items: center; gap: 4px;">
            <a-select
              :value="displayProtocolValue"
              size="small"
              style="flex: 1; min-width: 140px;"
              :loading="loading.protocol || isSystemBusy"
              :disabled="isSystemBusy || !dserv.state.currentSystem"
              @change="setProtocol"
              :placeholder="getProtocolPlaceholder"
              :allow-clear="true"
              :show-search="false"
							>
              <a-select-option v-for="protocol in availableProtocols" :key="protocol" :value="protocol">
                {{ protocol }}
              </a-select-option>
            </a-select>
            <a-button
              size="small"
              :icon="h(ReloadOutlined)"
              :disabled="isSystemBusy"
              @click="reloadProtocol"
              style="width: 24px; height: 24px;"
							/>
          </div>
        </a-form-item>

        <a-form-item label="Variant" style="margin-bottom: 0; min-height: 32px;" class="system-config-item">
          <div style="display: flex; align-items: center; gap: 4px;">
            <a-select
              :value="displayVariantValue"
              size="small"
              style="flex: 1; min-width: 140px;"
              :loading="loading.variant || isSystemBusy"
              :disabled="isSystemBusy || !dserv.state.currentProtocol"
              @change="setVariant"
              :placeholder="getVariantPlaceholder"
              :allow-clear="true"
              :show-search="false"
							>
              <a-select-option v-for="variant in availableVariants" :key="variant" :value="variant">
                {{ variant }}
              </a-select-option>
            </a-select>
            <a-button
              size="small"
              :icon="h(ReloadOutlined)"
              :disabled="isSystemBusy"
              @click="reloadVariant"
              style="width: 24px; height: 24px;"
							/>
          </div>
        </a-form-item>
      </div>

  <!-- Settings Management -->
      <div style="background: #f8f9fa; border: 1px solid #e8e8e8; border-radius: 6px; padding: 8px; margin-bottom: 8px; width: 100%; max-width: 100%; min-height: 60px;">
        <div style="font-weight: 500; margin-bottom: 8px; font-size: 12px;">Settings</div>
        <div style="display: flex; justify-content: center; padding-bottom: 4px;">
          <a-space size="small" wrap>
            <a-button size="small" :disabled="isSystemBusy" @click="saveSettings">Save</a-button>
            <a-button size="small" :disabled="isSystemBusy" @click="resetSettings">Reset</a-button>
            <a-button size="small" :disabled="isSystemBusy" @click="loadSettings">Load</a-button>
          </a-space>
        </div>
      </div>			

    </div>

    <!-- SCROLLABLE SECTION - Optional/advanced controls -->
    <div style="flex: 1; overflow-y: auto; overflow-x: hidden; padding: 0 4px; display: flex; flex-direction: column; align-items: center;">

      <!-- Variant Options - Collapsible -->
      <a-collapse
        v-if="variantOptions.length > 0"
        ghost
        :bordered="true"
        style="margin-bottom: 12px; width: 100%; max-width: 100%;"
        :default-active-key="['variant-options']"
				>
        <a-collapse-panel key="variant-options" header="Variant Options">
          <template #extra>
            <div style="display: flex; align-items: center; gap: 4px;" @click.stop>
              <a-checkbox
                v-model:checked="autoReload"
                size="small"
                style="font-size: 9px; transform: scale(0.8);"
								/>
              <span style="font-size: 9px;">Auto-reload</span>
              <a-button
                size="small"
                :icon="h(ReloadOutlined)"
                :disabled="isSystemBusy"
                @click="reloadVariant"
                style="width: 18px; height: 18px; font-size: 8px; margin-left: 4px;"
                title="Reload variant manually"
								/>
            </div>
          </template>

          <div style="max-height: 200px; overflow-y: auto; padding-right: 4px;">
            <div v-for="option in variantOptions" :key="option.name" style="margin-bottom: 6px;">
              <div style="display: flex; align-items: center; justify-content: space-between; gap: 8px;">
                <span style="font-size: 11px; flex: 1; text-align: left; white-space: nowrap; overflow: hidden; text-overflow: ellipsis;">
                  {{ option.name }}
                </span>
                <a-select
                  v-model:value="option.selectedValue"
                  size="small"
                  class="variant-option-select"
                  style="width: 85px; flex-shrink: 0;"
                  :disabled="isSystemBusy"
                  @change="(value) => setVariantOption(option.name, value)"
									>
                  <a-select-option
                    v-for="choice in option.choices"
                    :key="choice.label"
                    :value="choice.value"
										>
                    {{ choice.label }}
                  </a-select-option>
                </a-select>
              </div>
            </div>
          </div>
        </a-collapse-panel>
      </a-collapse>

      <!-- System Parameters - Collapsible -->
      <a-collapse
        v-if="systemParameters.length > 0"
        ghost
        :bordered="true"
        style="margin-bottom: 12px; width: 100%; max-width: 100%;"
        :default-active-key="['system-parameters']"
				>
        <a-collapse-panel key="system-parameters" header="System Parameters">
          <div style="max-height: 200px; overflow-y: auto; padding-right: 4px;">
            <div v-for="param in systemParameters" :key="param.name" style="margin-bottom: 6px;">
              <div style="display: flex; align-items: center; justify-content: space-between; gap: 8px;">
                <span style="font-size: 11px; flex: 1; text-align: left; white-space: nowrap; overflow: hidden; text-overflow: ellipsis;">
                  {{ param.name }}
                </span>
                <a-input-number
                  v-if="param.dataType === 'int' || param.dataType === 'float'"
                  v-model:value="param.value"
                  size="small"
                  style="width: 85px; flex-shrink: 0;"
                  :precision="param.dataType === 'float' ? 2 : 0"
                  :step="param.dataType === 'float' ? 0.1 : 1"
                  :disabled="isSystemBusy"
                  @change="() => setSystemParameter(param.name, param.value)"
									/>
                <a-input
                  v-else
                  v-model:value="param.value"
                  size="small"
                  style="width: 85px; flex-shrink: 0;"
                  :disabled="isSystemBusy"
                  @change="() => setSystemParameter(param.name, param.value)"
									/>
              </div>
            </div>
          </div>
        </a-collapse-panel>
      </a-collapse>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, h, watch, onMounted, onUnmounted } from 'vue'
import { ReloadOutlined, BulbOutlined, FolderOpenOutlined, CloseOutlined } from '@ant-design/icons-vue'
import { dserv } from '../services/dserv.js'
import { errorService } from '../services/errorService.js'

// Local state
const subjects = ['sally', 'momo', 'riker', 'glenn', 'human']

const datafilename = ref('')
const currentDatafile = ref('')
const datafileLoading = ref(false)
const suggestLoading = ref(false)
const suggestedFilename = ref('')
const filenameInput = ref(null)

const variantOptions = ref([])
const systemParameters = ref([])
const autoReload = ref(true) // Default to auto-reload enabled

const loading = ref({
  start: false,
  stop: false,
  reset: false,
  system: false,
  protocol: false,
  variant: false
})

// Track which operation is in progress to avoid premature clearing
const operationInProgress = ref({
  system: false,
  protocol: false,
  variant: false
})

// Polling mechanism to ensure we recover from stuck loading states
const statusPollTimer = ref(null)
const loadingStartTime = ref(null)

// FIXED: Better system busy detection - check both local and global states
const isSystemBusy = computed(() => {
  const localLoading = loading.value.system || loading.value.protocol || loading.value.variant
  const globalLoading = dserv.state.essStatus === 'loading'
  return localLoading || globalLoading
})

// FIXED: Protocol-specific loading check
const isProtocolLoading = computed(() => {
  return loading.value.system || loading.value.protocol || dserv.state.essStatus === 'loading'
})

// FIXED: Variant-specific loading check  
const isVariantLoading = computed(() => {
  return loading.value.system || loading.value.protocol || loading.value.variant || dserv.state.essStatus === 'loading'
})

// FIXED: Display values should be null during loading for immediate clearing
const displayProtocolValue = computed(() => {
  if (isProtocolLoading.value) {
    return null
  }
  return dserv.state.currentProtocol || null
})

const displayVariantValue = computed(() => {
  if (isVariantLoading.value) {
    return null
  }
  return dserv.state.currentVariant || null
})

// FIXED: Clear options during loading for immediate UI feedback
const availableProtocols = computed(() => {
  if (isProtocolLoading.value) {
    return [] // Clear options during any relevant loading
  }
  return dserv.state.protocols
})

const availableVariants = computed(() => {
  if (isVariantLoading.value) {
    return [] // Clear options during any relevant loading
  }
  return dserv.state.variants
})

// FIXED: Updated placeholders with better loading messages
const getProtocolPlaceholder = computed(() => {
  if (isProtocolLoading.value) {
    return "Loading protocols..."
  }
  if (!dserv.state.currentSystem) {
    return "Select system first"
  }
  return "Select protocol..."
})

const getVariantPlaceholder = computed(() => {
  if (isVariantLoading.value) {
    return "Loading variants..."
  }
  if (!dserv.state.currentProtocol) {
    return "Select protocol first"
  }
  return "Select variant..."
})

const showPerformance = computed(() => {
  return dserv.state.blockPctComplete > 0 || dserv.state.blockPctCorrect > 0
})

// Polling function to check status and recover from stuck states
function startStatusPolling() {
  if (statusPollTimer.value) {
    clearInterval(statusPollTimer.value)
  }
  
  loadingStartTime.value = Date.now()
  
  statusPollTimer.value = setInterval(() => {
    // Check if we've been loading too long (30 seconds timeout)
    const elapsed = Date.now() - loadingStartTime.value
    if (elapsed > 30000) {
      console.warn('Loading timeout detected, forcing recovery')
      stopStatusPolling()
      forceLoadingRecovery()
      return
    }
    
    // Check if loading is complete
    if (dserv.state.essStatus === 'stopped' || dserv.state.essStatus === 'running') {
      stopStatusPolling()
      ensureLoadingStatesCleared()
    }
  }, 500)
}

function stopStatusPolling() {
  if (statusPollTimer.value) {
    clearInterval(statusPollTimer.value)
    statusPollTimer.value = null
  }
  loadingStartTime.value = null
}

function ensureLoadingStatesCleared() {
  // Only clear loading states that don't have operations in progress
  if (!operationInProgress.value.system) {
    loading.value.system = false
  }
  if (!operationInProgress.value.protocol) {
    loading.value.protocol = false
  }
  if (!operationInProgress.value.variant) {
    loading.value.variant = false
  }
}

function forceLoadingRecovery() {
  console.warn('Forcing loading state recovery due to timeout')
  // Force clear everything on timeout
  loading.value.system = false
  loading.value.protocol = false
  loading.value.variant = false
  operationInProgress.value.system = false
  operationInProgress.value.protocol = false
  operationInProgress.value.variant = false
  logToConsole('Loading timeout - recovered control', 'warning')
}

onMounted(() => {
  console.log('ExperimentControl component mounted')

  // Register component with subscription for variant info JSON
  const cleanup = dserv.registerComponent('ExperimentControl', {
    subscriptions: [
      { pattern: 'ess/variant_info_json', every: 1 },
      { pattern: 'ess/param_settings', every: 1 },
      { pattern: 'ess/datafile', every: 1 },
      { pattern: 'ess/lastfile', every: 1 }
    ]
  })

  // Listen for comprehensive variant info data
  dserv.on('datapoint:ess/variant_info_json', (data) => {
    console.log('Received variant_info_json:', data.data)
    parseVariantInfoJson(data.data)
  })

  // Listen for system parameters data
  dserv.on('datapoint:ess/param_settings', (data) => {
    console.log('Received param_settings:', data.data)
    parseSystemParameters(data.data)
  })

  // Listen for datafile status
  dserv.on('datapoint:ess/datafile', (data) => {
    console.log('Received datafile status:', data.data)
    currentDatafile.value = data.data || ''
    if (!data.data) {
      // File was closed, clear the filename for next use
      datafilename.value = ''
    }
  })

  // Listen for last file info (when file is closed and processed)
  dserv.on('datapoint:ess/lastfile', (data) => {
    console.log('Last file processed:', data.data)
    if (data.data) {
      logToConsole(`File processed: ${data.data}`, 'success')
    }
  })

  // Cleanup on unmount
  onUnmounted(() => {
    cleanup()
    stopStatusPolling()
  })
})

function parseVariantInfoJson(rawData) {
  try {
    console.log('Parsing variant info JSON:', rawData)

    // Parse JSON data
    let variantInfo
    if (typeof rawData === 'string') {
      variantInfo = JSON.parse(rawData)
    } else {
      variantInfo = rawData
    }

    // Extract the main components
    const {
      loader_proc,
      loader_arg_names,
      loader_args,
      options
    } = variantInfo

    console.log('Extracted variant info:', {
      loader_proc,
      loader_arg_names,
      loader_args,
      options
    })

    // Convert to our internal format for variant options
    const variantOptionsArray = []

    // Use loader_arg_names to maintain proper order
    loader_arg_names.forEach((argName, index) => {
      if (options[argName]) {
        // Find the currently selected option
        const currentValue = loader_args[index]
        let selectedValue = null

        // Look through the options to find which one is selected
        const optionChoices = options[argName].map(option => {
          const isSelected = option.selected === true
          if (isSelected) {
            selectedValue = option.value
          }

          return {
            label: option.label,
            value: option.value
          }
        })

        // If no option was marked as selected, try to match by value
        if (selectedValue === null && optionChoices.length > 0) {
          const matchingChoice = optionChoices.find(choice =>
            choice.value === currentValue ||
            (typeof currentValue === 'string' && choice.value === currentValue.trim())
          )
          selectedValue = matchingChoice ? matchingChoice.value : optionChoices[0].value
        }

        variantOptionsArray.push({
          name: argName,
          choices: optionChoices,
          selectedValue: selectedValue || optionChoices[0]?.value || ''
        })

        console.log(`Processed ${argName}: selected=${selectedValue}, choices=${optionChoices.length}`)
      }
    })

    console.log('Final variant options array:', variantOptionsArray)
    variantOptions.value = variantOptionsArray

    // Log the loader information for debugging
    console.log(`Loader proc: ${loader_proc}`)
    console.log(`Current args: [${loader_args.join(', ')}]`)

  } catch (error) {
    console.error('Failed to parse variant info JSON:', error)
    variantOptions.value = []
  }
}

// Parse system parameters from Tcl format
function parseSystemParameters(rawData) {
  try {
    console.log('Parsing system parameters:', rawData)

    const params = []
    // Split by parameter pattern: param_name {value type datatype}
    const paramMatches = rawData.match(/(\w+)\s+{([^}]+)}/g)

    if (paramMatches) {
      paramMatches.forEach(match => {
        const [, paramName, paramData] = match.match(/(\w+)\s+{([^}]+)}/)
        const parts = paramData.trim().split(/\s+/)

        if (parts.length >= 3) {
          const value = parts[0]
          const variableType = parts[1] // 1=time, 2=variable, 5=other
          const dataType = parts[2] // int, float, ipaddr, etc.

          // Convert value to appropriate type
          let convertedValue = value
          if (dataType === 'int') {
            convertedValue = parseInt(value, 10)
          } else if (dataType === 'float') {
            convertedValue = parseFloat(value)
          }

          params.push({
            name: paramName,
            value: convertedValue,
            variableType: parseInt(variableType, 10),
            dataType: dataType
          })
        }
      })
    }

    console.log('Parsed system parameters:', params)
    systemParameters.value = params
  } catch (error) {
    console.error('Failed to parse system parameters:', error)
    systemParameters.value = []
  }
}

async function refreshSuggestion() {
  if (suggestLoading.value) return

  suggestLoading.value = true
  try {
    const suggestion = await dserv.essCommand('ess::file_suggest')
    if (suggestion && suggestion.trim()) {
      suggestedFilename.value = suggestion.trim()
      logToConsole(`Filename suggested: ${suggestion}`, 'info')
    } else {
      suggestedFilename.value = ''
      logToConsole('No filename suggestion available', 'info')
    }
  } catch (error) {
    console.error('Failed to get filename suggestion:', error)
    suggestedFilename.value = ''
    logToConsole(`Failed to suggest filename: ${error.message}`, 'error')
  } finally {
    suggestLoading.value = false
  }
}

async function openDatafile() {
  // Use manual input if provided, otherwise use suggestion, otherwise auto-suggest
  let filename = datafilename.value.trim()

  if (!filename && suggestedFilename.value) {
    filename = suggestedFilename.value
  }

  if (!filename) {
    // No manual input and no existing suggestion - try to get one
    try {
      const suggestion = await dserv.essCommand('ess::file_suggest')
      if (suggestion && suggestion.trim()) {
        filename = suggestion.trim()
        logToConsole(`Auto-suggested filename: ${filename}`, 'info')
      }
    } catch (error) {
      console.error('Failed to auto-suggest filename:', error)
    }
  }

  if (!filename) {
    logToConsole('Unable to determine filename. Please enter one manually.', 'error')
    return
  }

  if (datafileLoading.value) return

  datafileLoading.value = true
  try {
    const result = await dserv.essCommand(`ess::file_open ${filename}`)

    if (result === '1' || result === 1) {
      logToConsole(`Datafile opened: ${filename}`, 'success')
      // Clear inputs after successful open
      datafilename.value = ''
      suggestedFilename.value = ''
    } else if (result === '0' || result === 0) {
      logToConsole(`File ${filename} already exists`, 'error')
    } else if (result === '-1' || result === -1) {
      logToConsole('Another file is already open. Close it first.', 'error')
    } else {
      logToConsole(`Unexpected response: ${result}`, 'error')
    }
  } catch (error) {
    console.error('Failed to open datafile:', error)
    logToConsole(`Failed to open datafile: ${error.message}`, 'error')
  } finally {
    datafileLoading.value = false
  }
}

async function closeDatafile() {
  if (!currentDatafile.value) {
    logToConsole('No datafile is currently open', 'error')
    return
  }

  if (datafileLoading.value) return

  datafileLoading.value = true
  try {
    const result = await dserv.essCommand('ess::file_close')

    if (result === '1' || result === 1) {
      logToConsole('Datafile closed successfully', 'success')
      // Clear any lingering inputs after close
      datafilename.value = ''
    } else {
      logToConsole('No datafile was open', 'info')
    }
  } catch (error) {
    console.error('Failed to close datafile:', error)
    logToConsole(`Failed to close datafile: ${error.message}`, 'error')
  } finally {
    datafileLoading.value = false
  }
}

async function setVariantOption(optionName, value) {
  try {
    const command = `ess::set_variant_args {${optionName} {${value}}}`;
    console.log('Setting variant option with command:', command);

    await dserv.essCommand(command);

    if (autoReload.value) {
      try {
        await dserv.essCommand('ess::reload_variant');
      } catch (reloadError) {
        console.error('Failed to auto-reload variant:', reloadError);
        logToConsole(`Auto-reload failed: ${reloadError.message}`, 'error');

        try {
          await dserv.essCommand('dservSet ess/status stopped');
          console.log('Reset system status to stopped after reload failure');
        } catch (resetError) {
          console.error('Failed to reset system status:', resetError);
        }
      }
    }
  } catch (error) {
    console.error('Failed to set variant option:', error);
    logToConsole(`Failed to set variant option: ${error.message}`, 'error');
  }
}

// System parameter change handler
async function setSystemParameter(paramName, value) {
  try {
    const command = `ess::set_param ${paramName} ${value}`
    await dserv.essCommand(command)
    logToConsole(`Parameter ${paramName} set to: ${value}`, 'success')
  } catch (error) {
    console.error('Failed to set system parameter:', error)
    logToConsole(`Failed to set parameter: ${error.message}`, 'error')
  }
}

// Watch for dserv connection status
watch(() => dserv.state.connected, (connected) => {
  if (connected) {
    logToConsole('Connected to dserv', 'success')
  } else {
    logToConsole('Disconnected from dserv', 'error')
  }
})

// FIXED: Better watch for loading state changes
watch(isSystemBusy, (isBusy, wasBusy) => {
  if (isBusy && !wasBusy) {
    // Just entered loading state - start polling
    startStatusPolling()
  } else if (!isBusy && wasBusy) {
    // Just exited loading state - stop polling and clear states
    stopStatusPolling()
    ensureLoadingStatesCleared()
  }
})

// FIXED: Enhanced status change watcher
watch(() => dserv.state.essStatus, (newStatus, oldStatus) => {
  if (oldStatus === 'loading' && (newStatus === 'stopped' || newStatus === 'running')) {
    logToConsole('System loading completed', 'success')
    stopStatusPolling()
    ensureLoadingStatesCleared()
  }
})

// FIXED: Enhanced watchers that clear loading states AND operation tracking when backend state actually changes
watch(() => dserv.state.currentSystem, (newVal, oldVal) => {
  if (newVal !== oldVal) {
    // System changed, clear system operation and loading
    operationInProgress.value.system = false
    loading.value.system = false
  }
})

watch(() => dserv.state.currentProtocol, (newVal, oldVal) => {
  if (newVal !== oldVal) {
    // Protocol changed, clear protocol operation and loading
    operationInProgress.value.protocol = false
    loading.value.protocol = false
  }
})

watch(() => dserv.state.currentVariant, (newVal, oldVal) => {
  if (newVal !== oldVal) {
    // Variant changed, clear variant operation and loading
    operationInProgress.value.variant = false
    loading.value.variant = false
  }
})

// FIXED: Also watch for when protocol/variant lists change, but only clear if no operation in progress
watch(() => dserv.state.protocols, (newVal, oldVal) => {
  if (newVal !== oldVal && !operationInProgress.value.protocol && !operationInProgress.value.system) {
    loading.value.protocol = false
  }
})

watch(() => dserv.state.variants, (newVal, oldVal) => {
  if (newVal !== oldVal && !operationInProgress.value.variant && !operationInProgress.value.protocol && !operationInProgress.value.system) {
    loading.value.variant = false
  }
})

// Clear inputs when file operations complete
watch(() => currentDatafile.value, (newFile, oldFile) => {
  if (oldFile && !newFile) {
    // File was closed, reset to clean state
    datafilename.value = ''
    suggestedFilename.value = ''
  }
})

// In ExperimentControl.vue, replace the logToTerminal function:
function logToConsole(message, type = 'normal') {
  try {
    // Map your existing types to log levels
    const levelMap = {
      'success': 'info',
      'error': 'error', 
      'warning': 'warning',
      'info': 'info',
      'normal': 'info'
    }
    
    const level = levelMap[type] || 'info'
    
    // Add directly to the error service logs (simulates a frontend log entry)
    errorService.addFrontendLog(level, message, 'frontend')
    
  } catch (error) {
    console.warn('Failed to log to terminal:', error)
    // Fallback to console if error service fails
    console.log(`[${type}] ${message}`)
  }
}

// Load initial data from dserv
watch(() => dserv.state.subject, async (newSubject) => {
  if (newSubject) {
    // Future: implement subject data loading if needed
  }
})

// Methods
async function setSubject(subject) {
  try {
    await dserv.setSubject(subject)
    logToConsole(`Subject set to: ${subject}`, 'success')
  } catch (error) {
    console.error('Failed to set subject:', error)
    logToConsole(`Failed to set subject: ${error.message}`, 'error')
  }
}

// FIXED: Track operations and don't clear loading states until actual state changes
async function setSystem(system) {
  // Set loading states IMMEDIATELY for instant UI feedback
  loading.value.system = true
  loading.value.protocol = true  // Also clear protocol dropdown
  loading.value.variant = true   // Also clear variant dropdown
  
  // Track that we're doing a system operation
  operationInProgress.value.system = true
  operationInProgress.value.protocol = true  // System change affects protocol
  operationInProgress.value.variant = true   // System change affects variant
  
  try {
    await dserv.setSystem(system)
    logToConsole(`System loaded: ${system}`, 'success')
    // Don't clear loading states here - let the watchers handle it when state actually changes
  } catch (error) {
    console.error('Failed to set system:', error)
    logToConsole(`Failed to load system: ${error.message}`, 'error')
    // Force recovery on error
    operationInProgress.value.system = false
    operationInProgress.value.protocol = false
    operationInProgress.value.variant = false
    ensureLoadingStatesCleared()
    stopStatusPolling()
  }
  // NO finally block - let the state watchers clear the loading flags
}

// FIXED: Track protocol operations
async function setProtocol(protocol) {
  loading.value.protocol = true
  loading.value.variant = true // Clear variant when protocol changes
  
  operationInProgress.value.protocol = true
  operationInProgress.value.variant = true  // Protocol change affects variant
  
  try {
    await dserv.setProtocol(protocol)
    logToConsole(`Protocol loaded: ${protocol}`, 'success')
    // Don't clear loading states here - let the watchers handle it
  } catch (error) {
    console.error('Failed to set protocol:', error)
    logToConsole(`Failed to load protocol: ${error.message}`, 'error')
    // Force recovery on error
    operationInProgress.value.protocol = false
    operationInProgress.value.variant = false
    ensureLoadingStatesCleared()
    stopStatusPolling()
  }
  // NO finally block - let the state watchers clear the loading flags
}

async function setVariant(variant) {
  loading.value.variant = true
  operationInProgress.value.variant = true
  
  try {
    await dserv.setVariant(variant)
    logToConsole(`Variant loaded: ${variant}`, 'success')
    // Don't clear loading state here - let the watcher handle it
  } catch (error) {
    console.error('Failed to set variant:', error)
    logToConsole(`Failed to load variant: ${error.message}`, 'error')
    // Force recovery on error
    operationInProgress.value.variant = false
    ensureLoadingStatesCleared()
    stopStatusPolling()
  }
  // NO finally block - let the state watcher clear the loading flag
}

async function startExperiment() {
  loading.value.start = true
  try {
    await dserv.startExperiment()
    logToConsole('Experiment started', 'success')
  } catch (error) {
    console.error('Failed to start experiment:', error)
    logToConsole(`Failed to start: ${error.message}`, 'error')
  } finally {
    loading.value.start = false
  }
}

async function stopExperiment() {
  loading.value.stop = true
  try {
    await dserv.stopExperiment()
    logToConsole('Experiment stopped', 'success')
  } catch (error) {
    console.error('Failed to stop experiment:', error)
    logToConsole(`Failed to stop: ${error.message}`, 'error')
  } finally {
    loading.value.stop = false
  }
}

async function resetExperiment() {
  loading.value.reset = true
  try {
    await dserv.resetExperiment()

    // Clear observation count after reset
    dserv.state.obsCount = ' '

    logToConsole('Experiment reset', 'success')
  } catch (error) {
    console.error('Failed to reset experiment:', error)
    logToConsole(`Failed to reset: ${error.message}`, 'error')
  } finally {
    loading.value.reset = false
  }
}

async function reloadSystem() {
  try {
    await dserv.essCommand('ess::reload_system')
    logToConsole('System reloaded', 'success')
  } catch (error) {
    console.error('Failed to reload system:', error)
    logToConsole(`Failed to reload system: ${error.message}`, 'error')
  }
}

async function reloadProtocol() {
  try {
    await dserv.essCommand('ess::reload_protocol')
    logToConsole('Protocol reloaded', 'success')
  } catch (error) {
    console.error('Failed to reload protocol:', error)
    logToConsole(`Failed to reload protocol: ${error.message}`, 'error')
  }
}

async function reloadVariant() {
  try {
    await dserv.essCommand('ess::reload_variant')
    logToConsole('Variant reloaded', 'success')
  } catch (error) {
    console.error('Failed to reload variant:', error)
    logToConsole(`Failed to reload variant: ${error.message}`, 'error')
  }
}

async function saveSettings() {
  try {
    await dserv.essCommand('ess::save_settings')
    logToConsole('Settings saved', 'success')
  } catch (error) {
    console.error('Failed to save settings:', error)
    logToConsole(`Failed to save settings: ${error.message}`, 'error')
  }
}

async function resetSettings() {
  try {
    await dserv.essCommand('ess::reset_settings')
    logToConsole('Settings reset to defaults', 'success')
  } catch (error) {
    console.error('Failed to reset settings:', error)
    logToConsole(`Failed to reset settings: ${error.message}`, 'error')
  }
}

async function loadSettings() {
  try {
    await dserv.essCommand('ess::load_settings')
    logToConsole('Settings loaded', 'success')
  } catch (error) {
    console.error('Failed to load settings:', error)
    logToConsole(`Failed to load settings: ${error.message}`, 'error')
  }
}

function getStatusColor(status) {
  switch (status) {
    case 'Running': return 'green'
    case 'Stopped': return 'red'
    case 'loading': return 'orange'
    default: return 'default'
  }
}
</script>

<style scoped>
/* Enhanced styles for fixed layout */
:deep(.ant-form-item) {
  margin-bottom: 1px;
}

:deep(label[title="Subject"]) {
  font-size: 11px !important;
  font-weight: 500 !important;
}

:deep(label[title="System"]),
:deep(label[title="Protocol"]),
:deep(label[title="Variant"]) {
  font-size: 10px !important;
  font-weight: 500 !important;
}

.subject-section :deep(.ant-form-item-label),
.system-config :deep(.ant-form-item-label) {
  width: 55px !important;
  padding-right: 6px !important;
  line-height: 1.2 !important;
  text-align: right;
}

:deep(.ant-select-selector) {
  font-size: 12px;
  padding: 2px 8px !important;
  transition: all 0.3s ease;
}

/* Enhanced loading states for selects */
:deep(.ant-select-loading .ant-select-selector) {
  background-color: #f5f5f5 !important;
  border-color: #d9d9d9 !important;
}

:deep(.ant-select-disabled .ant-select-selector) {
  background-color: #f5f5f5 !important;
  color: #999 !important;
  border-color: #d9d9d9 !important;
}

/* Special styling for loading state placeholders */
:deep(.ant-select-selector .ant-select-selection-placeholder) {
  color: #1890ff !important;
  font-style: italic;
}

/* When system is busy, make protocol/variant appear more "ghosted" */
.system-config :deep(.ant-select-disabled.ant-select-loading .ant-select-selector) {
  background-color: #fafafa !important;
  opacity: 0.7;
}

.system-config :deep(.ant-select-disabled.ant-select-loading .ant-select-selection-placeholder) {
  color: #1890ff !important;
  animation: pulse 1.5s infinite;
}

@keyframes pulse {
  0%, 100% { opacity: 0.7; }
  50% { opacity: 1; }
}

:global(.ant-select-dropdown .ant-select-item) {
  font-size: 10px !important;
  padding: 2px 8px !important;
  line-height: 1.2 !important;
  min-height: 20px !important;
}

:global(.ant-select-dropdown .ant-select-item-option-content) {
  font-size: 10px !important;
}

:global(.ant-select-dropdown) {
  font-size: 10px !important;
}

:deep(.ant-input) {
  font-size: 12px;
  padding: 2px 8px !important;
}

:deep(.ant-input-number) {
  font-size: 12px;
}

:deep(.ant-input-number .ant-input-number-input) {
  padding: 2px 8px !important;
}

:deep(.ant-btn-sm) {
  font-size: 11px;
  height: 22px;
  padding: 0 6px;
}

.control-buttons {
  display: flex;
  justify-content: center;
}

.control-buttons :deep(.ant-btn) {
  width: 60px;
}

.system-config .ant-select {
  flex: 1;
}

.system-config .ant-select-disabled .ant-select-selector {
  opacity: 0.6;
}

.obs-indicator {
  width: 12px;
  height: 12px;
  border-radius: 2px;
  background-color: transparent;
  transition: background-color 0.3s ease;
  flex-shrink: 0;
}

.obs-indicator.is-active {
  background-color: #f5222d;
}

.datafile-config-item :deep(.ant-form-item-label) {
  width: 55px !important;
  padding-right: 6px !important;
  line-height: 1.2 !important;
  text-align: right;
}

.datafile-config-item :deep(label[title="Filename"]) {
  font-size: 10px !important;
  font-weight: 500 !important;
}

.datafile-config-item :deep(.ant-input::placeholder) {
  font-style: italic;
  color: #bfbfbf;
}

.variant-option-select :deep(.ant-select-selector) {
  font-size: 10px !important;
  padding: 1px 4px !important;
}

.variant-option-select :deep(.ant-select-selection-item) {
  font-size: 10px !important;
}

:global(.variant-option-select .ant-select-dropdown .ant-select-item) {
  font-size: 9px !important;
  padding: 1px 6px !important;
  line-height: 1.1 !important;
  min-height: 18px !important;
}

:global(.variant-option-select .ant-select-dropdown .ant-select-item-option-content) {
  font-size: 9px !important;
}

.ant-input-number {
  font-size: 10px !important;
}

.ant-input-number :deep(.ant-input-number-input) {
  font-size: 10px !important;
}

.ant-input {
  font-size: 10px !important;
}

:deep(.ant-collapse-ghost .ant-collapse-item) {
  border: 1px solid #d9d9d9;
  border-radius: 6px;
}

:deep(.ant-collapse-header) {
  font-size: 12px !important;
  font-weight: 500;
  padding: 8px 12px !important;
}

:deep(.ant-collapse-content-box) {
  padding: 8px 12px !important;
}

/* Fixed scrollbar styles */
div[style*="max-height: 200px"]::-webkit-scrollbar {
  width: 6px;
}

div[style*="max-height: 200px"]::-webkit-scrollbar-track {
  background: #f1f1f1;
  border-radius: 3px;
}

div[style*="max-height: 200px"]::-webkit-scrollbar-thumb {
  background: #c1c1c1;
  border-radius: 3px;
}

div[style*="max-height: 200px"]::-webkit-scrollbar-thumb:hover {
  background: #a8a8a8;
}

div[style*="overflow-y: auto"]::-webkit-scrollbar {
  width: 8px;
}

div[style*="overflow-y: auto"]::-webkit-scrollbar-track {
  background: #f5f5f5;
  border-radius: 4px;
}

div[style*="overflow-y: auto"]::-webkit-scrollbar-thumb {
  background: #d9d9d9;
  border-radius: 4px;
}

div[style*="overflow-y: auto"]::-webkit-scrollbar-thumb:hover {
  background: #bfbfbf;
}

/* Prevent layout jumps with consistent sizing */
.ant-tag {
  min-width: 60px;
  text-align: center;
  display: inline-block;
}

/* Ensure loading states are visible */
.ant-spin {
  color: #1890ff;
}

/* Loading indicator for combo boxes */
:deep(.ant-select-arrow .ant-select-suffix) {
  transition: all 0.3s ease;
}
</style>
