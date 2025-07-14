// errorService.js with multi-level logging support
import { reactive, ref } from 'vue'

class ErrorService {
  constructor() {
    this.errors = reactive([])
    this.logs = reactive([])  // NEW: General logging separate from errors
    this.isTracing = ref(false)
    this.errorIdCounter = 0
    this.logIdCounter = 0
    this.dservCleanup = null
    this.isInitialized = false
    
    // NEW: Separate counters for different log levels
    this.logCounts = reactive({
      error: 0,
      warning: 0,
      info: 0,
      debug: 0,
      general: 0,
      total: 0
    })
  }

  // Initialize the service and start listening globally
  async initializeGlobalTracking(dservInstance) {
    if (this.isInitialized) return // Already initialized
    
    console.log('Starting global error and logging tracking...')
    
    // Register with dserv globally
    this.dservCleanup = dservInstance.registerComponent('GlobalErrorService')

    // Set up handlers for all log levels
    dservInstance.on('datapoint:ess/errorInfo', this.handleError.bind(this))
    dservInstance.on('datapoint:ess/warningInfo', this.handleWarning.bind(this))
    dservInstance.on('datapoint:ess/infoLog', this.handleInfo.bind(this))
    dservInstance.on('datapoint:ess/debugLog', this.handleDebug.bind(this))
    dservInstance.on('datapoint:ess/generalLog', this.handleGeneral.bind(this))
    
    this.dservInstance = dservInstance
    this.isInitialized = true
    console.log('Global error and logging tracking started')
  }

  // Specific handlers for each log level
  handleError(data) {
    this.handleLogMessage(data, 'error', true) // true = also add to errors array
  }

  handleWarning(data) {
    this.handleLogMessage(data, 'warning', false)
  }

  handleInfo(data) {
    this.handleLogMessage(data, 'info', false)
  }

  handleDebug(data) {
    this.handleLogMessage(data, 'debug', false)
  }

  handleGeneral(data) {
    this.handleLogMessage(data, 'general', false)
  }

  // Universal log message handler
  handleLogMessage(data, level, isError = false) {
    if (!data.data || data.data.trim() === '') return
    
    const timestamp = Date.now()
    const messageText = data.data
    
    // Parse the formatted message from ESS
    // Format: [HH:MM:SS] [category] message
    const match = messageText.match(/^\[(\d{2}:\d{2}:\d{2})\]\s*\[([^\]]+)\]\s*(.+)$/)
    
    let timeString, category, text
    if (match) {
      timeString = match[1]
      category = match[2]
      text = match[3]
    } else {
      // Fallback if format doesn't match
      timeString = new Date(timestamp).toLocaleTimeString()
      category = 'unknown'
      text = messageText
    }
    
    const logEntry = {
      id: ++this.logIdCounter,
      timestamp,
      timeString,
      text,
      level,
      category,
      system: this.dservInstance?.state?.currentSystem || 'unknown',
      protocol: this.dservInstance?.state?.currentProtocol || 'unknown',
      variant: this.dservInstance?.state?.currentVariant || 'unknown',
      source: 'ess_backend'
    }
    
    // Add to general logs array
    this.logs.push(logEntry)
    
    // Also add to errors array if it's an error
    if (isError) {
      const errorEntry = {
        ...logEntry,
        id: ++this.errorIdCounter,
        stackTrace: [] // ESS errors don't typically have stack traces
      }
      this.errors.push(errorEntry)
      this.logCounts.error++
    } else {
      this.logCounts[level]++
    }
    
    this.logCounts.total++
    
    // Limit arrays to prevent memory issues
    if (this.logs.length > 2000) {
      this.logs.splice(0, this.logs.length - 2000)
    }
    if (this.errors.length > 1000) {
      this.errors.splice(0, this.errors.length - 1000)
    }
    
    console.log(`ESS ${level.toUpperCase()}: ${text}`)
  }

  // Get all logs (not just errors)
  getLogs() {
    return this.logs
  }

  // Get logs filtered by level
  getLogsByLevel(level) {
    return this.logs.filter(log => log.level === level)
  }

  // Get recent logs (last N entries)
  getRecentLogs(count = 50) {
    return this.logs.slice(-count)
  }

  // Clear logs
  clearLogs() {
    this.logs.splice(0, this.logs.length)
    this.logIdCounter = 0
    
    // Reset counts
    Object.keys(this.logCounts).forEach(key => {
      this.logCounts[key] = 0
    })
  }

  // Get log counts
  getLogCounts() {
    return this.logCounts
  }

  // Export logs to CSV
  exportLogs() {
    if (this.logs.length === 0) return null
    
    const csvContent = [
      ['Timestamp', 'Level', 'Category', 'System', 'Protocol', 'Variant', 'Message'].join(','),
      ...this.logs.map(log => [
        log.timeString,
        log.level,
        log.category,
        log.system,
        log.protocol,
        log.variant,
        `"${log.text.replace(/"/g, '""')}"`
      ].join(','))
    ].join('\n')
    
    const blob = new Blob([csvContent], { type: 'text/csv' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = `ess_logs_${new Date().toISOString().slice(0, 19).replace(/:/g, '-')}.csv`
    document.body.appendChild(a)
    a.click()
    document.body.removeChild(a)
    URL.revokeObjectURL(url)
    
    return true
  }

  // Send a log message from the frontend to ESS
  async sendLogMessage(level, message, category = 'frontend') {
    if (!this.dservInstance) {
      console.error('Cannot send log: ESS not connected')
      return false
    }

    try {
      // Format the message similar to ESS format
      const timestamp = new Date().toLocaleTimeString()
      const formatted = `[${timestamp}] [${category}] ${message}`
      
      // Send via ESS command to have it logged properly
      const command = `::ess::ess_${level} "${message}" "${category}"`
      await this.dservInstance.essCommand(command)
      
      return true
    } catch (error) {
      console.error('Failed to send log message:', error)
      return false
    }
  }

  // Convenience methods for sending different log levels
  async sendError(message, category = 'frontend') {
    return this.sendLogMessage('error', message, category)
  }

  async sendWarning(message, category = 'frontend') {
    return this.sendLogMessage('warning', message, category)
  }

  async sendInfo(message, category = 'frontend') {
    return this.sendLogMessage('info', message, category)
  }

  async sendDebug(message, category = 'frontend') {
    return this.sendLogMessage('debug', message, category)
  }

  // Enable/disable debug mode in ESS
  async setDebugMode(enabled) {
    if (!this.dservInstance) {
      throw new Error('ESS not connected')
    }
    
    const command = `::ess::set_debug_mode ${enabled ? 1 : 0}`
    await this.dservInstance.essCommand(command)
    
    return enabled
  }

  // Original error-specific methods (maintained for compatibility)
  
  // Global error handler - processes traditional Tcl errors
  handleGlobalError(data) {
    if (!data.data || data.data.trim() === '') return
    
    const timestamp = Date.now()
    const errorText = data.data
    const classification = this.classifyError(errorText)
    const stackTrace = this.parseStackTrace(errorText)
    
    const newError = {
      id: ++this.errorIdCounter,
      timestamp,
      timeString: new Date(timestamp).toLocaleTimeString(),
      text: errorText,
      level: classification.level,
      category: classification.category,
      stackTrace,
      system: this.dservInstance?.state?.currentSystem || 'unknown',
      protocol: this.dservInstance?.state?.currentProtocol || 'unknown',
      variant: this.dservInstance?.state?.currentVariant || 'unknown',
      source: 'tcl_error'
    }
    
    this.errors.push(newError)
    
    // Also add to logs
    const logEntry = {
      ...newError,
      id: ++this.logIdCounter
    }
    this.logs.push(logEntry)
    
    // Update counts
    this.logCounts[classification.level]++
    this.logCounts.total++
    
    // Limit to last 1000 errors to prevent memory issues
    if (this.errors.length > 1000) {
      this.errors.splice(0, this.errors.length - 1000)
    }
    
    console.log(`ESS Error captured: ${classification.level} - ${errorText.split('\n')[0]}`)
  }

  // Error classification (unchanged)
  classifyError(errorText) {
    const text = errorText.toLowerCase()
    
    if (text.includes('syntax error') || text.includes('parse error') || text.includes('invalid command')) {
      return { level: 'error', category: 'syntax' }
    } else if (text.includes('undefined variable') || text.includes('no such variable')) {
      return { level: 'error', category: 'variable' }
    } else if (text.includes('wrong # args') || text.includes('too many arguments')) {
      return { level: 'error', category: 'arguments' }
    } else if (text.includes('can\'t read') || text.includes('permission denied')) {
      return { level: 'error', category: 'file' }
    } else if (text.includes('warning') || text.includes('deprecated')) {
      return { level: 'warning', category: 'warning' }
    } else if (text.includes('info') || text.includes('notice')) {
      return { level: 'info', category: 'info' }
    } else {
      return { level: 'error', category: 'general' }
    }
  }

  // Parse stack trace (unchanged)
  parseStackTrace(errorText) {
    const lines = errorText.split('\n')
    const stackTrace = []
    
    for (let i = 0; i < lines.length; i++) {
      const line = lines[i].trim()
      if (line.includes('(file ') || line.includes('invoked from within')) {
        stackTrace.push({
          line: line,
          isFile: line.includes('(file '),
          isInvocation: line.includes('invoked from within')
        })
      }
    }
    
    return stackTrace
  }

  // Toggle error tracing globally
  async toggleTracing() {
    if (!this.dservInstance) {
      throw new Error('Error service not initialized')
    }
    
    const command = this.isTracing.value ? 'ess::error_untrace' : 'ess::error_trace'
    await this.dservInstance.essCommand(command)
    this.isTracing.value = !this.isTracing.value
    
    console.log(`Error tracing ${this.isTracing.value ? 'enabled' : 'disabled'} globally`)
    return this.isTracing.value
  }

  // Clear all errors (maintain compatibility)
  clearErrors() {
    this.errors.splice(0, this.errors.length)
    this.errorIdCounter = 0
    
    // Also clear error count from logs
    this.logCounts.error = 0
    this.logCounts.total = this.logs.length
  }

  // Get current errors (maintain compatibility)
  getErrors() {
    return this.errors
  }

  // Get error counts (maintain compatibility)
  getErrorCounts() {
    const counts = { error: 0, warning: 0, info: 0, total: this.errors.length }
    this.errors.forEach(error => {
      counts[error.level] = (counts[error.level] || 0) + 1
    })
    return counts
  }

  // Export errors (maintain compatibility)
  exportErrors() {
    if (this.errors.length === 0) return null
    
    const csvContent = [
      ['Timestamp', 'Level', 'Category', 'System', 'Protocol', 'Variant', 'Error'].join(','),
      ...this.errors.map(error => [
        error.timeString,
        error.level,
        error.category,
        error.system,
        error.protocol,
        error.variant,
        `"${error.text.replace(/"/g, '""')}"`
      ].join(','))
    ].join('\n')
    
    const blob = new Blob([csvContent], { type: 'text/csv' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = `ess_errors_${new Date().toISOString().slice(0, 19).replace(/:/g, '-')}.csv`
    document.body.appendChild(a)
    a.click()
    document.body.removeChild(a)
    URL.revokeObjectURL(url)
    
    return true
  }

    // Add this method to the ErrorService class in errorService.js
    addFrontendLog(level, message, category = 'frontend') {
	const timestamp = Date.now()
	const timeString = new Date(timestamp).toLocaleTimeString()
	
	const logEntry = {
	    id: ++this.logIdCounter,
	    timestamp,
	    timeString,
	    text: message,
	    level,
	    category,
	    system: this.dservInstance?.state?.currentSystem || 'current',
	    protocol: this.dservInstance?.state?.currentProtocol || 'current', 
	    variant: this.dservInstance?.state?.currentVariant || 'current',
	    source: 'frontend'
	}
	
	// Add to logs array
	this.logs.push(logEntry)
	
	// Update counts
	this.logCounts[level]++
	this.logCounts.total++
	
	// If it's an error, also add to errors array
	if (level === 'error') {
	    const errorEntry = {
		...logEntry,
		id: ++this.errorIdCounter,
		stackTrace: []
	    }
	    this.errors.push(errorEntry)
	}
	
	// Limit arrays to prevent memory issues
	if (this.logs.length > 2000) {
	    this.logs.splice(0, this.logs.length - 2000)
	}
	
	console.log(`Frontend ${level.toUpperCase()}: ${message}`)
    }
    
  // Check if tracing is active
  isTracingActive() {
    return this.isTracing.value
  }

  // Cleanup
  cleanup() {
    if (this.dservCleanup) {
      this.dservCleanup()
      this.dservCleanup = null
    }
    this.isInitialized = false
  }
}

// Create singleton instance
export const errorService = new ErrorService()

// Auto-initialize when dserv is available
export const initializeErrorTracking = async (dservInstance) => {
  return await errorService.initializeGlobalTracking(dservInstance)
}
