import { reactive, ref } from 'vue';
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

// Throttle helper for high-frequency data
function throttle(func, delay) {
  let timeoutId;
  let lastExecTime = 0;
  return function (...args) {
    const currentTime = Date.now();
    
    if (currentTime - lastExecTime > delay) {
      func.apply(this, args);
      lastExecTime = currentTime;
    } else {
      clearTimeout(timeoutId);
      timeoutId = setTimeout(() => {
        func.apply(this, args);
        lastExecTime = Date.now();
      }, delay - (currentTime - lastExecTime));
    }
  };
}

class DservWebSocket {
  constructor() {
    this.ws = null;
    this.reconnectAttempts = 0;
    this.maxReconnectAttempts = 5;
    this.reconnectInterval = 2000;
    this.requestCallbacks = new Map();
    
    // Event system for component-specific handlers
    this.eventHandlers = new Map();
    this.globalEventHandlers = [];
    
    // Performance monitoring
    this.stats = reactive({
      messagesReceived: 0,
      messagesSent: 0,
      avgProcessingTime: 0,
      lastMessageTime: 0
    });

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
    });

    this.loadingOperations = reactive({
      system: false,
      protocol: false,
      variant: false,
    });

    // Active subscriptions tracking
    this.activeSubscriptions = new Set();
    
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
      this.reconnectAttempts = 0;
      
      // Re-establish subscriptions
      this.reestablishSubscriptions();
      
      this.initializeState();
      this.emit('connection', { connected: true });
    };

    this.ws.onmessage = (event) => {
      const startTime = performance.now();
      this.stats.messagesReceived++;
      
      try {
        const data = JSON.parse(event.data);
        this.handleMessage(data);
        
        // Update performance stats
        const processingTime = performance.now() - startTime;
        this.stats.avgProcessingTime = (this.stats.avgProcessingTime * 0.9) + (processingTime * 0.1);
        this.stats.lastMessageTime = Date.now();
      } catch (error) {
        console.error('Failed to parse WebSocket message:', error, 'Raw data:', event.data);
      }
    };

    this.ws.onclose = () => {
      this.state.connected = false;
      this.emit('connection', { connected: false });
      this.attemptReconnect();
    };

    this.ws.onerror = (error) => {
      console.error('WebSocket error:', error);
      this.emit('error', { error });
    };
  }

  // Event system methods
  on(eventName, handler, options = {}) {
    if (!this.eventHandlers.has(eventName)) {
      this.eventHandlers.set(eventName, []);
    }
    
    const wrappedHandler = options.throttle ? 
      throttle(handler, options.throttle) : handler;
    
    const handlerInfo = {
      original: handler,
      wrapped: wrappedHandler,
      once: options.once || false,
      pattern: options.pattern // For pattern matching
    };
    
    this.eventHandlers.get(eventName).push(handlerInfo);
    
    // Return unsubscribe function
    return () => this.off(eventName, handler);
  }

  off(eventName, handler) {
    const handlers = this.eventHandlers.get(eventName);
    if (handlers) {
      const index = handlers.findIndex(h => h.original === handler);
      if (index !== -1) {
        handlers.splice(index, 1);
      }
    }
  }

  emit(eventName, data) {
    // Handle pattern-based datapoint events
    if (eventName.startsWith('datapoint:')) {
      const datapointName = data.name;
      this.eventHandlers.forEach((handlers, pattern) => {
        if (pattern.includes('*') && this.matchPattern(datapointName, pattern)) {
          handlers.forEach(handlerInfo => {
            handlerInfo.wrapped(data);
            if (handlerInfo.once) {
              this.off(pattern, handlerInfo.original);
            }
          });
        }
      });
    }
    
    // Handle specific events
    const handlers = this.eventHandlers.get(eventName);
    if (handlers) {
      handlers.forEach(handlerInfo => {
        handlerInfo.wrapped(data);
        if (handlerInfo.once) {
          this.off(eventName, handlerInfo.original);
        }
      });
    }
    
    // Global handlers
    this.globalEventHandlers.forEach(handler => handler(eventName, data));
  }

  matchPattern(name, pattern) {
    const regex = new RegExp(pattern.replace(/\*/g, '.*'));
    return regex.test(name);
  }

  // Improved subscription management
  subscribe(pattern, every = 1, componentId = null) {
    const subscription = { pattern, every, componentId };
    this.activeSubscriptions.add(subscription);
    this.send({ cmd: 'subscribe', match: pattern, every });
    
    console.log(`Subscribed to ${pattern} (every ${every}) for ${componentId || 'global'}`);
    return () => this.unsubscribe(pattern, componentId);
  }

  unsubscribe(pattern, componentId = null) {
    // Find and remove subscription
    for (const sub of this.activeSubscriptions) {
      if (sub.pattern === pattern && sub.componentId === componentId) {
        this.activeSubscriptions.delete(sub);
        this.send({ cmd: 'unsubscribe', match: pattern });
        console.log(`Unsubscribed from ${pattern} for ${componentId || 'global'}`);
        break;
      }
    }
  }

  reestablishSubscriptions() {
    console.log('Re-establishing subscriptions...');
    // Core subscriptions
    this.subscribe('ess/*');
    this.subscribe('system/*');
    this.subscribe('stimdg');
    this.subscribe('trialdg');
    this.subscribe('openiris/settings');
    this.subscribe('print');
    
    // Re-establish component subscriptions
    this.activeSubscriptions.forEach(sub => {
      if (sub.componentId) {
        this.send({ cmd: 'subscribe', match: sub.pattern, every: sub.every });
      }
    });
  }

  send(messageObj) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(messageObj));
      this.stats.messagesSent++;
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

  handleDatapoint(data) {
    const { name, data: value } = data;
    
    // Update central state for core datapoints
    this.updateCentralState(name, value);
    
    // Emit datapoint event for component handlers
    this.emit(`datapoint:${name}`, data);
    this.emit('datapoint:*', data);
  }

  updateCentralState(name, value) {
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
      case 'system/hostname': this.state.systemName = value; break;
      case 'system/os': this.state.systemOS = value; break;
      case 'print': console.log('ESS:', value); break;
      // Component-specific data is handled by event system, not stored centrally
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

  parseList(tclList) {
    if (!tclList || tclList === '') return [];
    return tclList.split(' ').filter(item => item.length > 0);
  }

  updateObsCount() {
    if (this.state.obsTotal > 0) {
      this.state.obsCount = `${this.state.obsId + 1}/${this.state.obsTotal}`;
    } else {
      this.state.obsCount = '';
    }
  }

  attemptReconnect() {
    if (this.reconnectAttempts < this.maxReconnectAttempts) {
      this.reconnectAttempts++;
      console.log(`Attempting to reconnect (${this.reconnectAttempts}/${this.maxReconnectAttempts})`);
      setTimeout(() => this.connect(), this.reconnectInterval);
    } else {
      console.error('Failed to reconnect to dserv. Manual reload required.');
      this.emit('connectionFailed', { attempts: this.reconnectAttempts });
    }
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
          ess/obs_id ess/obs_total
          ess/block_pct_complete ess/block_pct_correct
          ess/git/branches ess/git/branch
          ess/system_script ess/protocol_script
          ess/variants_script ess/loaders_script
	  ess/stim_script          
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

  // Component lifecycle helpers
  registerComponent(componentId, config = {}) {
    console.log(`Registering component: ${componentId}`);
    
    // Component-specific subscriptions
    if (config.subscriptions) {
      config.subscriptions.forEach(sub => {
        this.subscribe(sub.pattern, sub.every || 1, componentId);
      });
    }
    
    // Return cleanup function
    return () => {
      console.log(`Unregistering component: ${componentId}`);
      // Remove component subscriptions
      const toRemove = [];
      this.activeSubscriptions.forEach(sub => {
        if (sub.componentId === componentId) {
          toRemove.push(sub);
        }
      });
      toRemove.forEach(sub => this.unsubscribe(sub.pattern, componentId));
      
      // Remove event handlers
      this.eventHandlers.forEach((handlers, eventName) => {
        this.eventHandlers.set(eventName, 
          handlers.filter(h => h.componentId !== componentId)
        );
      });
    };
  }

  // Performance monitoring
  getPerformanceStats() {
    return {
      ...this.stats,
      activeSubscriptions: this.activeSubscriptions.size,
      eventHandlers: Array.from(this.eventHandlers.keys()).length,
      connectionUptime: this.state.connected ? Date.now() - this.stats.lastMessageTime : 0
    };
  }

    // Git command wrapper (matching essgui.cxx git_eval pattern)
    async gitCommand(command, timeout = 0) {
	const fullCommand = `send git {${command}}`
	return this.essCommand(fullCommand, timeout)
    }

    // Additional git helpers
    async gitPull() {
	return this.gitCommand('git::pull')
    }
    
    async gitPush() {
	return this.gitCommand('git::commit_and_push')
    }
    
    async gitStatus() {
	return this.gitCommand('git::status')
    }
    
    // System control methods remain the same but with event emissions
    async setSystem(system) {
    this.loadingOperations.system = true;
    this.emit('systemChange', { type: 'system', value: system, loading: true });
    try {
      await this.essCommand(`ess::load_system ${system}`, 30000);
      this.emit('systemChange', { type: 'system', value: system, loading: false, success: true });
    } catch (error) {
      this.loadingOperations.system = false;
      this.emit('systemChange', { type: 'system', value: system, loading: false, success: false, error });
      throw error;
    }
  }

  async setProtocol(protocol) {
    this.loadingOperations.protocol = true;
    this.emit('systemChange', { type: 'protocol', value: protocol, loading: true });
    try {
      await this.essCommand(`ess::load_system ${this.state.currentSystem} ${protocol}`, 30000);
      this.emit('systemChange', { type: 'protocol', value: protocol, loading: false, success: true });
    } catch (error) {
      this.loadingOperations.protocol = false;
      this.emit('systemChange', { type: 'protocol', value: protocol, loading: false, success: false, error });
      throw error;
    }
  }

  async setVariant(variant) {
    this.loadingOperations.variant = true;
    this.emit('systemChange', { type: 'variant', value: variant, loading: true });
    try {
      await this.essCommand(
        `ess::load_system ${this.state.currentSystem} ${this.state.currentProtocol} ${variant}`,
        30000
      );
      this.emit('systemChange', { type: 'variant', value: variant, loading: false, success: true });
    } catch (error) {
      this.loadingOperations.variant = false;
      this.emit('systemChange', { type: 'variant', value: variant, loading: false, success: false, error });
      throw error;
    }
  }

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

export const dserv = new DservWebSocket();
