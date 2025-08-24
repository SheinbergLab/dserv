// Minimal dserv.js - Just what we need for graphics testing
import { reactive } from 'vue';

class MinimalDserv {
  constructor() {
    this.ws = null;
    this.reconnectAttempts = 0;
    this.maxReconnectAttempts = 5;
    
    // Simple event system
    this.eventHandlers = new Map();
    
    // Minimal reactive state
    this.state = reactive({
      connected: false
    });
    
    this.connect();
  }

  connect() {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const host = window.location.host || 'localhost:3001';
    const wsUrl = `${protocol}//${host}/ws`;

    console.log(`🔌 Connecting to ${wsUrl}`);
    this.ws = new WebSocket(wsUrl);

    this.ws.onopen = () => {
      console.log('✅ Connected to dserv');
      this.state.connected = true;
      this.reconnectAttempts = 0;
      this.emit('connection', { connected: true });
    };

    this.ws.onmessage = (event) => {
      try {
        const data = JSON.parse(event.data);
        console.log('📨 Raw message received:', data);
        
        // Handle datapoint updates
        if (data.type === 'datapoint' && data.name) {
          console.log('📡 Datapoint update:', data.name, '=', data.data);
          this.emit(`datapoint:${data.name}`, data);
          this.emit('datapoint:*', data);
        }
        
        // Handle command responses (if needed)
        if (data.status && data.result !== undefined) {
          console.log('💬 Command response:', data);
          this.emit('response', data);
        }
        
      } catch (error) {
        console.error('❌ Failed to parse message:', error);
      }
    };

    this.ws.onclose = () => {
      console.log('📴 Disconnected from dserv');
      this.state.connected = false;
      this.emit('connection', { connected: false });
      this.attemptReconnect();
    };

    this.ws.onerror = (error) => {
      console.error('⚠️ WebSocket error:', error);
    };
  }

  attemptReconnect() {
    if (this.reconnectAttempts < this.maxReconnectAttempts) {
      this.reconnectAttempts++;
      console.log(`🔄 Reconnecting... (${this.reconnectAttempts}/${this.maxReconnectAttempts})`);
      setTimeout(() => this.connect(), 2000);
    } else {
      console.error('💀 Max reconnection attempts reached');
    }
  }

  // Simple event system
  on(eventName, handler, componentId = null) {
    if (!this.eventHandlers.has(eventName)) {
      this.eventHandlers.set(eventName, []);
    }
    
    this.eventHandlers.get(eventName).push({ handler, componentId });
    console.log(`👂 Component ${componentId || 'unknown'} listening to ${eventName}`);
    
    // Return unsubscribe function
    return () => {
      const handlers = this.eventHandlers.get(eventName);
      if (handlers) {
        const index = handlers.findIndex(h => h.handler === handler);
        if (index !== -1) {
          handlers.splice(index, 1);
        }
      }
    };
  }

  emit(eventName, data) {
    const handlers = this.eventHandlers.get(eventName) || [];
    handlers.forEach(({ handler }) => {
      try {
        handler(data);
      } catch (error) {
        console.error(`❌ Error in event handler for ${eventName}:`, error);
      }
    });
  }

  // Send raw messages
  send(message) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(message));
      console.log('📤 Sent:', message);
    } else {
      console.warn('⚠️ Cannot send message - not connected');
    }
  }

  // Simple command execution (for terminal)
  sendCommand(command) {
    this.send({
      cmd: 'eval',
      script: command
    });
  }

  // Subscribe to datapoint
  subscribe(pattern) {
    this.send({
      cmd: 'subscribe',
      match: pattern,
      every: 1
    });
    console.log(`📊 Subscribed to: ${pattern}`);
  }

  // Unsubscribe from datapoint
  unsubscribe(pattern) {
    this.send({
      cmd: 'unsubscribe', 
      match: pattern
    });
    console.log(`❌ Unsubscribed from: ${pattern}`);
  }

  // Set datapoint value
  set(name, value) {
    this.send({
      cmd: 'set',
      name: name,
      value: value
    });
    console.log(`📝 Set ${name} = ${value}`);
  }

  // Get datapoint value
  get(name) {
    this.send({
      cmd: 'get',
      name: name
    });
    console.log(`❓ Get ${name}`);
  }
}

// Create singleton instance
export const dserv = new MinimalDserv();