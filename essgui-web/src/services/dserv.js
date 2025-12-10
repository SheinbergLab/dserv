// Complete dserv.js - Updated for degrees-based eye tracking
import { reactive, ref, computed } from 'vue';
import { message } from 'ant-design-vue';

function withTimeout(promise, ms, errorMessage = 'Operation timed out') {
  const timeout = new Promise((_, reject) => {
    const id = setTimeout(() => { clearTimeout(id); reject(new Error(errorMessage)); }, ms);
  });
  return Promise.race([promise, timeout]);
}

const DATAPOINT_CONFIG = {
  'ess/em_pos': { expectedHz: 60, maxHz: 120, priority: 'high', category: 'eye_tracking' },
  'ess/em_region_status': { expectedHz: 30, maxHz: 60, priority: 'high', category: 'eye_tracking' },
  'ess/touch_region_status': { expectedHz: 30, maxHz: 60, priority: 'high', category: 'touch_tracking' },
  'mtouch/event': { expectedHz: 60, maxHz: 120, priority: 'high', category: 'touch_tracking' },
  'ess/block_pct_complete': { expectedHz: 2, maxHz: 10, priority: 'medium', category: 'performance' },
  'ess/block_pct_correct': { expectedHz: 2, maxHz: 10, priority: 'medium', category: 'performance' },
  'ess/obs_id': { expectedHz: 0.1, maxHz: 1, priority: 'medium', category: 'experiment' },
  'ess/system': { expectedHz: 0.01, maxHz: 0.1, priority: 'low', category: 'configuration' },
  'ess/protocol': { expectedHz: 0.01, maxHz: 0.1, priority: 'low', category: 'configuration' },
  'ess/variant': { expectedHz: 0.01, maxHz: 0.1, priority: 'low', category: 'configuration' },
  'ess/status': { expectedHz: 0.1, maxHz: 1, priority: 'low', category: 'state' },
  'graphics/main': { expectedHz: 30, maxHz: 60, priority: 'medium', category: 'graphics' },
  'graphics/dev': { expectedHz: 30, maxHz: 60, priority: 'medium', category: 'graphics' },
  'gbuf/main': { expectedHz: 30, maxHz: 60, priority: 'medium', category: 'graphics' },
  'gbuf/dev': { expectedHz: 30, maxHz: 60, priority: 'medium', category: 'graphics' },
};

class DservWebSocket {
  constructor() {
    this.ws = null;
    this.reconnectAttempts = 0;
    this.maxReconnectAttempts = 5;
    this.reconnectInterval = 2000;
    this.requestCallbacks = new Map();
    this.messageChunks = new Map();
    this.chunkTimeouts = new Map();
    this.eventHandlers = new Map();
    this.componentHandlers = new Map();

    this.stats = reactive({
      messagesReceived: 0,
      messagesSent: 0,
      avgProcessingTime: 0,
      lastMessageTime: 0
    });

    this.monitoring = reactive({
      connectionStats: { connected: false, connectedSince: null, reconnectAttempts: 0, uptime: 0 },
      messageStats: { totalReceived: 0, totalSent: 0, messagesPerSecond: 0, bytesPerSecond: 0, lastSecondCount: 0, lastSecondBytes: 0 },
      datapointFrequencies: reactive(new Map()),
      performance: { avgProcessingTime: 0, maxProcessingTime: 0, processedThisSecond: 0 },
      subscriptions: { active: 0, byCategory: reactive(new Map()), byPriority: reactive(new Map()), components: 0 },
      bandwidth: { currentKbps: 0, peakKbps: 0, averageKbps: 0, samples: [] }
    });

    // Central state - eye windows now store values directly in degrees
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
      pointsPerDegX: 8.0,  // Kept for backward compatibility
      pointsPerDegY: 8.0,
      // Eye windows - now in degrees (matching FLTK EyeRegion)
      eyeWindows: Array(8).fill(null).map((_, i) => ({
        id: i,
        active: false,
        state: 0,
        type: 'rectangle',
        center: { x: 0, y: 0 },     // degrees
        size: { width: 2, height: 2 } // half-width/half-height in degrees (plusminus)
      })),
      eyeWindowStatusMask: 0,
      // Touch windows - still in pixels, converted to degrees in component
      touchWindows: Array(8).fill(null).map((_, i) => ({
        id: i,
        active: false,
        state: 0,
        type: 'rectangle',
        center: { x: 0, y: 0 },
        centerRaw: { x: 400, y: 320 },
        size: { width: 2, height: 2 },
        sizeRaw: { width: 100, height: 100 }
      })),
      touchWindowStatusMask: 0,
      screenWidth: 800,
      screenHeight: 600,
      screenHalfX: 10.0,
      screenHalfY: 7.5,
    });

    this.loadingState = reactive({
      isLoading: false, operationId: null, startTime: null, stage: '', message: '',
      percent: 0, elapsed: 0, timeout: false, error: null
    });

    this.loadingTimer = null;
    this.loadingWatchdog = null;
    this.LOADING_TIMEOUT = 60000;
    this.PROGRESS_TIMEOUT = 15000;
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
      this.establishGlobalSubscriptions();
      this.initializeState();
      this.emit('connection', { connected: true });
    };

    this.ws.onmessage = (event) => {
      const startTime = performance.now();
      this.stats.messagesReceived++;
      this.monitoring.messageStats.totalReceived++;
      try {
        const data = JSON.parse(event.data);
        if (data.isChunkedMessage) {
          this.handleChunkedMessage(data);
          return;
        }
        this.handleMessage(data);
        const processingTime = performance.now() - startTime;
        this.stats.avgProcessingTime = (this.stats.avgProcessingTime * 0.9) + (processingTime * 0.1);
      } catch (error) {
        console.error('Failed to parse WebSocket message:', error);
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

  handleChunkedMessage(chunk) {
    const { messageId, chunkIndex, totalChunks, data } = chunk;
    if (!this.messageChunks.has(messageId)) {
      this.messageChunks.set(messageId, { chunks: new Array(totalChunks), receivedCount: 0, totalChunks, startTime: Date.now() });
      console.log(`Receiving chunked message ${messageId} (${totalChunks} chunks)`);
      setTimeout(() => {
        if (this.messageChunks.has(messageId)) {
          const message = this.messageChunks.get(messageId);
          if (message.receivedCount < totalChunks) {
            console.warn(`Cleaning up incomplete chunked message ${messageId}`);
            this.messageChunks.delete(messageId);
          }
        }
      }, 30000);
    }
    const message = this.messageChunks.get(messageId);
    message.chunks[chunkIndex] = data;
    message.receivedCount++;
    if (message.receivedCount === totalChunks) {
      const totalTime = Date.now() - message.startTime;
      console.log(`All chunks received for ${messageId}, reassembling... (${totalTime}ms)`);
      const completeData = message.chunks.join('');
      this.messageChunks.delete(messageId);
      try {
        const parsedData = JSON.parse(completeData);
        console.log(`Successfully reassembled message: ${(completeData.length/1024).toFixed(1)}KB in ${totalTime}ms`);
        this.handleMessage(parsedData);
      } catch (error) {
        console.error('Failed to parse reassembled chunked message:', error);
      }
    }
  }

  establishGlobalSubscriptions() {
    console.log('Establishing global subscriptions...');
    this.send({ cmd: 'subscribe', match: 'ess/*', every: 1 });
    this.send({ cmd: 'subscribe', match: 'system/*', every: 1 });
    this.send({ cmd: 'subscribe', match: 'eventlog/*', every: 1 });
    this.send({ cmd: 'subscribe', match: 'ess/em_pos', every: 1 });
    this.send({ cmd: 'subscribe', match: 'ess/em_region_setting', every: 1 });
    this.send({ cmd: 'subscribe', match: 'ess/em_region_status', every: 1 });
    this.send({ cmd: 'subscribe', match: 'ess/touch_region_setting', every: 1 });
    this.send({ cmd: 'subscribe', match: 'ess/touch_region_status', every: 1 });
    this.send({ cmd: 'subscribe', match: 'mtouch/event', every: 1 });
    this.send({ cmd: 'subscribe', match: 'openiris/settings', every: 1 });
    this.send({ cmd: 'subscribe', match: 'em/settings', every: 1 });
    this.send({ cmd: 'subscribe', match: 'ess/warningInfo', every: 1 });
    this.send({ cmd: 'subscribe', match: 'ess/infoLog', every: 1 });
    this.send({ cmd: 'subscribe', match: 'ess/debugLog', every: 1 });
    this.send({ cmd: 'subscribe', match: 'ess/generalLog', every: 1 });
    this.send({ cmd: 'subscribe', match: 'print', every: 1 });
    this.send({ cmd: 'subscribe', match: 'ess/loading_progress', every: 1 });
    this.send({ cmd: 'subscribe', match: 'ess/loading_operation_id', every: 1 });
    this.send({ cmd: 'subscribe', match: 'ess/loading_start_time', every: 1 });
    this.send({ cmd: 'subscribe', match: 'graphics/*', every: 1 });
    this.send({ cmd: 'subscribe', match: 'gbuf/*', every: 1 });
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
          ess/params ess/viz_config
          ess/stim_script ess/rmt_connected
          em/settings
          ess/screen_w ess/screen_h ess/screen_halfx ess/screen_halfy
          system/hostname system/os
        } {
          catch { dservTouch $v }
        }
        for {set i 0} {$i < 8} {incr i} {
          catch { ainGetRegionInfo $i }
          catch { touchGetRegionInfo $i }
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

  handleMessage(data) {
    if (data.requestId && this.requestCallbacks.has(data.requestId)) {
      const { resolve, reject } = this.requestCallbacks.get(data.requestId);
      this.requestCallbacks.delete(data.requestId);
      if (data.status === 'ok') resolve(data.result);
      else reject(new Error(data.error || 'Command failed'));
      return;
    }
    if (data.type === 'datapoint') this.handleDatapoint(data);
  }

  handleDatapoint(data) {
    const { name, data: value } = data;
    this.updateCentralState(name, value);
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
      case 'ess/status': this.state.essStatus = value; this.handleEssStatus(value); break;
      case 'ess/state': this.state.status = value; break;
      case 'ess/obs_id': this.state.obsId = parseInt(value, 10) || 0; this.updateObsCount(); break;
      case 'ess/obs_total': this.state.obsTotal = parseInt(value, 10) || 0; this.updateObsCount(); break;
      case 'ess/in_obs': this.state.inObs = value === '1'; break;
      case 'em/settings': this.updateEyeTrackingSettings(value); break;
      case 'ess/block_pct_complete': this.state.blockPctComplete = Math.round(parseFloat(value) * 100); break;
      case 'ess/block_pct_correct': this.state.blockPctCorrect = Math.round(parseFloat(value) * 100); break;
      case 'ess/subject': this.state.subject = value; break;
      case 'ess/em_region_setting':
        this.processEyeWindowSetting(value);
        break;
      case 'ess/em_region_status': this.processEyeWindowStatus(value); break;
      case 'ess/touch_region_setting': this.processTouchWindowSetting(value); break;
      case 'ess/touch_region_status': this.processTouchWindowStatus(value); break;
      case 'ess/screen_w': this.state.screenWidth = parseInt(value, 10) || 800; break;
      case 'ess/screen_h': this.state.screenHeight = parseInt(value, 10) || 600; break;
      case 'ess/screen_halfx': this.state.screenHalfX = parseFloat(value) || 10.0; break;
      case 'ess/screen_halfy': this.state.screenHalfY = parseFloat(value) || 7.5; break;
      case 'ess/stiminfo': this.state.stiminfo = value; break;
      case 'system/hostname': this.state.systemName = value; break;
      case 'system/os': this.state.systemOS = value; break;
      case 'print': console.log('ESS:', value); break;
      case 'ess/loading_progress': this.handleLoadingProgress(value); break;
      case 'ess/loading_operation_id': this.handleLoadingOperationId(value); break;
    }
  }

  // Eye window setting - values now come directly in degrees
  // Format: "reg active state type center_x center_y plusminus_x plusminus_y"
  // Matching FLTK's eye_region_set(int win, int active, int state, int type, float cx, float cy, float pmx, float pmy)
  processEyeWindowSetting(value) {
    // Split on whitespace and filter out empty strings (handles multiple spaces)
    const parts = value.split(/\s+/).filter(p => p.length > 0);
    if (parts.length < 8) {
      console.warn('Insufficient eye window setting data:', value, 'parsed as:', parts);
      return;
    }

    // Parse: reg, active, state, type as ints; cx, cy, pmx, pmy as floats
    const reg = parseInt(parts[0], 10);
    const active = parseInt(parts[1], 10);
    const state = parseInt(parts[2], 10);
    const type = parseInt(parts[3], 10);
    const cx = parseFloat(parts[4]);
    const cy = parseFloat(parts[5]);
    const pmx = parseFloat(parts[6]);
    const pmy = parseFloat(parts[7]);

    console.log(`Eye window ${reg}: active=${active}, type=${type}, center=(${cx}, ${cy}), pm=(${pmx}, ${pmy})`);

    if (reg >= 0 && reg < 8 && !isNaN(cx) && !isNaN(cy) && !isNaN(pmx) && !isNaN(pmy)) {
      this.state.eyeWindows[reg] = {
        id: reg,
        active: active === 1,
        state: state,
        type: type === 1 ? 'ellipse' : 'rectangle',
        // Center position in degrees (directly from backend)
        center: { x: cx, y: cy },
        // Size in degrees (plusminus_x and plusminus_y from backend)
        size: { width: Math.abs(pmx), height: Math.abs(pmy) }
      };
    }
  }

  processEyeWindowStatus(value) {
    const parts = value.split(' ').map(Number);
    if (parts.length < 2) return;
    const [changes, states] = parts;
    this.state.eyeWindowStatusMask = states;
    for (let i = 0; i < 8; i++) {
      this.state.eyeWindows[i].state = ((states & (1 << i)) !== 0) && this.state.eyeWindows[i].active;
    }
  }

  processTouchWindowSetting(value) {
    const [reg, active, state, type, cx, cy, dx, dy] = value.split(' ').map(Number);
    if (reg >= 0 && reg < 8) {
      const screenPixPerDegX = this.state.screenWidth / (2 * this.state.screenHalfX);
      const screenPixPerDegY = this.state.screenHeight / (2 * this.state.screenHalfY);
      this.state.touchWindows[reg] = {
        id: reg,
        active: active === 1,
        state: state,
        type: type === 1 ? 'ellipse' : 'rectangle',
        center: {
          x: (cx - this.state.screenWidth / 2) / screenPixPerDegX,
          y: -1 * (cy - this.state.screenHeight / 2) / screenPixPerDegY
        },
        centerRaw: { x: cx, y: cy },
        size: {
          width: Math.abs(dx / screenPixPerDegX),
          height: Math.abs(dy / screenPixPerDegY)
        },
        sizeRaw: { width: dx, height: dy }
      };
    }
  }

  processTouchWindowStatus(value) {
    const [changes, states] = value.split(' ').map(Number);
    this.state.touchWindowStatusMask = states;
    for (let i = 0; i < 8; i++) {
      this.state.touchWindows[i].state = ((states & (1 << i)) !== 0) && this.state.touchWindows[i].active;
    }
  }

  updateEyeTrackingSettings(value) {
    try {
      // Parse Tcl dict format: "key1 value1 key2 value2 ..."
      const parts = value.split(/\s+/);
      const settings = {};
      for (let i = 0; i < parts.length - 1; i += 2) {
        settings[parts[i]] = parts[i + 1];
      }

      if (settings.scale_h !== undefined) this.state.scaleH = parseFloat(settings.scale_h);
      if (settings.scale_v !== undefined) this.state.scaleV = parseFloat(settings.scale_v);
      console.log(`Eye tracking settings updated: scale_h=${settings.scale_h}, scale_v=${settings.scale_v}`);
    } catch (error) {
      console.error('Failed to parse em/settings:', error);
    }
  }

  handleLoadingProgress(value) {
    if (!value) { this.clearLoadingState(); return; }
    try {
      const progress = JSON.parse(value);
      const elapsed = Date.now() - (this.loadingState.startTime || Date.now());
      Object.assign(this.loadingState, {
        stage: progress.stage || this.loadingState.stage,
        message: progress.message || this.loadingState.message,
        percent: progress.percent || this.loadingState.percent,
        elapsed
      });
      this.resetProgressWatchdog();
      this.emit('loadingProgress', this.loadingState);
      if (progress.stage === 'complete' || progress.percent >= 100) {
        setTimeout(() => this.clearLoadingState(), 500);
      }
    } catch (error) {
      console.error('Failed to parse loading progress:', error);
    }
  }

  handleLoadingOperationId(operationId) {
    if (!operationId) { this.clearLoadingState(); return; }
    if (!this.loadingState.isLoading) {
      Object.assign(this.loadingState, {
        isLoading: true, operationId, startTime: Date.now(), stage: 'initializing',
        message: 'Loading...', percent: 5, elapsed: 0, timeout: false, error: null
      });
      this.emit('loadingStarted', { operationId });
    }
    this.resetProgressWatchdog();
  }

  handleEssStatus(status) {
    if ((status === 'stopped' || status === 'ready') && this.loadingState.isLoading) {
      setTimeout(() => this.clearLoadingState(), 300);
    }
  }

  resetProgressWatchdog() {
    if (this.loadingWatchdog) clearTimeout(this.loadingWatchdog);
    this.loadingWatchdog = setTimeout(() => {
      if (this.loadingState.isLoading) this.checkBackendLoadingStatus();
    }, this.PROGRESS_TIMEOUT);
  }

  handleLoadingTimeout() {
    this.loadingState.timeout = true;
    this.loadingState.error = 'Operation timed out';
    this.emit('loadingTimeout', this.loadingState);
    this.forceLoadingRecovery();
  }

  async checkBackendLoadingStatus() {
    try {
      const status = await withTimeout(this.essCommand('dservGet ess/status'), 5000, 'Status check timeout');
      if (status !== 'loading') this.clearLoadingState();
      else this.resetProgressWatchdog();
    } catch (error) {
      this.forceLoadingRecovery();
    }
  }

  async forceLoadingRecovery() {
    try {
      await withTimeout(this.essCommand('dservSet ess/status stopped'), 3000, 'Recovery timeout');
      this.clearLoadingState();
      await this.essCommand('dservTouch ess/system');
      await this.essCommand('dservTouch ess/protocol');
      await this.essCommand('dservTouch ess/variant');
      this.emit('loadingRecovered', { forced: true });
    } catch (error) {
      this.emit('loadingFailed', { error: 'Recovery failed' });
    }
  }

  clearLoadingState() {
    if (this.loadingTimer) { clearTimeout(this.loadingTimer); this.loadingTimer = null; }
    if (this.loadingWatchdog) { clearTimeout(this.loadingWatchdog); this.loadingWatchdog = null; }
    const wasLoading = this.loadingState.isLoading;
    Object.assign(this.loadingState, {
      isLoading: false, operationId: null, startTime: null, stage: '', message: '',
      percent: 0, elapsed: 0, timeout: false, error: null
    });
    if (wasLoading) this.emit('loadingFinished', this.loadingState);
  }

  startMonitoring() {
    setInterval(() => this.updateSecondlyStats(), 1000);
    setInterval(() => {
      if (this.monitoring.connectionStats.connected && this.monitoring.connectionStats.connectedSince) {
        this.monitoring.connectionStats.uptime = Date.now() - this.monitoring.connectionStats.connectedSince;
      }
    }, 100);
  }

  updateSecondlyStats() {
    const stats = this.monitoring.messageStats;
    const bandwidth = this.monitoring.bandwidth;
    stats.messagesPerSecond = stats.lastSecondCount;
    stats.bytesPerSecond = stats.lastSecondBytes;
    const currentKbps = (stats.lastSecondBytes * 8) / 1024;
    bandwidth.currentKbps = currentKbps;
    bandwidth.peakKbps = Math.max(bandwidth.peakKbps, currentKbps);
    bandwidth.samples.push(currentKbps);
    if (bandwidth.samples.length > 60) bandwidth.samples.shift();
    bandwidth.averageKbps = bandwidth.samples.reduce((a, b) => a + b, 0) / bandwidth.samples.length;
    stats.lastSecondCount = 0;
    stats.lastSecondBytes = 0;
    this.updateSubscriptionStats();
  }

  updateSubscriptionStats() {
    const subs = this.monitoring.subscriptions;
    subs.active = this.eventHandlers.size;
    subs.components = this.componentHandlers.size;
    const newByCategory = new Map();
    const newByPriority = new Map();
    this.monitoring.datapointFrequencies.forEach((freq, name) => {
      const category = freq.config.category || 'unknown';
      const priority = freq.config.priority || 'unknown';
      newByCategory.set(category, (newByCategory.get(category) || 0) + 1);
      newByPriority.set(priority, (newByPriority.get(priority) || 0) + 1);
    });
    subs.byCategory = reactive(newByCategory);
    subs.byPriority = reactive(newByPriority);
  }

  attemptReconnect() {
    if (this.reconnectAttempts < this.maxReconnectAttempts) {
      this.reconnectAttempts++;
      this.monitoring.connectionStats.reconnectAttempts = this.reconnectAttempts;
      setTimeout(() => this.connect(), this.reconnectInterval);
    } else {
      message.error('Connection lost. Manual reload required.');
      this.emit('connectionFailed', { attempts: this.reconnectAttempts });
    }
  }

  registerComponent(componentId) {
    return () => {
      const componentEvents = this.componentHandlers.get(componentId) || [];
      componentEvents.forEach(({ eventPattern, handler }) => this.off(eventPattern, handler));
      this.componentHandlers.delete(componentId);
    };
  }

  on(eventPattern, handler, componentId = null) {
    if (!this.eventHandlers.has(eventPattern)) this.eventHandlers.set(eventPattern, []);
    this.eventHandlers.get(eventPattern).push({ handler, componentId });
    if (componentId) {
      if (!this.componentHandlers.has(componentId)) this.componentHandlers.set(componentId, []);
      this.componentHandlers.get(componentId).push({ eventPattern, handler });
    }
    return () => this.off(eventPattern, handler);
  }

  off(eventPattern, handler) {
    const handlers = this.eventHandlers.get(eventPattern);
    if (handlers) {
      const index = handlers.findIndex(h => h.handler === handler);
      if (index !== -1) handlers.splice(index, 1);
    }
  }

  emit(eventName, data) {
    const handlers = this.eventHandlers.get(eventName) || [];
    handlers.forEach(({ handler }) => { try { handler(data); } catch (error) { console.error(`Error in handler for ${eventName}:`, error); } });
    if (eventName.startsWith('datapoint:')) {
      this.eventHandlers.forEach((handlers, pattern) => {
        if (pattern.includes('*') && this.matchPattern(eventName, pattern)) {
          handlers.forEach(({ handler }) => { try { handler(data); } catch (error) { console.error(`Error in pattern handler:`, error); } });
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
      if (!this.state.connected) return reject(new Error('Not connected to dserv'));
      const requestId = Date.now().toString() + Math.random().toString(36).substr(2, 9);
      this.requestCallbacks.set(requestId, { resolve, reject });
      this.send({ cmd: 'eval', script: command, requestId });
    });
    if (timeout > 0) return withTimeout(promise, timeout, `Command timed out: ${command}`);
    return promise;
  }

  async gitCommand(command, timeout = 15000) {
    return withTimeout(this.essCommand(`send git {${command}}`), timeout, 'Git command timed out');
  }

  async gitPull() { return this.gitCommand('git::pull'); }
  async gitPush() { return this.gitCommand('git::commit_and_push'); }
  async gitStatus() { return this.gitCommand('git::status'); }
  async gitSwitchBranch(branchName) { return this.gitCommand(`git::switch_and_pull ${branchName}`); }
  async gitGetBranches() { return this.gitCommand('git::get_branches'); }
  async gitGetCurrentBranch() { return this.gitCommand('git::get_current_branch'); }

  parseList(tclList) {
    if (!tclList || tclList === '') return [];
    return tclList.split(' ').filter(item => item.length > 0);
  }

  updateObsCount() {
    if (this.state.obsTotal > 0) this.state.obsCount = `${this.state.obsId + 1}/${this.state.obsTotal}`;
    else this.state.obsCount = '';
  }

  getSystemStatus() {
    return {
      connection: this.monitoring.connectionStats,
      messages: this.monitoring.messageStats,
      performance: this.monitoring.performance,
      bandwidth: this.monitoring.bandwidth,
      subscriptions: this.monitoring.subscriptions,
      datapoints: Array.from(this.monitoring.datapointFrequencies.entries()).map(([name, data]) => ({
        name, ...data, health: this.assessDatapointHealth(data)
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
      if (!categories.has(category)) categories.set(category, []);
      categories.get(category).push({ name, ...data });
    });
    return categories;
  }

  getPerformanceStats() {
    return {
      ...this.stats,
      activeHandlers: Array.from(this.eventHandlers.keys()).length,
      componentCount: this.componentHandlers.size,
      connectionUptime: this.state.connected ? Date.now() - (this.monitoring.connectionStats.connectedSince || Date.now()) : 0
    };
  }

  async startExperiment() { await this.essCommand('ess::start'); this.emit('experimentControl', { action: 'start', success: true }); }
  async stopExperiment() { await this.essCommand('ess::stop'); this.emit('experimentControl', { action: 'stop', success: true }); }
  async resetExperiment() { await this.essCommand('ess::reset'); this.emit('experimentControl', { action: 'reset', success: true }); }
  async setSubject(subject) { await this.essCommand(`ess::set_subject ${subject}`); this.emit('subjectChange', { subject, success: true }); }

  async setSystem(system) {
    this.startImmediateLoadingState('system', `Loading system: ${system}`);
    try { await this.essCommand(`ess::load_system ${system}`); this.emit('systemLoaded', { system, success: true }); }
    catch (error) { this.clearLoadingState(); this.emit('systemLoaded', { system, success: false, error }); throw error; }
  }

  async setProtocol(protocol) {
    this.startImmediateLoadingState('protocol', `Loading protocol: ${protocol}`);
    try { await this.essCommand(`ess::load_system ${this.state.currentSystem} ${protocol}`); this.emit('protocolLoaded', { protocol, success: true }); }
    catch (error) { this.clearLoadingState(); this.emit('protocolLoaded', { protocol, success: false, error }); throw error; }
  }

  async setVariant(variant) {
    this.startImmediateLoadingState('variant', `Loading variant: ${variant}`);
    try { await this.essCommand(`ess::load_system ${this.state.currentSystem} ${this.state.currentProtocol} ${variant}`); this.emit('variantLoaded', { variant, success: true }); }
    catch (error) { this.clearLoadingState(); this.emit('variantLoaded', { variant, success: false, error }); throw error; }
  }

  startImmediateLoadingState(operationType, msg) {
    Object.assign(this.loadingState, {
      isLoading: true, operationId: `${operationType}_${Date.now()}`, startTime: Date.now(),
      stage: 'initializing', message: msg, percent: 5, elapsed: 0, timeout: false, error: null
    });
    this.loadingTimer = setTimeout(() => this.handleLoadingTimeout(), this.LOADING_TIMEOUT);
    this.resetProgressWatchdog();
    this.emit('loadingStarted', { operationId: this.loadingState.operationId });
    this.emit('loadingProgress', this.loadingState);
  }

  async reloadSystem() {
    this.startImmediateLoadingState('system', `Loading system: ${this.state.currentSystem}`);
    try { await this.essCommand('ess::reload_system'); this.emit('systemLoaded', { system: this.state.currentSystem, success: true }); }
    catch (error) { this.clearLoadingState(); throw error; }
  }

  async reloadProtocol() {
    this.startImmediateLoadingState('protocol_reload', 'Reloading protocol...');
    try { await this.essCommand('ess::reload_protocol'); this.emit('protocolReloaded', { success: true }); }
    catch (error) { this.clearLoadingState(); throw error; }
  }

  async reloadVariant() {
    this.startImmediateLoadingState('variant_reload', 'Reloading variant...');
    try { await this.essCommand('ess::reload_variant'); this.emit('variantReloaded', { success: true }); }
    catch (error) { this.clearLoadingState(); throw error; }
  }

  async saveSettings() { await this.essCommand('ess::save_settings'); this.emit('settingsSaved', { success: true }); }
  async resetSettings() { await this.essCommand('ess::reset_settings'); this.emit('settingsReset', { success: true }); }
  async setBranch(branch) { await this.essCommand(`send git {git::switch_and_pull ${branch}}`); this.emit('branchChanged', { branch, success: true }); }
  async openDataFile(filename) { await this.essCommand(`ess::file_open ${filename}`); this.emit('dataFileOpened', { filename, success: true }); }
  async closeDataFile() { await this.essCommand('ess::file_close'); this.emit('dataFileClosed', { success: true }); }
  async suggestFilename() { return await this.essCommand('ess::file_suggest'); }
}

export const dserv = new DservWebSocket();

export const useDservMonitoring = () => {
  return {
    getSystemStatus: () => dserv.getSystemStatus(),
    getDatapointsByCategory: () => dserv.getDatapointsByCategory(),
    monitoring: dserv.monitoring
  };
};
