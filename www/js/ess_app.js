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
    
    // Expose dpManager globally for EyeSettings
    window.dpManager = dpManager;
    
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
        initMeshManager();
        
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
          ess/subject_ids ess/subject ess/state ess/status
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
 * Canvas drawing buffer size must match CSS display size to avoid distortion
 */
function initStimRenderer() {
    const canvas = document.getElementById('stim-canvas');
    if (canvas) {
        // Get the actual rendered size from CSS (after layout)
        // This ensures drawing buffer matches display size
        const rect = canvas.getBoundingClientRect();
        const width = Math.floor(rect.width) || canvas.width || 480;
        const height = Math.floor(rect.height) || canvas.height || 300;
        
        // Set canvas buffer to match display size
        canvas.width = width;
        canvas.height = height;
        
        stimRenderer = new GraphicsRenderer(canvas, {
            width: width,
            height: height,
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
        
        log(`Stimulus Renderer initialized (${width}×${height})`, 'info');
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
    
    // Normalize to lowercase for comparison
    const stateLower = (state || '').toLowerCase();
    
    // Set display text (capitalize first letter)
    const displayText = state ? state.charAt(0).toUpperCase() + state.slice(1).toLowerCase() : '--';
    stateEl.textContent = displayText;
    stateEl.className = 'ess-system-state';
    
    switch (stateLower) {
        case 'running':
            stateEl.classList.add('running');
            break;
        case 'stopped':
            stateEl.classList.add('stopped');
            break;
        case 'loading':
            stateEl.classList.add('loading');
            break;
        case 'initialized':
            stateEl.classList.add('initialized');
            break;
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

let meshDropdown = null;
let tclTerminal = null;
let activeBottomTab = 'console';

// Initialize when DOM is ready
document.addEventListener('DOMContentLoaded', init);

// Initialize mesh dropdown after connection is established
function initMeshManager() {
    if (connection && !meshDropdown) {
        const container = document.getElementById('mesh-dropdown-container');
        if (container) {
            meshDropdown = new MeshDropdown(container, connection, {
                guiPath: '/essgui/',
                pollInterval: 5000
            });
        }
    }
}

// Initialize Tcl terminal
function initTerminal() {
    if (connection && !tclTerminal) {
        try {
            tclTerminal = new TclTerminal('tcl-terminal-container', connection, {
                interpreter: 'dserv',
                useLinkedSubprocess: false,
                showWelcome: true,
                welcomeMessage: 'dserv Terminal - Type "help" for assistance'
            });
        } catch (e) {
            console.error('Failed to initialize terminal:', e);
        }
    }
}

// Switch between bottom panel tabs
function switchBottomTab(tabName) {
    activeBottomTab = tabName;
    
    // Update tab buttons
    document.querySelectorAll('.ess-bottom-tab').forEach(tab => {
        tab.classList.toggle('active', tab.dataset.tab === tabName);
    });
    
    // Update panes
    document.querySelectorAll('.ess-bottom-pane').forEach(pane => {
        pane.classList.toggle('active', pane.id === `${tabName}-pane`);
    });
    
    // Focus terminal input if switching to terminal
    if (tabName === 'terminal' && tclTerminal) {
        tclTerminal.focus();
    }
    
    // Initialize terminal on first switch (lazy init)
    if (tabName === 'terminal' && !tclTerminal) {
        initTerminal();
    }
}

// Clear active bottom panel
function clearBottomPanel() {
    if (activeBottomTab === 'console') {
        clearConsole();
    } else if (activeBottomTab === 'terminal' && tclTerminal) {
        tclTerminal.clear();
    }
}

// Toggle bottom panel visibility
function toggleBottomPanel() {
    const panel = document.getElementById('bottom-panel');
    if (panel) {
        panel.classList.toggle('collapsed');
    }
}

// Export for use in HTML
window.reconnect = reconnect;
window.clearConsole = clearConsole;
window.toggleConsole = toggleBottomPanel; // Alias for compatibility
window.switchBottomTab = switchBottomTab;
window.clearBottomPanel = clearBottomPanel;
window.toggleBottomPanel = toggleBottomPanel;