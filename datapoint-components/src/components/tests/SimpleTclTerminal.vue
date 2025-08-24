<template>
  <div class="tcl-terminal">
    <div class="terminal-header">
      <h2>🖥️ Simple Tcl Terminal</h2>
      <div class="connection-status" :class="{ connected: isConnected }">
        <div class="status-dot"></div>
        <span>{{ isConnected ? 'Connected' : 'Disconnected' }}</span>
      </div>
    </div>

    <div class="terminal-body">
      <!-- Command Input -->
      <div class="command-input-section">
        <div class="input-group">
          <input 
            v-model="currentCommand" 
            @keyup.enter="executeCommand"
            placeholder="Enter Tcl command (e.g., expr 2+2, info patchlevel, dservGet ess/status)"
            class="command-input"
            ref="commandInput"
            :disabled="!isConnected"
          >
          <button @click="executeCommand" class="execute-btn" :disabled="!isConnected || !currentCommand.trim()">
            Execute
          </button>
        </div>
        <div class="quick-commands">
          <button @click="quickCommand('info patchlevel')" class="quick-btn">Tcl Version</button>
          <button @click="quickCommand('expr 2+2')" class="quick-btn">Math Test</button>
          <button @click="quickCommand('dservGet ess/status')" class="quick-btn">ESS Status</button>
          <button @click="quickCommand('dservList')" class="quick-btn">List Datapoints</button>
          <button @click="clearHistory" class="quick-btn clear-btn">Clear History</button>
        </div>
      </div>

      <!-- Terminal Output -->
      <div class="terminal-output" ref="terminalOutput">
        <div class="output-header">
          <span>Command History & Results</span>
          <span class="command-count">{{ commandHistory.length }} commands</span>
        </div>
        
        <div class="history-content">
          <div 
            v-for="(entry, index) in commandHistory" 
            :key="index"
            class="history-entry"
            :class="{ error: entry.error, pending: entry.status === 'pending' }"
          >
            <div class="history-command">
              <span class="prompt">tcl></span> {{ entry.command }}
            </div>
            
            <div v-if="entry.status === 'pending'" class="history-pending">
              ⏳ Executing...
            </div>
            
            <div v-else-if="entry.result" class="history-result">
              {{ entry.result }}
            </div>
            
            <div v-else-if="entry.error" class="history-error">
              ❌ Error: {{ entry.error }}
            </div>
            
            <div class="history-meta">
              <span class="history-time">{{ entry.timestamp }}</span>
              <span class="history-status" :class="entry.status">{{ entry.status }}</span>
            </div>
          </div>
          
          <div v-if="commandHistory.length === 0" class="no-history">
            No commands executed yet. Try typing a simple Tcl command above!
          </div>
        </div>
      </div>
    </div>

    <!-- Status Bar -->
    <div class="status-bar">
      <span>{{ statusMessage }}</span>
      <span class="backend-info">Backend: {{ backendInfo }}</span>
    </div>
  </div>
</template>

<script>
export default {
  name: 'SimpleTclTerminal',
  
  data() {
    return {
      isConnected: false,
      currentCommand: '',
      commandHistory: [],
      statusMessage: 'Connecting...',
      backendInfo: 'Unknown',
      unsubscribeFunctions: []
    }
  },

  methods: {
    async executeCommand() {
      const command = this.currentCommand.trim()
      if (!command) return
      
      console.log('Executing command:', command)
      
      // Add to history as pending
      this.addToHistory(command, null, 'pending')
      
      // Clear input
      this.currentCommand = ''
      
      // Send command directly like your working terminal.html does
      try {
        if (this.$dserv.ws && this.$dserv.ws.readyState === WebSocket.OPEN) {
          console.log('Sending command directly via WebSocket')
          
          // Set up response handler BEFORE sending
          const responseHandler = (event) => {
            console.log('Raw WebSocket message received:', event.data)
            
            try {
              const data = JSON.parse(event.data)
              console.log('Parsed response:', data)
              
              // Update the last history entry
              const lastEntry = this.commandHistory[this.commandHistory.length - 1]
              
              if (data.status === 'error' || (data.result && data.result.startsWith('!TCL_ERROR'))) {
                lastEntry.error = data.result || data.error || 'Unknown error'
                lastEntry.status = 'error'
              } else {
                lastEntry.result = data.result || '(command completed)'
                lastEntry.status = 'success'
              }
              
              // Remove the handler after getting response
              this.$dserv.ws.removeEventListener('message', responseHandler)
              
            } catch (e) {
              console.error('Failed to parse response:', e)
              const lastEntry = this.commandHistory[this.commandHistory.length - 1]
              lastEntry.result = event.data // Use raw data if parsing fails
              lastEntry.status = 'success'
              this.$dserv.ws.removeEventListener('message', responseHandler)
            }
            
            // Force reactivity update
            this.$forceUpdate()
            
            // Auto-scroll
            this.$nextTick(() => {
              this.scrollToBottom()
            })
          }
          
          // Add response handler
          this.$dserv.ws.addEventListener('message', responseHandler)
          
          // Send command (exactly like your working terminal)
          this.$dserv.ws.send(JSON.stringify({
            cmd: 'eval',
            script: command
          }))
          
          console.log('Command sent via WebSocket')
          
          // Set timeout to clean up handler if no response
          setTimeout(() => {
            this.$dserv.ws.removeEventListener('message', responseHandler)
            const lastEntry = this.commandHistory[this.commandHistory.length - 1]
            if (lastEntry.status === 'pending') {
              lastEntry.error = 'Command timeout (10 seconds)'
              lastEntry.status = 'error'
              this.$forceUpdate()
            }
          }, 10000)
          
        } else {
          throw new Error('WebSocket not connected')
        }
        
      } catch (error) {
        console.error('Command failed:', error)
        
        // Update the last history entry with error
        const lastEntry = this.commandHistory[this.commandHistory.length - 1]
        lastEntry.error = error.message || error.toString()
        lastEntry.status = 'error'
        
        this.statusMessage = `Command failed: ${error.message}`
      }
      
      // Focus input
      this.$nextTick(() => {
        this.$refs.commandInput?.focus()
      })
    },

    quickCommand(command) {
      this.currentCommand = command
      this.executeCommand()
    },

    addToHistory(command, result, status = 'success') {
      this.commandHistory.push({
        command,
        result,
        error: null,
        status,
        timestamp: new Date().toLocaleTimeString()
      })

      // Limit history size
      if (this.commandHistory.length > 100) {
        this.commandHistory.shift()
      }
    },

    clearHistory() {
      this.commandHistory = []
      this.statusMessage = 'Command history cleared'
    },

    scrollToBottom() {
      const container = this.$refs.terminalOutput
      if (container) {
        container.scrollTop = container.scrollHeight
      }
    },

    // Test backend connectivity
    async testConnection() {
      try {
        const result = await this.$dserv.essCommand('info patchlevel')
        this.backendInfo = `Tcl ${result}`
        return true
      } catch (error) {
        this.backendInfo = 'Connection failed'
        console.error('Backend test failed:', error)
        return false
      }
    }
  },

  async mounted() {
    console.log('Simple Tcl Terminal mounted')
    console.log('$dserv available:', !!this.$dserv)
    console.log('$dserv methods:', this.$dserv ? Object.getOwnPropertyNames(this.$dserv) : 'N/A')
    
    if (this.$dserv) {
      console.log('dserv.essCommand available:', typeof this.$dserv.essCommand)
      this.isConnected = this.$dserv.state.connected
      console.log('Initial connection state:', this.isConnected)
      
      // Listen for connection changes
      const connectionUnsub = this.$dserv.on('connection', async (data) => {
        console.log('Connection changed:', data)
        this.isConnected = data.connected
        
        if (data.connected) {
          this.statusMessage = 'Connected to backend'
          // Test the connection
          const connected = await this.testConnection()
          if (connected) {
            this.statusMessage = 'Ready for commands'
          }
        } else {
          this.statusMessage = 'Disconnected from backend'
          this.backendInfo = 'Disconnected'
        }
      }, 'SimpleTclTerminal')
      
      this.unsubscribeFunctions.push(connectionUnsub)
      
      // If already connected, test immediately
      if (this.isConnected) {
        this.statusMessage = 'Testing backend connection...'
        const connected = await this.testConnection()
        if (connected) {
          this.statusMessage = 'Ready for commands'
        }
      }
    } else {
      console.error('$dserv service not available!')
      this.statusMessage = 'Error: dserv service not available'
    }
    
    // Focus input
    this.$nextTick(() => {
      this.$refs.commandInput?.focus()
    })
  },

  beforeUnmount() {
    this.unsubscribeFunctions.forEach(fn => fn())
  }
}
</script>

<style scoped>
.tcl-terminal {
  max-width: 900px;
  margin: 0 auto;
  background: #1a1a1a;
  color: #e0e0e0;
  border-radius: 8px;
  box-shadow: 0 4px 20px rgba(0,0,0,0.3);
  overflow: hidden;
  font-family: 'SF Mono', 'Monaco', 'Inconsolata', 'Roboto Mono', monospace;
}

/* Header */
.terminal-header {
  background: linear-gradient(135deg, #2d3748 0%, #4a5568 100%);
  padding: 15px 20px;
  display: flex;
  justify-content: space-between;
  align-items: center;
  border-bottom: 1px solid #4a5568;
}

.terminal-header h2 {
  margin: 0;
  color: #f7fafc;
  font-size: 1.2rem;
  font-weight: 600;
}

.connection-status {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 0.9rem;
  color: #cbd5e0;
}

.status-dot {
  width: 10px;
  height: 10px;
  border-radius: 50%;
  background: #f56565;
  transition: background-color 0.3s;
}

.connection-status.connected .status-dot {
  background: #48bb78;
}

/* Terminal Body */
.terminal-body {
  background: #1a1a1a;
  min-height: 500px;
  display: flex;
  flex-direction: column;
}

/* Command Input Section */
.command-input-section {
  padding: 15px 20px;
  border-bottom: 1px solid #2d3748;
  background: #2d3748;
}

.input-group {
  display: flex;
  gap: 10px;
  margin-bottom: 10px;
}

.command-input {
  flex: 1;
  padding: 12px 15px;
  background: #4a5568;
  border: 1px solid #718096;
  border-radius: 6px;
  color: #f7fafc;
  font-family: inherit;
  font-size: 0.9rem;
}

.command-input:focus {
  outline: none;
  border-color: #63b3ed;
  box-shadow: 0 0 0 3px rgba(99, 179, 237, 0.1);
}

.command-input:disabled {
  background: #2d3748;
  color: #718096;
  cursor: not-allowed;
}

.command-input::placeholder {
  color: #a0aec0;
}

.execute-btn {
  padding: 12px 20px;
  background: #4299e1;
  color: white;
  border: none;
  border-radius: 6px;
  cursor: pointer;
  font-weight: 600;
  transition: background-color 0.2s;
}

.execute-btn:hover:not(:disabled) {
  background: #3182ce;
}

.execute-btn:disabled {
  background: #4a5568;
  cursor: not-allowed;
}

/* Quick Commands */
.quick-commands {
  display: flex;
  gap: 8px;
  flex-wrap: wrap;
}

.quick-btn {
  padding: 6px 12px;
  background: #4a5568;
  color: #e2e8f0;
  border: 1px solid #718096;
  border-radius: 4px;
  cursor: pointer;
  font-size: 0.8rem;
  transition: all 0.2s;
}

.quick-btn:hover {
  background: #718096;
}

.quick-btn.clear-btn {
  background: #e53e3e;
  border-color: #e53e3e;
}

.quick-btn.clear-btn:hover {
  background: #c53030;
}

/* Terminal Output */
.terminal-output {
  flex: 1;
  padding: 15px 20px;
  overflow-y: auto;
  max-height: 400px;
}

.output-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 15px;
  color: #a0aec0;
  font-size: 0.9rem;
  padding-bottom: 8px;
  border-bottom: 1px solid #2d3748;
}

.command-count {
  font-size: 0.8rem;
  color: #718096;
}

/* History Content */
.history-content {
  space-y: 10px;
}

.history-entry {
  margin-bottom: 15px;
  padding: 10px;
  background: #2d3748;
  border-left: 3px solid #4a5568;
  border-radius: 6px;
}

.history-entry.error {
  border-left-color: #f56565;
  background: rgba(245, 101, 101, 0.1);
}

.history-entry.pending {
  border-left-color: #ed8936;
  background: rgba(237, 137, 54, 0.1);
}

.history-command {
  margin-bottom: 8px;
  color: #e2e8f0;
  font-weight: 500;
}

.prompt {
  color: #4299e1;
  margin-right: 8px;
}

.history-pending {
  color: #ed8936;
  font-style: italic;
  margin-bottom: 5px;
}

.history-result {
  color: #68d391;
  background: rgba(104, 211, 145, 0.1);
  padding: 8px;
  border-radius: 4px;
  white-space: pre-wrap;
  margin-bottom: 8px;
  font-family: inherit;
}

.history-error {
  color: #feb2b2;
  background: rgba(254, 178, 178, 0.1);
  padding: 8px;
  border-radius: 4px;
  margin-bottom: 8px;
}

.history-meta {
  display: flex;
  justify-content: space-between;
  align-items: center;
  font-size: 0.75rem;
  color: #a0aec0;
}

.history-status.success {
  color: #68d391;
}

.history-status.error {
  color: #feb2b2;
}

.history-status.pending {
  color: #ed8936;
}

.no-history {
  color: #718096;
  font-style: italic;
  text-align: center;
  padding: 40px;
}

/* Status Bar */
.status-bar {
  background: #2d3748;
  padding: 10px 20px;
  border-top: 1px solid #4a5568;
  display: flex;
  justify-content: space-between;
  align-items: center;
  font-size: 0.8rem;
  color: #a0aec0;
}

.backend-info {
  color: #718096;
}

/* Scrollbar Styling */
.terminal-output::-webkit-scrollbar {
  width: 8px;
}

.terminal-output::-webkit-scrollbar-track {
  background: #2d3748;
}

.terminal-output::-webkit-scrollbar-thumb {
  background: #4a5568;
  border-radius: 4px;
}

.terminal-output::-webkit-scrollbar-thumb:hover {
  background: #718096;
}
</style>