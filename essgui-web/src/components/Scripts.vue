<template>
  <div style="height: 100%; display: flex; flex-direction: column; overflow: hidden;">
    
    <!-- Header with tabs and controls -->
    <div style="flex-shrink: 0; display: flex; align-items: center; justify-content: space-between; padding: 8px; border-bottom: 1px solid #d9d9d9; background: #fafafa;">
      <!-- Script tabs -->
      <div style="display: flex; gap: 4px;">
        <a-button
          v-for="script in scriptTabs"
          :key="script.name"
          :type="activeScript === script.name ? 'primary' : 'default'"
          size="small"
          @click="switchScript(script.name)"
          style="position: relative;"
        >
          {{ script.label }}
          <span 
            v-if="scripts[script.name]?.modified" 
            style="position: absolute; top: -2px; right: -2px; width: 8px; height: 8px; background: #ff4d4f; border-radius: 50%; font-size: 10px;"
          ></span>
        </a-button>
      </div>

      <!-- Controls -->
      <div style="display: flex; align-items: center; gap: 8px;">
        <a-button 
          size="small" 
          @click="formatScript"
          :disabled="!currentScript?.content"
        >
          Format
        </a-button>

        <a-button 
          size="small" 
          @click="pullScripts"
          :loading="isGitBusy"
          title="Pull latest scripts from git"
        >
          Pull
        </a-button>

        <a-button 
          size="small" 
          @click="pushScripts"
          :loading="isGitBusy"
          :disabled="!canPushToGit"
          title="Commit and push scripts to git"
        >
          Push
        </a-button>

        <a-button 
          type="primary" 
          size="small" 
          @click="saveScript"
          :loading="isSaving"
          :disabled="!currentScript?.modified"
        >
          Save {{ scriptTabs.find(s => s.name === activeScript)?.label }}
        </a-button>

        <a-button 
          size="small" 
          @click="saveAllScripts"
          :loading="isSavingAll"
          :disabled="!hasModifiedScripts"
        >
          Save All
        </a-button>
      </div>
    </div>

    <!-- Editor container -->
    <div 
      ref="editorContainer" 
      style="flex: 1; overflow: hidden; background: #1e1e1e;"
    ></div>

    <!-- Status bar -->
    <div style="flex-shrink: 0; display: flex; justify-content: space-between; align-items: center; padding: 4px 8px; background: #f0f0f0; border-top: 1px solid #d9d9d9; font-size: 11px; color: #666;">
      <div style="display: flex; gap: 12px;">
        <span>{{ currentScript?.name?.toUpperCase() }} Script</span>
        <span v-if="currentScript?.modified" style="color: #ff9500;">Modified</span>
        <span>Tcl</span>
      </div>
      <div style="display: flex; gap: 12px;">
        <span>{{ keyBindings === 'emacs' ? 'Emacs Bindings' : 'Default Bindings' }}</span>
        <span>Ctrl+S: Save | {{ keyBindings === 'emacs' ? 'Ctrl+/: Search' : 'Ctrl+F: Search' }} | Enter: Auto-indent</span>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, nextTick, watch } from 'vue'
import { dserv } from '../services/dserv.js'

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
  insertTab
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
const isSavingAll = ref(false)
const isGitBusy = ref(false)
const isUpdatingContent = ref(false) // Flag to prevent marking as modified during programmatic updates

const scripts = ref({
  system: { name: 'system', content: '', modified: false, loaded: false },
  protocol: { name: 'protocol', content: '', modified: false, loaded: false },
  loaders: { name: 'loaders', content: '', modified: false, loaded: false },
  variants: { name: 'variants', content: '', modified: false, loaded: false },
  stim: { name: 'stim', content: '', modified: false, loaded: false }
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
const hasModifiedScripts = computed(() => 
  Object.values(scripts.value).some(script => script.modified)
)

// Track if we've made any changes since last git operation
const hasChangesSinceLastGit = ref(false)

// Watch for any script modifications to track git state
watch(scripts, (newScripts) => {
  // If any script becomes modified, mark as having changes since git
  const hasModified = Object.values(newScripts).some(script => script.modified)
  if (hasModified) {
    hasChangesSinceLastGit.value = true
  }
}, { deep: true })

// Git push should be available if we have changes since last git operation
// or if there are saved changes that haven't been pushed
const canPushToGit = computed(() => hasChangesSinceLastGit.value)

// CodeMirror editor creation
function createEditor() {
  if (!editorContainer.value) return

  const saveCommand = {
    key: 'Ctrl-s',
    run: () => {
      saveScript()
      return true
    }
  }

  const saveAllCommand = {
    key: 'Ctrl-Shift-s', 
    run: () => {
      saveAllScripts()
      return true
    }
  }

  // Custom search command that works well with both emacs and default bindings
  const searchCommand = {
    key: 'Ctrl-/',  // Emacs-friendly search key
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

  // Emacs-style key bindings with auto-indent on Enter
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
    searchCommand // Add our custom search binding for emacs mode
  ]

  // Default bindings still use Ctrl+F for search
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

  // Choose appropriate keybindings
  const currentKeybindings = keyBindings.value === 'emacs' ? emacsBindings : defaultBindings

  // For non-emacs mode, use default search keybindings (Ctrl+F)
  const searchKeybindings = keyBindings.value === 'emacs' ? [] : searchKeymap

  const allKeybindings = [
    ...currentKeybindings,
    ...searchKeybindings,
    saveCommand,
    saveAllCommand
  ]

  const state = EditorState.create({
    doc: currentScript.value.content,
    extensions: [
      basicSetup,
      StreamLanguage.define(tcl),
      oneDark, // Dark theme
      lineNumbers(),
      search(), // Enable search functionality
      highlightSelectionMatches(), // Highlight all matches when text is selected
      // Simple tab configuration
      EditorState.tabSize.of(4),
      EditorView.lineWrapping,
      keymap.of(allKeybindings),
EditorView.updateListener.of((update) => {
  // Handle content changes for marking as modified
  if (update.docChanged && !isUpdatingContent.value) {
    const newContent = update.state.doc.toString()
    scripts.value[activeScript.value].content = newContent
    scripts.value[activeScript.value].modified = true
  }
  
  // Handle auto-indent for newlines
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
        // Style the search panel
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
}

// Update editor content without triggering change event
function updateEditorContent(content) {
  if (!editorView) return
  
  const currentContent = editorView.state.doc.toString()
  if (currentContent !== content) {
    isUpdatingContent.value = true
    editorView.dispatch({
      changes: {
        from: 0,
        to: editorView.state.doc.length,
        insert: content
      }
    })
    // Use nextTick to ensure the flag is reset after the update is processed
    nextTick(() => {
      isUpdatingContent.value = false
    })
  }
}

// Switch between scripts
function switchScript(scriptName) {
  if (activeScript.value === scriptName) return
  
  activeScript.value = scriptName
  if (editorView) {
    updateEditorContent(currentScript.value.content)
  }
}

// Set key bindings and recreate editor
function setKeyBindings(bindings) {
  keyBindings.value = bindings
  if (editorView) {
    editorView.destroy()
    nextTick(() => {
      createEditor()
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
    console.error('Stack trace:', error.stack)
    // Fallback to your existing simple formatter
    console.log('Using fallback formatter...')
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

// Save current script
async function saveScript() {
  if (!currentScript.value.modified) return
  
  isSaving.value = true
  try {
    const scriptContent = currentScript.value.content
    const cmd = `ess::save_script ${activeScript.value} {${scriptContent}}`
    
    console.log(`Saving ${activeScript.value} script...`)
    await dserv.essCommand(cmd)
    
    // Mark as saved (matching essgui behavior)
    scripts.value[activeScript.value].modified = false
    console.log(`${activeScript.value} script saved successfully`)
    
    // Could emit an event here for global menu integration if needed
    // dserv.emit('scriptSaved', { scriptType: activeScript.value })
    
  } catch (error) {
    console.error(`Failed to save ${activeScript.value} script:`, error)
    // Could show an error notification here
  } finally {
    isSaving.value = false
  }
}

// Git operations (matching essgui.cxx git functionality)
async function pullScripts() {
  isGitBusy.value = true
  try {
    console.log('Pulling scripts from git...')
    await dserv.gitCommand('git::pull')
    console.log('Git pull completed successfully')
    
    // Reset git change tracking after successful pull
    hasChangesSinceLastGit.value = false
    
    // After pull, request updated script data
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
    
    // Reset git change tracking after successful push
    hasChangesSinceLastGit.value = false
  } catch (error) {
    console.error('Failed to push scripts:', error)
  } finally {
    isGitBusy.value = false
  }
}

async function saveAllScripts() {
  if (!hasModifiedScripts.value) return
  
  isSavingAll.value = true
  try {
    const modifiedScripts = Object.entries(scripts.value)
      .filter(([_, script]) => script.modified)
    
    console.log(`Saving ${modifiedScripts.length} modified scripts...`)
    
    for (const [scriptName, script] of modifiedScripts) {
      const cmd = `ess::save_script ${scriptName} {${script.content}}`
      await dserv.essCommand(cmd)
      script.modified = false
      console.log(`${scriptName} script saved`)
    }
    
    console.log('All scripts saved successfully')
  } catch (error) {
    console.error('Failed to save scripts:', error)
  } finally {
    isSavingAll.value = false
  }
}

// Component lifecycle
onMounted(() => {
  console.log('Scripts component mounted')
  
  // Register component with dserv
  const cleanup = dserv.registerComponent('Scripts', {
    subscriptions: []
  })

  // Expose save function globally for menu integration (like essgui)
  window.saveCurrentScript = saveScript
  
  // Listen for global save events (if triggered from menu)
  dserv.on('saveScript', () => {
    saveScript()
  })
  
  // Listen for script data from dserv
  dserv.on('datapoint:ess/system_script', (data) => {
    console.log('Received system script data')
    scripts.value.system.content = data.data
    scripts.value.system.modified = false
    scripts.value.system.loaded = true
    if (activeScript.value === 'system') {
      updateEditorContent(data.data)
    }
  })
  
  dserv.on('datapoint:ess/protocol_script', (data) => {
    console.log('Received protocol script data')
    scripts.value.protocol.content = data.data
    scripts.value.protocol.modified = false
    scripts.value.protocol.loaded = true
    if (activeScript.value === 'protocol') {
      updateEditorContent(data.data)
    }
  })
  
  dserv.on('datapoint:ess/loaders_script', (data) => {
    console.log('Received loaders script data')
    scripts.value.loaders.content = data.data
    scripts.value.loaders.modified = false
    scripts.value.loaders.loaded = true
    if (activeScript.value === 'loaders') {
      updateEditorContent(data.data)
    }
  })
  
  dserv.on('datapoint:ess/variants_script', (data) => {
    console.log('Received variants script data')
    scripts.value.variants.content = data.data
    scripts.value.variants.modified = false
    scripts.value.variants.loaded = true
    if (activeScript.value === 'variants') {
      updateEditorContent(data.data)
    }
  })
  
  dserv.on('datapoint:ess/stim_script', (data) => {
    console.log('Received stim script data')
    scripts.value.stim.content = data.data
    scripts.value.stim.modified = false
    scripts.value.stim.loaded = true
    if (activeScript.value === 'stim') {
      updateEditorContent(data.data)
    }
  })

  // Listen for connection events to request script data
  dserv.on('connection', ({ connected }) => {
    if (connected) {
      console.log('Connected - requesting script data')
      requestScriptData()
    }
  })

  // Listen for initialization completion
  dserv.on('initialized', () => {
    console.log('dserv initialized - requesting script data')
    requestScriptData()
  })
  
  // Create editor after mount
  nextTick(() => {
    createEditor()
  })

  // If already connected, request script data immediately
  if (dserv.state.connected) {
    console.log('Already connected - requesting script data')
    setTimeout(() => {
      requestScriptData()
    }, 100)
  }
  
  // Cleanup on unmount
  onUnmounted(() => {
    cleanup()
    if (editorView) {
      editorView.destroy()
    }
    // Clean up global reference
    delete window.saveCurrentScript
  })
})

// Helper function to request all script data
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

function autoIndentNewline(view) {
  console.log('=== autoIndentNewline called ===')
  
  const state = view.state
  const selection = state.selection.main
  const line = state.doc.lineAt(selection.head)
  
  console.log('Current line:', line.text)
  console.log('Cursor position:', selection.head)
  
  // Get all lines up to current position
  const allText = state.doc.toString()
  const lines = TclFormatter.splitLines(allText)
  const currentLineNum = line.number - 1
  
  console.log('Current line number:', currentLineNum)
  
  try {
    // Calculate proper indent for next line (after the newline we're about to insert)
    const nextLineIndent = TclFormatter.calculateLineIndent(lines, currentLineNum + 1, 4)
    
    console.log('Calculated next line indent:', nextLineIndent)
    
    // Create the new line with proper indentation
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
    // Fallback to simple auto-indent
    console.log('Using fallback auto-indent')
    // ... your existing fallback code
    return true
  }
}

function handleSmartTab(view) {
  const state = view.state
  const selection = state.selection.main
  const line = state.doc.lineAt(selection.head)
  
  // Get all lines
  const allText = state.doc.toString()
  const lines = TclFormatter.splitLines(allText)
  const currentLineNum = line.number - 1
  
  try {
    // Always calculate and apply proper indent for current line
    const targetIndent = TclFormatter.calculateLineIndent(lines, currentLineNum, 4)
    
    // Get current line's existing indent
    const lineText = line.text
    const currentIndentMatch = lineText.match(/^(\s*)/)
    const currentIndent = currentIndentMatch ? currentIndentMatch[1].length : 0
    
    // Always adjust to proper indentation (don't insert extra spaces)
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
    // Fallback to simple tab
    view.dispatch({
      changes: { from: selection.head, insert: '    ' },
      selection: { anchor: selection.head + 4 }
    })
    return true
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
</style>