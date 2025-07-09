<template>
  <div class="ess-terminal" style="height: 100%; display: flex; flex-direction: column;">
    <!-- Terminal Header -->
    <div class="terminal-header">
      <span>ESS Terminal</span>
      <a-button
        size="small"
        type="text"
        :icon="h(ClearOutlined)"
        @click="clearTerminal"
        class="clear-button"
      />
    </div>

    <!-- Terminal Content -->
    <div
      ref="terminalOutput"
      class="terminal-output"
    >
      <div v-for="(line, index) in lines" :key="index" class="terminal-line">
        <span :class="getLineClass(line.type)">
          {{ line.text }}
        </span>
      </div>
    </div>

    <!-- Terminal Input -->
    <div class="terminal-input">
      <span class="prompt">ess></span>
      <a-input
        ref="terminalInputRef"
        v-model:value="currentCommand"
        size="small"
        :bordered="false"
        @press-enter="executeCommand"
        @keydown="handleKeydown"
        class="command-input"
        placeholder="Enter ESS command..."
      />
    </div>
  </div>
</template>

<script setup>
import { ref, nextTick, onMounted, watch } from 'vue'
import { ClearOutlined } from '@ant-design/icons-vue'
import { h } from 'vue'
import { dserv } from '../services/dserv.js'

// Terminal state
const currentCommand = ref('')
const lines = ref([
  { text: 'ESS Terminal - Connected to local WebSocket', type: 'info' },
  { text: 'Type ESS commands or use the controls above', type: 'info' },
  { text: '', type: 'normal' }
])
const terminalOutput = ref(null)
const terminalInputRef = ref(null)
const commandHistory = ref([])
const historyIndex = ref(-1)

// Methods
function addLine(text, type = 'normal') {
  lines.value.push({ text, type })
  nextTick(() => {
    scrollToBottom()
  })
}

function scrollToBottom() {
  if (terminalOutput.value) {
    terminalOutput.value.scrollTop = terminalOutput.value.scrollHeight
  }
}

async function executeCommand() {
  const command = currentCommand.value.trim()
  if (!command) return

  // Add to history
  commandHistory.value.push(command)
  historyIndex.value = -1

  // Add command to terminal
  addLine(`ess> ${command}`, 'command')
  currentCommand.value = ''

  try {
    const result = await dserv.essCommand(command)
    console.log('Terminal received result:', result); // For debugging
    if (result && result.length > 0) {
      // Handle multi-line results
      const resultLines = result.split('\n')
      resultLines.forEach(line => {
        if (line.trim()) {
          addLine(line, 'normal')
        }
      })
    }
  } catch (error) {
    addLine(error.message, 'error')
  }
}

function handleKeydown(event) {
  // Handle command history navigation
  if (event.key === 'ArrowUp') {
    event.preventDefault()
    if (commandHistory.value.length > 0) {
      if (historyIndex.value === -1) {
        historyIndex.value = commandHistory.value.length - 1
      } else if (historyIndex.value > 0) {
        historyIndex.value--
      }
      currentCommand.value = commandHistory.value[historyIndex.value] || ''
    }
  } else if (event.key === 'ArrowDown') {
    event.preventDefault()
    if (historyIndex.value >= 0) {
      historyIndex.value++
      if (historyIndex.value >= commandHistory.value.length) {
        historyIndex.value = -1
        currentCommand.value = ''
      } else {
        currentCommand.value = commandHistory.value[historyIndex.value] || ''
      }
    }
  }
}

function clearTerminal() {
  lines.value = [
    { text: 'ESS Terminal - Connected to local WebSocket', type: 'info' },
    { text: 'Type ESS commands or use the controls above', type: 'info' },
    { text: '', type: 'normal' }
  ]
}

function getLineClass(type) {
  return `terminal-line-${type}`
}

// Public methods for external components to use
function logSuccess(message) {
  addLine(message, 'success')
}

function logError(message) {
  addLine(message, 'error')
}

function logInfo(message) {
  addLine(message, 'info')
}

function logCommand(command, result = null) {
  addLine(`ess> ${command}`, 'command')
  if (result) {
    addLine(result, 'normal')
  }
}

// Expose methods for parent components
defineExpose({
  logSuccess,
  logError,
  logInfo,
  logCommand,
  addLine,
  clearTerminal
})

// Watch connection status
watch(() => dserv.state.connected, (connected) => {
  if (connected) {
    addLine('Connected to dserv', 'success')
  } else {
    addLine('Disconnected from dserv', 'error')
  }
})

// Focus input on mount
onMounted(() => {
  nextTick(() => {
    if (terminalInputRef.value) {
      terminalInputRef.value.focus()
    }
  })
})
</script>

<style scoped>
.ess-terminal {
  background: #1e1e1e;
  color: #ccc;
  font-family: 'Courier New', monospace;
  font-size: 12px;
}

.terminal-header {
  background: #2d2d2d;
  color: white;
  padding: 4px 8px;
  font-size: 11px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  border-bottom: 1px solid #444;
}

.clear-button {
  color: #ccc !important;
  height: 20px !important;
  width: 20px !important;
}

.clear-button:hover {
  color: white !important;
  background: #444 !important;
}

.terminal-output {
  flex: 1;
  padding: 8px;
  overflow-y: auto;
  background: #1e1e1e;
}

.terminal-line {
  margin-bottom: 2px;
  word-wrap: break-word;
}

.terminal-line-normal {
  color: #ccc;
}

.terminal-line-command {
  color: #87CEEB;
  font-weight: bold;
}

.terminal-line-success {
  color: #51cf66;
}

.terminal-line-error {
  color: #ff6b6b;
}

.terminal-line-info {
  color: #74c0fc;
}

.terminal-input {
  display: flex;
  align-items: center;
  background: #2d2d2d;
  padding: 4px 8px;
  border-top: 1px solid #444;
}

.prompt {
  color: #51cf66;
  margin-right: 8px;
  font-weight: bold;
}

.command-input {
  background: transparent !important;
  color: #ccc !important;
  border: none !important;
  box-shadow: none !important;
}

.command-input:focus {
  border: none !important;
  box-shadow: none !important;
}

/* Scrollbar styling */
.terminal-output::-webkit-scrollbar {
  width: 8px;
}

.terminal-output::-webkit-scrollbar-track {
  background: #2d2d2d;
}

.terminal-output::-webkit-scrollbar-thumb {
  background: #555;
  border-radius: 4px;
}

.terminal-output::-webkit-scrollbar-thumb:hover {
  background: #777;
}
</style>
