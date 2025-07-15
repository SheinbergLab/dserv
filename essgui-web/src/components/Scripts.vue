<template>
  <div style="height: 100%; display: flex; flex-direction: column; overflow: hidden;">

    <!-- Header with script selector and controls -->
    <div
      style="flex-shrink: 0; display: flex; align-items: center; justify-content: space-between; padding: 8px; border-bottom: 1px solid #d9d9d9; background: #fafafa;">
      <!-- Script Selector Dropdown -->
      <div style="display: flex; align-items: center; gap: 12px;">
        <a-select v-model:value="activeScript" size="small" style="width: 140px;"
          @change="(value) => switchScriptWithValidation(value)">
          <a-select-option v-for="script in scriptTabs" :key="script.name" :value="script.name">
            <div style="display: flex; align-items: center; justify-content: space-between; width: 100%;">
              <span>{{ script.label }}</span>
              <div style="display: flex; align-items: center; gap: 4px;">
                <!-- Modified indicator -->
                <span v-if="scripts[script.name]?.modified"
                  style="width: 6px; height: 6px; background: #ff4d4f; border-radius: 50%;"></span>
                <!-- Validation error indicator -->
                <span v-if="activeScript === script.name && validationResult && !validationResult.isValid"
                  style="width: 6px; height: 6px; background: #ff4d4f; border-radius: 50%;"
                  title="Has validation errors"></span>
              </div>
            </div>
          </a-select-option>
        </a-select>
      </div>

      <!-- Controls with icons and validation -->
      <div style="display: flex; align-items: center; gap: 12px;">
        <!-- Validation controls -->
        <a-tooltip title="Validate Script (Ctrl+Shift+V)">
          <a-button size="small" @click="validateCurrentScript" :loading="isValidating" :icon="h(CheckCircleOutlined)"
            :type="validationResult && !validationResult.isValid ? 'danger' : 'default'"
            style="width: 32px; height: 24px;" />
        </a-tooltip>

        <a-tooltip title="Auto-validate while typing">
          <a-checkbox v-model:checked="autoValidate" size="small">
            Auto
          </a-checkbox>
        </a-tooltip>

        <!-- NEW: Undo/Redo controls -->
        <a-tooltip title="Undo (Ctrl+Z)">
          <a-button size="small" @click="undoEdit" :disabled="!canUndo" :icon="h(UndoOutlined)"
            style="width: 32px; height: 24px;" />
        </a-tooltip>

        <a-tooltip title="Redo (Ctrl+Y)">
          <a-button size="small" @click="redoEdit" :disabled="!canRedo" :icon="h(RedoOutlined)"
            style="width: 32px; height: 24px;" />
        </a-tooltip>

        <!-- Existing toolbar -->
        <a-tooltip title="Format Code (Ctrl+Shift+F)">
          <a-button size="small" @click="formatScript" :disabled="!currentScript?.content"
            :icon="h(FormatPainterOutlined)" style="width: 32px; height: 24px;" />
        </a-tooltip>

        <a-tooltip title="Save Script (Ctrl+S)">
          <a-button type="primary" size="small" @click="saveScriptWithErrorHandling" :loading="isSaving"
            :disabled="!currentScript?.modified" :icon="h(SaveOutlined)" style="width: 32px; height: 24px;" />
        </a-tooltip>

        <a-tooltip title="Pull from Git">
          <a-button size="small" @click="pullScripts" :loading="isGitBusy" :icon="h(DownloadOutlined)"
            style="width: 32px; height: 24px;" />
        </a-tooltip>

        <a-tooltip title="Push to Git">
          <a-button size="small" @click="pushScripts" :loading="isGitBusy" :disabled="isGitBusy"
            :icon="h(UploadOutlined)" style="width: 32px; height: 24px;" />
        </a-tooltip>

        <!-- Divider -->
        <a-divider type="vertical" style="margin: 0;" />

        <!-- Git Branch Dropdown -->
        <a-select v-model:value="currentBranch" size="small" style="width: 120px;" :loading="isGitBusy"
          @change="switchBranch" :disabled="isGitBusy" placeholder="Select branch">
          <a-select-option v-for="branch in availableBranches" :key="branch" :value="branch">
            {{ branch }}
          </a-select-option>
        </a-select>

        <!-- Backup menu dropdown -->
        <a-dropdown>
          <template #overlay>
            <a-menu>
              <a-menu-item key="backups" @click="showBackupManager = true">
                <HistoryOutlined /> View Backups
              </a-menu-item>
              <a-menu-item key="validate-all" @click="validateAllScripts">
                <CheckCircleOutlined /> Validate All Scripts
              </a-menu-item>
              <a-menu-divider />
              <a-menu-item key="debug-mode" @click="showDebugMode = !showDebugMode">
                <BugOutlined /> {{ showDebugMode ? 'Hide' : 'Show' }} Debug Mode
              </a-menu-item>
            </a-menu>
          </template>
          <a-button size="small" style="width: 32px; height: 24px;">
            <EllipsisOutlined />
          </a-button>
        </a-dropdown>
      </div>
    </div>

    <!-- Editor container -->
    <div ref="editorContainer" style="flex: 1; overflow: hidden; background: #1e1e1e;"></div>

    <!-- Validation Panel -->
    <div v-if="showValidationPanel && validationResult"
      style="flex-shrink: 0; max-height: 200px; overflow-y: auto; background: #fafafa; border-top: 1px solid #d9d9d9; padding: 8px;">
      <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px;">
        <div style="font-weight: 500; font-size: 12px;">
          Validation Results: {{ validationResult.summary }}
        </div>
        <div style="display: flex; gap: 8px;">
          <a-button size="small" @click="validateCurrentScript" :loading="isValidating" style="height: 24px;">
            Re-validate
          </a-button>
          <a-button size="small" @click="showValidationPanel = false" style="height: 24px;">
            Hide
          </a-button>
        </div>
      </div>

      <!-- Debug Panel -->
      <div v-if="showDebugMode" style="flex-shrink: 0; background: #fafafa; border-top: 1px solid #d9d9d9;">
        <backup-debugger />
      </div>
      <!-- Errors -->
      <div v-if="validationResult.errors.length > 0" style="margin-bottom: 8px;">
        <div style="font-weight: 500; color: #ff4d4f; font-size: 11px; margin-bottom: 4px;">
          Errors ({{ validationResult.errors.length }})
        </div>
        <div v-for="error in validationResult.errors" :key="`error-${error.line}-${error.column}`"
          style="font-size: 11px; margin-bottom: 2px; cursor: pointer; padding: 2px 4px; border-radius: 2px;"
          class="validation-item error-item" @click="jumpToError(error)">
          <span style="color: #ff4d4f;">●</span>
          Line {{ error.line }}: {{ error.message }}
        </div>
      </div>

      <!-- Warnings -->
      <div v-if="validationResult.warnings.length > 0">
        <div style="font-weight: 500; color: #faad14; font-size: 11px; margin-bottom: 4px;">
          Warnings ({{ validationResult.warnings.length }})
        </div>
        <div v-for="warning in validationResult.warnings" :key="`warning-${warning.line}-${warning.column}`"
          style="font-size: 11px; margin-bottom: 2px; cursor: pointer; padding: 2px 4px; border-radius: 2px;"
          class="validation-item warning-item" @click="jumpToError(warning)">
          <span style="color: #faad14;">⚠</span>
          Line {{ warning.line }}: {{ warning.message }}
        </div>
      </div>
    </div>

    <!-- Status bar -->
    <div
      style="flex-shrink: 0; display: flex; justify-content: space-between; align-items: center; padding: 4px 8px; background: #f0f0f0; border-top: 1px solid #d9d9d9; font-size: 11px; color: #666;">
      <div style="display: flex; gap: 12px;">
        <span>{{ currentScript?.name?.toUpperCase() }} Script</span>
        <span v-if="currentScript?.modified" style="color: #ff9500;">Modified</span>

        <!-- Validation status - show ONLY ONE status -->
        <template v-if="validationResult">
          <!-- Errors take priority -->
          <span v-if="!validationResult.isValid" style="color: #ff4d4f;">
            {{ validationResult.errors.length }} error(s)
          </span>
          <!-- Then warnings (only if no errors) -->
          <span v-else-if="validationResult.warnings.length > 0" style="color: #faad14;">
            {{ validationResult.warnings.length }} warning(s)
          </span>
          <!-- Valid only if no errors AND no warnings -->
          <span v-else style="color: #52c41a;">
            ✓ Valid
          </span>
          <span v-if="showDebugMode" style="color: #722ed1;">🐛 Debug</span>
        </template>

        <span>Tcl</span>
        <span v-if="currentBranch" style="color: #1890ff;">{{ currentBranch }}</span>
      </div>
      <div style="display: flex; gap: 12px;">
        <span>{{ keyBindings === 'emacs' ? 'Emacs Bindings' : 'Default Bindings' }}</span>
        <span>Ctrl+S: Save | Ctrl+Z: Undo | Ctrl+Y: Redo | Ctrl+Shift+V: Validate | {{ keyBindings === 'emacs' ?
          'Ctrl+/: Search' : 'Ctrl+F: Search' }}</span>
      </div>
    </div>

    <!-- Backup Manager Modal -->
    <a-modal v-model:open="showBackupManager" title="Script Backups" width="600px" :footer="null">
      <backup-manager :script-type="activeScript" @restore="restoreBackup" @close="showBackupManager = false" />
    </a-modal>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, nextTick, watch } from 'vue'
import {
  FormatPainterOutlined,
  SaveOutlined,
  DownloadOutlined,
  UploadOutlined,
  CheckCircleOutlined,
  HistoryOutlined,
  EllipsisOutlined,
  ReloadOutlined,
  BugOutlined,
  UndoOutlined,    // NEW
  RedoOutlined     // NEW
} from '@ant-design/icons-vue'
import { h } from 'vue'
import { Modal, message } from 'ant-design-vue'
import { dserv } from '../services/dserv.js'

// Import validation and backup components
import { TclLinter } from '../utils/TclLinter.js'
import BackupManager from '../components/BackupManager.vue'
import BackupDebugger from '../components/BackupDebugger.vue'

// CodeMirror imports
import { EditorSelection } from '@codemirror/state'
import { EditorState } from '@codemirror/state'
import { EditorView, keymap, lineNumbers } from '@codemirror/view'
import { basicSetup } from 'codemirror'
import { StreamLanguage, indentUnit } from '@codemirror/language'
import { tcl } from '@codemirror/legacy-modes/mode/tcl'
import {
  cursorLineStart,
  cursorLineEnd,
  deleteToLineEnd,
  cursorDocStart,
  cursorDocEnd,
  indentSelection,
  indentLess,
  insertTab,
  undo,           // NEW
  redo,           // NEW
  history,        // NEW
  historyField    // NEW
} from '@codemirror/commands'
import { search, highlightSelectionMatches, searchKeymap } from '@codemirror/search'
import { oneDark } from '@codemirror/theme-one-dark'
import TclFormatter from '../utils/TclFormatter.js'

// Component state
const editorContainer = ref(null)
let editorView = null

const activeScript = ref('system')
const keyBindings = ref('emacs')
const isSaving = ref(false)
const isGitBusy = ref(false)
const isUpdatingContent = ref(false)

// Validation state
const validationResult = ref(null)
const showValidationPanel = ref(false)
const isValidating = ref(false)
const autoValidate = ref(true)
const showBackupManager = ref(false)
const showDebugMode = ref(false)

// NEW: Undo/Redo state
const canUndo = ref(false)
const canRedo = ref(false)

// Git branch state
const currentBranch = ref('')
const availableBranches = ref([])

// UPDATED: Scripts with undo tracking
const scripts = ref({
  system: {
    name: 'system',
    content: '',
    modified: false,
    loaded: false,
    unchangedHistoryDepth: 0  // NEW: Track history depth at last save
  },
  protocol: {
    name: 'protocol',
    content: '',
    modified: false,
    loaded: false,
    unchangedHistoryDepth: 0  // NEW
  },
  loaders: {
    name: 'loaders',
    content: '',
    modified: false,
    loaded: false,
    unchangedHistoryDepth: 0  // NEW
  },
  variants: {
    name: 'variants',
    content: '',
    modified: false,
    loaded: false,
    unchangedHistoryDepth: 0  // NEW
  },
  stim: {
    name: 'stim',
    content: '',
    modified: false,
    loaded: false,
    unchangedHistoryDepth: 0  // NEW
  }
})

const scriptTabs = [
  { name: 'system', label: 'System' },
  { name: 'protocol', label: 'Protocol' },
  { name: 'loaders', label: 'Loaders' },
  { name: 'variants', label: 'Variants' },
  { name: 'stim', label: 'Stim' }
]

// Computed properties
const currentScript = computed(() => scripts.value[activeScript.value])

// Track if we've made any changes since last git operation
const hasChangesSinceLastGit = ref(false)

// Watch for any script modifications to track git state
watch(scripts, (newScripts) => {
  const hasModified = Object.values(newScripts).some(script => script.modified)
  if (hasModified) {
    hasChangesSinceLastGit.value = true
  }
}, { deep: true })

// Auto-validation watcher (debounced)
let validationTimeout = null
watch(() => currentScript.value?.content, (newContent) => {
  if (!autoValidate.value || !newContent) return

  if (validationTimeout) {
    clearTimeout(validationTimeout)
  }

  validationTimeout = setTimeout(() => {
    validateCurrentScript()
  }, 1000)
})

// NEW: Undo/Redo functions
function undoEdit() {
  if (editorView && canUndo.value) {
    undo(editorView)
    updateUndoRedoState()
  }
}

function redoEdit() {
  if (editorView && canRedo.value) {
    redo(editorView)
    updateUndoRedoState()
  }
}

function updateUndoRedoState() {
  if (!editorView) return

  const histState = editorView.state.field(historyField, false)
  if (histState) {
    canUndo.value = histState.done.length > 0
    canRedo.value = histState.undone.length > 0

    // Check if we're back to unchanged state
    const currentDepth = histState.done.length
    const script = currentScript.value

    if (script) {
      const wasModified = script.modified // Track what it was before

      // If we're at depth 0 and unchanged depth is 0, we're at the loaded state
      if (currentDepth === 0 && script.unchangedHistoryDepth === 0) {
        script.modified = false
      } else if (currentDepth <= script.unchangedHistoryDepth) {
        // We've undone back to or past the unchanged state
        script.modified = false
      } else {
        // We're beyond the unchanged state
        script.modified = true
      }

      // ADD THIS DEBUG LINE:
      if (wasModified !== script.modified) {
        console.log(`updateUndoRedoState: changed ${script.name} from modified=${wasModified} to modified=${script.modified}, depth=${currentDepth}, unchangedDepth=${script.unchangedHistoryDepth}`)
      }
    }
  }
}

// Fixed backend validation in Scripts.vue validateCurrentScript
async function validateCurrentScript() {
  if (!currentScript.value?.content) return

  isValidating.value = true
  try {
    const linter = new TclLinter()
    const result = linter.lint(currentScript.value.content)

    validationResult.value = result
    showValidationPanel.value = result.errors.length > 0 || result.warnings.length > 0

    // Backend validation if frontend passes - USE MINIMAL LEVEL for auto-validation
    if (result.isValid) {
      try {
        // Always use minimal for auto-validation to avoid backend warnings
        const backendResult = await dserv.essCommand(
          `ess::validate_script_minimal {${currentScript.value.content}}`
        )

        console.log('Backend validation result:', backendResult)

        // Only report backend issues if there are actual syntax errors
        if (backendResult && typeof backendResult === 'string') {
          if (backendResult.includes('valid 0')) {
            // Backend found actual syntax errors
            validationResult.value.errors.push({
              line: 0,
              column: 0,
              message: 'Backend syntax validation failed',
              severity: 'error'
            })
            validationResult.value.isValid = false
            console.error('Backend validation details:', backendResult)
          }
          // Don't add warnings for backend unavailable - just log it
        }
      } catch (backendError) {
        console.log('Backend validation unavailable:', backendError.message)
        // Don't add any warnings to the UI for backend unavailability
      }
    }
  } catch (error) {
    console.error('Validation error:', error)
    validationResult.value = {
      isValid: false,
      errors: [{
        line: 0,
        column: 0,
        message: 'Validation failed: ' + error.message,
        severity: 'error'
      }],
      warnings: [],
      summary: 'Validation error'
    }
  } finally {
    isValidating.value = false
  }
}

// Updated save script validation
async function saveScriptWithErrorHandling() {
  if (!currentScript.value.modified) return

  try {
    // Use strict validation for save operations
    const linter = new TclLinter()
    const frontendResult = linter.lint(currentScript.value.content)

    // Always validate with backend on save using minimal level to avoid false positives
    let backendResult
    try {
      backendResult = await dserv.essCommand(
        `ess::validate_script_minimal {${currentScript.value.content}}`
      )
    } catch (error) {
      console.warn('Backend validation unavailable for save:', error)
      // Continue with save if backend unavailable
    }

    validationResult.value = frontendResult

    // Check for critical errors
    if (!frontendResult.isValid) {
      const criticalErrors = frontendResult.errors.filter(e =>
        e.message.includes('syntax') ||
        e.message.includes('brace') ||
        e.message.includes('quote')
      )

      if (criticalErrors.length > 0) {
        const shouldContinue = await new Promise((resolve) => {
          Modal.confirm({
            title: 'Syntax Errors Detected',
            content: `This script has ${criticalErrors.length} syntax error(s). Saving may cause issues when the system loads this script.`,
            okText: 'Save Anyway',
            okType: 'danger',
            cancelText: 'Fix Errors First',
            onOk: () => resolve(true),
            onCancel: () => resolve(false)
          })
        })

        if (!shouldContinue) {
          showValidationPanel.value = true
          return
        }
      }
    }

    // Proceed with save
    await saveScript()

  } catch (error) {
    console.error('Save with error handling failed:', error)
    message.error(`Failed to save script: ${error.message}`)
  }
}

async function validateCurrentScriptStrict() {
  if (!currentScript.value?.content) return

  isValidating.value = true
  try {
    // Use frontend strict validation
    const linter = new TclLinter()
    const result = linter.lint(currentScript.value.content)

    // Always try backend strict validation for manual validation
    try {
      const backendResult = await dserv.essCommand(
        `ess::validate_script_strict {${currentScript.value.content}}`
      )

      console.log('Backend strict validation result:', backendResult)

      // Parse backend warnings and add them
      if (backendResult && typeof backendResult === 'string') {
        if (backendResult.includes('warnings')) {
          // Could parse and add backend warnings here if needed
          console.log('Backend found additional warnings:', backendResult)
        }
      }
    } catch (backendError) {
      console.log('Backend strict validation unavailable:', backendError.message)
    }

    validationResult.value = result
    showValidationPanel.value = true // Always show panel for manual validation

  } catch (error) {
    console.error('Strict validation error:', error)
    validationResult.value = {
      isValid: false,
      errors: [{
        line: 0,
        column: 0,
        message: 'Validation failed: ' + error.message,
        severity: 'error'
      }],
      warnings: [],
      summary: 'Validation error'
    }
  } finally {
    isValidating.value = false
  }
}

// Update the validateAllScripts to also use minimal validation
async function validateAllScripts() {
  const results = {}

  for (const scriptTab of scriptTabs) {
    if (scripts.value[scriptTab.name]?.content) {
      const linter = new TclLinter()
      results[scriptTab.name] = linter.lint(scripts.value[scriptTab.name].content)
    }
  }

  const allValid = Object.values(results).every(r => r.isValid)
  const totalErrors = Object.values(results).reduce((sum, r) => sum + r.errors.length, 0)
  const totalWarnings = Object.values(results).reduce((sum, r) => sum + r.warnings.length, 0)

  if (allValid && totalWarnings === 0) {
    message.success('All scripts are valid!')
  } else if (allValid) {
    message.info(`All scripts are syntactically valid but have ${totalWarnings} warning(s)`)
  } else {
    const summary = []
    if (totalErrors > 0) summary.push(`${totalErrors} error(s)`)
    if (totalWarnings > 0) summary.push(`${totalWarnings} warning(s)`)

    message.warning(`Validation complete: ${summary.join(', ')} found across all scripts`)
  }

  console.log('Validation results for all scripts:', results)
}

function jumpToError(error) {
  if (!editorView || !error.line) return

  try {
    const line = editorView.state.doc.line(error.line)
    const pos = line.from + (error.column || 0)

    editorView.dispatch({
      selection: { anchor: pos },
      effects: EditorView.scrollIntoView(pos, { y: 'center' })
    })

    editorView.focus()
  } catch (e) {
    console.error('Failed to jump to error:', e)
  }
}

// Enhanced script switching with validation
function switchScriptWithValidation(scriptName) {
  validationResult.value = null
  showValidationPanel.value = false
  switchScript(scriptName)

  if (autoValidate.value) {
    setTimeout(() => {
      validateCurrentScript()
    }, 500)
  }
}

// Replace the existing switchScript function with this:

function switchScript(scriptName) {
  activeScript.value = scriptName
  const newContent = scripts.value[scriptName]?.content || ''

  if (editorView) {
    // Always recreate the editor when switching scripts to ensure clean history
    editorView.destroy()
    createEditorWithContent(newContent)

    // Set the unchanged state properly for the new script
    nextTick(() => {
      if (scripts.value[scriptName] && editorView) {
        const histState = editorView.state.field(historyField, false)
        if (histState) {
          // For script switching, we want a clean slate
          scripts.value[scriptName].unchangedHistoryDepth = 0
          
          // Only mark as modified if the script was actually modified before
          // (this preserves the modified state when switching between tabs)
          console.log(`switchScript: Set ${scriptName} unchangedDepth=0, preserving modified=${scripts.value[scriptName].modified}`)
        }
      }
      updateUndoRedoState()
    })
  }
}

// Backup methods
async function restoreBackup(backupFile) {
  try {
    await dserv.essCommand(`ess::restore_script_backup ${activeScript.value} {${backupFile}}`)
    requestScriptData()
    message.success('Script restored from backup')
    showBackupManager.value = false
  } catch (error) {
    console.error('Failed to restore backup:', error)
    message.error(`Failed to restore backup: ${error.message}`)
  }
}

// Git branch operations
async function switchBranch(branchName) {
  if (branchName === currentBranch.value) return

  isGitBusy.value = true
  try {
    console.log(`Switching to branch: ${branchName}`)
    await dserv.gitSwitchBranch(branchName)
    console.log(`Successfully switched to branch: ${branchName}`)

    hasChangesSinceLastGit.value = false

    setTimeout(() => {
      requestScriptData()
    }, 500)
  } catch (error) {
    console.error(`Failed to switch to branch ${branchName}:`, error)
    requestGitData()
  } finally {
    isGitBusy.value = false
  }
}

function createEditor() {
   createEditorWithContent(currentScript.value.content)
}

// Add this function right after the createEditor() function in your Scripts.vue

function createEditorWithContent(initialContent = '') {
  if (!editorContainer.value) return

  const saveCommand = {
    key: 'Ctrl-s',
    run: () => {
      saveScriptWithErrorHandling()
      return true
    }
  }

  const searchCommand = {
    key: 'Ctrl-/',
    run: (view) => {
      import('@codemirror/search').then(({ openSearchPanel }) => {
        openSearchPanel(view)
      })
      return true
    }
  }

  const formatCommand = {
    key: 'Ctrl-Shift-f',
    run: () => {
      formatScript()
      return true
    }
  }

  const validateCommand = {
    key: 'Ctrl-Shift-v',
    run: () => {
      validateCurrentScript()
      return true
    }
  }

  const undoCommand = {
    key: 'Ctrl-z',
    run: (view) => {
      const result = undo(view)
      updateUndoRedoState()
      return result
    }
  }

  const redoCommand = {
    key: 'Ctrl-y',
    run: (view) => {
      const result = redo(view)
      updateUndoRedoState()
      return result
    }
  }

  const redoAltCommand = {
    key: 'Ctrl-Shift-z',
    run: (view) => {
      const result = redo(view)
      updateUndoRedoState()
      return result
    }
  }

  const emacsBindings = [
    { key: 'Ctrl-a', run: cursorLineStart },
    { key: 'Ctrl-e', run: cursorLineEnd },
    { key: 'Ctrl-k', run: deleteToLineEnd },
    { key: 'Alt-<', run: cursorDocStart },
    { key: 'Alt->', run: cursorDocEnd },
    {
      key: 'Enter',
      run: (view) => {
        return autoIndentNewline(view)
      }
    },
    { key: 'Tab', run: handleSmartTab },
    { key: 'Shift-Tab', run: indentLess },
    formatCommand,
    searchCommand
  ]

  const defaultBindings = [
    {
      key: 'Enter',
      run: (view) => {
        return autoIndentNewline(view)
      }
    },
    { key: 'Tab', run: handleSmartTab },
    formatCommand,
    { key: 'Shift-Tab', run: indentLess }
  ]

  const currentKeybindings = keyBindings.value === 'emacs' ? emacsBindings : defaultBindings
  const searchKeybindings = keyBindings.value === 'emacs' ? [] : searchKeymap

  const allKeybindings = [
    ...currentKeybindings,
    ...searchKeybindings,
    saveCommand,
    validateCommand,
    undoCommand,
    redoCommand,
    redoAltCommand
  ]

  const state = EditorState.create({
    doc: initialContent,
    extensions: [
      basicSetup,
      history(),  // Fresh history
      StreamLanguage.define(tcl),
      oneDark,
      lineNumbers(),
      search(),
      highlightSelectionMatches(),
      EditorState.tabSize.of(4),
      EditorView.lineWrapping,
      keymap.of(allKeybindings),
      EditorView.updateListener.of((update) => {
        if (update.docChanged && !isUpdatingContent.value) {
          const newContent = update.state.doc.toString()
          scripts.value[activeScript.value].content = newContent
          scripts.value[activeScript.value].modified = true

          updateUndoRedoState()
        }

        // Auto-indent logic (same as in createEditor)
        if (update.docChanged) {
          try {
            let foundNewline = false
            let newlinePos = -1

            update.changes.iterChanges((fromA, toA, fromB, toB, inserted) => {
              const insertedText = inserted.toString()
              if (insertedText.includes('\n')) {
                foundNewline = true
                newlinePos = fromB + insertedText.indexOf('\n') + 1
              }
            })

            if (foundNewline && newlinePos > 0) {
              setTimeout(() => {
                try {
                  const view = update.view
                  const state = view.state
                  const line = state.doc.lineAt(newlinePos)

                  if (line.text.trim() === '') {
                    const allText = state.doc.toString()
                    const lines = TclFormatter.splitLines(allText)
                    const lineNum = line.number - 1

                    const indent = TclFormatter.calculateLineIndent(lines, lineNum, 4)

                    if (indent > 0) {
                      const indentText = ' '.repeat(indent)

                      isUpdatingContent.value = true

                      view.dispatch({
                        changes: {
                          from: line.from,
                          to: line.to,
                          insert: indentText
                        },
                        selection: EditorSelection.cursor(line.from + indent)
                      })

                      nextTick(() => {
                        isUpdatingContent.value = false
                      })
                    }
                  }
                } catch (error) {
                  console.error('Error in auto-indent:', error)
                }
              }, 10)
            }
          } catch (error) {
            console.error('Error in update listener:', error)
          }
        }
      }),
      EditorView.theme({
        '&': { height: '100%' },
        '.cm-scroller': {
          fontFamily: 'Monaco, Menlo, "Ubuntu Mono", monospace',
          fontSize: '12px'
        },
        '.cm-focused': { outline: 'none' },
        '.cm-search': {
          backgroundColor: '#2d3748',
          border: '1px solid #4a5568',
          borderRadius: '4px'
        },
        '.cm-search input': {
          backgroundColor: '#1a202c',
          color: '#e2e8f0',
          border: '1px solid #4a5568',
          borderRadius: '2px'
        },
        '.cm-search button': {
          backgroundColor: '#4a5568',
          color: '#e2e8f0',
          border: '1px solid #4a5568',
          borderRadius: '2px'
        },
        '.cm-search button:hover': {
          backgroundColor: '#718096'
        }
      })
    ]
  })

  editorView = new EditorView({
    state,
    parent: editorContainer.value
  })

  // Initialize undo/redo state after editor creation
  nextTick(() => {
    updateUndoRedoState()
  })
}

function updateEditorContent(content, clearHistory = false) {
  if (!editorView) return

  const currentContent = editorView.state.doc.toString()
  if (currentContent !== content) {
    isUpdatingContent.value = true

    if (clearHistory) {
      // For fresh content from backend, destroy and recreate the editor
      const parent = editorView.dom.parentNode
      editorView.destroy()
      
      // Recreate with fresh content and history
      createEditorWithContent(content)
    } else {
      // For user edits, just update the content
      editorView.dispatch({
        changes: {
          from: 0,
          to: editorView.state.doc.length,
          insert: content
        }
      })
    }

    nextTick(() => {
      isUpdatingContent.value = false

      // Set the unchanged history depth and modified state
      if (scripts.value[activeScript.value] && editorView) {
        const histState = editorView.state.field(historyField, false)
        if (histState) {
          const currentDepth = histState.done.length

          if (clearHistory) {
            // Fresh content from backend - this is the clean baseline
            scripts.value[activeScript.value].unchangedHistoryDepth = 0
            scripts.value[activeScript.value].modified = false
            console.log(`updateEditorContent: Cleared history for ${activeScript.value}, unchangedDepth=0, modified=false`)
          } else {
            // User edit - maintain existing unchanged depth
            scripts.value[activeScript.value].unchangedHistoryDepth = currentDepth
            scripts.value[activeScript.value].modified = false
            console.log(`updateEditorContent: Set ${activeScript.value} unchangedDepth=${currentDepth}, modified=false`)
          }
        }
      }

      updateUndoRedoState()
    })
  }
}

function formatScript() {
  if (!currentScript.value.content) return

  try {
    const formattedContent = TclFormatter.formatTclCode(currentScript.value.content, 4)
    scripts.value[activeScript.value].content = formattedContent
    scripts.value[activeScript.value].modified = true
    updateEditorContent(formattedContent)
  } catch (error) {
    console.error('Error formatting script:', error)
    const lines = currentScript.value.content.split('\n')
    let indentLevel = 0
    const formatted = []

    for (let line of lines) {
      const trimmed = line.trim()
      if (!trimmed) {
        formatted.push('')
        continue
      }

      if (trimmed.startsWith('}')) {
        indentLevel = Math.max(0, indentLevel - 1)
      }

      formatted.push('    '.repeat(indentLevel) + trimmed)

      if (trimmed.endsWith('{')) {
        indentLevel++
      }
    }

    const formattedContent = formatted.join('\n')
    scripts.value[activeScript.value].content = formattedContent
    scripts.value[activeScript.value].modified = true
    updateEditorContent(formattedContent)
  }
}

// UPDATED: saveScript with unchanged history depth tracking
async function saveScript() {
  if (!currentScript.value.modified) return

  isSaving.value = true
  try {
    const scriptContent = currentScript.value.content
    const cmd = `ess::save_script ${activeScript.value} {${scriptContent}}`

    console.log(`Saving ${activeScript.value} script...`)
    await dserv.essCommand(cmd)

    // NEW: Mark as unchanged and record history depth
    scripts.value[activeScript.value].modified = false

    // NEW: Record the current history depth as the "unchanged" point
    if (editorView) {
      const histState = editorView.state.field(historyField, false)
      if (histState) {
        scripts.value[activeScript.value].unchangedHistoryDepth = histState.done.length
      }
    }

    console.log(`${activeScript.value} script saved successfully`)

    validationResult.value = null
    showValidationPanel.value = false

  } catch (error) {
    console.error(`Failed to save ${activeScript.value} script:`, error)
  } finally {
    isSaving.value = false
  }
}

async function pullScripts() {
  isGitBusy.value = true
  try {
    console.log('Pulling scripts from git...')
    await dserv.gitCommand('git::pull')
    console.log('Git pull completed successfully')

    hasChangesSinceLastGit.value = false

    setTimeout(() => {
      requestScriptData()
    }, 500)
  } catch (error) {
    console.error('Failed to pull scripts:', error)
  } finally {
    isGitBusy.value = false
  }
}

async function pushScripts() {
  isGitBusy.value = true
  try {
    console.log('Committing and pushing scripts to git...')
    await dserv.gitCommand('git::commit_and_push')
    console.log('Git push completed successfully')

    hasChangesSinceLastGit.value = false
  } catch (error) {
    console.error('Failed to push scripts:', error)
  } finally {
    isGitBusy.value = false
  }
}

async function reloadSystem() {
  try {
    await dserv.essCommand('ess::reload_system')
    message.success('System reloaded')
  } catch (error) {
    console.error('Failed to reload system:', error)
    message.error(`Failed to reload system: ${error.message}`)
  }
}

async function reloadProtocol() {
  try {
    await dserv.essCommand('ess::reload_protocol')
    message.success('Protocol reloaded')
  } catch (error) {
    console.error('Failed to reload protocol:', error)
    message.error(`Failed to reload protocol: ${error.message}`)
  }
}

async function reloadVariant() {
  try {
    await dserv.essCommand('ess::reload_variant')
    message.success('Variant reloaded')
  } catch (error) {
    console.error('Failed to reload variant:', error)
    message.error(`Failed to reload variant: ${error.message}`)
  }
}

function autoIndentNewline(view) {
  console.log('=== autoIndentNewline called ===')

  const state = view.state
  const selection = state.selection.main
  const line = state.doc.lineAt(selection.head)

  console.log('Current line:', line.text)
  console.log('Cursor position:', selection.head)

  const allText = state.doc.toString()
  const lines = TclFormatter.splitLines(allText)
  const currentLineNum = line.number - 1

  console.log('Current line number:', currentLineNum)

  try {
    const nextLineIndent = TclFormatter.calculateLineIndent(lines, currentLineNum + 1, 4)

    console.log('Calculated next line indent:', nextLineIndent)

    const newIndent = ' '.repeat(nextLineIndent)
    const newline = '\n' + newIndent

    console.log('Inserting newline with indent:', JSON.stringify(newline))

    view.dispatch({
      changes: { from: selection.head, insert: newline },
      selection: { anchor: selection.head + newline.length }
    })

    console.log('Auto-indent completed successfully')
    return true
  } catch (error) {
    console.error('Error in auto-indent:', error)
    console.log('Using fallback auto-indent')

    // Fallback to simple indentation
    const currentIndent = line.text.match(/^\s*/)[0]
    const newline = '\n' + currentIndent

    view.dispatch({
      changes: { from: selection.head, insert: newline },
      selection: { anchor: selection.head + newline.length }
    })

    return true
  }
}

function handleSmartTab(view) {
  const state = view.state
  const selection = state.selection.main
  const line = state.doc.lineAt(selection.head)

  const allText = state.doc.toString()
  const lines = TclFormatter.splitLines(allText)
  const currentLineNum = line.number - 1

  try {
    const targetIndent = TclFormatter.calculateLineIndent(lines, currentLineNum, 4)

    const lineText = line.text
    const currentIndentMatch = lineText.match(/^(\s*)/)
    const currentIndent = currentIndentMatch ? currentIndentMatch[1].length : 0

    const lineStart = line.from
    const contentStart = lineStart + currentIndent
    const newIndent = ' '.repeat(targetIndent)

    view.dispatch({
      changes: {
        from: lineStart,
        to: contentStart,
        insert: newIndent
      },
      selection: { anchor: lineStart + targetIndent }
    })

    return true
  } catch (error) {
    console.error('Error in smart tab:', error)
    view.dispatch({
      changes: { from: selection.head, insert: '    ' },
      selection: { anchor: selection.head + 4 }
    })
    return true
  }
}

// Component lifecycle
onMounted(() => {
  console.log('Scripts component mounted')

  const cleanup = dserv.registerComponent('Scripts')

  dserv.on('datapoint:ess/git/branches', (data) => {
    console.log('Received git branches:', data.data)
    if (data.data) {
      availableBranches.value = data.data.trim().split(/\s+/).filter(branch => branch.length > 0)
      console.log('Available branches:', availableBranches.value)
    }
  }, 'Scripts')

  dserv.on('datapoint:ess/git/branch', (data) => {
    console.log('Received current git branch:', data.data)
    currentBranch.value = data.data || ''
  }, 'Scripts')

  dserv.on('datapoint:ess/system_script', (data) => {
    console.log('Received system script data')
    scripts.value.system.content = data.data
    scripts.value.system.loaded = true
    scripts.value.system.unchangedHistoryDepth = 0
    if (activeScript.value === 'system') {
      updateEditorContent(data.data, true) // Clear history for new script from backend
    }
  }, 'Scripts')

  dserv.on('datapoint:ess/protocol_script', (data) => {
    console.log('Received protocol script data')
    scripts.value.protocol.content = data.data
    scripts.value.protocol.loaded = true
    scripts.value.protocol.unchangedHistoryDepth = 0
    if (activeScript.value === 'protocol') {
      updateEditorContent(data.data, true) // Clear history for new script from backend
    }
  }, 'Scripts')

  dserv.on('datapoint:ess/loaders_script', (data) => {
    console.log('Received loaders script data')
    scripts.value.loaders.content = data.data
    scripts.value.loaders.loaded = true
    scripts.value.loaders.unchangedHistoryDepth = 0
    if (activeScript.value === 'loaders') {
      updateEditorContent(data.data, true) // Clear history for new script from backend
    }
  }, 'Scripts')

  dserv.on('datapoint:ess/variants_script', (data) => {
    console.log('Received variants script data')
    scripts.value.variants.content = data.data
    scripts.value.variants.loaded = true
    scripts.value.variants.unchangedHistoryDepth = 0
    if (activeScript.value === 'variants') {
      updateEditorContent(data.data, true) // Clear history for new script from backend
    }
  }, 'Scripts')

  dserv.on('datapoint:ess/stim_script', (data) => {
    console.log('Received stim script data')
    scripts.value.stim.content = data.data
    scripts.value.stim.loaded = true
    scripts.value.stim.unchangedHistoryDepth = 0
    if (activeScript.value === 'stim') {
      updateEditorContent(data.data, true) // Clear history for new script from backend
    }
  }, 'Scripts')

  dserv.on('connection', ({ connected }) => {
    if (connected) {
      console.log('Connected - requesting script and git data')
      requestScriptData()
      requestGitData()
    }
  }, 'Scripts')

  dserv.on('initialized', () => {
    console.log('dserv initialized - requesting script and git data')
    requestScriptData()
    requestGitData()
  }, 'Scripts')

  nextTick(() => {
    createEditor()
  })

  if (dserv.state.connected) {
    console.log('Already connected - requesting script and git data')
    setTimeout(() => {
      requestScriptData()
      requestGitData()
    }, 100)
  }

  onUnmounted(() => {
    cleanup()
    if (editorView) {
      editorView.destroy()
    }
  })
})

// Helper functions
function requestScriptData() {
  try {
    console.log('Touching script variables...')
    dserv.essCommand('dservTouch ess/system_script')
    dserv.essCommand('dservTouch ess/protocol_script')
    dserv.essCommand('dservTouch ess/loaders_script')
    dserv.essCommand('dservTouch ess/variants_script')
    dserv.essCommand('dservTouch ess/stim_script')
  } catch (error) {
    console.error('Failed to request script data:', error)
  }
}

function requestGitData() {
  try {
    console.log('Touching git variables...')
    dserv.essCommand('dservTouch ess/git/branches')
    dserv.essCommand('dservTouch ess/git/branch')
  } catch (error) {
    console.error('Failed to request git data:', error)
  }
}
</script>

<style scoped>
/* CodeMirror will handle most styling, but we can add any custom styles here */
:deep(.cm-editor) {
  height: 100%;
}

:deep(.cm-focused) {
  outline: none;
}

/* Validation styles */
.validation-item {
  transition: background-color 0.2s;
}

.validation-item:hover {
  background-color: #f0f0f0;
}

.error-item:hover {
  background-color: #fff2f0;
}

.warning-item:hover {
  background-color: #fffbe6;
}

/* Enhanced status bar styling */
:deep(.validation-status) {
  display: flex;
  align-items: center;
  gap: 4px;
}

/* Validation panel scrollbar */
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

/* Existing compact styling */
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
