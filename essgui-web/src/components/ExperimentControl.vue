<template>
  <div style="height: 100%; display: flex; flex-direction: column; padding: 8px; overflow: hidden;">
    <!-- Clean Header - Just the title -->
    <div style="flex-shrink: 0; display: flex; align-items: center; justify-content: center; margin-bottom: 12px; padding: 4px 0;">
      <div style="font-weight: 500; font-size: 14px;">ESS Control</div>
    </div>

    <!-- FIXED SECTION - Always visible essential controls -->
    <div style="flex-shrink: 0; margin-bottom: 12px; padding: 0 4px; display: flex; flex-direction: column; align-items: center;">
      <!-- Subject Selection -->
      <div class="system-config subject-section" style="background: #f8f9fa; border: 1px solid #e8e8e8; border-radius: 6px; padding: 4px; margin-bottom: 8px; width: 100%; max-width: 100%;">
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

      <!-- Control Buttons and Status Display - Active Control Area -->
      <div style="background: #f8f9fa; border: 1px solid #e8e8e8; border-radius: 8px; padding: 8px; margin-bottom: 12px; width: 100%; max-width: 100%;">
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
              :disabled="isSystemBusy"
              @click="resetExperiment"
            >
              Reset
            </a-button>
          </a-space>
        </div>

        <!-- Status Display -->
        <div style="font-size: 12px;">
          <div style="display: flex; align-items: center; gap: 8px; margin-bottom: 4px;">
            <div class="obs-indicator" :class="{ 'is-active': dserv.state.inObs }"></div>
            <div style="display: flex; align-items: center; gap: 8px;">
              <span style="width: 50px; text-align: right;"><strong>Status:</strong></span>
              <a-tag
                :color="getStatusColor(dserv.state.status)"
                style="margin: 0;"
              >
                {{ dserv.state.status }}
              </a-tag>
            </div>
          </div>

          <!-- Additional status info aligned with Status line -->
          <div style="padding-left: 20px;">
            <!-- Show loading indicator when system is busy -->
            <div
              v-if="isSystemBusy"
              style="display: flex; align-items: center; gap: 8px; margin-bottom: 4px;"
            >
              <span style="width: 50px; text-align: right;"><strong>ESS:</strong></span>
              <a-tag color="orange" style="margin: 0;">
                {{ dserv.state.essStatus }}
              </a-tag>
            </div>

            <div
              v-if="dserv.state.obsCount"
              style="display: flex; align-items: center; gap: 8px; margin-bottom: 4px;"
            >
              <span style="width: 50px; text-align: right;"><strong>Obs:</strong></span>
              <a-tag
                :color="dserv.state.inObs ? 'red' : 'default'"
                style="margin: 0;"
              >
                {{ dserv.state.obsCount }}
              </a-tag>
            </div>

            <div
              v-if="currentDatafile"
              style="display: flex; align-items: center; gap: 8px;"
            >
              <a-tag
                color="green"
                style="margin: 0; font-family: monospace; font-size: 10px; max-width: 200px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;"
              >
                {{ currentDatafile.split('/').pop().replace('.ess', '') }}
              </a-tag>
            </div>
          </div>
        </div>
      </div>

      <!-- System Configuration -->
      <div class="system-config" style="background: #f8f9fa; border: 1px solid #e8e8e8; border-radius: 6px; padding: 6px; margin-bottom: 8px; width: 100%; max-width: 100%;">
        <div style="font-weight: 500; margin-bottom: 6px; font-size: 12px;">
          System Configuration
          <a-spin v-if="isSystemBusy" size="small" style="margin-left: 8px;" />
        </div>

        <a-form-item label="System" style="margin-bottom: 4px;" class="system-config-item">
          <div style="display: flex; align-items: center; gap: 4px;">
            <a-select
              v-model:value="dserv.state.currentSystem"
              size="small"
              style="flex: 1; min-width: 140px;"
              :loading="loading.system || isSystemBusy"
              :disabled="isSystemBusy"
              @change="setSystem"
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

        <a-form-item label="Protocol" style="margin-bottom: 4px;" class="system-config-item">
          <div style="display: flex; align-items: center; gap: 4px;">
            <a-select
              v-model:value="dserv.state.currentProtocol"
              size="small"
              style="flex: 1; min-width: 140px;"
              :loading="loading.protocol || isSystemBusy"
              :disabled="isSystemBusy || !dserv.state.currentSystem"
              @change="setProtocol"
            >
              <a-select-option v-for="protocol in dserv.state.protocols" :key="protocol" :value="protocol">
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

        <a-form-item label="Variant" style="margin-bottom: 0;" class="system-config-item">
          <div style="display: flex; align-items: center; gap: 4px;">
            <a-select
              v-model:value="dserv.state.currentVariant"
              size="small"
              style="flex: 1; min-width: 140px;"
              :loading="loading.variant || isSystemBusy"
              :disabled="isSystemBusy || !dserv.state.currentProtocol"
              @change="setVariant"
            >
              <a-select-option v-for="variant in dserv.state.variants" :key="variant" :value="variant">
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

      <!-- Datafile Management -->
      <div style="background: #f8f9fa; border: 1px solid #e8e8e8; border-radius: 6px; padding: 6px; width: 100%; max-width: 100%;">
        <div style="font-weight: 500; margin-bottom: 6px; font-size: 12px;">
          Datafile Management
          <a-spin v-if="datafileLoading" size="small" style="margin-left: 8px;" />
        </div>

        <!-- Filename input -->
        <a-form-item
          label="Filename"
          style="margin-bottom: 4px;"
          class="datafile-config-item"
        >
          <div style="display: flex; align-items: center; gap: 4px;">
            <a-input
              ref="filenameInput"
              v-model:value="datafilename"
              size="small"
              style="flex: 1; min-width: 100px;"
              :disabled="isSystemBusy || !!currentDatafile"
              :placeholder="datafilename ? '' : (suggestedFilename ? 'use suggested file' : 'enter filename or click open')"
              @press-enter="openDatafile"
            />
            <a-button
              size="small"
              :icon="h(BulbOutlined)"
              :disabled="isSystemBusy || !!currentDatafile"
              @click="refreshSuggestion"
              :loading="suggestLoading"
              style="width: 24px; height: 24px;"
              title="Refresh suggestion"
            />
          </div>
        </a-form-item>

        <!-- Action buttons -->
        <div style="display: flex; justify-content: center; gap: 4px;">
          <a-button
            type="primary"
            size="small"
            :disabled="isSystemBusy || !!currentDatafile"
            @click="openDatafile"
            :loading="datafileLoading"
            style="width: 60px;"
          >
            Open
          </a-button>
          <a-button
            danger
            size="small"
            :disabled="isSystemBusy || !currentDatafile"
            @click="closeDatafile"
            :loading="datafileLoading"
            style="width: 60px;"
          >
            Close
          </a-button>
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

      <!-- Settings Management - Always visible at bottom -->
      <div style="background: #f8f9fa; border: 1px solid #e8e8e8; border-radius: 6px; padding: 8px; margin-bottom: 12px; width: 100%; max-width: 100%;">
        <div style="font-weight: 500; margin-bottom: 8px; font-size: 12px;">Settings</div>
        <div style="display: flex; justify-content: center; padding-bottom: 4px;">
          <a-space size="small" wrap>
            <a-button size="small" :disabled="isSystemBusy" @click="saveSettings">Save</a-button>
            <a-button size="small" :disabled="isSystemBusy" @click="resetSettings">Reset</a-button>
            <a-button size="small" :disabled="isSystemBusy" @click="loadSettings">Load</a-button>
          </a-space>
        </div>
      </div>

      <!-- Quick Performance Summary (minimal) -->
      <div v-if="showPerformance" style="background: #f8f9fa; border: 1px solid #e8e8e8; border-radius: 6px; padding: 8px; width: 100%; max-width: 100%;">
        <div style="font-weight: 500; margin-bottom: 8px; font-size: 12px;">Performance</div>
        <div style="font-size: 11px;">
          <div>{{ dserv.state.blockPctCorrect }}% Correct</div>
          <div>{{ dserv.state.blockPctComplete }}% Complete</div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, h, watch, onMounted, onUnmounted } from 'vue'
import { ReloadOutlined, BulbOutlined } from '@ant-design/icons-vue'
import { dserv } from '../services/dserv.js'

// Define emits for terminal logging
const emit = defineEmits(['status-update'])

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
      logToTerminal(`File processed: ${data.data}`, 'success')
    }
  })

  // Cleanup on unmount
  onUnmounted(cleanup)
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
      logToTerminal(`Filename suggested: ${suggestion}`, 'info')
    } else {
      suggestedFilename.value = ''
      logToTerminal('No filename suggestion available', 'info')
    }
  } catch (error) {
    console.error('Failed to get filename suggestion:', error)
    suggestedFilename.value = ''
    logToTerminal(`Failed to suggest filename: ${error.message}`, 'error')
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
        logToTerminal(`Auto-suggested filename: ${filename}`, 'info')
      }
    } catch (error) {
      console.error('Failed to auto-suggest filename:', error)
    }
  }

  if (!filename) {
    logToTerminal('Unable to determine filename. Please enter one manually.', 'error')
    return
  }

  if (datafileLoading.value) return

  datafileLoading.value = true
  try {
    const result = await dserv.essCommand(`ess::file_open ${filename}`)

    if (result === '1' || result === 1) {
      logToTerminal(`Datafile opened: ${filename}`, 'success')
      // Clear inputs after successful open
      datafilename.value = ''
      suggestedFilename.value = ''
    } else if (result === '0' || result === 0) {
      logToTerminal(`File ${filename} already exists`, 'error')
    } else if (result === '-1' || result === -1) {
      logToTerminal('Another file is already open. Close it first.', 'error')
    } else {
      logToTerminal(`Unexpected response: ${result}`, 'error')
    }
  } catch (error) {
    console.error('Failed to open datafile:', error)
    logToTerminal(`Failed to open datafile: ${error.message}`, 'error')
  } finally {
    datafileLoading.value = false
  }
}

async function closeDatafile() {
  if (!currentDatafile.value) {
    logToTerminal('No datafile is currently open', 'error')
    return
  }

  if (datafileLoading.value) return

  datafileLoading.value = true
  try {
    const result = await dserv.essCommand('ess::file_close')

    if (result === '1' || result === 1) {
      logToTerminal('Datafile closed successfully', 'success')
      // Clear any lingering inputs after close
      datafilename.value = ''
    } else {
      logToTerminal('No datafile was open', 'info')
    }
  } catch (error) {
    console.error('Failed to close datafile:', error)
    logToTerminal(`Failed to close datafile: ${error.message}`, 'error')
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
        logToTerminal(`Auto-reload failed: ${reloadError.message}`, 'error');

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
    logToTerminal(`Failed to set variant option: ${error.message}`, 'error');
  }
}

// System parameter change handler
async function setSystemParameter(paramName, value) {
  try {
    const command = `ess::set_param ${paramName} ${value}`
    await dserv.essCommand(command)
    logToTerminal(`Parameter ${paramName} set to: ${value}`, 'success')
  } catch (error) {
    console.error('Failed to set system parameter:', error)
    logToTerminal(`Failed to set parameter: ${error.message}`, 'error')
  }
}

// Watch for dserv connection status
watch(() => dserv.state.connected, (connected) => {
  if (connected) {
    logToTerminal('Connected to dserv', 'success')
  } else {
    logToTerminal('Disconnected from dserv', 'error')
  }
})

// Computed properties
const showPerformance = computed(() => {
  return dserv.state.blockPctComplete > 0 || dserv.state.blockPctCorrect > 0
})

const isSystemBusy = computed(() => {
  return dserv.state.essStatus === 'loading'
})

// Watch for loading state changes
watch(isSystemBusy, (isBusy) => {
  if (!isBusy) {
    // Reset all loading states when system is no longer busy
    loading.value.system = false
    loading.value.protocol = false
    loading.value.variant = false
  }
})

// Watch for successful loading completion
watch(() => dserv.state.essStatus, (newStatus, oldStatus) => {
  if (oldStatus === 'loading' && newStatus === 'stopped') {
    logToTerminal('System loading completed', 'success')
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

// Helper function to emit terminal logs
function logToTerminal(message, type = 'normal') {
  emit('status-update', { message, type })
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
    logToTerminal(`Subject set to: ${subject}`, 'success')
  } catch (error) {
    console.error('Failed to set subject:', error)
    logToTerminal(`Failed to set subject: ${error.message}`, 'error')
  }
}

async function setSystem(system) {
  loading.value.system = true
  try {
    await dserv.setSystem(system)
    logToTerminal(`System loaded: ${system}`, 'success')
  } catch (error) {
    console.error('Failed to set system:', error)
    logToTerminal(`Failed to load system: ${error.message}`, 'error')
  } finally {
    loading.value.system = false
  }
}

async function setProtocol(protocol) {
  loading.value.protocol = true
  try {
    await dserv.setProtocol(protocol)
    logToTerminal(`Protocol loaded: ${protocol}`, 'success')
  } catch (error) {
    console.error('Failed to set protocol:', error)
    logToTerminal(`Failed to load protocol: ${error.message}`, 'error')
  } finally {
    loading.value.protocol = false
  }
}

async function setVariant(variant) {
  loading.value.variant = true
  try {
    await dserv.setVariant(variant)
    logToTerminal(`Variant loaded: ${variant}`, 'success')
  } catch (error) {
    console.error('Failed to set variant:', error)
    logToTerminal(`Failed to load variant: ${error.message}`, 'error')
  } finally {
    loading.value.variant = false
  }
}

async function startExperiment() {
  loading.value.start = true
  try {
    await dserv.startExperiment()
    logToTerminal('Experiment started', 'success')
  } catch (error) {
    console.error('Failed to start experiment:', error)
    logToTerminal(`Failed to start: ${error.message}`, 'error')
  } finally {
    loading.value.start = false
  }
}

async function stopExperiment() {
  loading.value.stop = true
  try {
    await dserv.stopExperiment()
    logToTerminal('Experiment stopped', 'success')
  } catch (error) {
    console.error('Failed to stop experiment:', error)
    logToTerminal(`Failed to stop: ${error.message}`, 'error')
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

    logToTerminal('Experiment reset', 'success')
  } catch (error) {
    console.error('Failed to reset experiment:', error)
    logToTerminal(`Failed to reset: ${error.message}`, 'error')
  } finally {
    loading.value.reset = false
  }
}

async function reloadSystem() {
  try {
    await dserv.essCommand('ess::reload_system')
    logToTerminal('System reloaded', 'success')
  } catch (error) {
    console.error('Failed to reload system:', error)
    logToTerminal(`Failed to reload system: ${error.message}`, 'error')
  }
}

async function reloadProtocol() {
  try {
    await dserv.essCommand('ess::reload_protocol')
    logToTerminal('Protocol reloaded', 'success')
  } catch (error) {
    console.error('Failed to reload protocol:', error)
    logToTerminal(`Failed to reload protocol: ${error.message}`, 'error')
  }
}

async function reloadVariant() {
  try {
    await dserv.essCommand('ess::reload_variant')
    logToTerminal('Variant reloaded', 'success')
  } catch (error) {
    console.error('Failed to reload variant:', error)
    logToTerminal(`Failed to reload variant: ${error.message}`, 'error')
  }
}

async function saveSettings() {
  try {
    await dserv.essCommand('ess::save_settings')
    logToTerminal('Settings saved', 'success')
  } catch (error) {
    console.error('Failed to save settings:', error)
    logToTerminal(`Failed to save settings: ${error.message}`, 'error')
  }
}

async function resetSettings() {
  try {
    await dserv.essCommand('ess::reset_settings')
    logToTerminal('Settings reset to defaults', 'success')
  } catch (error) {
    console.error('Failed to reset settings:', error)
    logToTerminal(`Failed to reset settings: ${error.message}`, 'error')
  }
}

async function loadSettings() {
  try {
    await dserv.essCommand('ess::load_settings')
    logToTerminal('Settings loaded', 'success')
  } catch (error) {
    console.error('Failed to load settings:', error)
    logToTerminal(`Failed to load settings: ${error.message}`, 'error')
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
/* Same styling as before, but without connection status related styles */
:deep(.ant-form-item) {
  margin-bottom: 4px;
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
</style>