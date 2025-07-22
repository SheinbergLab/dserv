<template>
  <div style="height: 100%; display: flex; flex-direction: column; padding: 8px; overflow: hidden;">
    <!-- Clean Header -->
    <div
      style="flex-shrink: 0; display: flex; align-items: center; justify-content: center; margin-bottom: 12px; padding: 4px 0;">
      <div style="font-weight: 500; font-size: 14px;">ESS Control</div>
    </div>

    <!-- FIXED SECTION - Always visible essential controls -->
    <div
      style="flex-shrink: 0; margin-bottom: 12px; padding: 0 4px; display: flex; flex-direction: column; align-items: center;">

      <!-- Loading Progress Indicator -->
      <div v-if="isSystemBusy"
        style="background: #f0f7ff; border: 1px solid #91d5ff; border-radius: 6px; padding: 8px; margin-bottom: 8px; width: 100%;">
        <a-progress :percent="dserv.loadingState.percent" size="small"
          :status="dserv.loadingState.error ? 'exception' : 'active'" :show-info="true" />
        <div style="font-size: 10px; color: #1890ff; text-align: center; margin-top: 4px;">
          {{ loadingMessage }}
        </div>
        <div v-if="dserv.loadingState.elapsed > 0" style="font-size: 9px; color: #999; text-align: center;">
          {{ Math.round(dserv.loadingState.elapsed / 1000) }}s elapsed
        </div>
      </div>

      <!-- Subject Selection -->
      <div class="system-config subject-section"
        style="background: #f8f9fa; border: 1px solid #e8e8e8; border-radius: 6px; padding: 4px; margin-bottom: 8px; width: 100%; max-width: 100%; min-height: 40px;">
        <a-form-item label="Subject" style="margin-bottom: 0;" class="subject-item">
          <a-select v-model:value="dserv.state.subject" size="small" style="flex: 1; min-width: 140px;"
            :disabled="isSystemBusy" @change="setSubject">
            <a-select-option v-for="subject in subjects" :key="subject" :value="subject">
              {{ subject }}
            </a-select-option>
          </a-select>
        </a-form-item>
      </div>

      <!-- Recovery Button (only when needed) -->
      <div v-if="showRecoveryButton" style="text-align: center; margin-bottom: 8px; width: 100%;">
        <a-button size="small" type="dashed" danger @click="forceLoadingRecovery" style="font-size: 10px;">
          Force Recovery
        </a-button>
      </div>

      <!-- Control Buttons and Status Display -->
      <div
        style="background: #f8f9fa; border: 1px solid #e8e8e8; border-radius: 8px; padding: 8px; margin-bottom: 12px; width: 100%; max-width: 100%; min-height: 120px;">
        <!-- Control Buttons -->
        <div class="control-buttons" style="margin-bottom: 8px;">
          <a-space size="small">
            <a-button type="primary" size="small"
              :disabled="isSystemBusy || dserv.state.status === 'Running' || !dserv.state.currentVariant"
              @click="startExperiment">
              Go
            </a-button>
            <a-button danger size="small" :disabled="isSystemBusy || dserv.state.status !== 'Running'"
              @click="stopExperiment">
              Stop
            </a-button>
            <a-button size="small" :disabled="isSystemBusy || dserv.state.status === 'Running'"
              @click="resetExperiment">
              Reset
            </a-button>
          </a-space>
        </div>

        <!-- Status Display -->
        <div style="font-size: 12px; min-height: 72px;">
          <div style="display: flex; align-items: center; gap: 8px; margin-bottom: 4px; height: 20px;">
            <div class="obs-indicator" :class="{ 'is-active': dserv.state.inObs }"></div>
            <div style="display: flex; align-items: center; gap: 8px;">
              <span style="width: 50px; text-align: right; white-space: nowrap;"><strong>Status:</strong></span>
              <a-tag :color="getStatusColor(dserv.state.status)"
                style="margin: 0; min-width: 60px; text-align: center;">
                {{ dserv.state.status }}
              </a-tag>
            </div>
          </div>

          <!-- Additional status info -->
          <div style="padding-left: 20px; min-height: 48px;">
            <div style="display: flex; align-items: center; gap: 8px; margin-bottom: 4px; height: 20px;">
              <span style="width: 50px; text-align: right; white-space: nowrap;"><strong>Obs:</strong></span>
              <a-tag :color="dserv.state.inObs ? 'red' : 'default'"
                style="margin: 0; min-width: 60px; text-align: center; height: 22px; line-height: 1; display: flex; align-items: center; justify-content: center; padding: 0 4px; box-sizing: border-box; font-size: 12px;">
                {{ dserv.state.obsCount || 'None' }}
              </a-tag>
              <!-- Show trial count when available -->
              <span v-if="trialCount > 0" style="font-size: 10px; color: #666; margin-left: 8px;">
                ({{ trialCount }} trials)
              </span>
            </div>

            <div style="display: flex; align-items: center; gap: 4px; min-height: 16px;">
              <span style="width: 50px; text-align: right; white-space: nowrap;"><strong>File:</strong></span>
              <div style="flex: 1; display: flex; align-items: center; gap: 2px;">
                <span v-if="currentDatafile"
                  style="color: green; font-family: monospace; font-size: 10px; flex: 1; overflow: hidden; text-overflow: ellipsis;">
                  {{ currentDatafile.split('/').pop().replace('.ess', '') }}
                </span>
                <span v-else style="color: #999; font-size: 10px; flex: 1;">No file</span>
                <a-button size="small" :icon="h(currentDatafile ? CloseOutlined : FolderOpenOutlined)"
                  :type="currentDatafile ? 'default' : 'primary'" :danger="!!currentDatafile" :disabled="isSystemBusy"
                  @click="currentDatafile ? closeDatafile() : openDatafile()" :loading="datafileLoading"
                  style="width: 16px; height: 16px; font-size: 8px;" />
              </div>
            </div>
            <!-- Show loading completion message briefly -->
            <div v-if="lastLoadedTrials > 0 && !isSystemBusy" style="font-size: 10px; color: #52c41a; margin-top: 2px;">
              ✓ Ready: {{ lastLoadedTrials }} trials loaded
            </div>
          </div>
        </div>
      </div>

      <!-- System Configuration -->
      <div class="system-config"
        style="background: #f8f9fa; border: 1px solid #e8e8e8; border-radius: 6px; padding: 6px; margin-bottom: 8px; width: 100%; max-width: 100%; min-height: 140px;">
        <div
          style="font-weight: 500; margin-bottom: 6px; font-size: 12px; min-height: 18px; display: flex; align-items: center;">
          System Configuration
          <!-- Just show loading state, no complex detection -->
          <a-spin v-if="isSystemBusy" size="small" style="margin-left: 8px;" />
          <span v-if="isSystemBusy" style="margin-left: 8px; font-size: 10px; color: #666;">{{ dserv.loadingState.stage
            }}...</span>
        </div>

        <!-- System selection -->
        <a-form-item label="System" style="margin-bottom: 1px; min-height: 32px;" class="system-config-item">
          <div style="display: flex; align-items: center; gap: 4px;">
            <a-select v-model:value="dserv.state.currentSystem" size="small" style="flex: 1; min-width: 140px;"
              :disabled="isSystemBusy" @change="setSystem"
              :placeholder="isSystemBusy ? 'Loading...' : 'Select system...'">
              <a-select-option v-for="system in dserv.state.systems" :key="system" :value="system">
                {{ system }}
              </a-select-option>
            </a-select>
            <a-button size="small" :icon="h(ReloadOutlined)" :disabled="isSystemBusy" @click="reloadSystem"
              style="width: 24px; height: 24px;" />
          </div>
        </a-form-item>

        <!-- Protocol selection -->
        <a-form-item label="Protocol" style="margin-bottom: 1px; min-height: 32px;" class="system-config-item">
          <div style="display: flex; align-items: center; gap: 4px;">
            <a-select :value="displayProtocolValue" size="small" style="flex: 1; min-width: 140px;"
              :disabled="isSystemBusy || !dserv.state.currentSystem" @change="setProtocol"
              :placeholder="getProtocolPlaceholder" :allow-clear="true" :show-search="false">
              <a-select-option v-for="protocol in availableProtocols" :key="protocol" :value="protocol">
                {{ protocol }}
              </a-select-option>
            </a-select>
            <a-button size="small" :icon="h(ReloadOutlined)" :disabled="isSystemBusy" @click="reloadProtocol"
              style="width: 24px; height: 24px;" />
          </div>
        </a-form-item>

        <!-- Variant selection -->
        <a-form-item label="Variant" style="margin-bottom: 0; min-height: 32px;" class="system-config-item">
          <div style="display: flex; align-items: center; gap: 4px;">
            <a-select :value="displayVariantValue" size="small" style="flex: 1; min-width: 140px;"
              :disabled="isSystemBusy || !dserv.state.currentProtocol" @change="setVariant"
              :placeholder="getVariantPlaceholder" :allow-clear="true" :show-search="false">
              <a-select-option v-for="variant in availableVariants" :key="variant" :value="variant">
                {{ variant }}
              </a-select-option>
            </a-select>
            <a-button size="small" :icon="h(ReloadOutlined)" :disabled="isSystemBusy" @click="reloadVariant"
              style="width: 24px; height: 24px;" />
          </div>
        </a-form-item>
      </div>

      <!-- Settings Management -->
      <div
        style="background: #f8f9fa; border: 1px solid #e8e8e8; border-radius: 6px; padding: 8px; margin-bottom: 8px; width: 100%; max-width: 100%; min-height: 60px;">
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
    <div
      style="flex: 1; overflow-y: auto; overflow-x: hidden; padding: 0 4px; display: flex; flex-direction: column; align-items: center;">

      <!-- Variant Options - Collapsible -->
      <a-collapse v-if="variantOptions.length > 0" ghost :bordered="true"
        style="margin-bottom: 12px; width: 100%; max-width: 100%;" :default-active-key="['variant-options']">
        <a-collapse-panel key="variant-options" header="Variant Options">
          <template #extra>
            <div style="display: flex; align-items: center; gap: 4px;" @click.stop>
              <a-checkbox v-model:checked="autoReload" size="small" style="font-size: 9px; transform: scale(0.8);" />
              <span style="font-size: 9px;">Auto-reload</span>
              <a-button size="small" :icon="h(ReloadOutlined)" :disabled="isSystemBusy" @click="reloadVariant"
                style="width: 18px; height: 18px; font-size: 8px; margin-left: 4px;" title="Reload variant manually" />
            </div>
          </template>

          <div style="max-height: 200px; overflow-y: auto; padding-right: 4px;">
            <div v-for="option in variantOptions" :key="option.name" style="margin-bottom: 6px;">
              <div style="display: flex; align-items: center; justify-content: space-between; gap: 8px;">
                <span
                  style="font-size: 11px; flex: 1; text-align: left; white-space: nowrap; overflow: hidden; text-overflow: ellipsis;">
                  {{ option.name }}
                </span>
                <a-select v-model:value="option.selectedValue" size="small" class="variant-option-select"
                  style="width: 85px; flex-shrink: 0;" :disabled="isSystemBusy"
                  @change="(value) => setVariantOption(option.name, value)">
                  <a-select-option v-for="choice in option.choices" :key="choice.label" :value="choice.value">
                    {{ choice.label }}
                  </a-select-option>
                </a-select>
              </div>
            </div>
          </div>
        </a-collapse-panel>
      </a-collapse>

      <!-- System Parameters - Collapsible -->
      <a-collapse v-if="systemParameters.length > 0" ghost :bordered="true"
        style="margin-bottom: 12px; width: 100%; max-width: 100%;" :default-active-key="['system-parameters']">
        <a-collapse-panel key="system-parameters" header="System Parameters">
          <div style="max-height: 200px; overflow-y: auto; padding-right: 4px;">
            <div v-for="param in systemParameters" :key="param.name" style="margin-bottom: 6px;">
              <div style="display: flex; align-items: center; justify-content: space-between; gap: 8px;">
                <span
                  style="font-size: 11px; flex: 1; text-align: left; white-space: nowrap; overflow: hidden; text-overflow: ellipsis;">
                  {{ param.name }}
                </span>
                <a-input-number v-if="param.dataType === 'int' || param.dataType === 'float'"
                  v-model:value="param.value" size="small" style="width: 85px; flex-shrink: 0;"
                  :precision="param.dataType === 'float' ? 2 : 0" :step="param.dataType === 'float' ? 0.1 : 1"
                  :disabled="isSystemBusy" @change="() => setSystemParameter(param.name, param.value)" />
                <a-input v-else v-model:value="param.value" size="small" style="width: 85px; flex-shrink: 0;"
                  :disabled="isSystemBusy" @change="() => setSystemParameter(param.name, param.value)" />
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
const autoReload = ref(true)

const isSystemBusy = computed(() => {
  return dserv.loadingState.isLoading
})

const loadingMessage = computed(() => {
  if (dserv.loadingState.isLoading) {
    const { stage, message, percent } = dserv.loadingState

    // Show more detailed progress information
    if (stage === 'complete' && trialCount.value > 0) {
      return `Loaded ${trialCount.value} trials successfully`
    }

    return `${stage}: ${message} (${percent}%)`
  }
  return ''
})

// Trial count tracking
const trialCount = ref(0)
const lastLoadedTrials = ref(0)

const showRecoveryButton = computed(() => {
  return dserv.loadingState.timeout || (dserv.loadingState.elapsed > 30000)
})

const displayProtocolValue = computed(() => dserv.state.currentProtocol || null)
const displayVariantValue = computed(() => dserv.state.currentVariant || null)
const availableProtocols = computed(() => dserv.state.protocols)
const availableVariants = computed(() => dserv.state.variants)

const getProtocolPlaceholder = computed(() => {
  if (dserv.loadingState.isLoading) return "Loading..."
  if (!dserv.state.currentSystem) return "Select system first"
  return "Select protocol..."
})

const getVariantPlaceholder = computed(() => {
  if (dserv.loadingState.isLoading) return "Loading..."
  if (!dserv.state.currentProtocol) return "Select protocol first"
  return "Select variant..."
})

async function forceLoadingRecovery() {
  try {
    await dserv.forceLoadingRecovery()
    logToConsole('Loading state recovered', 'success')
  } catch (error) {
    logToConsole('Recovery failed', 'error')
  }
}

onMounted(() => {
  console.log('ExperimentControl component mounted')

  const cleanup = dserv.registerComponent('ExperimentControl', {
    subscriptions: [
      { pattern: 'ess/variant_info_json', every: 1 },
      { pattern: 'ess/param_settings', every: 1 },
      { pattern: 'ess/datafile', every: 1 },
      { pattern: 'ess/lastfile', every: 1 }
    ]
  })

  // Event handlers
  const unsubscribeLoading = dserv.on('loadingProgress', (progress) => {
    console.log('Loading progress:', progress)
  }, 'ExperimentControl')

  const unsubscribeStarted = dserv.on('loadingStarted', (data) => {
    console.log('Loading started:', data.operationId)
  }, 'ExperimentControl')

  const unsubscribeFinished = dserv.on('loadingFinished', (data) => {
    console.log('Loading finished')

    // If we have trial count info, show it
    if (lastLoadedTrials.value > 0) {
      setTimeout(() => {
        logToConsole(`Ready: ${lastLoadedTrials.value} trials loaded`, 'success')
      }, 500)
    }
  }, 'ExperimentControl')

  const unsubscribeError = dserv.on('loadingFailed', (data) => {
    logToConsole(`Loading failed: ${data.error}`, 'error')
  }, 'ExperimentControl')

  const unsubscribeTimeout = dserv.on('loadingTimeout', () => {
    logToConsole('Loading operation timed out', 'error')
  }, 'ExperimentControl')

  dserv.on('datapoint:ess/variant_info_json', (data) => {
    console.log('Received variant_info_json:', data.data)
    parseVariantInfoJson(data.data)
  }, 'ExperimentControl') // Add component ID

  dserv.on('datapoint:ess/param_settings', (data) => {
    console.log('Received param_settings:', data.data)
    parseSystemParameters(data.data)
  }, 'ExperimentControl') // Add component ID

  dserv.on('datapoint:ess/datafile', (data) => {
    console.log('Received datafile status:', data.data)
    currentDatafile.value = data.data || ''
    if (!data.data) {
      datafilename.value = ''
    }
  }, 'ExperimentControl') // Add component ID

  dserv.on('datapoint:ess/lastfile', (data) => {
    console.log('Last file processed:', data.data)
    if (data.data) {
      logToConsole(`File processed: ${data.data}`, 'success')
    }
  }, 'ExperimentControl') // Add component ID

  dserv.on('trialsLoaded', (data) => {
    trialCount.value = data.count
    lastLoadedTrials.value = data.count

    logToConsole(`Loaded ${data.count} trials`, 'success')
    console.log('Trials loaded event:', data)
  }, 'ExperimentControl')

  dserv.on('datapoint:ess/stiminfo', (data) => {
    console.log('Received stiminfo data:', data.data ? 'HAS DATA' : 'NO DATA')

    if (data.data) {
      try {
        const stimData = JSON.parse(data.data)
        if (stimData && stimData.rows && Array.isArray(stimData.rows)) {
          const newTrialCount = stimData.rows.length
          trialCount.value = newTrialCount
          lastLoadedTrials.value = newTrialCount

          console.log(`Stiminfo parsed: ${newTrialCount} trials available`)

          if (newTrialCount > 0) {
            setTimeout(() => {
              if (dserv.state.obsTotal === newTrialCount && dserv.state.obsId === 0) {
                console.log(`Confirmed obs count reset to 1/${newTrialCount}`)
              }
            }, 100)
          }
        }
      } catch (error) {
        console.error('Failed to parse stiminfo:', error)
      }
    }
  }, 'ExperimentControl') // Add component ID

  onUnmounted(() => {
    cleanup()
  })
})

const enhancedStatusInfo = computed(() => {
  const baseInfo = {
    status: dserv.state.status,
    obsCount: dserv.state.obsCount,
    inObs: dserv.state.inObs
  }

  if (trialCount.value > 0) {
    baseInfo.trialInfo = `${trialCount.value} trials loaded`
  }

  return baseInfo
})

function parseVariantInfoJson(rawData) {
  try {
    console.log('Parsing variant info JSON:', rawData)

    let variantInfo
    if (typeof rawData === 'string') {
      variantInfo = JSON.parse(rawData)
    } else {
      variantInfo = rawData
    }

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

    const variantOptionsArray = []

    loader_arg_names.forEach((argName, index) => {
      if (options[argName]) {
        const currentValue = loader_args[index]
        let selectedValue = null

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

    console.log(`Loader proc: ${loader_proc}`)
    console.log(`Current args: [${loader_args.join(', ')}]`)

  } catch (error) {
    console.error('Failed to parse variant info JSON:', error)
    variantOptions.value = []
  }
}

function parseSystemParameters(rawData) {
  try {
    console.log('Parsing system parameters:', rawData)

    const params = []
    const paramMatches = rawData.match(/(\w+)\s+{([^}]+)}/g)

    if (paramMatches) {
      paramMatches.forEach(match => {
        const [, paramName, paramData] = match.match(/(\w+)\s+{([^}]+)}/)
        const parts = paramData.trim().split(/\s+/)

        if (parts.length >= 3) {
          const value = parts[0]
          const variableType = parts[1]
          const dataType = parts[2]

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
  let filename = datafilename.value.trim()

  if (!filename && suggestedFilename.value) {
    filename = suggestedFilename.value
  }

  if (!filename) {
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
    const command = `ess::set_variant_args {${optionName} {${value}}}`
    console.log('Setting variant option with command:', command)

    await dserv.essCommand(command)

    if (autoReload.value) {
      try {
        await dserv.reloadVariant()
      } catch (reloadError) {
        console.error('Failed to auto-reload variant:', reloadError)
        logToConsole(`Auto-reload failed: ${reloadError.message}`, 'error')

        try {
          await dserv.essCommand('dservSet ess/status stopped')
          console.log('Reset system status to stopped after reload failure')
        } catch (resetError) {
          console.error('Failed to reset system status:', resetError)
        }
      }
    }
  } catch (error) {
    console.error('Failed to set variant option:', error)
    logToConsole(`Failed to set variant option: ${error.message}`, 'error')
  }
}

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

// WATCHERS
watch(() => dserv.state.connected, async (connected, wasConnected) => {
  if (connected && !wasConnected) {
    logToConsole('Reconnected to dserv', 'success')

    if (dserv.loadingState.isLoading) {
      console.log('Recovering loading state after reconnection')

      try {
        const actualStatus = await dserv.essCommand('dservGet ess/status', 3000)
        if (actualStatus !== 'loading') {
          console.log('Backend not loading after reconnect - clearing loading states')
          forceLoadingRecovery()
        }
      } catch (error) {
        console.warn('Could not verify backend state after reconnection')
        setTimeout(() => forceLoadingRecovery(), 2000)
      }
    }
  } else if (!connected) {
    logToConsole('Disconnected from dserv', 'error')
  }
})

watch(() => currentDatafile.value, (newFile, oldFile) => {
  if (oldFile && !newFile) {
    datafilename.value = ''
    suggestedFilename.value = ''
  }
})

function logToConsole(message, type = 'normal') {
  try {
    const levelMap = {
      'success': 'info',
      'error': 'error',
      'warning': 'warning',
      'info': 'info',
      'normal': 'info'
    }

    const level = levelMap[type] || 'info'
    errorService.addFrontendLog(level, message, 'frontend')

  } catch (error) {
    console.warn('Failed to log to terminal:', error)
    console.log(`[${type}] ${message}`)
  }
}

watch(() => dserv.state.subject, async (newSubject) => {
  if (newSubject) {
    // Future: implement subject data loading if needed
  }
})

// METHODS
async function setSubject(subject) {
  try {
    await dserv.setSubject(subject)
    logToConsole(`Subject set to: ${subject}`, 'success')
  } catch (error) {
    console.error('Failed to set subject:', error)
    logToConsole(`Failed to set subject: ${error.message}`, 'error')
  }
}

async function setSystem(system) {
  // RESET OBS INDICATOR IMMEDIATELY (keep this important behavior)
  dserv.state.obsCount = ''
  dserv.state.inObs = false

  try {
    await dserv.setSystem(system)
    logToConsole(`System loaded: ${system}`, 'success')
  } catch (error) {
    console.error('Failed to set system:', error)
    logToConsole(`Failed to load system: ${error.message}`, 'error')
  }
}

async function setProtocol(protocol) {
  try {
    await dserv.setProtocol(protocol)
    logToConsole(`Protocol loaded: ${protocol}`, 'success')
  } catch (error) {
    console.error('Failed to set protocol:', error)
    logToConsole(`Failed to load protocol: ${error.message}`, 'error')
  }
}

async function setVariant(variant) {
  try {
    await dserv.setVariant(variant)
    logToConsole(`Variant loaded: ${variant}`, 'success')
  } catch (error) {
    console.error('Failed to set variant:', error)
    logToConsole(`Failed to load variant: ${error.message}`, 'error')
  }
}

async function startExperiment() {
  try {
    await dserv.startExperiment()
    logToConsole('Experiment started', 'success')
  } catch (error) {
    console.error('Failed to start experiment:', error)
    logToConsole(`Failed to start: ${error.message}`, 'error')
  }
}

async function stopExperiment() {
  try {
    await dserv.stopExperiment()
    logToConsole('Experiment stopped', 'success')
  } catch (error) {
    console.error('Failed to stop experiment:', error)
    logToConsole(`Failed to stop: ${error.message}`, 'error')
  }
}

async function resetExperiment() {
  try {
    await dserv.resetExperiment()
    dserv.state.obsCount = ' '
    logToConsole('Experiment reset', 'success')
  } catch (error) {
    console.error('Failed to reset experiment:', error)
    logToConsole(`Failed to reset: ${error.message}`, 'error')
  }
}

async function reloadSystem() {
  try {
    await dserv.reloadSystem()
    logToConsole('System reloaded', 'success')
  } catch (error) {
    console.error('Failed to reload system:', error)
    logToConsole(`Failed to reload system: ${error.message}`, 'error')
  }
}

async function reloadProtocol() {
  try {
    await dserv.reloadProtocol()
    logToConsole('Protocol reloaded', 'success')
  } catch (error) {
    console.error('Failed to reload protocol:', error)
    logToConsole(`Failed to reload protocol: ${error.message}`, 'error')
  }
}

async function reloadVariant() {
  try {
    await dserv.reloadVariant()
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

:deep(.ant-select-loading .ant-select-selector) {
  background-color: #f5f5f5 !important;
  border-color: #d9d9d9 !important;
}

:deep(.ant-select-disabled .ant-select-selector) {
  background-color: #f5f5f5 !important;
  color: #999 !important;
  border-color: #d9d9d9 !important;
}

:deep(.ant-select-selector .ant-select-selection-placeholder) {
  color: #1890ff !important;
  font-style: italic;
}

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

.ant-tag {
  min-width: 60px;
  text-align: center;
  display: inline-block;
}

.ant-spin {
  color: #1890ff;
}

:deep(.ant-select-arrow .ant-select-suffix) {
  transition: all 0.3s ease;
}
</style>
