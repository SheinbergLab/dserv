import { reactive } from 'vue';
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

class DservWebSocket {
  constructor() {
    this.ws = null;
    this.reconnectAttempts = 0;
    this.maxReconnectAttempts = 5;
    this.reconnectInterval = 2000;
    this.requestCallbacks = new Map();

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

    this.pendingOperations = [];
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
      this.subscribe('ess/*');
      this.subscribe('system/*');
      this.subscribe('stimdg');
      this.subscribe('trialdg');
      this.subscribe('openiris/settings');
      this.subscribe('print');
      this.initializeState();
      // message.success('Connected to dserv');
    };

    this.ws.onmessage = (event) => {
      try {
        const data = JSON.parse(event.data);
        this.handleMessage(data);
      } catch (error) {
        console.error('Failed to parse WebSocket message:', error, 'Raw data:', event.data);
      }
    };

    this.ws.onclose = () => {
      this.state.connected = false;
      this.attemptReconnect();
    };

    this.ws.onerror = (error) => {
      console.error('WebSocket error:', error);
    };
  }

  subscribe(pattern, every = 1) {
    this.send({ cmd: 'subscribe', match: pattern, every });
  }

  send(messageObj) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(messageObj));
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
    switch (name) {
      case 'ess/systems': this.state.systems = this.parseList(value); break;
      case 'ess/protocols': this.state.protocols = this.parseList(value); break;
      case 'ess/variants': this.state.variants = this.parseList(value); break;
      case 'ess/git/branches': this.state.branches = this.parseList(value); break;
      case 'ess/system': this.state.currentSystem = value; break;
      case 'ess/protocol': this.state.currentProtocol = value; break;
      case 'ess/variant': this.state.currentVariant = value; break;
      case 'ess/git/branch': this.state.currentBranch = value; break;
      case 'ess/status': this.state.essStatus = value; this.handleStatusChange(value); break;
      case 'ess/state': this.state.status = value; break;
      case 'ess/obs_id': this.state.obsId = parseInt(value, 10) || 0; this.updateObsCount(); break;
      case 'ess/obs_total': this.state.obsTotal = parseInt(value, 10) || 0; this.updateObsCount(); break;
      case 'ess/in_obs': this.state.inObs = value === '1'; break;
      case 'ess/block_pct_complete': this.state.blockPctComplete = Math.round(parseFloat(value) * 100); break;
      case 'ess/block_pct_correct': this.state.blockPctCorrect = Math.round(parseFloat(value) * 100); break;
      case 'ess/subject': this.state.subject = value; break;
      case 'system/hostname': this.state.systemName = value; break;
      case 'system/os': this.state.systemOS = value; break;
      case 'print': console.log('ESS:', value); break;
      // default: console.log(`Unhandled datapoint: ${name} = ${value}`);
    }
  }

  handleStatusChange(status) {
    if (status === 'loading') {
      console.log('System entering loading state');
    } else if (status === 'stopped') {
        // When loading is finished, reset all loading flags
        this.loadingOperations.system = false;
        this.loadingOperations.protocol = false;
        this.loadingOperations.variant = false;
        console.log('System finished loading.');
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
      // message.error('Failed to reconnect to dserv. Please reload the page.');
    }
  }

  async initializeState() {
    console.log('Initializing ESS state...');
    try {
      const touchCommand = `
        foreach v {
          ess/systems ess/protocols ess/variants
          ess/system ess/protocol ess/variant
          ess/subject ess/state ess/status
          ess/obs_id ess/obs_total
          ess/block_pct_complete ess/block_pct_correct
          ess/git/branches ess/git/branch
          system/hostname system/os
        } {
          catch { dservTouch $v }
        }
      `;
      await this.essCommand(touchCommand);
      console.log('Initial state synchronization completed');
    } catch (error) {
      console.error('Failed to initialize state:', error);
    }
  }

  async setSystem(system) {
    this.loadingOperations.system = true;
    try {
      await this.essCommand(`ess::load_system ${system}`, 30000);
    } catch (error) {
      // message.error(`Failed to load system: ${error.message}`);
      this.loadingOperations.system = false; // Reset on error
      throw error;
    }
  }

  async setProtocol(protocol) {
    this.loadingOperations.protocol = true;
    try {
      await this.essCommand(`ess::load_system ${this.state.currentSystem} ${protocol}`, 30000);
    } catch (error) {
      // message.error(`Failed to load protocol: ${error.message}`);
      this.loadingOperations.protocol = false; // Reset on error
      throw error;
    }
  }

  async setVariant(variant) {
    this.loadingOperations.variant = true;
    try {
      await this.essCommand(
        `ess::load_system ${this.state.currentSystem} ${this.state.currentProtocol} ${variant}`,
        30000
      );
    } catch (error) {
      // message.error(`Failed to load variant: ${error.message}`);
      this.loadingOperations.variant = false; // Reset on error
      throw error;
    }
  }

  async startExperiment() {
    try {
      await this.essCommand('ess::start');
      // message.success('Experiment started');
    } catch (error) {
      // message.error(`Failed to start: ${error.message}`);
      throw error;
    }
  }

  async stopExperiment() {
    try {
      await this.essCommand('ess::stop');
      // message.warning('Experiment stopped');
    } catch (error) {
      // message.error(`Failed to stop: ${error.message}`);
      throw error;
    }
  }

  async resetExperiment() {
    try {
      await this.essCommand('ess::reset');
      // message.info('Experiment reset');
    } catch (error) {
      // message.error(`Failed to reset: ${error.message}`);
      throw error;
    }
  }

  async setSubject(subject) {
    try {
      await this.essCommand(`ess::set_subject ${subject}`);
      // message.success(`Subject set to ${subject}`);
    } catch (error) {
      // message.error(`Failed to set subject: ${error.message}`);
      throw error;
    }
  }
}

export const dserv = new DservWebSocket();
