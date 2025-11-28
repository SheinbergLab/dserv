// ============================================
// WebSocket Manager
// ============================================

class DservWebSocket {
    constructor() {
        this.ws = null;
        this.connected = false;
        this.handlers = new Map();
        this.reconnectTimer = null;
        this.reconnectDelay = 3000;
    }

    on(event, handler) {
        if (!this.handlers.has(event)) {
            this.handlers.set(event, []);
        }
        this.handlers.get(event).push(handler);
    }

    emit(event, data) {
        const handlers = this.handlers.get(event);
        if (handlers) {
            handlers.forEach(handler => handler(data));
        }
    }

    connect() {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const wsUrl = `${protocol}//${window.location.hostname}:${window.location.port}/ws`;

        try {
            this.ws = new WebSocket(wsUrl);

            this.ws.onopen = () => {
                this.connected = true;
                this.updateStatus('connected');
                console.log('WebSocket connected');
                this.emit('connected');
                
                // Clear reconnect timer
                if (this.reconnectTimer) {
                    clearTimeout(this.reconnectTimer);
                    this.reconnectTimer = null;
                }
            };

            this.ws.onmessage = (event) => {
                try {
                    const data = JSON.parse(event.data);
                    this.handleMessage(data);
                } catch (e) {
                    console.error('Failed to parse WebSocket message:', e);
                }
            };

            this.ws.onerror = (error) => {
                console.error('WebSocket error:', error);
                this.emit('error', error);
            };

            this.ws.onclose = () => {
                this.connected = false;
                this.updateStatus('disconnected');
                console.log('WebSocket disconnected');
                this.emit('disconnected');
                
                // Auto-reconnect
                if (!this.reconnectTimer) {
                    this.reconnectTimer = setTimeout(() => {
                        console.log('Reconnecting...');
                        this.connect();
                    }, this.reconnectDelay);
                }
            };

        } catch (error) {
            console.error('Failed to connect:', error);
            this.scheduleReconnect();
        }
    }

    scheduleReconnect() {
        if (!this.reconnectTimer) {
            this.reconnectTimer = setTimeout(() => {
                this.connect();
            }, this.reconnectDelay);
        }
    }

    handleMessage(data) {
        // Route messages based on type/cmd
        if (data.result !== undefined || data.status === 'error') {
            // Command response (terminal)
            this.emit('terminal:response', data);
        } else if (data.action === 'subscribed' || data.action === 'unsubscribed') {
            // Subscription confirmation - just acknowledge
            console.log(`Subscription ${data.action}: ${data.match}`);
        } else if (data.type === 'keys' || (data.name === '@keys')) {
            // Datapoint keys list
            console.log('Received @keys:', data);
            this.emit('datapoint:keys', data);
        } else if (data.type === 'datapoint') {
            // Datapoint update (explicit type)
            console.log('Datapoint update (explicit):', data.name);
            this.emit('datapoint:update', data);
        } else if (data.name && data.data !== undefined) {
            // Datapoint update (implicit - just has name/data fields)
            console.log('Datapoint update (implicit):', data.name);
            // Special case: check if this IS the @keys data
            if (data.name === '@keys') {
                console.log('Found @keys in implicit format:', data);
                this.emit('datapoint:keys', data);
            } else {
                const normalized = {
                    type: 'datapoint',
                    name: data.name,
                    data: data.data,
                    timestamp: data.timestamp,
                    dtype: data.dtype
                };
                this.emit('datapoint:update', normalized);
            }
        } else {
            // Generic message
            console.log('Unhandled message:', data);
        }
    }

    send(data) {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify(data));
            return true;
        }
        console.warn('WebSocket not connected');
        return false;
    }

    updateStatus(status) {
        const statusElement = document.getElementById('status');
        if (statusElement) {
            statusElement.textContent = status === 'connected' ? 'Connected' : 'Disconnected';
            statusElement.className = `status ${status}`;
        }
    }

    close() {
        if (this.reconnectTimer) {
            clearTimeout(this.reconnectTimer);
            this.reconnectTimer = null;
        }
        if (this.ws) {
            this.ws.close();
            this.ws = null;
        }
    }
}

// ============================================
// Global WebSocket instance
// ============================================

const wsManager = new DservWebSocket();

// ============================================
// Resize handle functionality
// ============================================

function initResizeHandles() {
    // Vertical resize (left pane vs datapoint pane)
    initVerticalResize();
    
    // Horizontal resize (terminal vs error log)
    initHorizontalResize();
}

function initVerticalResize() {
    const handle = document.getElementById('vertical-resize-handle');
    const leftPane = document.getElementById('left-pane');
    const datapointPane = document.getElementById('datapoint-pane');
    let isResizing = false;
    let startX = 0;
    let startWidth = 0;

    handle.addEventListener('mousedown', (e) => {
        isResizing = true;
        startX = e.clientX;
        startWidth = datapointPane.offsetWidth;
        document.body.style.cursor = 'col-resize';
        document.body.style.userSelect = 'none';
        e.preventDefault();
    });

    document.addEventListener('mousemove', (e) => {
        if (!isResizing) return;

        const deltaX = startX - e.clientX;
        const newWidth = Math.max(300, Math.min(800, startWidth + deltaX));
        datapointPane.style.width = `${newWidth}px`;
    });

    document.addEventListener('mouseup', () => {
        if (isResizing) {
            isResizing = false;
            document.body.style.cursor = '';
            document.body.style.userSelect = '';
        }
    });
}

function initHorizontalResize() {
    const handle = document.getElementById('horizontal-resize-handle');
    const terminalPane = document.getElementById('terminal-pane');
    const errorLogPane = document.getElementById('error-log-pane');
    let isResizing = false;
    let startY = 0;
    let startHeightTerminal = 0;
    let startHeightError = 0;

    handle.addEventListener('mousedown', (e) => {
        isResizing = true;
        startY = e.clientY;
        startHeightTerminal = terminalPane.offsetHeight;
        startHeightError = errorLogPane.offsetHeight;
        document.body.style.cursor = 'row-resize';
        document.body.style.userSelect = 'none';
        e.preventDefault();
    });

    document.addEventListener('mousemove', (e) => {
        if (!isResizing) return;

        const deltaY = e.clientY - startY;
        const newTerminalHeight = Math.max(150, startHeightTerminal + deltaY);
        const newErrorHeight = Math.max(100, startHeightError - deltaY);
        
        terminalPane.style.flex = 'none';
        terminalPane.style.height = `${newTerminalHeight}px`;
        errorLogPane.style.height = `${newErrorHeight}px`;
    });

    document.addEventListener('mouseup', () => {
        if (isResizing) {
            isResizing = false;
            document.body.style.cursor = '';
            document.body.style.userSelect = '';
        }
    });
}

// ============================================
// Initialization
// ============================================

document.addEventListener('DOMContentLoaded', () => {
    // Initialize resize handles
    initResizeHandles();

    // Connect to WebSocket
    wsManager.connect();
});

// Cleanup on page unload
window.addEventListener('beforeunload', () => {
    wsManager.close();
});
