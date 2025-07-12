// Corrected dserv.js - keeping original working logic + adding monitoring
import { reactive, ref, computed } from 'vue';
import { message } from 'ant-design-vue';

// Helper to add a timeout to a promise
function withTimeout(promise, ms, errorMessage = 'Operation timed out') {
  const timeout = new Promise((_, reject) => {
    const id = setTimeout(() => {
      clearTimeout(id);
      reject(new Error(errorMessage));
    }, ms);
  });
  return Promise.race([promise, timeout]);
}

// Datapoint frequency configuration (for monitoring)
const DATAPOINT_CONFIG = {
  'ess/em_pos': { expectedHz: 60, maxHz: 120, priority: 'high', category: 'eye_tracking' },
  'ess/em_region_status': { expectedHz: 30, maxHz: 60, priority: 'high', category: 'eye_tracking' },
  'ess/block_pct_complete': { expectedHz: 2, maxHz: 10, priority: 'medium', category: 'performance' },
  'ess/block_pct_correct': { expectedHz: 2, maxHz: 10, priority: 'medium', category: 'performance' },
  'ess/obs_id': { expectedHz: 0.1, maxHz: 1, priority: 'medium', category: 'experiment' },
  'ess/system': { expectedHz: 0.01, maxHz: 0.1, priority: 'low', category: 'configuration' },
  'ess/protocol': { expectedHz: 0.01, maxHz: 0.1, priority: 'low', category: 'configuration' },
  'ess/variant': { expectedHz: 0.01, maxHz: 0.1, priority: 'low', category: 'configuration' },
  'ess/status': { expectedHz: 0.1, maxHz: 1, priority: 'low', category: 'state' },
};

class DservWebSocket {
  constructor() {
    this.ws = null;
    this.reconnectAttempts = 0;
    this.maxReconnectAttempts = 5;
    this.reconnectInterval = 2000;
    this.requestCallbacks = new Map();
    
    // Simple event system - no complex subscription tracking
    this.eventHandlers = new Map();
    this.componentHandlers = new Map(); // Track which components are listening
    
    // ORIGINAL: Performance monitoring (enhanced but non-intrusive)
    this.stats = reactive({
      messagesReceived: 0,
      messagesSent: 0,
      avgProcessingTime: 0,
      lastMessageTime: 0
    });

    // NEW: Additional monitoring (separate from main state)
    this.monitoring = reactive({
      connectionStats: {
        connected: false,
        connectedSince: null,
        reconnectAttempts: 0,
        uptime: 0
      },
      messageStats: {
        totalReceived: 0,
        totalSent: 0,
        messagesPerSecond: 0,
        bytesPerSecond: 0,
        lastSecondCount: 0,
        lastSecondBytes: 0
      },
      datapointFrequencies: reactive(new Map()),
      performance: {
        avgProcessingTime: 0,
        maxProcessingTime: 0,
        processedThisSecond: 0
      },
      subscriptions: {
        active: 0,
        byCategory: reactive(new Map()),
        byPriority: reactive(new Map()),
        components: 0
      },
      bandwidth: {
        currentKbps: 0,
        peakKbps: 0,
        averageKbps: 0,
        samples: []
      }
    });

    // ORIGINAL: Central state - this is the main value of dserv.js
    this.state = reactive({
      connected: false,
      systems: [],
      protocols: [],
      variants: [],
      branches: [],
      currentSystem: '',
      currentProtocol: '',
      currentVariant: '',
      currentBranch: '',
      status: 'disconnected',
      essStatus: 'stopped',
      obsCount: '',
      obsId: 0,
      obsTotal: 0,
      blockPctComplete: 0,
      blockPctCorrect: 0,
      subject: '',
      systemName: '',
      systemOS: '',
      inObs: false,
      eyeWindows: Array(8).fill(null).map((_, i) => ({
        id: i,
        active: false,
        state: 0,
        type: 'rectangle',
        center: { x: 0, y: 0 },
        centerRaw: { x: 2048, y: 2048 },
        size: { width: 2, height: 2 },
        sizeRaw: { width: 400, height: 400 }
      })),
      eyeWindowStatusMask: 0,
    });

    this.loadingOperations = reactive({
      system: false,
      protocol: false,
      variant: false,
    });

    // Start monitoring
    this.startMonitoring();

    this.connect();
  }

  connect() {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const host = window.location.host || 'localhost:2565';
    const wsUrl = `${protocol}//${host}/ws`;

    console.log(`Connecting to ${wsUrl}`);
    this.ws = new WebSocket(wsUrl);

    this.ws.onopen = () => {
      console.log('Connected to dserv');
      this.state.connected = true;
      this.monitoring.connectionStats.connected = true;
      this.monitoring.connectionStats.connectedSince = Date.now();
      this.reconnectAttempts = 0;
      
      // Subscribe to ALL patterns we'll ever need - just once
      this.establishGlobalSubscriptions();
      this.initializeState();
      this.emit('connection', { connected: true });
    };

    this.ws.onmessage = (event) => {
      const startTime = performance.now();
      const messageSize = event.data.length;
      
      // ORIGINAL: Basic stats
      this.stats.messagesReceived++;
      
      // NEW: Enhanced monitoring stats
      this.monitoring.messageStats.totalReceived++;
      this.monitoring.messageStats.lastSecondCount++;
      this.monitoring.messageStats.lastSecondBytes += messageSize;
      
      try {
        const data = JSON.parse(event.data);
        
        // NEW: Track datapoint frequencies (non-intrusive)
        if (data.type === 'datapoint' && data.data) {
          this.trackDatapointFrequency(data.data.name, startTime);
        }
        
        // ORIGINAL: Handle message (this is the critical part!)
        this.handleMessage(data);
        
        // ORIGINAL: Update performance stats
        const processingTime = performance.now() - startTime;
        this.stats.avgProcessingTime = (this.stats.avgProcessingTime * 0.9) + (processingTime * 0.1);
        this.stats.lastMessageTime = Date.now();
        
        // NEW: Enhanced performance tracking
        this.monitoring.performance.avgProcessingTime = this.stats.avgProcessingTime;
        this.monitoring.performance.maxProcessingTime = Math.max(
          this.monitoring.performance.maxProcessingTime, 
          processingTime
        );
        
      } catch (error) {
        console.error('Failed to parse WebSocket message:', error, 'Raw data:', event.data);
      }
    };

    this.ws.onclose = () => {
      this.state.connected = false;
      this.monitoring.connectionStats.connected = false;
      this.emit('connection', { connected: false });
      this.attemptReconnect();
    };

    this.ws.onerror = (error) => {
      console.error('WebSocket error:', error);
      this.emit('error', { error });
    };
  }

  // Subscribe to everything we need - once and forever
  establishGlobalSubscriptions() {
    console.log('Establishing global subscriptions...');
    
    // Core system subscriptions
    this.send({ cmd: 'subscribe', match: 'ess/*', every: 1 });
    this.send({ cmd: 'subscribe', match: 'system/*', every: 1 });
    this.send({ cmd: 'subscribe', match: 'eventlog/*', every: 1 });
    
    // High-frequency data subscriptions
    this.send({ cmd: 'subscribe', match: 'ess/em_pos', every: 1 });
    this.send({ cmd: 'subscribe', match: 'ess/em_region_setting', every: 1 });
    this.send({ cmd: 'subscribe', match: 'ess/em_region_status', every: 1 });
    
    // Other useful subscriptions
    this.send({ cmd: 'subscribe', match: 'openiris/settings', every: 1 });
    this.send({ cmd: 'subscribe', match: 'print', every: 1 });
    
    console.log('All global subscriptions established');
  }

  async initializeState() {
    console.log('Initializing ESS state...');
    try {
      const touchCommand = `
        foreach v {
          ess/systems ess/protocols ess/variants
          ess/system ess/protocol ess/variant
          ess/variant_info_json ess/param_settings
          ess/subject ess/state ess/status
          ess/obs_id ess/obs_total ess/in_obs
          ess/block_pct_complete ess/block_pct_correct
          ess/git/branches ess/git/branch
          ess/system_script ess/protocol_script
          ess/variants_script ess/loaders_script
          ess/stim_script ess/rmt_connected
          system/hostname system/os
        } {
          catch { dservTouch $v }
        }
      `;
      await this.essCommand(touchCommand);
      console.log('Initial state synchronization completed');
      this.emit('initialized', { state: this.state });
    } catch (error) {
      console.error('Failed to initialize state:', error);
      this.emit('initializationFailed', { error });
    }
  }

  // ORIGINAL: This is the critical method that was broken!
  handleMessage(data) {
    if (data.requestId && this.requestCallbacks.has(data.requestId)) {
      const { resolve, reject } = this.requestCallbacks.get(data.requestId);
      this.requestCallbacks.delete(data.requestId);
      if (data.status === 'ok') {
        resolve(data.result);
      } else {
        reject(new Error(data.error || 'Command failed'));
      }
      return;
    }

    if (data.type === 'datapoint') {
      this.handleDatapoint(data);
    }
  }

  // ORIGINAL: This is the method that actually updates state and emits to components!
  handleDatapoint(data) {
    const { name, data: value } = data;
    
    // ORIGINAL: Update state and enrich data
    this.updateCentralState(name, value);
    
    // ORIGINAL: Route to components
    this.emit(`datapoint:${name}`, data);
    this.emit('datapoint:*', data);
  }

  // ORIGINAL: Central state management (this was working!)
  updateCentralState(name, value) {
    // This is where dserv.js adds value - central state management
    switch (name) {
      case 'ess/systems': this.state.systems = this.parseList(value); break;
      case 'ess/protocols': this.state.protocols = this.parseList(value); break;
      case 'ess/variants': this.state.variants = this.parseList(value); break;
      case 'ess/git/branches': this.state.branches = this.parseList(value); break;
      case 'ess/system': this.state.currentSystem = value; break;
      case 'ess/protocol': this.state.currentProtocol = value; break;
      case 'ess/variant': this.state.currentVariant = value; break;
      case 'ess/git/branch': this.state.currentBranch = value; break;
      case 'ess/status': 
        this.state.essStatus = value; 
        this.handleStatusChange(value); 
        break;
      case 'ess/state': this.state.status = value; break;
      case 'ess/obs_id': 
        this.state.obsId = parseInt(value, 10) || 0; 
        this.updateObsCount(); 
        break;
      case 'ess/obs_total': 
        this.state.obsTotal = parseInt(value, 10) || 0; 
        this.updateObsCount(); 
        break;
      case 'ess/in_obs': this.state.inObs = value === '1'; break;
      case 'ess/block_pct_complete': 
        this.state.blockPctComplete = Math.round(parseFloat(value) * 100); 
        break;
      case 'ess/block_pct_correct': 
        this.state.blockPctCorrect = Math.round(parseFloat(value) * 100); 
        break;
      case 'ess/subject': this.state.subject = value; break;

      // Eye tracking data processing
      case 'ess/em_region_setting':
        this.processEyeWindowSetting(value);
        break;
      case 'ess/em_region_status':
        this.processEyeWindowStatus(value);
        break;
        
      case 'system/hostname': this.state.systemName = value; break;
      case 'system/os': this.state.systemOS = value; break;
      case 'print': console.log('ESS:', value); break;
    }
  }

  // NEW: Non-intrusive frequency tracking
  trackDatapointFrequency(datapointName, timestamp) {
    if (!this.monitoring.datapointFrequencies.has(datapointName)) {
      this.monitoring.datapointFrequencies.set(datapointName, reactive({
        timestamps: [],
        count: 0,
        currentHz: 0,
        avgHz: 0,
        maxHz: 0,
        lastUpdate: timestamp,
        config: DATAPOINT_CONFIG[datapointName] || { 
          expectedHz: 'unknown', 
          priority: 'unknown', 
          category: 'unknown' 
        }
      }));
    }
    
    const freq = this.monitoring.datapointFrequencies.get(datapointName);
    freq.count++;
    freq.timestamps.push(timestamp);
    freq.lastUpdate = timestamp;
    
    // Keep only last 60 timestamps for frequency calculation
    if (freq.timestamps.length > 60) {
      freq.timestamps.shift();
    }
    
    // Calculate current frequency (messages in last second)
    const oneSecondAgo = timestamp - 1000;
    const recentTimestamps = freq.timestamps.filter(t => t > oneSecondAgo);
    freq.currentHz = recentTimestamps.length;
    
    // Update max frequency
    if (freq.currentHz > freq.maxHz) {
      freq.maxHz = freq.currentHz;
    }
    
    // Update average (exponential moving average)
    freq.avgHz = freq.avgHz * 0.95 + freq.currentHz * 0.05;
  }

  // NEW: Monitoring intervals
  startMonitoring() {
    // Update statistics every second
    setInterval(() => {
      this.updateSecondlyStats();
    }, 1000);
    
    // Update uptime
    setInterval(() => {
      if (this.monitoring.connectionStats.connected && this.monitoring.connectionStats.connectedSince) {
        this.monitoring.connectionStats.uptime = Date.now() - this.monitoring.connectionStats.connectedSince;
      }
    }, 100);
  }

  updateSecondlyStats() {
    const stats = this.monitoring.messageStats;
    const bandwidth = this.monitoring.bandwidth;
    
    // Calculate messages per second
    stats.messagesPerSecond = stats.lastSecondCount;
    stats.bytesPerSecond = stats.lastSecondBytes;
    
    // Calculate bandwidth in Kbps
    const currentKbps = (stats.lastSecondBytes * 8) / 1024;
    bandwidth.currentKbps = currentKbps;
    bandwidth.peakKbps = Math.max(bandwidth.peakKbps, currentKbps);
    
    // Keep bandwidth samples for averaging
    bandwidth.samples.push(currentKbps);
    if (bandwidth.samples.length > 60) {
      bandwidth.samples.shift();
    }
    bandwidth.averageKbps = bandwidth.samples.reduce((a, b) => a + b, 0) / bandwidth.samples.length;
    
    // Reset per-second counters
    stats.lastSecondCount = 0;
    stats.lastSecondBytes = 0;
    
    // Update subscription stats
    this.updateSubscriptionStats();
  }

  updateSubscriptionStats() {
    const subs = this.monitoring.subscriptions;
    subs.active = this.eventHandlers.size;
    subs.components = this.componentHandlers.size;
    
    // Clear and rebuild category/priority maps (avoid modifying computed-like objects)
    const newByCategory = new Map();
    const newByPriority = new Map();
    
    this.monitoring.datapointFrequencies.forEach((freq, name) => {
      const category = freq.config.category || 'unknown';
      const priority = freq.config.priority || 'unknown';
      
      newByCategory.set(category, (newByCategory.get(category) || 0) + 1);
      newByPriority.set(priority, (newByPriority.get(priority) || 0) + 1);
    });
    
    // Replace the entire Maps instead of modifying them
    subs.byCategory = reactive(newByCategory);
    subs.byPriority = reactive(newByPriority);
  }

  // ORIGINAL: All the methods your components depend on!
  attemptReconnect() {
    if (this.reconnectAttempts < this.maxReconnectAttempts) {
      this.reconnectAttempts++;
      this.monitoring.connectionStats.reconnectAttempts = this.reconnectAttempts;
      console.log(`Attempting to reconnect (${this.reconnectAttempts}/${this.maxReconnectAttempts})`);
      setTimeout(() => this.connect(), this.reconnectInterval);
    } else {
      console.error('Failed to reconnect to dserv. Manual reload required.');
      message.error('Connection lost. Manual reload required.');
      this.emit('connectionFailed', { attempts: this.reconnectAttempts });
    }
  }

  // ORIGINAL: Component registration
  registerComponent(componentId, options = {}) {
    console.log(`Registering component: ${componentId}`);
    
    // Return cleanup function that only removes event handlers
    return () => {
      console.log(`Cleaning up component: ${componentId}`);
      
      // Remove all event handlers for this component
      const componentEvents = this.componentHandlers.get(componentId) || [];
      componentEvents.forEach(({ eventPattern, handler }) => {
        const handlers = this.eventHandlers.get(eventPattern);
        if (handlers) {
          const index = handlers.findIndex(h => h.handler === handler);
          if (index !== -1) {
            handlers.splice(index, 1);
          }
        }
      });
      
      this.componentHandlers.delete(componentId);
    };
  }

  // ORIGINAL: Event system
  on(eventPattern, handler, componentId = null) {
    if (!this.eventHandlers.has(eventPattern)) {
      this.eventHandlers.set(eventPattern, []);
    }
    
    const handlerInfo = { handler, componentId };
    this.eventHandlers.get(eventPattern).push(handlerInfo);
    
    // Track which components are listening to what
    if (componentId) {
      if (!this.componentHandlers.has(componentId)) {
        this.componentHandlers.set(componentId, []);
      }
      this.componentHandlers.get(componentId).push({ eventPattern, handler });
    }
    
    console.log(`Component ${componentId} listening to ${eventPattern}`);
    
    // Return unsubscribe function
    return () => {
      const handlers = this.eventHandlers.get(eventPattern);
      if (handlers) {
        const index = handlers.findIndex(h => h.handler === handler);
        if (index !== -1) {
          handlers.splice(index, 1);
        }
      }
    };
  }

  off(eventName, handler) {
    const handlers = this.eventHandlers.get(eventName);
    if (handlers) {
      const index = handlers.findIndex(h => h.handler === handler);
      if (index !== -1) {
        handlers.splice(index, 1);
      }
    }
  }

  emit(eventName, data) {
    // Direct event emission
    const handlers = this.eventHandlers.get(eventName) || [];
    handlers.forEach(({ handler }) => {
      try {
        handler(data);
      } catch (error) {
        console.error(`Error in event handler for ${eventName}:`, error);
      }
    });
    
    // Pattern matching for datapoint events
    if (eventName.startsWith('datapoint:')) {
      this.eventHandlers.forEach((handlers, pattern) => {
        if (pattern.includes('*') && this.matchPattern(eventName, pattern)) {
          handlers.forEach(({ handler }) => {
            try {
              handler(data);
            } catch (error) {
              console.error(`Error in pattern handler for ${eventName}:`, error);
            }
          });
        }
      });
    }
  }

  matchPattern(eventName, pattern) {
    const regex = new RegExp(pattern.replace(/\*/g, '.*'));
    return regex.test(eventName);
  }

  send(messageObj) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(messageObj));
      this.stats.messagesSent++;
      this.monitoring.messageStats.totalSent++;
    }
  }

  essCommand(command, timeout = 0) {
    const promise = new Promise((resolve, reject) => {
      if (!this.state.connected) {
        return reject(new Error('Not connected to dserv'));
      }
      const requestId = Date.now().toString() + Math.random().toString(36).substr(2, 9);
      this.requestCallbacks.set(requestId, { resolve, reject });
      this.send({ cmd: 'eval', script: command, requestId });
    });

    if (timeout > 0) {
        return withTimeout(promise, timeout, `Command timed out: ${command}`);
    }

    return promise;
  }

  async gitCommand(command) {
    return withTimeout(this.sendRequest('git_command', { command }), 15000, 'Git command timed out');
  }

  async stimCommand(command) {
    return withTimeout(this.sendRequest('stim_command', { command }), 10000, 'Stim command timed out');
  }

  sendRequest(type, data) {
    return new Promise((resolve, reject) => {
      const requestId = Math.random().toString(36).substr(2, 9);
      
      this.requestCallbacks.set(requestId, { resolve, reject });
      
      this.send({
        type,
        requestId,
        ...data
      });
      
      setTimeout(() => {
        if (this.requestCallbacks.has(requestId)) {
          this.requestCallbacks.delete(requestId);
          reject(new Error('Request timeout'));
        }
      }, 30000);
    });
  }

  // ORIGINAL: Helper methods
  parseList(tclList) {
    if (!tclList || tclList === '') return [];
    return tclList.split(' ').filter(item => item.length > 0);
  }

  processEyeWindowSetting(value) {
    const [reg, active, state, type, cx, cy, dx, dy] = value.split(' ').map(Number);
    if (reg >= 0 && reg < 8) {
      this.state.eyeWindows[reg] = {
        id: reg,
        active: active === 1,
        state: state,
        type: type === 1 ? 'ellipse' : 'rectangle',
        center: {
          x: (cx - 2048) / 200.0,
          y: -1 * (cy - 2048) / 200.0
        },
        centerRaw: { x: cx, y: cy },
        size: {
          width: Math.abs(dx / 200.0),
          height: Math.abs(dy / 200.0)
        },
        sizeRaw: { width: dx, height: dy }
      };
    }
  }

  processEyeWindowStatus(value) {
    const [changes, states, adc_x, adc_y] = value.split(' ').map(Number);
    this.state.eyeWindowStatusMask = states;
    for (let i = 0; i < 8; i++) {
      this.state.eyeWindows[i].state = ((states & (1 << i)) !== 0) && this.state.eyeWindows[i].active;
    }
  }

  handleStatusChange(status) {
    if (status === 'loading') {
      console.log('System entering loading state');
      this.emit('systemState', { loading: true });
    } else if (status === 'stopped') {
        // When loading is finished, reset all loading flags
        this.loadingOperations.system = false;
        this.loadingOperations.protocol = false;
        this.loadingOperations.variant = false;
        console.log('System finished loading.');
        this.emit('systemState', { loading: false });
    }
  }

  updateObsCount() {
    if (this.state.obsTotal > 0) {
      this.state.obsCount = `${this.state.obsId + 1}/${this.state.obsTotal}`;
    } else {
      this.state.obsCount = '';
    }
  }

  // NEW: Monitoring API for SystemStatus component
  getSystemStatus() {
    return {
      connection: this.monitoring.connectionStats,
      messages: this.monitoring.messageStats,
      performance: this.monitoring.performance,
      bandwidth: this.monitoring.bandwidth,
      subscriptions: this.monitoring.subscriptions,
      datapoints: Array.from(this.monitoring.datapointFrequencies.entries()).map(([name, data]) => ({
        name,
        ...data,
        health: this.assessDatapointHealth(data)
      }))
    };
  }

  assessDatapointHealth(datapointData) {
    const { currentHz, config } = datapointData;
    const expected = config.expectedHz;
    
    if (expected === 'unknown') return 'unknown';
    
    const ratio = currentHz / expected;
    if (ratio > 2) return 'too_fast';
    if (ratio > 0.8) return 'healthy';
    if (ratio > 0.3) return 'slow';
    return 'stalled';
  }

  getDatapointsByCategory() {
    const categories = new Map();
    
    this.monitoring.datapointFrequencies.forEach((data, name) => {
      const category = data.config.category || 'unknown';
      if (!categories.has(category)) {
        categories.set(category, []);
      }
      categories.get(category).push({ name, ...data });
    });
    
    return categories;
  }

  // ORIGINAL: Performance monitoring methods
  getPerformanceStats() {
    return {
      ...this.stats,
      activeHandlers: Array.from(this.eventHandlers.keys()).length,
      componentCount: this.componentHandlers.size,
      connectionUptime: this.state.connected ? Date.now() - (this.monitoring.connectionStats.connectedSince || Date.now()) : 0
    };
  }

  // Experiment control methods (if these exist in your original)
  async startExperiment() {
    try {
      await this.essCommand('ess::start');
      this.emit('experimentControl', { action: 'start', success: true });
    } catch (error) {
      this.emit('experimentControl', { action: 'start', success: false, error });
      throw error;
    }
  }

  async stopExperiment() {
    try {
      await this.essCommand('ess::stop');
      this.emit('experimentControl', { action: 'stop', success: true });
    } catch (error) {
      this.emit('experimentControl', { action: 'stop', success: false, error });
      throw error;
    }
  }

  async resetExperiment() {
    try {
      await this.essCommand('ess::reset');
      this.emit('experimentControl', { action: 'reset', success: true });
    } catch (error) {
      this.emit('experimentControl', { action: 'reset', success: false, error });
      throw error;
    }
  }

  async setSubject(subject) {
    try {
      await this.essCommand(`ess::set_subject ${subject}`);
      this.emit('subjectChange', { subject, success: true });
    } catch (error) {
      this.emit('subjectChange', { subject, success: false, error });
      throw error;
    }
  }
}

// Create singleton
export const dserv = new DservWebSocket();

// Export monitoring API
export const useDservMonitoring = () => {
  return {
    getSystemStatus: () => dserv.getSystemStatus(),
    getDatapointsByCategory: () => dserv.getDatapointsByCategory(),
    monitoring: dserv.monitoring // Direct reference instead of computed
  }
};
