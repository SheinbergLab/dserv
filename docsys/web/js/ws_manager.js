/**
 * ws_manager.js - WebSocket connection manager for dserv
 * 
 * Handles connection, reconnection, and message routing to subprocesses.
 */

class DservConnection {
    constructor(options = {}) {
        this.options = {
            subprocess: options.subprocess || 'docs',
            // For dev, specify dserv host:port; in production use same host
            dservHost: options.dservHost || null,
            // Force wss:// even if page is http (for dev server connecting to secure dserv)
            forceSecure: options.forceSecure || false,
            autoReconnect: options.autoReconnect !== false,
            reconnectDelay: options.reconnectDelay || 2000,
            maxReconnectDelay: options.maxReconnectDelay || 30000,
            onStatus: options.onStatus || (() => {}),
            onMessage: options.onMessage || (() => {}),
            onError: options.onError || (() => {}),
        };
        
        this.ws = null;
        this.connected = false;
        this.reconnectAttempts = 0;
        this.pendingRequests = new Map();
        this.requestId = 0;
    }

    /**
     * Build WebSocket URL based on current page location or explicit host
     */
    getWsUrl() {
        let protocol;
        if (this.options.forceSecure) {
            protocol = 'wss:';
        } else {
            protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        }
        
        // If explicit dservHost provided (e.g., for dev server), use it
        if (this.options.dservHost) {
            return `${protocol}//${this.options.dservHost}/ws`;
        }
        // Otherwise use same host as page (production)
        const host = window.location.host;
        return `${protocol}//${host}/ws`;
    }

    /**
     * Connect to dserv WebSocket
     */
    connect() {
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
                reject(e);
                return;
            }

            this.ws.onopen = () => {
                this.connected = true;
                this.reconnectAttempts = 0;
                this.options.onStatus('connected', 'Connected');
                resolve();
            };

            this.ws.onclose = (event) => {
                this.connected = false;
                this.options.onStatus('disconnected', `Disconnected (code: ${event.code})`);
                
                // Reject pending requests
                for (const [id, req] of this.pendingRequests) {
                    req.reject(new Error('Connection closed'));
                }
                this.pendingRequests.clear();

                // Auto-reconnect
                if (this.options.autoReconnect && event.code !== 1000) {
                    this.scheduleReconnect();
                }
            };

            this.ws.onerror = (error) => {
                this.options.onError('WebSocket error');
                reject(error);
            };

            this.ws.onmessage = (event) => {
                this.handleMessage(event.data);
            };
        });
    }

    /**
     * Disconnect from dserv
     */
    disconnect() {
        this.options.autoReconnect = false;
        if (this.ws) {
            this.ws.close(1000, 'User disconnect');
            this.ws = null;
        }
    }

    /**
     * Schedule a reconnection attempt
     */
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

    /**
     * Handle incoming WebSocket message
     */
    handleMessage(data) {
        // Messages from dserv are typically just the response string
        // If we have a pending request, resolve it
        if (this.pendingRequests.size > 0) {
            // Get the oldest pending request (FIFO)
            const [id, req] = this.pendingRequests.entries().next().value;
            this.pendingRequests.delete(id);
            req.resolve(data);
        } else {
            // Unsolicited message
            this.options.onMessage(data);
        }
    }

    /**
     * Send a command to a subprocess and wait for response
     */
    async send(command, subprocess = null) {
        if (!this.connected || !this.ws) {
            throw new Error('Not connected');
        }

        const target = subprocess || this.options.subprocess;
        const fullCommand = `send ${target} {${command}}`;
        
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

            this.ws.send(fullCommand);
        });
    }

    /**
     * Send a command and parse JSON response
     */
    async sendJson(command, subprocess = null) {
        const response = await this.send(command, subprocess);
        try {
            return JSON.parse(response);
        } catch (e) {
            throw new Error(`Invalid JSON response: ${response}`);
        }
    }
}

// Export for use
window.DservConnection = DservConnection;