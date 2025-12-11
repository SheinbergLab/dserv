/**
 * ws_manager.js - WebSocket connection manager for dserv
 * 
 * Handles connection, reconnection, and message routing.
 * Supports both promise-based and event-based patterns.
 */

class DservConnection {
    constructor(options = {}) {
        this.options = {
            subprocess: options.subprocess || 'docs',
            dservHost: options.dservHost || null,
            forceSecure: options.forceSecure || false,
            autoReconnect: options.autoReconnect !== false,
            reconnectDelay: options.reconnectDelay || 2000,
            maxReconnectDelay: options.maxReconnectDelay || 30000,
            createLinkedSubprocess: options.createLinkedSubprocess || false,
            onStatus: options.onStatus || (() => {}),
            onMessage: options.onMessage || (() => {}),
            onError: options.onError || (() => {}),
        };
        
        this.ws = null;
        this.connected = false;
        this.reconnectAttempts = 0;
        this.pendingRequests = new Map();
        this.requestId = 0;
        this.linkedSubprocess = null;
        
        // Event handlers for event emitter pattern
        this.eventHandlers = new Map();
    }

    // Event emitter methods
    on(event, handler) {
        if (!this.eventHandlers.has(event)) {
            this.eventHandlers.set(event, []);
        }
        this.eventHandlers.get(event).push(handler);
        return () => {
            const handlers = this.eventHandlers.get(event);
            if (handlers) {
                const index = handlers.indexOf(handler);
                if (index > -1) handlers.splice(index, 1);
            }
        };
    }
    
    emit(event, data) {
        const handlers = this.eventHandlers.get(event);
        if (handlers) {
            handlers.forEach(handler => {
                try { handler(data); }
                catch (e) { console.error(`Error in ${event} handler:`, e); }
            });
        }
    }

    getWsUrl() {
        let protocol = this.options.forceSecure ? 'wss:' : 
                      (window.location.protocol === 'https:' ? 'wss:' : 'ws:');
        
        if (this.options.dservHost) {
            return `${protocol}//${this.options.dservHost}/ws`;
        }
        return `${protocol}//${window.location.host}/ws`;
    }

    async connect() {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            return Promise.resolve();
        }

        return new Promise((resolve, reject) => {
            const url = this.getWsUrl();
            this.options.onStatus('connecting', `Connecting to ${url}...`);
            
            try {
                this.ws = new WebSocket(url);
            } catch (e) {
                this.options.onError(`Failed to create WebSocket: ${e.message}`);
                this.emit('error', e);
                reject(e);
                return;
            }

            this.ws.onopen = async () => {
                this.connected = true;
                this.reconnectAttempts = 0;
                this.options.onStatus('connected', 'Connected');
                this.emit('connected');
                
                // Create linked subprocess if requested
                if (this.options.createLinkedSubprocess && !this.linkedSubprocess) {
                    try {
                        const name = await this.sendRaw('subprocess -link');
                        this.linkedSubprocess = name.trim();
                        console.log('Created linked subprocess:', this.linkedSubprocess);
                    } catch (e) {
                        console.error('Failed to create linked subprocess:', e);
                    }
                }
                
                resolve();
            };

            this.ws.onclose = (event) => {
                this.connected = false;
                this.options.onStatus('disconnected', `Disconnected (code: ${event.code})`);
                this.emit('disconnected');
                
                for (const [id, req] of this.pendingRequests) {
                    req.reject(new Error('Connection closed'));
                }
                this.pendingRequests.clear();

                if (this.options.autoReconnect && event.code !== 1000) {
                    this.scheduleReconnect();
                }
            };

            this.ws.onerror = (error) => {
                this.options.onError('WebSocket error');
                this.emit('error', error);
                reject(error);
            };

            this.ws.onmessage = (event) => {
                this.handleMessage(event.data);
            };
        });
    }

    disconnect() {
        this.options.autoReconnect = false;
        if (this.ws) {
            this.ws.close(1000, 'User disconnect');
            this.ws = null;
        }
    }

    scheduleReconnect() {
        this.reconnectAttempts++;
        const delay = Math.min(
            this.options.reconnectDelay * Math.pow(1.5, this.reconnectAttempts - 1),
            this.options.maxReconnectDelay
        );
        
        this.options.onStatus('connecting', `Reconnecting in ${(delay/1000).toFixed(1)}s...`);
        
        setTimeout(() => {
            if (!this.connected) {
                this.connect().catch(() => {});
            }
        }, delay);
    }

    handleMessage(rawData) {
        // Parse JSON if possible
        let data;
        try {
            data = JSON.parse(rawData);
        } catch (e) {
            data = rawData;
        }
        
        // Determine if this is a datapoint message (should NOT resolve pending requests)
        const isDatapoint = typeof data === 'object' && data.name && 
                           (data.data !== undefined || data.value !== undefined);
        
        // Only resolve pending requests for non-datapoint messages
        if (this.pendingRequests.size > 0 && !isDatapoint) {
            const [id, req] = this.pendingRequests.entries().next().value;
            this.pendingRequests.delete(id);
            
            // Resolve with string or result field
            if (typeof data === 'string') {
                req.resolve(data);
            } else if (data && data.result !== undefined) {
                req.resolve(data.result);
            } else {
                req.resolve(JSON.stringify(data));
            }
        }
        
        // Emit events for all object messages (datapoints, terminal responses)
        if (typeof data === 'object') {
            // Terminal responses
            if (data.result !== undefined || data.status === 'error') {
                this.emit('terminal:response', data);
            }
            // Datapoint updates - check for type: 'datapoint' (Vue format) or name+data fields
            if (data.type === 'datapoint' || (data.name && (data.data !== undefined || data.value !== undefined))) {
                const normalized = {
                    name: data.name,
                    data: data.data !== undefined ? data.data : data.value,
                    value: data.data !== undefined ? data.data : data.value,
                    timestamp: data.timestamp,
                    dtype: data.dtype
                };
                if (data.name === '@keys') {
                    this.emit('datapoint:keys', normalized);
                }
                this.emit('datapoint:update', normalized);
            }
        }
        
        // Call legacy callback
        this.options.onMessage(data);
    }

    async sendRaw(command) {
        if (!this.connected || !this.ws) {
            throw new Error('Not connected');
        }
        
        return new Promise((resolve, reject) => {
            const id = ++this.requestId;
            const timeout = setTimeout(() => {
                this.pendingRequests.delete(id);
                reject(new Error('Request timeout'));
            }, 30000);

            this.pendingRequests.set(id, {
                resolve: (data) => {
                    clearTimeout(timeout);
                    resolve(data);
                },
                reject: (err) => {
                    clearTimeout(timeout);
                    reject(err);
                }
            });

            this.ws.send(command);
        });
    }

    async send(commandOrObject, subprocess = null) {
        if (!this.connected || !this.ws) {
            throw new Error('Not connected');
        }

        // If it's an object (like {cmd: 'subscribe'}), send as JSON directly
        if (typeof commandOrObject === 'object') {
            const json = JSON.stringify(commandOrObject);
            this.ws.send(json);
            return Promise.resolve(); // Subscribe commands don't wait for response
        }

        // Otherwise, wrap as subprocess command
        const target = subprocess || this.options.subprocess;
        const fullCommand = `send ${target} {${commandOrObject}}`;
        return this.sendRaw(fullCommand);
    }

    async sendJson(command, subprocess = null) {
        const response = await this.send(command, subprocess);
        try {
            return JSON.parse(response);
        } catch (e) {
            throw new Error(`Invalid JSON response: ${response}`);
        }
    }
    
    getLinkedSubprocess() {
        return this.linkedSubprocess;
    }
    
    async sendToLinked(command) {
        if (!this.linkedSubprocess) {
            throw new Error('No linked subprocess available');
        }
        return this.send(command, this.linkedSubprocess);
    }
}

if (typeof window !== 'undefined') {
    window.DservConnection = DservConnection;
}