<template>
  <div class="terminal">
    <!-- Terminal Header -->
    <div class="terminal-header">
      <span>Terminal</span>
      <div class="terminal-controls">
        <button 
          @click="clearTerminal"
          class="terminal-btn"
          title="Clear Terminal (Ctrl+L)"
        >
          🗑️
        </button>
        <button 
          @click="focusInput"
          class="terminal-btn"
          title="Focus Input"
        >
          📝
        </button>
        <button 
          @click="showHelp = !showHelp"
          class="terminal-btn"
          title="Toggle Help"
        >
          ❓
        </button>
      </div>
    </div>
    
    <!-- Terminal Output -->
    <div 
      ref="terminalOutput"
      class="terminal-output"
      @click="focusInput"
    >
      <div 
        v-for="entry in history" 
        :key="entry.id" 
        :class="['terminal-line', entry.type]"
      >
        <span class="timestamp">{{ formatTimestamp(entry.timestamp) }}</span>
        <span v-if="entry.type === 'command'" class="prompt">{{ getPrompt() }} </span>
        <span class="content">{{ entry.content }}</span>
      </div>
      
      <div v-if="isExecuting" class="terminal-line executing">
        <span class="timestamp">{{ formatTimestamp(new Date()) }}</span>
        <span class="content">Executing...</span>
      </div>
    </div>

    <!-- Terminal Input -->
    <div class="terminal-input-form">
      <span class="input-prompt">{{ getPrompt() }}</span>
      <input
        ref="terminalInput"
        v-model="currentCommand"
        type="text"
        class="terminal-input"
        placeholder="Enter command..."
        :disabled="isExecuting"
        autocomplete="off"
        spellcheck="false"
        @keydown="handleKeyDown"
        @keyup.enter="executeCommand"
      />
    </div>

    <!-- Help Panel -->
    <div v-if="showHelp" class="terminal-help">
      <div class="help-content">
        <div class="help-section">
          <strong>Keyboard Shortcuts:</strong>
          <ul>
            <li>↑/↓ - Navigate command history</li>
            <li>Tab - Auto-complete common commands</li>
            <li>Ctrl+L - Clear terminal</li>
            <li>Enter - Execute command</li>
          </ul>
        </div>
        <div class="help-section">
          <strong>Quick Commands:</strong>
          <ul>
            <li><code>info patchlevel</code> - Tcl version</li>
            <li><code>package names</code> - Available packages</li>
            <li><code>expr {2 + 2}</code> - Math expression</li>
          </ul>
        </div>
      </div>
    </div>

    <!-- Status Bar -->
    <div class="terminal-status">
      <div class="status-left">
        {{ getStatusMessage() }}
      </div>
      <div class="status-right">
        History: {{ commandHistory.length }} | Lines: {{ history.length }}
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, nextTick, onMounted, watch, inject } from 'vue'

// State
const history = ref([])
const currentCommand = ref('')
const isExecuting = ref(false)
const commandHistory = ref([])
const historyIndex = ref(-1)
const showHelp = ref(false)
const serviceMode = inject('serviceMode')

// Refs
const terminalOutput = ref(null)
const terminalInput = ref(null)

// Common Tcl commands for tab completion
const tclCommands = [
  'set', 'puts', 'expr', 'if', 'for', 'foreach', 'while', 'proc', 'return',
  'package', 'source', 'info', 'string', 'list', 'array', 'dict', 'file',
  'glob', 'regexp', 'regsub', 'split', 'join', 'format', 'scan', 'clock',
  'exec', 'eval', 'uplevel', 'upvar', 'global', 'variable', 'namespace',
  'interp', 'catch', 'error', 'throw', 'try', 'zipfs', 'lappend', 'lindex',
  'llength', 'lrange', 'lsearch', 'lsort', 'incr', 'append', 'cd', 'pwd'
]

// Initialize terminal
onMounted(() => {
  addSystemMessage('Terminal Ready - Connected to backend')
  addSystemMessage('Type "help" for assistance or try: info patchlevel')
  focusInput()
})

// Auto-scroll when history changes
watch(() => history.value.length, () => {
  nextTick(() => {
    if (terminalOutput.value) {
      terminalOutput.value.scrollTop = terminalOutput.value.scrollHeight
    }
  })
})

// Methods
function addSystemMessage(content) {
  history.value.push({
    id: Date.now() + Math.random(),
    type: 'system',
    content,
    timestamp: new Date()
  })
}

function addCommandEntry(command) {
  history.value.push({
    id: Date.now() + Math.random(),
    type: 'command',
    content: command,
    timestamp: new Date()
  })
}

function addResultEntry(content, isError = false) {
  history.value.push({
    id: Date.now() + Math.random(),
    type: isError ? 'error' : 'result',
    content: content || '(no output)',
    timestamp: new Date()
  })
}

async function executeCommand() {
  const command = currentCommand.value.trim()
  if (!command || isExecuting.value) return

  // Handle built-in commands
  if (command === 'help') {
    showHelp.value = !showHelp.value
    currentCommand.value = ''
    return
  }

  if (command === 'clear') {
    clearTerminal()
    currentCommand.value = ''
    return
  }

  if (command === '/ess') {
    changeServiceMode('ess')
    currentCommand.value = ''
    return
  }

  addCommandEntry(command)
  isExecuting.value = true

  // Add to command history
  if (!commandHistory.value.includes(command)) {
    commandHistory.value.unshift(command)
    if (commandHistory.value.length > 100) {
      commandHistory.value.pop()
    }
  }
  historyIndex.value = -1

  try {
  
    const response = await fetch(`/api/${serviceMode.value}/eval`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
      },
      body: JSON.stringify({ script: command }),
    })

    if (!response.ok) {
      throw new Error(`HTTP ${response.status}: ${response.statusText}`)
    }

    const result = await response.json()
    addResultEntry(result.error || result.result, !!result.error)

  } catch (error) {
    addResultEntry(`Network error: ${error.message}`, true)
  } finally {
    isExecuting.value = false
    currentCommand.value = ''
    focusInput()
  }
}

function handleKeyDown(event) {
  if (event.key === 'ArrowUp') {
    event.preventDefault()
    navigateHistory(1)
  } else if (event.key === 'ArrowDown') {
    event.preventDefault()
    navigateHistory(-1)
  } else if (event.key === 'Tab') {
    event.preventDefault()
    handleTabCompletion()
  } else if (event.key === 'l' && event.ctrlKey) {
    event.preventDefault()
    clearTerminal()
  } else if (event.key === 'c' && event.ctrlKey) {
    event.preventDefault()
    if (isExecuting.value) {
      addResultEntry('Command interrupted by user', true)
      isExecuting.value = false
      currentCommand.value = ''
    }
  }
}

function navigateHistory(direction) {
  if (commandHistory.value.length === 0) return

  const newIndex = historyIndex.value + direction
  
  if (newIndex >= 0 && newIndex < commandHistory.value.length) {
    historyIndex.value = newIndex
    currentCommand.value = commandHistory.value[newIndex]
  } else if (newIndex < 0) {
    historyIndex.value = -1
    currentCommand.value = ''
  }
}

function handleTabCompletion() {
  const words = currentCommand.value.split(' ')
  const currentWord = words[words.length - 1]
  
  if (!currentWord) return

  const matches = tclCommands.filter(cmd => cmd.startsWith(currentWord))
  
  if (matches.length === 1) {
    words[words.length - 1] = matches[0]
    currentCommand.value = words.join(' ') + ' '
  } else if (matches.length > 1) {
    // Show available completions
    const completionText = `Available completions: ${matches.join(', ')}`
    addResultEntry(completionText)
  }
}

function changeServiceMode(mode) {
  history.value = []
  serviceMode.value = mode
}

function clearTerminal() {
  history.value = []
  addSystemMessage('Terminal cleared')
}

function focusInput() {
  nextTick(() => {
    if (terminalInput.value) {
      terminalInput.value.focus()
    }
  })
}

function formatTimestamp(timestamp) {
  return timestamp.toLocaleTimeString([], { 
    hour12: false, 
    hour: '2-digit', 
    minute: '2-digit', 
    second: '2-digit' 
  })
}

function getPrompt() {
  return isExecuting.value ? `${serviceMode.value}> ...` : `${serviceMode.value}> `
}

function getStatusMessage() {
  if (isExecuting.value) {
    return 'Executing command...'
  }
  return 'Ready - Enter Tcl commands'
}
</script>

<style scoped>
.tcl-terminal {
  display: flex;
  flex-direction: column;
  height: 100%;
  background: #000;
  color: #00ff00;
  font-family: 'Courier New', 'Monaco', 'Lucida Console', monospace;
  font-size: 12px;
  border: 2px inset #f0f0f0;
}

.terminal-header {
  height: 28px;
  background: linear-gradient(to bottom, #f0f0f0, #e0e0e0);
  border-bottom: 1px solid #ccc;
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 8px;
  font-weight: 500;
  font-size: 12px;
  color: #333;
  flex-shrink: 0;
}

.terminal-controls {
  display: flex;
  gap: 4px;
}

.terminal-btn {
  width: 20px;
  height: 20px;
  border: 1px solid #999;
  background: linear-gradient(to bottom, #fff, #e0e0e0);
  font-size: 10px;
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: 2px;
}

.terminal-btn:hover {
  background: linear-gradient(to bottom, #fff, #d0d0d0);
}

.terminal-output {
  flex: 1;
  overflow-y: auto;
  padding: 8px;
  background: #000;
  cursor: text;
  min-height: 200px;
  max-height: 400px;
}

.terminal-line {
  margin: 2px 0;
  display: flex;
  align-items: flex-start;
  gap: 8px;
  word-wrap: break-word;
  white-space: pre-wrap;
  line-height: 1.3;
}

.terminal-line.system {
  color: #ffff00;
}

.terminal-line.command {
  color: #ffffff;
  font-weight: bold;
}

.terminal-line.result {
  color: #00ff00;
  margin-left: 70px;
}

.terminal-line.error {
  color: #ff6666;
  margin-left: 70px;
}

.terminal-line.executing {
  color: #888;
  font-style: italic;
}

.timestamp {
  color: #666;
  font-size: 10px;
  min-width: 60px;
  flex-shrink: 0;
}

.prompt {
  color: #00ffff;
  font-weight: bold;
  min-width: 35px;
  flex-shrink: 0;
}

.content {
  flex: 1;
  word-break: break-word;
}

.terminal-input-form {
  display: flex;
  align-items: center;
  padding: 8px;
  background: #111;
  border-top: 1px solid #333;
  gap: 4px;
  flex-shrink: 0;
}

.input-prompt {
  color: #00ffff;
  font-weight: bold;
  min-width: 50px;
  flex-shrink: 0;
}

.terminal-input {
  flex: 1;
  background: transparent;
  border: none;
  color: #ffffff;
  font-family: inherit;
  font-size: inherit;
  outline: none;
  padding: 2px 0;
}

.terminal-input:disabled {
  opacity: 0.6;
}

.terminal-input::placeholder {
  color: #666;
}

.terminal-help {
  background: #1a1a1a;
  border-top: 1px solid #333;
  max-height: 200px;
  overflow-y: auto;
  flex-shrink: 0;
}

.help-content {
  padding: 12px;
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 16px;
  font-size: 11px;
}

.help-section {
  color: #ccc;
}

.help-section strong {
  color: #ffff00;
  display: block;
  margin-bottom: 6px;
}

.help-section ul {
  margin: 0;
  padding-left: 16px;
  list-style-type: disc;
}

.help-section li {
  margin: 2px 0;
  line-height: 1.3;
}

.help-section code {
  color: #00ff00;
  background: #222;
  padding: 1px 3px;
  border-radius: 2px;
  font-size: 10px;
}

.terminal-status {
  background: #f0f0f0;
  border-top: 1px solid #ccc;
  padding: 2px 8px;
  font-size: 10px;
  color: #666;
  display: flex;
  justify-content: space-between;
  align-items: center;
  flex-shrink: 0;
  height: 20px;
}

/* Scrollbar styling */
.terminal-output::-webkit-scrollbar,
.terminal-help::-webkit-scrollbar {
  width: 12px;
}

.terminal-output::-webkit-scrollbar-track,
.terminal-help::-webkit-scrollbar-track {
  background: #222;
}

.terminal-output::-webkit-scrollbar-thumb,
.terminal-help::-webkit-scrollbar-thumb {
  background: #555;
  border-radius: 6px;
}

.terminal-output::-webkit-scrollbar-thumb:hover,
.terminal-help::-webkit-scrollbar-thumb:hover {
  background: #777;
}

/* Selection styling */
.terminal-output ::selection {
  background: #4CAF50;
  color: #000;
}

/* Responsive adjustments */
@media (max-width: 768px) {
  .help-content {
    grid-template-columns: 1fr;
  }
  
  .terminal-output {
    max-height: 300px;
  }
}
</style>