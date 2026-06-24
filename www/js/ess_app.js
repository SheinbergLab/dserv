/**
 * ess_app.js
 * Main application script for ESS Control Panel
 * 
 * Wires together:
 * - WebSocket connection (DservConnection)
 * - Datapoint subscriptions (DatapointManager)
 * - ESS Control panel (ESSControl)
 * - Config Manager (ConfigManager)
 * - Eye/Touch Visualizer (EyeTouchVisualizer)
 * - Stimulus display (GraphicsRenderer)
 * - Eye Settings
 * - Performance display
 */

// Global state
let connection = null;
let dpManager = null;
let essControl = null;
let eyeTouchViz = null;
let stimRenderer = null;
let eyeSettings = null;
let buttonControls = null;
let sliderControls = null;
let projectSelector = null;
let lastKnownHostname = null;
let isConnectionActive = false;
let batteryPct = null;
let batteryCharging = null;
let batteryV = null;
let batteryA = null;
let batteryW = null;
let batteryHrsRemaining = null;
let hasBatteryData = false;

const BOTTLE_CAPACITY_ML = 500;
const JUICE_LOW_THRESHOLD_ML = 50;

let juiceLevel = null;
let juiceRewardMls = null;
let juiceRewardNumber = null;
let hasJuiceData = false;

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
        connectTimeout: 10000,  // 10 second timeout for WiFi connections
        onStatus: handleConnectionStatus,
        onError: handleConnectionError
    });
    
    // Create DatapointManager (but don't subscribe yet)
    dpManager = new DatapointManager(connection, {
        autoGetKeys: false  // Don't auto-fetch keys until connected
    });
    
    // Expose dpManager globally (for debugging, not for initialization timing)
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
        initEyeSettings();
        initButtonControls();
        initSliderControls();
	initProjectSelector();
        initOpenEphysStatus();
        initHostnameDisplay();
        initJuiceIndicator();
        initBatteryIndicator();
        
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
    // Guard against sending on closed/closing socket
    if (!connection?.isReady?.()) {
        log('Cannot request initial data - connection not ready', 'warn');
        return;
    }
    
    log('Touching ESS datapoints to get current state...', 'info');
    
    // Send using eval command format 
    // This matches how dserv.js does it
    const touchCommand = `
        foreach v {
          ess/systems ess/protocols ess/variants
          ess/system ess/protocol ess/variant
          ess/variant_info_json ess/param_settings
          ess/subject_ids ess/subject ess/state ess/status
          ess/obs_id ess/obs_total ess/in_obs
          ess/block_pct_complete ess/block_pct_correct
          ess/screen_w ess/screen_h ess/screen_halfx ess/screen_halfy
          ess/params ess/datafile ess/sortby_columns ess/block_id
          ess/buttons/channels ess/slider_active slider/settings
          ess/session_stats
          em/settings mesh/peers
          openephys/status
          system/hostname system/os
          powermon/pct powermon/charging powermon/hrs_remaining powermon/v powermon/a powermon/w
          juicer/juice_level juicer/reward_mls juicer/reward_number
          configs/list configs/tags configs/quick_picks configs/current
          configs/remote_servers
          queues/list queues/state queues/items
          projects/list projects/active projects/active_detail
          ess/registry/url ess/registry/workgroup ess/registry/sync_status
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
    
    // Listen for log messages from ESSControl
    essControl.on('log', ({ message, level }) => {
        log(message, level || 'info');
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
let essPerformance = null;

function initPerformanceDisplay() {
    essPerformance = new ESSPerformance(dpManager, 'performance-container');
    window.essPerformance = essPerformance;
    log('Performance display initialized', 'info');
}

/**
 * Initialize Eye Settings panel
 */
function initEyeSettings() {
    // EyeSettings class is defined in ess_control.html
    if (typeof EyeSettings !== 'undefined') {
        eyeSettings = new EyeSettings(dpManager);
        window.eyeSettings = eyeSettings;
        log('Eye Settings initialized', 'info');
    }
}

/**
 * Initialize virtual button controls
 */
function initButtonControls() {
    if (typeof ButtonControls !== 'undefined') {
        buttonControls = new ButtonControls(dpManager);
        log('Button Controls initialized', 'info');
    }
}

/**
 * Initialize virtual slider controls
 */
function initSliderControls() {
    if (typeof SliderControls !== 'undefined') {
        sliderControls = new SliderControls(dpManager);
        log('Slider Controls initialized', 'info');
    }
}

/**
 * Initialize Project Selector
 */
function initProjectSelector() {
    const container = document.getElementById('project-selector-container');
    if (container && !projectSelector) {
        projectSelector = new ProjectSelector(container, dpManager);
        window.projectSelector = projectSelector;  // For debugging
        log('Project Selector initialized', 'info');
    }
}

/**
 * Initialize Open Ephys status indicator
 * Hidden by default; appears only when openephys subprocess publishes status
 */
function initOpenEphysStatus() {
    const container = document.getElementById('openephys-status');
    if (!container) return;

    container.innerHTML = '<div class="ess-oe-dot"></div><span class="ess-oe-label"></span>';
    const label = container.querySelector('.ess-oe-label');

    dpManager.subscribe('openephys/status', (data) => {
        const mode = (data.value || '').toUpperCase();

        if (!mode || mode === 'DISCONNECTED') {
            container.className = 'ess-oe-status';
            return;
        }

        let stateClass = 'idle';
        if (mode === 'ACQUIRE') stateClass = 'acquiring';
        else if (mode === 'RECORD') stateClass = 'recording';

        container.className = `ess-oe-status active ${stateClass}`;
        label.textContent = `OE: ${mode}`;
    });

    log('Open Ephys status initialized', 'info');
}

/**
 * Format hostname for display (matches MeshDropdown.formatName)
 */
function formatHostname(raw) {
    return (raw || '').replace('Lab Station ', '');
}

/**
 * Update centered hostname display and document title
 */
function updateHostnameDisplay() {
    const display = document.getElementById('hostname-display');
    const name = lastKnownHostname || '—';

    if (display) {
        if (isConnectionActive) {
            display.textContent = name;
            display.classList.remove('disconnected');
        } else {
            display.textContent = `[${name}] - disconnected`;
            display.classList.add('disconnected');
        }
        display.title = display.textContent;
    }

    updateDocumentTitle();
}

/**
 * Update browser tab title with hostname and connection state
 */
function updateDocumentTitle() {
    const name = lastKnownHostname || '—';
    if (isConnectionActive) {
        document.title = `${name} | ESS Control`;
    } else {
        document.title = `${name} | ESS Control (disconnected)`;
    }
}

/**
 * Initialize hostname display from system/hostname datapoint
 */
function initHostnameDisplay() {
    const display = document.getElementById('hostname-display');
    if (!display) return;

    dpManager.subscribe('system/hostname', (data) => {
        const raw = data.value ?? data.data ?? '';
        if (raw) lastKnownHostname = formatHostname(String(raw));
        updateHostnameDisplay();
    });

    updateHostnameDisplay();
    log('Hostname display initialized', 'info');
}

/**
 * Parse powermon/charging datapoint value
 */
function parseBatteryCharging(raw) {
    const value = String(raw ?? '').toLowerCase();
    return value === '1' || value === 'true';
}

/**
 * Parse numeric powermon datapoint value
 */
function parseBatteryMetric(raw) {
    const parsed = parseFloat(raw);
    return Number.isNaN(parsed) ? null : parsed;
}

/**
 * Build battery tooltip with charge state and power metrics
 */
function formatBatteryTooltip() {
    const parts = [];

    if (batteryPct != null) {
        const pct = Math.round(Math.max(0, Math.min(100, batteryPct)));
        parts.push(batteryCharging ? `${pct}% (charging)` : `${pct}%`);
    } else if (batteryCharging) {
        parts.push('(charging)');
    }

    if (batteryHrsRemaining != null) {
        parts.push(`${batteryHrsRemaining.toFixed(1)} hrs remaining`);
    }

    const metrics = [];
    if (batteryV != null) metrics.push(`${batteryV.toFixed(1)} V`);
    if (batteryA != null) metrics.push(`${batteryA.toFixed(2)} A`);
    if (batteryW != null) metrics.push(`${batteryW.toFixed(2)} W`);
    if (metrics.length) parts.push(metrics.join(' · '));

    return parts.join(' · ') || '';
}

/**
 * Update battery indicator display
 */
function updateBatteryDisplay() {
    const container = document.getElementById('battery-status');
    if (!container) return;

    if (batteryPct == null && batteryCharging == null) {
        container.hidden = true;
        return;
    }

    container.hidden = false;

    const pct = batteryPct != null ? Math.max(0, Math.min(100, batteryPct)) : 0;
    container.style.setProperty('--battery-pct', pct);
    container.classList.toggle('charging', !!batteryCharging);
    container.classList.toggle('low', pct <= 20 && !batteryCharging);
    container.classList.toggle('critical', pct <= 10 && !batteryCharging);

    container.title = formatBatteryTooltip();
}

/**
 * Initialize battery indicator from powermon datapoints
 */
function initBatteryIndicator() {
    const container = document.getElementById('battery-status');
    if (!container) return;

    dpManager.subscribe('powermon/pct', (data) => {
        const raw = data.value ?? data.data;
        const parsed = parseFloat(raw);
        if (!Number.isNaN(parsed)) {
            batteryPct = parsed;
            hasBatteryData = true;
        }
        updateBatteryDisplay();
    });

    dpManager.subscribe('powermon/charging', (data) => {
        const raw = data.value ?? data.data;
        if (raw !== '' && raw != null) {
            batteryCharging = parseBatteryCharging(raw);
            hasBatteryData = true;
        }
        updateBatteryDisplay();
    });

    dpManager.subscribe('powermon/hrs_remaining', (data) => {
        const parsed = parseBatteryMetric(data.value ?? data.data);
        if (parsed != null) {
            batteryHrsRemaining = parsed;
            hasBatteryData = true;
        }
        updateBatteryDisplay();
    });

    dpManager.subscribe('powermon/v', (data) => {
        const parsed = parseBatteryMetric(data.value ?? data.data);
        if (parsed != null) {
            batteryV = parsed;
            hasBatteryData = true;
        }
        updateBatteryDisplay();
    });

    dpManager.subscribe('powermon/a', (data) => {
        const parsed = parseBatteryMetric(data.value ?? data.data);
        if (parsed != null) {
            batteryA = parsed;
            hasBatteryData = true;
        }
        updateBatteryDisplay();
    });

    dpManager.subscribe('powermon/w', (data) => {
        const parsed = parseBatteryMetric(data.value ?? data.data);
        if (parsed != null) {
            batteryW = parsed;
            hasBatteryData = true;
        }
        updateBatteryDisplay();
    });

    updateBatteryDisplay();
    log('Battery indicator initialized', 'info');
}

/**
 * Parse juicer juice_level string for low sensor state
 */
function isJuiceLevelLow(level) {
    const value = String(level ?? '').trim();
    if (!value) return false;
    return value.startsWith('<') || value.includes('<50');
}

/**
 * Estimate remaining juice from device cumulative reward_mls
 */
function getJuiceRemainingMl() {
    if (juiceRewardMls == null) return null;
    return Math.max(0, BOTTLE_CAPACITY_ML - juiceRewardMls);
}

/**
 * Remaining volume for display; sensor low wins over inflated estimate
 */
function getJuiceDisplayRemainingMl() {
    const inferred = getJuiceRemainingMl();
    const sensorLow = isJuiceLevelLow(juiceLevel);

    if (sensorLow && inferred != null && inferred >= JUICE_LOW_THRESHOLD_ML) {
        return JUICE_LOW_THRESHOLD_ML - 1;
    }
    if (sensorLow && inferred == null) {
        return JUICE_LOW_THRESHOLD_ML - 1;
    }
    return inferred;
}

/**
 * Build juice indicator tooltip
 */
function formatJuiceTooltip() {
    const inferred = getJuiceRemainingMl();
    const displayRemainingMl = getJuiceDisplayRemainingMl();
    const parts = [];

    if (displayRemainingMl != null) {
        parts.push(`Juice: ${Math.round(displayRemainingMl)} mL remaining`);
    } else {
        parts.push('Juice');
    }

    if (juiceLevel) parts.push(`${juiceLevel} sensor`);
    if (juiceRewardNumber != null) parts.push(`${juiceRewardNumber} rewards`);
    if (juiceRewardMls != null) {
        parts.push(`${juiceRewardMls.toFixed(1)} mL dispensed`);
    }
    if (isJuiceLevelLow(juiceLevel) && inferred != null && inferred >= JUICE_LOW_THRESHOLD_ML) {
        parts.push('low sensor overrides estimate');
    }

    return parts.join(' · ');
}

/**
 * Update juice bottle indicator display
 */
function updateJuiceDisplay() {
    const container = document.getElementById('juice-status');
    if (!container) return;

    if (!hasJuiceData) {
        container.hidden = true;
        return;
    }

    container.hidden = false;

    const displayRemainingMl = getJuiceDisplayRemainingMl();
    const pct = displayRemainingMl != null
        ? Math.max(0, Math.min(100, (displayRemainingMl / BOTTLE_CAPACITY_ML) * 100))
        : (isJuiceLevelLow(juiceLevel) ? 10 : 100);

    container.style.setProperty('--juice-pct', pct);

    const isLow = isJuiceLevelLow(juiceLevel)
        || (displayRemainingMl != null && displayRemainingMl < JUICE_LOW_THRESHOLD_ML);
    container.classList.toggle('low', isLow);

    container.title = formatJuiceTooltip();
}

/**
 * Initialize juice indicator from juicer datapoints
 */
function initJuiceIndicator() {
    const container = document.getElementById('juice-status');
    if (!container) return;

    dpManager.subscribe('juicer/juice_level', (data) => {
        const raw = data.value ?? data.data;
        if (raw !== '' && raw != null) {
            juiceLevel = String(raw);
            hasJuiceData = true;
        }
        updateJuiceDisplay();
    });

    dpManager.subscribe('juicer/reward_mls', (data) => {
        const parsed = parseFloat(data.value ?? data.data);
        if (!Number.isNaN(parsed)) {
            juiceRewardMls = parsed;
            hasJuiceData = true;
        }
        updateJuiceDisplay();
    });

    dpManager.subscribe('juicer/reward_number', (data) => {
        const parsed = parseInt(data.value ?? data.data, 10);
        if (!Number.isNaN(parsed)) {
            juiceRewardNumber = parsed;
            hasJuiceData = true;
        }
        updateJuiceDisplay();
    });

    updateJuiceDisplay();
    log('Juice indicator initialized', 'info');
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

    isConnectionActive = (status === 'connected');
    updateHostnameDisplay();
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
 * Sync scripts by calling ess::sync_base
 */
function syncScripts() {
    if (connection && connection.ws && connection.connected) {
        const message = { cmd: 'eval', script: 'ess::sync_base' };
        connection.ws.send(JSON.stringify(message));
        log('Syncing scripts (ess::sync_base)...', 'info');
    } else {
        log('Cannot sync: not connected to dserv', 'error');
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
                guiPath: '/ess_control.html',
                dpManager: dpManager
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
