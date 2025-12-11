/**
 * ess_app.js
 * Main application script for ESS Control Panel
 * 
 * Wires together:
 * - WebSocket connection (DservConnection)
 * - Datapoint subscriptions (DatapointManager)
 * - ESS Control panel (ESSControl)
 * - Eye/Touch Visualizer (EyeTouchVisualizer)
 * - Stimulus display (GraphicsRenderer)
 * - Performance display
 */

// Global state
let connection = null;
let dpManager = null;
let essControl = null;
let eyeTouchViz = null;
let stimRenderer = null;

// Console logging
const consoleOutput = [];
let errorCount = 0;

/**
 * Initialize the application
 */
async function init() {
    log('Initializing ESS Control Panel...', 'info');
    
    // Create WebSocket connection
    connection = new DservConnection({
        subprocess: 'ess',
        autoReconnect: true,
        onStatus: handleConnectionStatus,
        onError: handleConnectionError
    });
    
    // Create DatapointManager (but don't subscribe yet)
    dpManager = new DatapointManager(connection, {
        autoGetKeys: false  // Don't auto-fetch keys until connected
    });
    
    // Connect first, then initialize components
    try {
        await connection.connect();
        log('Connected to dserv', 'info');
        
        // Now that we're connected, initialize UI components
        // These set up subscriptions
        initESSControl();
        initEyeTouchVisualizer();
        initStimRenderer();
        initPerformanceDisplay();
        
        // Small delay to ensure subscriptions are registered on server
        // before we touch the datapoints
        await new Promise(resolve => setTimeout(resolve, 100));
        
        // Request initial values for all ESS datapoints
        requestInitialData();
    } catch (e) {
        log(`Connection failed: ${e.message}`, 'error');
    }
    
    // Re-initialize components on reconnect
    connection.on('connected', () => {
        log('Reconnected - refreshing data...', 'info');
        requestInitialData();
    });
}

/**
 * Request initial values for all ESS-related datapoints
 * Uses dservTouch to cause server to republish current values
 * Sends as a single foreach command for efficiency
 */
function requestInitialData() {
    log('Touching ESS datapoints to get current state...', 'info');
    
    // Send using eval command format (like Vue's essCommand)
    // This matches how dserv.js does it
    const touchCommand = `
        foreach v {
          ess/systems ess/protocols ess/variants
          ess/system ess/protocol ess/variant
          ess/variant_info_json ess/param_settings
          ess/subject ess/state ess/status
          ess/obs_id ess/obs_total
          ess/block_pct_complete ess/block_pct_correct
          ess/screen_w ess/screen_h ess/screen_halfx ess/screen_halfy
          ess/params
          system/hostname system/os
        } {
          catch { dservTouch $v }
        }
    `;
    
    // Send as JSON eval command like Vue does
    const message = { cmd: 'eval', script: touchCommand };
    connection.ws.send(JSON.stringify(message));
}

/**
 * Initialize ESS Control panel
 */
function initESSControl() {
    essControl = new ESSControl('ess-control-container', dpManager);
    
    // Listen for state changes to update status bar
    essControl.on('stateChange', ({ state }) => {
        updateSystemState(state);
    });
    
    essControl.on('statusChange', ({ status }) => {
        log(`ESS status: ${status}`, 'info');
        updateSystemStatus(status);
    });
    
    log('ESS Control initialized', 'info');
}

/**
 * Initialize Eye/Touch Visualizer
 */
function initEyeTouchVisualizer() {
    const canvas = document.getElementById('eyetouch-canvas');
    if (canvas) {
        eyeTouchViz = new EyeTouchVisualizer(canvas, dpManager);
        log('Eye/Touch Visualizer initialized', 'info');
    }
}

/**
 * Initialize Stimulus Renderer (using existing GraphicsRenderer)
 */
function initStimRenderer() {
    const canvas = document.getElementById('stim-canvas');
    if (canvas) {
        // Get initial size from container
        const container = canvas.parentElement;
        const rect = container.getBoundingClientRect();
        const style = getComputedStyle(container);
        const paddingX = parseFloat(style.paddingLeft) + parseFloat(style.paddingRight);
        const paddingY = parseFloat(style.paddingTop) + parseFloat(style.paddingBottom);
        
        stimRenderer = new GraphicsRenderer(canvas, {
            width: rect.width - paddingX,
            height: rect.height - paddingY,
            backgroundColor: '#1a1a2a',
            datapointManager: dpManager,
            streamId: 'graphics/stimulus',
            onStats: (stats) => {
                const statsEl = document.getElementById('stim-stats');
                if (statsEl) {
                    statsEl.textContent = `${stats.commandCount} cmds @ ${stats.time}`;
                }
            }
        });
        
        // Handle resize
        const resizeObserver = new ResizeObserver(() => {
            const rect = container.getBoundingClientRect();
            const w = rect.width - paddingX;
            const h = rect.height - paddingY;
            if (w > 0 && h > 0) {
                stimRenderer.resize(w, h);
            }
        });
        resizeObserver.observe(container);
        
        log('Stimulus Renderer initialized', 'info');
    }
}

/**
 * Initialize Performance Display
 */
function initPerformanceDisplay() {
    // Subscribe to performance-related datapoints
    dpManager.subscribe('ess/obs_id', (data) => {
        updatePerformanceValue('obs_id', data.value);
    });
    
    dpManager.subscribe('ess/obs_total', (data) => {
        const obsId = parseInt(localStorage.getItem('ess_obs_id') || '0');
        const total = parseInt(data.value) || 0;
        document.getElementById('perf-trial').textContent = `${obsId + 1}/${total}`;
    });
    
    dpManager.subscribe('ess/block_pct_correct', (data) => {
        const pct = Math.round(parseFloat(data.value) * 100);
        document.getElementById('perf-correct').textContent = `${pct}%`;
    });
    
    dpManager.subscribe('ess/block_pct_complete', (data) => {
        const pct = Math.round(parseFloat(data.value) * 100);
        document.getElementById('perf-complete').textContent = `${pct}%`;
    });
    
    log('Performance display initialized', 'info');
}

/**
 * Store obs_id for display
 */
function updatePerformanceValue(key, value) {
    if (key === 'obs_id') {
        localStorage.setItem('ess_obs_id', value);
    }
}

/**
 * Handle connection status changes
 */
function handleConnectionStatus(status, message) {
    const indicator = document.getElementById('status-indicator');
    const statusText = document.getElementById('status-text');
    
    indicator.className = 'ess-status-indicator';
    
    switch (status) {
        case 'connected':
            indicator.classList.add('connected');
            statusText.textContent = 'Connected';
            break;
        case 'connecting':
            indicator.classList.add('connecting');
            statusText.textContent = message || 'Connecting...';
            break;
        case 'disconnected':
            statusText.textContent = message || 'Disconnected';
            break;
    }
}

/**
 * Handle connection errors
 */
function handleConnectionError(error) {
    log(`Connection error: ${error}`, 'error');
}

/**
 * Update system state display in status bar
 */
function updateSystemState(state) {
    const stateEl = document.getElementById('system-state');
    if (!stateEl) return;
    
    // Don't overwrite if we're showing Loading
    if (stateEl.classList.contains('loading')) return;
    
    stateEl.textContent = state || '--';
    stateEl.className = 'ess-system-state';
    
    switch (state) {
        case 'Running':
            stateEl.classList.add('running');
            break;
        case 'Stopped':
            stateEl.classList.add('stopped');
            break;
        case 'Initialized':
            stateEl.classList.add('initialized');
            break;
    }
}

/**
 * Update system status (loading/stopped) in status bar
 */
function updateSystemStatus(status) {
    const stateEl = document.getElementById('system-state');
    if (!stateEl) return;
    
    if (status === 'loading') {
        stateEl.textContent = 'Loading...';
        stateEl.className = 'ess-system-state loading';
    } else {
        // Remove loading class and let state update show the actual state
        stateEl.classList.remove('loading');
        // Restore the actual state
        if (essControl && essControl.state) {
            updateSystemState(essControl.state.essState);
        }
    }
}

/**
 * Reconnect to server
 */
function reconnect() {
    log('Reconnecting...', 'info');
    if (connection) {
        connection.disconnect();
        setTimeout(() => {
            connection.connect().catch(e => {
                log(`Reconnection failed: ${e.message}`, 'error');
            });
        }, 500);
    }
}

/**
 * Log message to console
 */
function log(message, level = 'info') {
    const timestamp = new Date().toLocaleTimeString();
    const entry = { timestamp, message, level };
    consoleOutput.push(entry);
    
    if (level === 'error') {
        errorCount++;
        updateErrorCount();
    }
    
    // Keep console limited
    if (consoleOutput.length > 200) {
        consoleOutput.shift();
    }
    
    // Update console display
    const consoleBody = document.getElementById('console-output');
    if (consoleBody) {
        const div = document.createElement('div');
        div.className = `ess-console-entry ${level}`;
        div.innerHTML = `<span class="timestamp">${timestamp}</span>${escapeHtml(message)}`;
        consoleBody.appendChild(div);
        consoleBody.scrollTop = consoleBody.scrollHeight;
    }
    
    // Also log to browser console
    if (level === 'error') {
        console.error(`[ESS] ${message}`);
    } else {
        console.log(`[ESS] ${message}`);
    }
}

/**
 * Update error count badge
 */
function updateErrorCount() {
    const countEl = document.getElementById('error-count');
    if (countEl) {
        countEl.textContent = errorCount > 0 ? `${errorCount} errors` : '';
    }
}

/**
 * Clear console
 */
function clearConsole() {
    consoleOutput.length = 0;
    errorCount = 0;
    updateErrorCount();
    
    const consoleBody = document.getElementById('console-output');
    if (consoleBody) {
        consoleBody.innerHTML = '';
    }
}

/**
 * Toggle console visibility
 */
function toggleConsole() {
    const console = document.getElementById('error-console');
    if (console) {
        console.classList.toggle('collapsed');
    }
}

/**
 * Escape HTML for safe display
 */
function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}

// Initialize when DOM is ready
document.addEventListener('DOMContentLoaded', init);

// Export for use in HTML
window.reconnect = reconnect;
window.clearConsole = clearConsole;
window.toggleConsole = toggleConsole;