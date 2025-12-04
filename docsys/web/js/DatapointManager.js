/**
 * DatapointManager.js
 * Reusable datapoint subscription manager for dserv
 * 
 * Usage:
 *   const dpManager = new DatapointManager(connection);
 *   dpManager.subscribe('graphics/main', (data) => {
 *       console.log('Got data:', data);
 *   });
 */

class DatapointManager {
    constructor(connection, options = {}) {
        this.connection = connection;
        this.options = {
            autoGetKeys: options.autoGetKeys !== false,
            updateInterval: options.updateInterval || 1, // every N updates
            ...options
        };
        
        // State
        this.subscriptions = new Map(); // pattern -> { callbacks: Set, lastValue, history }
        this.allKeys = [];
        this.listeners = new Map(); // event type -> Set of callbacks
        
        // Statistics
        this.stats = {
            totalUpdates: 0,
            subscriptionCount: 0
        };
        
        // Setup connection listeners
        this.setupConnectionListeners();
        
        // Auto-fetch keys if enabled
        if (this.options.autoGetKeys) {
            this.refreshKeys();
        }
    }
    
    setupConnectionListeners() {
        // Listen for datapoint updates from WebSocket manager
        this.connection.on('datapoint:update', (data) => {
            this.handleDatapointUpdate(data);
        });
        
        // Listen for @keys updates
        this.connection.on('datapoint:keys', (data) => {
            this.handleKeysUpdate(data);
        });
        
        // Listen for connection events
        this.connection.on('connected', () => {
            this.emit('connected');
            // Re-subscribe to all patterns on reconnect
            this.resubscribeAll();
        });
        
        this.connection.on('disconnected', () => {
            this.emit('disconnected');
        });
    }
    
    /**
     * Re-subscribe to all patterns (after reconnect)
     */
    resubscribeAll() {
        for (const [pattern, sub] of this.subscriptions.entries()) {
            this.sendSubscribe(pattern, this.options.updateInterval);
        }
    }
    
    /**
     * Subscribe to a datapoint pattern
     * @param {string} pattern - Datapoint name or pattern (e.g., 'graphics/main', 'sensor/*')
     * @param {function} callback - Called with (data) when datapoint updates
     * @param {object} options - Subscription options
     * @returns {function} Unsubscribe function
     */
    subscribe(pattern, callback, options = {}) {
        const updateInterval = options.updateInterval || this.options.updateInterval;
        
        // Get or create subscription entry
        let sub = this.subscriptions.get(pattern);
        if (!sub) {
            sub = {
                callbacks: new Set(),
                lastValue: null,
                lastTimestamp: null,
                dtype: null,
                history: [],
                updateCount: 0
            };
            this.subscriptions.set(pattern, sub);
            
            // Send subscribe command to server
            this.sendSubscribe(pattern, updateInterval);
            this.stats.subscriptionCount++;
            this.emit('subscription:added', { pattern });
        }
        
        // Add callback
        sub.callbacks.add(callback);
        
        // If we already have data, immediately call callback
        if (sub.lastValue !== null) {
            callback({
                name: pattern,
                data: sub.lastValue,
                value: sub.lastValue,
                timestamp: sub.lastTimestamp,
                dtype: sub.dtype
            });
        }
        
        // Return unsubscribe function
        return () => this.unsubscribe(pattern, callback);
    }
    
    /**
     * Unsubscribe from a datapoint
     * @param {string} pattern - Datapoint pattern
     * @param {function} callback - Optional specific callback to remove
     */
    unsubscribe(pattern, callback = null) {
        const sub = this.subscriptions.get(pattern);
        if (!sub) return;
        
        if (callback) {
            // Remove specific callback
            sub.callbacks.delete(callback);
            
            // If no more callbacks, unsubscribe from server
            if (sub.callbacks.size === 0) {
                this.subscriptions.delete(pattern);
                this.sendUnsubscribe(pattern);
                this.stats.subscriptionCount--;
                this.emit('subscription:removed', { pattern });
            }
        } else {
            // Remove all callbacks
            this.subscriptions.delete(pattern);
            this.sendUnsubscribe(pattern);
            this.stats.subscriptionCount--;
            this.emit('subscription:removed', { pattern });
        }
    }
    
    /**
     * Unsubscribe from all datapoints
     */
    unsubscribeAll() {
        const patterns = Array.from(this.subscriptions.keys());
        patterns.forEach(pattern => {
            if (pattern !== '@keys') { // Keep @keys subscription
                this.unsubscribe(pattern);
            }
        });
    }
    
    /**
     * Get current value of a datapoint (one-time fetch)
     * @param {string} name - Datapoint name
     * @returns {Promise} Resolves with datapoint value
     */
    async get(name) {
        return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
                reject(new Error(`Timeout getting datapoint: ${name}`));
            }, 5000);
            
            // Subscribe temporarily to get the value
            const unsubscribe = this.subscribe(name, (data) => {
                clearTimeout(timeout);
                unsubscribe();
                resolve(data);
            });
            
            // Also send explicit get command
            this.sendGet(name);
        });
    }
    
    /**
     * Refresh list of all available datapoints
     */
    refreshKeys() {
        // Subscribe to @keys to get list
        this.subscribe('@keys', (data) => {
            this.handleKeysUpdate(data);
        });
        
        // Also get current value
        this.sendGet('@keys');
    }
    
    /**
     * Handle @keys update
     */
    handleKeysUpdate(data) {
        if (data.name === '@keys' && (data.value || data.data)) {
            try {
                const keysData = data.data !== undefined ? data.data : data.value;
                
                let keys;
                if (typeof keysData === 'string') {
                    // Split on whitespace (Tcl list format)
                    keys = keysData.trim().split(/\s+/).filter(k => k.length > 0);
                } else if (Array.isArray(keysData)) {
                    keys = keysData;
                } else {
                    console.error('Unexpected keys format:', keysData);
                    return;
                }
                
                this.allKeys = keys.sort();
                this.emit('keys:update', { keys: this.allKeys });
            } catch (e) {
                console.error('Failed to parse keys:', e);
            }
        }
    }
    
    /**
     * Handle incoming datapoint update
     * This should be called by your connection/WebSocket manager
     */
    handleDatapointUpdate(data) {
        const name = data.name;
        if (!name) return;
        
        const value = data.data !== undefined ? data.data : data.value;
        
        // Update all matching subscriptions
        for (const [pattern, sub] of this.subscriptions.entries()) {
            if (this.matchesPattern(name, pattern)) {
                // Update subscription data
                sub.lastValue = value;
                sub.lastTimestamp = data.timestamp;
                sub.dtype = data.dtype;
                sub.updateCount++;
                
                // Add to history
                sub.history.push({
                    time: new Date(),
                    value: value,
                    timestamp: data.timestamp
                });
                
                // Keep history limited
                if (sub.history.length > 100) {
                    sub.history.shift();
                }
                
                // Call all callbacks
                sub.callbacks.forEach(callback => {
                    try {
                        callback({
                            name: name,
                            data: value,
                            value: value,
                            timestamp: data.timestamp,
                            dtype: data.dtype
                        });
                    } catch (e) {
                        console.error('Error in datapoint callback:', e);
                    }
                });
            }
        }
        
        this.stats.totalUpdates++;
        this.emit('datapoint:update', data);
    }
    
    /**
     * Check if datapoint name matches pattern
     */
    matchesPattern(name, pattern) {
        // Exact match
        if (name === pattern) return true;
        
        // Wildcard matching (simple implementation)
        if (pattern.includes('*')) {
            const regex = new RegExp('^' + pattern.replace(/\*/g, '.*') + '$');
            return regex.test(name);
        }
        
        return false;
    }
    
    /**
     * Send subscribe command to server
     */
    sendSubscribe(pattern, every = 1) {
        if (this.connection.send) {
            this.connection.send({
                cmd: 'subscribe',
                match: pattern,
                every: every
            });
        } else {
            console.error('Connection does not support send method');
        }
    }
    
    /**
     * Send unsubscribe command to server
     */
    sendUnsubscribe(pattern) {
        if (this.connection.send) {
            this.connection.send({
                cmd: 'unsubscribe',
                match: pattern
            });
        }
    }
    
    /**
     * Send get command to server
     */
    sendGet(name) {
        if (this.connection.send) {
            this.connection.send({
                cmd: 'get',
                name: name
            });
        }
    }
    
    /**
     * Get list of all available datapoint keys
     */
    getKeys() {
        return [...this.allKeys];
    }
    
    /**
     * Get list of currently subscribed patterns
     */
    getSubscriptions() {
        return Array.from(this.subscriptions.keys());
    }
    
    /**
     * Get subscription info for a pattern
     */
    getSubscriptionInfo(pattern) {
        const sub = this.subscriptions.get(pattern);
        if (!sub) return null;
        
        return {
            pattern: pattern,
            callbackCount: sub.callbacks.size,
            lastValue: sub.lastValue,
            lastTimestamp: sub.lastTimestamp,
            dtype: sub.dtype,
            updateCount: sub.updateCount,
            historyLength: sub.history.length
        };
    }
    
    /**
     * Get statistics
     */
    getStats() {
        return {
            ...this.stats,
            subscriptions: this.subscriptions.size,
            totalKeys: this.allKeys.length
        };
    }
    
    /**
     * Event listener management
     */
    on(event, callback) {
        if (!this.listeners.has(event)) {
            this.listeners.set(event, new Set());
        }
        this.listeners.get(event).add(callback);
        
        // Return unsubscribe function
        return () => {
            const callbacks = this.listeners.get(event);
            if (callbacks) {
                callbacks.delete(callback);
            }
        };
    }
    
    /**
     * Emit event to listeners
     */
    emit(event, data) {
        const callbacks = this.listeners.get(event);
        if (callbacks) {
            callbacks.forEach(callback => {
                try {
                    callback(data);
                } catch (e) {
                    console.error(`Error in event listener for ${event}:`, e);
                }
            });
        }
    }
    
    /**
     * Get datatype name from numeric code
     */
    getDatatypeName(dtype) {
        const types = {
            0: 'BYTE',
            1: 'STRING',
            2: 'FLOAT',
            3: 'DOUBLE',
            4: 'SHORT',
            5: 'INT',
            6: 'DG',
            7: 'SCRIPT',
            8: 'TRIGGER_SCRIPT',
            9: 'EVENT',
            10: 'NONE',
            11: 'JSON',
            12: 'ARROW',
            13: 'MSGPACK',
            14: 'JPEG',
            15: 'PPM',
            16: 'INT64',
            17: 'UNKNOWN'
        };
        return types[dtype] || `Unknown (${dtype})`;
    }
    
    /**
     * Cleanup and disconnect
     */
    dispose() {
        this.unsubscribeAll();
        this.listeners.clear();
        this.subscriptions.clear();
        console.log('DatapointManager disposed');
    }
}

// Export for use
if (typeof window !== 'undefined') {
    window.DatapointManager = DatapointManager;
}
