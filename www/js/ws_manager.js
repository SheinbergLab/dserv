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
            connectTimeout: options.connectTimeout || 10000,  // New: connection timeout
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
        // Already connected - nothing to do
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            return Promise.resolve();
        }

        // Clean up any existing socket in a bad state (CONNECTING or CLOSING)
        if (this.ws) {
            try {
                this.ws.onopen = null;
                this.ws.onclose = null;
                this.ws.onerror = null;
                this.ws.onmessage = null;
                this.ws.close();
            } catch (e) {
                // Ignore cleanup errors
            }
            this.ws = null;
        }

        return new Promise((resolve, reject) => {
            const url = this.getWsUrl();
            this.options.onStatus('connecting', `Connecting to ${url}...`);
            
            // Track if promise has been settled to prevent double resolve/reject
            let settled = false;
            
            // Connection timeout - critical for WiFi/remote connections
            const connectTimeout = setTimeout(() => {
                if (!settled) {
                    settled = true;
                    const err = new Error(`Connection timeout after ${this.options.connectTimeout}ms`);
                    this.options.onError(err.message);
                    this.emit('error', err);
                    
                    // Clean up the socket
                    if (this.ws) {
                        try {
                            this.ws.onopen = null;
                            this.ws.onclose = null;
                            this.ws.onerror = null;
                            this.ws.onmessage = null;
                            this.ws.close();
                        } catch (e) {}
                        this.ws = null;
                    }
                    
                    reject(err);
                    
                    // Schedule reconnect if enabled
                    if (this.options.autoReconnect) {
                        this.scheduleReconnect();
                    }
                }
            }, this.options.connectTimeout);
            
            try {
                this.ws = new WebSocket(url);
            } catch (e) {
                settled = true;
                clearTimeout(connectTimeout);
                this.options.onError(`Failed to create WebSocket: ${e.message}`);
                this.emit('error', e);
                reject(e);
                return;
            }

            this.ws.onopen = async () => {
                // If we already rejected (e.g., due to timeout), ignore this
                if (settled) return;
                settled = true;
                clearTimeout(connectTimeout);
                
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
                
                // If we haven't settled yet, this is a connection failure during handshake
                if (!settled) {
                    settled = true;
                    clearTimeout(connectTimeout);
                    const err = new Error(`Connection closed during handshake (code: ${event.code})`);
                    this.options.onError(err.message);
                    reject(err);
                    
                    // Schedule reconnect if enabled
                    if (this.options.autoReconnect) {
                        this.scheduleReconnect();
                    }
                    return;
                }
                
                // Normal disconnect after connection was established
                this.options.onStatus('disconnected', `Disconnected (code: ${event.code})`);
                this.emit('disconnected');
                
                // Reject all pending requests
                for (const [id, req] of this.pendingRequests) {
                    req.reject(new Error('Connection closed'));
                }
                this.pendingRequests.clear();

                // Auto-reconnect on unexpected close
                if (this.options.autoReconnect && event.code !== 1000) {
                    this.scheduleReconnect();
                }
            };

            this.ws.onerror = (error) => {
                // Log the error, but don't reject here
                // The WebSocket spec guarantees onclose fires after onerror,
                // so we let onclose handle the rejection to avoid race conditions
                console.warn('WebSocket error event:', error);
                this.options.onError('WebSocket error');
                this.emit('error', error);
                
                // Note: Do NOT reject here - onclose will handle it
                // This prevents the race condition where onerror fires but
                // connection still succeeds
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
        
        // Check for requestId-based response matching first (new async path).
        // If the response carries a requestId that matches a pending request,
        // route it directly — no FIFO ambiguity.
        if (typeof data === 'object' && data.requestId) {
            const reqId = String(data.requestId);
            if (this.pendingRequests.has(reqId)) {
                const req = this.pendingRequests.get(reqId);
                this.pendingRequests.delete(reqId);

                if (data.status === 'error') {
                    req.reject(new Error(data.error || 'Unknown error'));
                } else {
                    req.resolve(data.result !== undefined ? data.result : JSON.stringify(data));
                }

                // Also emit terminal:response for any listeners (e.g. data_manager.js)
                this.emit('terminal:response', data);

                // Call legacy callback
                this.options.onMessage(data);
                return;
            }
        }
        
        // Determine if this is a datapoint message (should NOT resolve pending requests)
        const isDatapoint = typeof data === 'object' && data.name && 
                           (data.data !== undefined || data.value !== undefined);
        
        // Legacy FIFO matching for pending requests without requestId.
        // This path is only used during connection setup (e.g. 'subprocess -link')
        // before the async path is active.
        if (this.pendingRequests.size > 0 && !isDatapoint) {
            // Only match requests that used the legacy path (numeric keys)
            const firstEntry = this.pendingRequests.entries().next().value;
            if (firstEntry) {
                const [id, req] = firstEntry;
                // Legacy requests use numeric keys; async requests use string keys
                if (typeof id === 'number') {
                    this.pendingRequests.delete(id);
                    
                    if (typeof data === 'string') {
                        req.resolve(data);
                    } else if (data && data.result !== undefined) {
                        req.resolve(data.result);
                    } else {
                        req.resolve(JSON.stringify(data));
                    }
                }
            }
        }
        
        // Emit events for all object messages (datapoints, terminal responses)
        if (typeof data === 'object') {
            // Terminal responses
            if (data.result !== undefined || data.status === 'error') {
                this.emit('terminal:response', data);
            }
            // Datapoint updates - check for type: 'datapoint' (Vue format) or name+data fields
            if (data.type === 'datapoint' || (data.name && (data.data !== undefined || data.value !== undefined || data.dtype !== undefined))) {
                // For dtype 9 (EVENT) and other special types, preserve ALL fields
                // since the event data is at the top level, not in a data/value field
                let normalized;
                
                if (data.dtype === 9) {
                    // Events: preserve all fields (e_type, e_subtype, e_params, etc.)
                    normalized = {
                        ...data,
                        // Still add data/value for consistency, even though they may be undefined
                        data: data.data,
                        value: data.value !== undefined ? data.value : data.data
                    };
                } else {
                    // Regular datapoints: normalize to standard format
                    normalized = {
                        name: data.name,
                        data: data.data !== undefined ? data.data : data.value,
                        value: data.data !== undefined ? data.data : data.value,
                        timestamp: data.timestamp,
                        dtype: data.dtype
                    };
                }
                
                if (data.name === '@keys') {
                    this.emit('datapoint:keys', normalized);
                }
                this.emit('datapoint:update', normalized);
            }
        }
        
        // Call legacy callback
        this.options.onMessage(data);
    }

    /**
     * Send a command and wait for the response.
     * Uses the JSON async path with requestId for non-blocking server-side execution.
     * @param {string} command - The Tcl command to evaluate
     * @param {number} timeout - Timeout in ms (default 30000)
     * @returns {Promise<string>} The result string
     */
    async sendRaw(command, timeout = 30000) {
        if (!this.connected || !this.ws) {
            throw new Error('Not connected');
        }
        
        // Additional check for socket state
        if (this.ws.readyState !== WebSocket.OPEN) {
            throw new Error(`WebSocket not ready (state: ${this.ws.readyState})`);
        }
        
        // Use string requestId to distinguish from legacy numeric keys
        const reqId = String(++this.requestId);
        
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this.pendingRequests.delete(reqId);
                reject(new Error('Request timeout'));
            }, timeout);

            this.pendingRequests.set(reqId, {
                resolve: (data) => {
                    clearTimeout(timer);
                    resolve(data);
                },
                reject: (err) => {
                    clearTimeout(timer);
                    reject(err);
                }
            });

            // Send as JSON eval with requestId — triggers async path on server.
            // Server evaluates the script without blocking the event loop,
            // then sends the response back with the matching requestId.
            const message = JSON.stringify({
                cmd: 'eval',
                script: command,
                requestId: reqId
            });
            this.ws.send(message);
        });
    }

    /**
     * Send a command using the legacy plain-text protocol (blocking server-side).
     * Only used internally for connection setup commands like 'subprocess -link'
     * that must complete before the async infrastructure is ready.
     * @param {string} command - Raw text command
     * @returns {Promise<string>} The result string
     */
    _sendRawLegacy(command) {
        if (!this.connected || !this.ws) {
            throw new Error('Not connected');
        }
        
        if (this.ws.readyState !== WebSocket.OPEN) {
            throw new Error(`WebSocket not ready (state: ${this.ws.readyState})`);
        }
        
        return new Promise((resolve, reject) => {
            const id = ++this.requestId;  // numeric key for legacy FIFO matching
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
        
        // Additional check for socket state
        if (this.ws.readyState !== WebSocket.OPEN) {
            throw new Error(`WebSocket not ready (state: ${this.ws.readyState})`);
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
    
    /**
     * Check if the connection is ready to send messages
     * @returns {boolean} True if connected and socket is open
     */
    isReady() {
        return this.connected && this.ws && this.ws.readyState === WebSocket.OPEN;
    }
}

if (typeof window !== 'undefined') {
    window.DservConnection = DservConnection;
}
