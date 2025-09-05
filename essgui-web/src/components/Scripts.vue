<template>
  <div style="height: 100%; display: flex; flex-direction: column; overflow: hidden;">

<!-- Header with script selector and controls -->
<div
  style="flex-shrink: 0; display: flex; align-items: center; justify-content: space-between; padding: 8px; border-bottom: 1px solid #d9d9d9; background: #fafafa;">
  <!-- Script Selector Dropdown and Lib File Controls -->
  <div style="display: flex; align-items: center; gap: 8px;">
    <!-- Main script selector - MADE SMALLER -->
    <a-select v-model:value="activeScript" size="small" style="width: 100px;"
      @change="(value) => switchScriptWithValidation(value)">
      <a-select-option v-for="script in scriptTabs" :key="script.name" :value="script.name">
        <div style="display: flex; align-items: center; justify-content: space-between; width: 100%;">
          <!-- SHORTENED LABELS -->
          <span>{{ script.name === 'system' ? 'System' :
                   script.name === 'protocol' ? 'Protocol' :
                   script.name === 'loaders' ? 'Loaders' :
                   script.name === 'variants' ? 'Variants' :
                   script.name === 'stim' ? 'Stim' :
                   script.name === 'lib' ? 'Libs' : script.label }}</span>
          <div style="display: flex; align-items: center; gap: 4px;">
            <!-- Modified indicator for regular scripts -->
            <span v-if="script.type === 'script' && scripts[script.name]?.modified"
              style="width: 6px; height: 6px; background: #ff4d4f; border-radius: 50%;"></span>
            <!-- Modified indicator for lib files -->
            <span v-if="script.type === 'library' && scripts.lib?.modified"
              style="width: 6px; height: 6px; background: #ff4d4f; border-radius: 50%;"></span>
            <!-- Validation error indicator -->
            <span v-if="activeScript === script.name && validationResult && !validationResult.isValid"
              style="width: 6px; height: 6px; background: #ff4d4f; border-radius: 50%;"
              title="Has validation errors"></span>
          </div>
        </div>
      </a-select-option>
    </a-select>

    <!-- Lib file selector (only show when lib tab active) - MADE SMALLER -->
    <div v-if="isLibMode" style="display: flex; align-items: center; gap: 6px;">
      <a-select
        v-model:value="selectedLibFile"
        size="small"
        style="width: 140px;"
        placeholder="Select .tm file"
        @change="loadLibFile"
        :loading="loadingLibFiles"
        allowClear
      >
        <a-select-option v-for="file in libFiles" :key="file" :value="file">
          <div style="display: flex; align-items: center; justify-content: space-between; width: 100%;">
            <!-- SHOW JUST FILENAME WITHOUT EXTENSION -->
            <span>{{ file.replace('.tm', '') }}</span>
            <span v-if="selectedLibFile === file && scripts.lib?.modified"
              style="width: 6px; height: 6px; background: #ff4d4f; border-radius: 50%;"
              title="Modified"></span>
          </div>
        </a-select-option>
      </a-select>

      <a-tooltip title="Refresh lib files">
        <a-button size="small" @click="loadLibFiles" :loading="loadingLibFiles"
          :icon="h(ReloadOutlined)" style="width: 28px; height: 22px;" />
      </a-tooltip>

      <a-tooltip title="Create new lib file">
        <a-button size="small" @click="showCreateLibFile = true"
          :icon="h(PlusOutlined)" style="width: 28px; height: 22px;" />
      </a-tooltip>
    </div>
  </div>

  <!-- Controls with icons and validation - MADE MORE COMPACT -->
  <div style="display: flex; align-items: center; gap: 8px;">
    <!-- Validation controls -->
    <a-tooltip title="Validate (Ctrl+Shift+V)">
      <a-button size="small" @click="validateCurrentScript" :loading="isValidating" :icon="h(CheckCircleOutlined)"
        :type="validationResult && !validationResult.isValid ? 'danger' : 'default'"
        style="width: 28px; height: 22px;" />
    </a-tooltip>

    <a-tooltip title="Auto-validate">
      <a-checkbox v-model:checked="autoValidate" size="small" style="transform: scale(0.85);">
      </a-checkbox>
    </a-tooltip>

    <!-- Undo/Redo controls -->
    <a-tooltip title="Undo (Ctrl+Z)">
      <a-button size="small" @click="undoEdit" :disabled="!canUndo" :icon="h(UndoOutlined)"
        style="width: 28px; height: 22px;" />
    </a-tooltip>

    <a-tooltip title="Redo (Ctrl+Y)">
      <a-button size="small" @click="redoEdit" :disabled="!canRedo" :icon="h(RedoOutlined)"
        style="width: 28px; height: 22px;" />
    </a-tooltip>

    <!-- Existing toolbar -->
    <a-tooltip title="Format (Ctrl+Shift+F)">
      <a-button size="small" @click="formatScript" :disabled="!currentScript?.content"
        :icon="h(FormatPainterOutlined)" style="width: 28px; height: 22px;" />
    </a-tooltip>

    <a-tooltip title="Save (Ctrl+S)">
      <a-button type="primary" size="small" @click="saveScriptWithErrorHandling" :loading="isSaving"
        :disabled="!currentScript?.modified" :icon="h(SaveOutlined)" style="width: 28px; height: 22px;" />
    </a-tooltip>

    <!-- Git operations - CONDENSED -->
    <a-tooltip title="Pull">
      <a-button size="small" @click="pullScripts" :loading="isGitBusy" :icon="h(DownloadOutlined)"
        style="width: 28px; height: 22px;" />
    </a-tooltip>

    <a-tooltip title="Push">
      <a-button size="small" @click="pushScripts" :loading="isGitBusy" :disabled="isGitBusy"
        :icon="h(UploadOutlined)" style="width: 28px; height: 22px;" />
    </a-tooltip>

    <!-- Git Branch Dropdown - MADE SMALLER -->
    <a-select v-model:value="currentBranch" size="small" style="width: 80px;" :loading="isGitBusy"
      @change="switchBranch" :disabled="isGitBusy" placeholder="Branch">
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
      <a-button size="small" style="width: 28px; height: 22px;">
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
        <span v-if="activeScript === 'lib' && selectedLibFile">
          {{ selectedLibFile.toUpperCase() }} (Lib)
        </span>
        <span v-else-if="activeScript === 'lib'">
          LIB FILES (No file selected)
        </span>
        <span v-else>
          {{ currentScript?.name?.toUpperCase() }} Script
        </span>

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

    <!-- NEW: Create Lib File Modal -->
    <a-modal v-model:open="showCreateLibFile" title="Create New Lib File" width="400px">
      <div style="margin: 16px 0;">
        <a-input
          v-model:value="newLibFileName"
          placeholder="Enter filename (e.g., mymodule.tm)"
          @keyup.enter="createLibFile"
          style="width: 100%;"
        />
        <div style="margin-top: 8px; font-size: 12px; color: #666;">
          File will be created in the lib/ directory. Extension .tm will be added if not provided.
        </div>
      </div>

      <template #footer>
        <a-button @click="showCreateLibFile = false">Cancel</a-button>
        <a-button type="primary" @click="createLibFile" :disabled="!newLibFileName.trim()">
          Create
        </a-button>
      </template>
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
  UndoOutlined,
  RedoOutlined,
  PlusOutlined  // NEW: Added for create lib file button
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
  undo,
  redo,
  history,
  historyField
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
const isSavingInProgress = ref(false)

// Validation state
const validationResult = ref(null)
const showValidationPanel = ref(false)
const isValidating = ref(false)
const autoValidate = ref(true)
const showBackupManager = ref(false)
const showDebugMode = ref(false)

// Undo/Redo state
const canUndo = ref(false)
const canRedo = ref(false)

// Git branch state
const currentBranch = ref('')
const availableBranches = ref([])

// NEW: Lib file state
const libFiles = ref([])
const selectedLibFile = ref('')
const loadingLibFiles = ref(false)
const showCreateLibFile = ref(false)
const newLibFileName = ref('')

// NEW: Computed property for lib mode
const isLibMode = computed(() => activeScript.value === 'lib')

// Scripts with undo tracking and original content preservation
const scripts = ref({
  system: {
    name: 'system',
    content: '',
    originalContent: '',
    modified: false,
    loaded: false,
    unchangedHistoryDepth: 0
  },
  protocol: {
    name: 'protocol',
    content: '',
    originalContent: '',
    modified: false,
    loaded: false,
    unchangedHistoryDepth: 0
  },
  loaders: {
    name: 'loaders',
    content: '',
    originalContent: '',
    modified: false,
    loaded: false,
    unchangedHistoryDepth: 0
  },
  variants: {
    name: 'variants',
    content: '',
    originalContent: '',
    modified: false,
    loaded: false,
    unchangedHistoryDepth: 0
  },
  stim: {
    name: 'stim',
    content: '',
    originalContent: '',
    modified: false,
    loaded: false,
    unchangedHistoryDepth: 0
  },
  // NEW: Lib file entry (will be populated when lib file is selected)
  lib: null
})

// UPDATED: Script tabs to include lib files
const scriptTabs = [
  { name: 'system', label: 'System', type: 'script' },
  { name: 'protocol', label: 'Protocol', type: 'script' },
  { name: 'loaders', label: 'Loaders', type: 'script' },
  { name: 'variants', label: 'Variants', type: 'script' },
  { name: 'stim', label: 'Stim', type: 'script' },
  { name: 'lib', label: 'Lib Files', type: 'library' }  // NEW
]

// UPDATED: Computed properties to handle lib files
const currentScript = computed(() => {
  if (activeScript.value === 'lib') {
    return scripts.value.lib
  }
  return scripts.value[activeScript.value]
})

// Track if we've made any changes since last git operation
const hasChangesSinceLastGit = ref(false)

// Watch for any script modifications to track git state
watch(scripts, (newScripts) => {
  const hasModified = Object.values(newScripts).some(script => script?.modified)
  if (hasModified) {
    hasChangesSinceLastGit.value = true
  }
}, { deep: true })

// Auto-validation watcher (debounced)
let validationTimeout = null
watch(() => currentScript.value?.content, (newContent) => {
  // Don't auto-validate during save operations
  if (!autoValidate.value || !newContent || isSavingInProgress.value) return

  if (validationTimeout) {
    clearTimeout(validationTimeout)
  }

  validationTimeout = setTimeout(() => {
    validateCurrentScript()
  }, 1000)
})

// NEW: Lib file methods
async function loadLibFiles() {
  loadingLibFiles.value = true
  try {
    const files = await dserv.essCommand('ess::get_lib_files')
    libFiles.value = Array.isArray(files) ? files : (files ? files.split(' ').filter(f => f.length > 0) : [])
    console.log('Loaded lib files:', libFiles.value)
  } catch (error) {
    console.error('Failed to load lib files:', error)
    message.error('Failed to load lib files')
    libFiles.value = []
  } finally {
    loadingLibFiles.value = false
  }
}

async function loadLibFile(filename) {
  if (!filename) {
    // Clear editor when no file selected
    selectedLibFile.value = ''
    if (scripts.value.lib) {
      scripts.value.lib = null
    }
    if (editorView) {
      updateEditorContent('', true)
    }
    return
  }

  try {
    console.log('Loading lib file:', filename)
    const content = await dserv.essCommand(`ess::get_lib_file_content {${filename}}`)

    // Create/update lib script entry
    scripts.value.lib = {
      name: 'lib',
      filename: filename,
      content: content,
      originalContent: content,
      modified: false,
      loaded: true,
      unchangedHistoryDepth: 0
    }

    selectedLibFile.value = filename
    updateEditorContent(content, true)

    console.log('Loaded lib file successfully:', filename)

  } catch (error) {
    console.error('Failed to load lib file:', error)
    message.error(`Failed to load lib file: ${error.message}`)
  }
}

async function saveLibFile() {
  if (!selectedLibFile.value || !scripts.value.lib?.modified) {
    console.log('No lib file to save or not modified')
    return
  }

  try {
    // Validate first (same as regular scripts)
    const linter = new TclLinter()
    const frontendResult = linter.lint(scripts.value.lib.content)

    if (!frontendResult.isValid) {
      const criticalErrors = frontendResult.errors.filter(e =>
        e.message.includes('syntax') ||
        e.message.includes('brace') ||
        e.message.includes('quote')
      )

      if (criticalErrors.length > 0) {
        const shouldContinue = await new Promise((resolve) => {
          Modal.confirm({
            title: 'Syntax Errors in Lib File',
            content: `This lib file has ${criticalErrors.length} syntax error(s). Save anyway?`,
            okText: 'Save Anyway',
            okType: 'danger',
            cancelText: 'Fix Errors First',
            onOk: () => resolve(true),
            onCancel: () => resolve(false)
          })
        })

        if (!shouldContinue) return
      }
    }

    isSaving.value = true

    console.log('Saving lib file:', selectedLibFile.value)
    await dserv.essCommand(`ess::save_lib_file {${selectedLibFile.value}} {${scripts.value.lib.content}}`)

    // Mark as saved
    scripts.value.lib.modified = false
    scripts.value.lib.originalContent = scripts.value.lib.content

    // Update undo/redo state
    if (editorView) {
      const histState = editorView.state.field(historyField, false)
      if (histState) {
        scripts.value.lib.unchangedHistoryDepth = histState.done.length
      }
    }

    updateUndoRedoState()
    message.success(`Lib file ${selectedLibFile.value} saved`)

  } catch (error) {
    console.error('Failed to save lib file:', error)
    message.error(`Failed to save lib file: ${error.message}`)
  } finally {
    isSaving.value = false
  }
}

async function createLibFile() {
  let filename = newLibFileName.value.trim()

  if (!filename) {
    message.error('Please enter a filename')
    return
  }

  if (!filename.endsWith('.tm')) {
    filename += '.tm'
  }

  // Basic validation
  if (!/^[a-zA-Z0-9._-]+\.tm$/.test(filename)) {
    message.error('Invalid filename. Use only letters, numbers, dots, dashes, and underscores.')
    return
  }

  try {
    // Create empty file with basic template
    const template = `# ${filename}\n# Tcl Module\n\npackage require Tcl 8.5\npackage provide ${filename.replace('.tm', '')} 1.0\n\n# Add your procedures here\n`

    await dserv.essCommand(`ess::save_lib_file {${filename}} {${template}}`)

    // Refresh list and select new file
    await loadLibFiles()
    selectedLibFile.value = filename
    await loadLibFile(filename)

    showCreateLibFile.value = false
    newLibFileName.value = ''

    message.success(`Created lib file: ${filename}`)

  } catch (error) {
    console.error('Failed to create lib file:', error)
    message.error(`Failed to create lib file: ${error.message}`)
  }
}

function switchToLibMode() {
  activeScript.value = 'lib'

  // Load lib files list if not already loaded
  if (libFiles.value.length === 0) {
    loadLibFiles()
  }

  // If no lib file selected, clear editor
  if (!selectedLibFile.value) {
    if (editorView) {
      updateEditorContent('', true)
    }
  }
  // If lib file already selected, load it
  else if (scripts.value.lib) {
    updateEditorContent(scripts.value.lib.content, false)
  }
}

// UPDATED: Enhanced script switching with lib file support
function switchScriptWithValidation(scriptName) {
  validationResult.value = null
  showValidationPanel.value = false

  // Handle lib files specially
  if (scriptName === 'lib') {
    switchToLibMode()
  } else {
    // Clear lib selection when switching away from lib
    if (activeScript.value === 'lib') {
      selectedLibFile.value = ''
      scripts.value.lib = null
    }
    switchScript(scriptName)
  }

  if (autoValidate.value && scriptName !== 'lib') {
    setTimeout(() => {
      validateCurrentScript()
    }, 500)
  }
}

// Save script with lib file support
async function saveScriptWithErrorHandling() {
  // Handle lib files
  if (activeScript.value === 'lib') {
    return await saveLibFile()
  }

  // Existing script save logic for regular scripts
  if (!currentScript.value?.modified) return

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

// Debugging helper function
function debugHistoryState(context = '') {
  if (!editorView) {
    console.log(`[${context}] No editor view`)
    return
  }

  const histState = editorView.state.field(historyField, false)
  if (!histState) {
    console.log(`[${context}] No history state`)
    return
  }

  const script = currentScript.value
  console.log(`[${context}] History debug:`, {
    scriptName: script?.name,
    doneLength: histState.done.length,
    undoneLength: histState.undone.length,
    canUndo: canUndo.value,
    canRedo: canRedo.value,
    modified: script?.modified,
    unchangedDepth: script?.unchangedHistoryDepth,
    doneEntries: histState.done.map((entry, i) => ({
      index: i,
      hasChanges: entry.changes ? entry.changes.length : 0
    }))
  })
}

// Improved undo/redo state management with content-aware undo state
function updateUndoRedoState() {
  if (!editorView) return

  const histState = editorView.state.field(historyField, false)
  if (histState) {
    // Get raw history state
    const doneLength = histState.done.length
    const undoneLength = histState.undone.length

    // Base undo/redo availability on CodeMirror's history
    const cmCanUndo = doneLength > 0
    const cmCanRedo = undoneLength > 0

    const currentDepth = doneLength
    const script = currentScript.value

    if (script) {
      const wasModified = script.modified
      const currentContent = editorView.state.doc.toString()

      // Check if we're back to the original loaded content
      const isBackToOriginalContent = currentContent === script.originalContent

      if (isBackToOriginalContent) {
        // We're back to the unchanged state by content match
        script.modified = false

        // If we're back to original content, don't allow further undo
        // (we shouldn't undo past the loaded state)
        canUndo.value = false
        console.log(`Content matches original - marking ${script.name} as unmodified and blocking undo`)
      } else if (currentDepth === script.unchangedHistoryDepth) {
        // We're back to the unchanged state by history depth
        script.modified = false
        canUndo.value = cmCanUndo // Use CodeMirror's undo state
        console.log(`History depth matches unchanged depth - marking ${script.name} as unmodified`)
      } else {
        // We have changes from the unchanged state
        script.modified = true
        canUndo.value = cmCanUndo // Use CodeMirror's undo state
      }

      // Redo is always based on CodeMirror's state
      canRedo.value = cmCanRedo

      // Enhanced debug logging
      console.log(`updateUndoRedoState: ${script.name}:`, {
        wasModified,
        nowModified: script.modified,
        currentDepth,
        unchangedDepth: script.unchangedHistoryDepth,
        cmCanUndo,
        cmCanRedo,
        finalCanUndo: canUndo.value,
        finalCanRedo: canRedo.value,
        doneLength,
        undoneLength,
        isBackToOriginalContent,
        currentContentLength: currentContent.length,
        originalContentLength: script.originalContent ? script.originalContent.length : 0
      })
    } else {
      // No script - use CodeMirror's raw state
      canUndo.value = cmCanUndo
      canRedo.value = cmCanRedo
    }
  } else {
    // No history state available
    canUndo.value = false
    canRedo.value = false
    console.warn('No history state available in updateUndoRedoState')
  }
}

// Undo/Redo functions with content-aware restrictions
function undoEdit() {
  if (editorView && canUndo.value) {
    console.log('undoEdit called')

    // Check if we're already at original content before undoing
    const currentContent = editorView.state.doc.toString()
    const script = currentScript.value

    if (script && currentContent === script.originalContent) {
      console.log('Already at original content - undo blocked')
      return false
    }

    const result = undo(editorView)

    // Ensure state update happens after undo
    nextTick(() => {
      updateUndoRedoState()
      debugHistoryState('after undoEdit')
    })

    return result
  }
  return false
}

function redoEdit() {
  if (editorView && canRedo.value) {
    console.log('redoEdit called')
    const result = redo(editorView)

    // Ensure state update happens after redo
    nextTick(() => {
      updateUndoRedoState()
      debugHistoryState('after redoEdit')
    })

    return result
  }
  return false
}

// Backend validation in validateCurrentScript
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

// Update the validateAllScripts to also use minimal validation
async function validateAllScripts() {
  const results = {}

  for (const scriptTab of scriptTabs) {
    if (scriptTab.type === 'script' && scripts.value[scriptTab.name]?.content) {
      const linter = new TclLinter()
      results[scriptTab.name] = linter.lint(scripts.value[scriptTab.name].content)
    }
  }

  // Also validate current lib file if one is selected
  if (scripts.value.lib?.content) {
    const linter = new TclLinter()
    results[`lib:${selectedLibFile.value}`] = linter.lint(scripts.value.lib.content)
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

// Script switching with proper history management
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
      debugHistoryState('after switchScript')
    })
  }
}

// Backup methods
async function restoreBackup(backupFile) {
  try {
    if (activeScript.value === 'lib' && selectedLibFile.value) {
      await dserv.essCommand(`ess::restore_lib_backup {${selectedLibFile.value}} {${backupFile}}`)
      await loadLibFile(selectedLibFile.value) // Reload the lib file
    } else {
      await dserv.essCommand(`ess::restore_script_backup ${activeScript.value} {${backupFile}}`)
      requestScriptData()
    }
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
      loadLibFiles() // Reload lib files after branch switch
    }, 500)
  } catch (error) {
    console.error(`Failed to switch to branch ${branchName}:`, error)
    requestGitData()
  } finally {
    isGitBusy.value = false
  }
}

function createEditor() {
   createEditorWithContent(currentScript.value?.content || '')
}

// Create editor with proper undo/redo command integration
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

  // Undo/redo commands with content-aware restrictions
  const undoCommand = {
    key: 'Ctrl-z',
    run: (view) => {
      console.log('Undo command triggered')

      // Check if we're already at original content
      const currentContent = view.state.doc.toString()
      const script = currentScript.value

      if (script && currentContent === script.originalContent) {
        console.log('Already at original content - undo blocked')
        return true // Return true to indicate we handled it (even though we blocked it)
      }

      const result = undo(view)

      // Use nextTick to ensure state is updated after undo completes
      nextTick(() => {
        updateUndoRedoState()
        debugHistoryState('after undo command')
      })

      return result
    }
  }

  const redoCommand = {
    key: 'Ctrl-y',
    run: (view) => {
      console.log('Redo command triggered')
      const result = redo(view)

      // Use nextTick to ensure state is updated after redo completes
      nextTick(() => {
        updateUndoRedoState()
        debugHistoryState('after redo command')
      })

      return result
    }
  }

  const redoAltCommand = {
    key: 'Ctrl-Shift-z',
    run: (view) => {
      console.log('Redo alt command triggered')
      const result = redo(view)

      // Use nextTick to ensure state is updated after redo completes
      nextTick(() => {
        updateUndoRedoState()
        debugHistoryState('after redo alt command')
      })

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

      // More careful update listener
      EditorView.updateListener.of((update) => {
        // Handle content changes ONLY
        if (update.docChanged && !isUpdatingContent.value) {
const newContent = update.state.doc.toString()
         if (activeScript.value === 'lib' && scripts.value.lib) {
           scripts.value.lib.content = newContent
           scripts.value.lib.modified = true
           console.log(`Content changed in lib file ${selectedLibFile.value}, marking as modified`)
         } else if (scripts.value[activeScript.value]) {
           scripts.value[activeScript.value].content = newContent
           scripts.value[activeScript.value].modified = true
           console.log(`Content changed in ${activeScript.value}, marking as modified`)
         }

         // Use setTimeout instead of immediate call to ensure history is updated
         setTimeout(() => {
           updateUndoRedoState()
           debugHistoryState('after content change')
         }, 0)
       }

       // Only update undo/redo state for actual document changes, not selection changes
       // Selection changes (clicks, cursor moves) should NOT trigger modification
       if (update.docChanged) {
         setTimeout(() => {
           updateUndoRedoState()
         }, 0)
       }

       // Auto-indent logic
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

 // Proper initialization timing
 nextTick(() => {
   // Set initial state for a freshly created editor
   const script = currentScript.value
   if (script) {
     script.unchangedHistoryDepth = 0
     console.log(`createEditorWithContent: Initialized ${activeScript.value} with unchangedDepth=0`)
   }
   updateUndoRedoState()
   debugHistoryState('after createEditorWithContent')
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
     const script = currentScript.value
     if (script && editorView) {
       const histState = editorView.state.field(historyField, false)
       if (histState) {
         const currentDepth = histState.done.length

         if (clearHistory) {
           // Fresh content from backend - this is the clean baseline
           script.unchangedHistoryDepth = 0
           script.modified = false
           console.log(`updateEditorContent: Cleared history for ${script.name || script.filename}, unchangedDepth=0, modified=false`)
         } else {
           // User edit - maintain existing unchanged depth
           script.unchangedHistoryDepth = currentDepth
           script.modified = false
           console.log(`updateEditorContent: Set ${script.name || script.filename} unchangedDepth=${currentDepth}, modified=false`)
         }
       }
     }

     updateUndoRedoState()
     debugHistoryState('after updateEditorContent')
   })
 }
}

function formatScript() {
 if (!currentScript.value?.content) return

 try {
   const formattedContent = TclFormatter.formatTclCode(currentScript.value.content, 4)

   if (activeScript.value === 'lib' && scripts.value.lib) {
     scripts.value.lib.content = formattedContent
     scripts.value.lib.modified = true
   } else {
     scripts.value[activeScript.value].content = formattedContent
     scripts.value[activeScript.value].modified = true
   }

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

   if (activeScript.value === 'lib' && scripts.value.lib) {
     scripts.value.lib.content = formattedContent
     scripts.value.lib.modified = true
   } else {
     scripts.value[activeScript.value].content = formattedContent
     scripts.value[activeScript.value].modified = true
   }

   updateEditorContent(formattedContent)
 }
}

// saveScript with unchanged history depth tracking and original content update
async function saveScript() {
  if (!currentScript.value?.modified) return

  isSaving.value = true
  isSavingInProgress.value = true // Prevent auto-validation

  try {
    const scriptContent = currentScript.value.content
    const cmd = `ess::save_script ${activeScript.value} {${scriptContent}}`

    console.log(`Saving ${activeScript.value} script...`)
    await dserv.essCommand(cmd)

    // Mark as unchanged and record history depth
    scripts.value[activeScript.value].modified = false

    // Update the original content baseline to the current content
    scripts.value[activeScript.value].originalContent = scriptContent

    // Record the current history depth as the "unchanged" point
    if (editorView) {
      const histState = editorView.state.field(historyField, false)
      if (histState) {
        scripts.value[activeScript.value].unchangedHistoryDepth = histState.done.length
        console.log(`saveScript: Set ${activeScript.value} unchangedDepth=${histState.done.length}, updated originalContent`)
      }
    }

    console.log(`${activeScript.value} script saved successfully`)

    // Clear validation without triggering watchers
    validationResult.value = null
    showValidationPanel.value = false

    // Update undo/redo state after save
    updateUndoRedoState()
    debugHistoryState('after saveScript')

  } catch (error) {
    console.error(`Failed to save ${activeScript.value} script:`, error)
  } finally {
    isSaving.value = false
    isSavingInProgress.value = false // Re-enable auto-validation
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
     loadLibFiles() // Reload lib files after pull
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
   scripts.value.system.originalContent = data.data
   scripts.value.system.loaded = true
   scripts.value.system.unchangedHistoryDepth = 0
   scripts.value.system.modified = false
   if (activeScript.value === 'system') {
     updateEditorContent(data.data, true)
   }
 }, 'Scripts')

 dserv.on('datapoint:ess/protocol_script', (data) => {
   console.log('Received protocol script data')
   scripts.value.protocol.content = data.data
   scripts.value.protocol.originalContent = data.data
   scripts.value.protocol.loaded = true
   scripts.value.protocol.unchangedHistoryDepth = 0
   scripts.value.protocol.modified = false
   if (activeScript.value === 'protocol') {
     updateEditorContent(data.data, true)
   }
 }, 'Scripts')

 dserv.on('datapoint:ess/loaders_script', (data) => {
   console.log('Received loaders script data')
   scripts.value.loaders.content = data.data
   scripts.value.loaders.originalContent = data.data
   scripts.value.loaders.loaded = true
   scripts.value.loaders.unchangedHistoryDepth = 0
   scripts.value.loaders.modified = false
   if (activeScript.value === 'loaders') {
     updateEditorContent(data.data, true)
   }
 }, 'Scripts')

 dserv.on('datapoint:ess/variants_script', (data) => {
   console.log('Received variants script data')
   scripts.value.variants.content = data.data
   scripts.value.variants.originalContent = data.data
   scripts.value.variants.loaded = true
   scripts.value.variants.unchangedHistoryDepth = 0
   scripts.value.variants.modified = false
   if (activeScript.value === 'variants') {
     updateEditorContent(data.data, true)
   }
 }, 'Scripts')

 dserv.on('datapoint:ess/stim_script', (data) => {
   console.log('Received stim script data')
   scripts.value.stim.content = data.data
   scripts.value.stim.originalContent = data.data
   scripts.value.stim.loaded = true
   scripts.value.stim.unchangedHistoryDepth = 0
   scripts.value.stim.modified = false
   if (activeScript.value === 'stim') {
     updateEditorContent(data.data, true)
   }
 }, 'Scripts')

 dserv.on('connection', ({ connected }) => {
   if (connected) {
     console.log('Connected - requesting script and git data')
     requestScriptData()
     requestGitData()
     loadLibFiles() // Load lib files on connection
   }
 }, 'Scripts')

 dserv.on('initialized', () => {
   console.log('dserv initialized - requesting script and git data')
   requestScriptData()
   requestGitData()
   loadLibFiles() // Load lib files on initialization
 }, 'Scripts')

 nextTick(() => {
   createEditor()
 })

 if (dserv.state.connected) {
   console.log('Already connected - requesting script and git data')
   setTimeout(() => {
     requestScriptData()
     requestGitData()
     loadLibFiles() // Load lib files if already connected
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

// DEBUGGING: Expose helper for console testing
if (typeof window !== 'undefined') {
 window.debugHistory = () => debugHistoryState('manual check')
 window.debugScriptState = () => {
   const script = currentScript.value
   console.log('Current script state:', {
     name: script?.name || script?.filename,
     modified: script?.modified,
     unchangedDepth: script?.unchangedHistoryDepth,
     canUndo: canUndo.value,
     canRedo: canRedo.value,
     hasEditor: !!editorView,
     isLibMode: isLibMode.value,
     selectedLibFile: selectedLibFile.value
   })
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
