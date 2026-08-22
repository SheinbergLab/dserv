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
let joystickControls = null;
let dialControls = null;
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

    // ?host=<host[:port]> -> connect to a different dserv than the one
    // serving this page (mirrors extio.html). Lets a dev copy of the GUI
    // (e.g. python -m http.server in the repo www/) drive a live dserv
    // with no install step. ?ssl=1|0 overrides the ws/wss choice, needed
    // when the page's protocol differs from the target dserv's (an http-
    // served dev page talking to the SSL dserv on 2565 needs ssl=1).
    const qs = new URLSearchParams(location.search);
    const qsHost = qs.get('host');
    const qsSsl = qs.get('ssl');

    // Create WebSocket connection
    connection = new DservConnection({
        subprocess: 'ess',
        dservHost: qsHost || null,
        forceSecure: qsSsl !== null ? qsSsl === '1' : undefined,
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
        initEyeSettings();
        initButtonControls();
        initJoystickControls();
        initDialControls();
        initSliderControls();
	initProjectSelector();
        initOpenEphysStatus();
        initStimStatus();
        initHostnameDisplay();
        initJuiceIndicator();
        initBatteryIndicator();
        initNetStatusIndicator();
        initSyncDirtyBadge();
        initInterpTracking();

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
          ess/load_error ess/last_good_system
          ess/sound/feedback_volume ess/sound/master_gain
          ess/obs_id ess/obs_total ess/in_obs
          ess/block_pct_complete ess/block_pct_correct
          ess/screen_w ess/screen_h ess/screen_halfx ess/screen_halfy
          ess/params ess/datafile ess/sortby_columns ess/block_id
          ess/buttons/channels ess/slider_active slider/settings
          ess/joystick_active ess/joystick/dir ess/joystick/response
          ess/dial_active ess/dial/geometry ess/dial/sources
          ess/dial/source_origin ess/dial/bound ess/dial/pointer
          ess/session_stats
          em/settings em/source_active mesh/peers
          openephys/status
          ess/rmt_connected ess/rmt_host ess/stim_required
          system/hostname system/hostaddr system/os
          system/net/state system/net/type system/net/iface system/net/ip
          system/net/wifi/ssid system/net/wifi/bssid
          system/net/wifi/signal_dbm system/net/wifi/bars
          powermon/pct powermon/charging powermon/hrs_remaining powermon/v powermon/a powermon/w
          juicer/juice_level juicer/reward_mls juicer/reward_number
          juicer/backend juicer/target juicer/error juicer/ms_per_ml
          juicer/hand_ml
          configs/list configs/tags configs/quick_picks configs/current
          configs/remote_servers
          queues/list queues/state queues/items
          projects/list projects/active projects/active_detail
          ess/registry/url ess/registry/workgroup ess/registry/sync_status
          dserv/interps
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
 * Initialize virtual joystick (D-pad) controls
 */
function initJoystickControls() {
    if (typeof JoystickControls !== 'undefined') {
        joystickControls = new JoystickControls(dpManager);
        log('Joystick Controls initialized', 'info');
    }
}

/**
 * Initialize the dial panel (shown when a system calls ::ess::dial_init)
 */
function initDialControls() {
    if (typeof DialControls !== 'undefined') {
        dialControls = new DialControls(dpManager);
        log('Dial Controls initialized', 'info');
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
 * Stim (stim2 display) connection indicator state
 */
let stimConnected = null;      // null = not yet reported
let stimHost = '';
let stimRequired = false;
let localHostname = '';        // system/hostname, raw (not the display form)
let localHostaddr = '';        // system/hostaddr: this box's LAN IPv4

/**
 * Host spellings that can only ever mean "the machine dserv runs on".
 */
const LOCAL_HOST_ALIASES = new Set([
    '', 'localhost', 'localhost.localdomain', '127.0.0.1', '0.0.0.0',
    '::1', '[::1]'
]);

/**
 * Canonical form for comparing two spellings of a host.
 *
 * Drops the port, case, a trailing root dot, and the mDNS `.local` suffix, so
 * `Rig1.local:4610`, `rig1` and `RIG1.` all collapse together.
 */
function canonicalHost(host) {
    return PageNav.stripPort(host)
        .toLowerCase()
        .replace(/\.$/, '')
        .replace(/\.local$/, '');
}

/**
 * Is the stimulus display on a different machine than dserv?
 *
 * A split rig points ess/rmt_host at another box; a self-contained one leaves
 * it at `localhost` (the ess-2.0.tm default) -- but not always, since a rig may
 * name itself by hostname or by its own LAN IP. So "local" is anything that
 * matches a loopback alias, system/hostname, system/hostaddr, or the host this
 * page is talking to. Ambiguity resolves to local, i.e. to offering nothing:
 * a missing button is a smaller wrong than one pointing at the wrong machine.
 */
function stimHostIsRemote() {
    const stim = canonicalHost(stimHost);
    if (LOCAL_HOST_ALIASES.has(stim)) return false;

    return ![localHostname, localHostaddr, PageNav.dservHostname()]
        .filter(Boolean)
        .some(h => canonicalHost(h) === stim);
}

/**
 * Update the stim connection chip in the status bar
 *
 * Deliberately stays visible when disconnected rather than hiding like the
 * OpenEphys chip: an absent display is exactly the condition this exists to
 * make impossible to miss on a deployed rig. It only hides before the first
 * report, when we genuinely don't know.
 */
function updateStimStatus() {
    const container = document.getElementById('stim-status');
    if (!container) return;

    if (stimConnected === null) {
        container.hidden = true;
        return;
    }

    const where = stimHost ? ` ${stimHost}` : '';
    container.hidden = false;
    // host lives in its own span so CSS can drop it on a narrow bar while
    // keeping the part that matters
    container.innerHTML = '<div class="ess-stim-dot"></div>' +
                          '<span class="ess-stim-label"></span>' +
                          '<span class="ess-stim-host"></span>';
    const label = container.querySelector('.ess-stim-label');
    const hostEl = container.querySelector('.ess-stim-host');

    if (stimConnected) {
        container.className = 'ess-stim-status connected';
        label.textContent = 'STIM';
        hostEl.textContent = stimHost || '';
        container.title = `Stimulus display connected${where ? ` (${stimHost})` : ''}`;
    } else {
        container.className = stimRequired
            ? 'ess-stim-status disconnected required'
            : 'ess-stim-status disconnected';
        label.textContent = 'STIM: not connected';
        // Name the dark machine when it is not this one. Locally it would only
        // ever say "localhost", but on a split rig it is the box you have to go
        // fix -- and, below, the one the chip hands you a panel for.
        if (stimHostIsRemote()) hostEl.textContent = stimHost;
        container.title = stimRequired
            ? `No stimulus display at ${stimHost || 'configured host'} — stim is REQUIRED, so the system will refuse to start`
            : `No stimulus display at ${stimHost || 'configured host'} — stimulus commands are being silently discarded`;
    }

    // On a split rig the chip is the only thing on this page that names the
    // display machine, so it is where its "Manage System" panel belongs -- the
    // hostname display next to it manages dserv's box, not this one.
    //
    // Deliberately NOT gated on stimConnected. A dark remote display is the
    // case where you most want to reach across and restart stim2, and
    // ess/rmt_host is configuration, so it still names the box while the link
    // is down.
    const remote = stimHostIsRemote();
    container.classList.toggle('clickable', remote);
    if (remote) {
        container.setAttribute('role', 'button');
        container.setAttribute('tabindex', '0');
        container.title += ` — click to manage ${PageNav.stripPort(stimHost)}`;
    } else {
        container.removeAttribute('role');
        container.removeAttribute('tabindex');
    }
}

/**
 * Initialize stim connection indicator from ess/rmt_connected
 *
 * ess/rmt_connected is published by the rmt module itself on every transition
 * (open, close, failed I/O, peer EOF), so this chip tracks the live link and
 * not just what was true when the system loaded.
 */
function initStimStatus() {
    const container = document.getElementById('stim-status');
    if (!container) return;

    // Listeners live on the container, which survives updateStimStatus's
    // innerHTML rewrite; openStimManagement re-checks remoteness itself, so a
    // chip that has gone back to local can't be clicked through to nowhere.
    container.addEventListener('click', openStimManagement);
    container.addEventListener('keydown', (e) => {
        if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            openStimManagement();
        }
    });

    // Local identity: what "not the host" is measured against. Both are set
    // once at dserv startup (dsconf.tcl set_hostinfo), so no re-render race.
    dpManager.subscribe('system/hostname', (data) => {
        localHostname = String(data.value ?? data.data ?? '');
        updateStimStatus();
    });

    dpManager.subscribe('system/hostaddr', (data) => {
        localHostaddr = String(data.value ?? data.data ?? '');
        updateStimStatus();
    });

    dpManager.subscribe('ess/rmt_connected', (data) => {
        const raw = data.value ?? data.data ?? '';
        stimConnected = parseInt(raw, 10) > 0;
        updateStimStatus();
    });

    dpManager.subscribe('ess/rmt_host', (data) => {
        stimHost = String(data.value ?? data.data ?? '');
        updateStimStatus();
    });

    dpManager.subscribe('ess/stim_required', (data) => {
        stimRequired = parseInt(data.value ?? data.data ?? '', 10) > 0;
        updateStimStatus();
    });

    log('Stim status indicator initialized', 'info');
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
        display.title = `${display.textContent} — click to manage this system`;
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

    // The hostname names the machine you're driving, so clicking it opens
    // dserv-agent's management panel for that same machine (update software,
    // restart services, reboot). Stays live while disconnected -- that is
    // often exactly when you want it.
    display.classList.add('clickable');
    display.setAttribute('role', 'button');
    display.setAttribute('tabindex', '0');
    display.addEventListener('click', openSystemManagement);
    display.addEventListener('keydown', (e) => {
        if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            openSystemManagement();
        }
    });

    updateHostnameDisplay();
    log('Hostname display initialized', 'info');
}

/**
 * Open dserv-agent's management panel for the machine this page drives.
 *
 * A popup rather than an embed: dserv-agent serves plain http on :80
 * (--no-tls in dserv-agent.service), so an HTTPS dserv page cannot iframe it.
 * Named per host so re-clicking focuses the existing window.
 */
function openSystemManagement() {
    window.open(
        PageNav.agentPanelUrl(),
        `agent_${PageNav.dservHostname()}`,
        'width=820,height=900,menubar=no,toolbar=no,location=no,status=no,resizable=yes,scrollbars=yes'
    );
}

/**
 * Open dserv-agent's management panel for the remote stimulus display.
 *
 * Same panel, same popup geometry as openSystemManagement -- only the machine
 * differs -- and named per host, so the stim window and the dserv window are
 * two distinct popups rather than one that keeps getting reused.
 *
 * No-op unless the display really is on another box: the local case is already
 * covered by the hostname display, which shows stim2's own version and update
 * state along with everything else on that machine.
 */
function openStimManagement() {
    if (!stimHostIsRemote()) return;
    const host = PageNav.stripPort(stimHost);
    window.open(
        PageNav.agentPanelUrl(host),
        `agent_${host}`,
        'width=820,height=900,menubar=no,toolbar=no,location=no,status=no,resizable=yes,scrollbars=yes'
    );
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

let netState = null;
let netType = null;
let netIface = null;
let netIp = null;
let netWifiSsid = null;
let netWifiBssid = null;
let netWifiSignalDbm = null;
let netWifiBars = null;

/**
 * Build network status tooltip (wifi: ssid/bssid/signal; always iface + IP)
 */
function formatNetTooltip() {
    const parts = [];
    if (netState === 'down') parts.push('link down');
    if (netType === 'wifi') {
        parts.push('Wi-Fi');
        if (netWifiSsid) parts.push(netWifiSsid);
        if (netWifiBssid) parts.push(netWifiBssid);
        if (netWifiSignalDbm != null) parts.push(`${netWifiSignalDbm} dBm`);
    } else if (netType === 'ethernet') {
        parts.push('Ethernet');
    }
    if (netIface) parts.push(netIface);
    if (netIp) parts.push(netIp);
    return parts.join(' · ') || '';
}

/**
 * Update network status icon (wifi vs ethernet, down-slash) for
 * the registry path published by netmon as system/net/*
 */
function updateNetStatusDisplay() {
    const container = document.getElementById('net-status');
    if (!container) return;

    const known = netType === 'wifi' || netType === 'ethernet';
    container.hidden = !known;
    container.classList.toggle('is-wifi', netType === 'wifi');
    container.classList.toggle('is-ethernet', netType === 'ethernet');
    container.classList.toggle('is-down', netState === 'down');

    for (let i = 0; i <= 4; i++) {
        container.classList.remove(`signal-${i}`);
    }
    if (netType === 'wifi' && netState !== 'down') {
        const bars = netWifiBars != null ? netWifiBars : 0;
        container.classList.add(`signal-${bars}`);
    }

    container.title = known
        ? `${formatNetTooltip()} (click for details)`
        : '';
}

/**
 * Initialize network status indicator from system/net/* datapoints
 */
function initNetStatusIndicator() {
    const container = document.getElementById('net-status');
    if (!container) return;

    dpManager.subscribe('system/net/state', (data) => {
        const raw = String(data.value ?? data.data ?? '').trim().toLowerCase();
        netState = (raw === 'up' || raw === 'down') ? raw : null;
        updateNetStatusDisplay();
    });

    dpManager.subscribe('system/net/type', (data) => {
        const raw = String(data.value ?? data.data ?? '').trim().toLowerCase();
        netType = (raw === 'wifi' || raw === 'ethernet') ? raw : null;
        updateNetStatusDisplay();
    });

    dpManager.subscribe('system/net/iface', (data) => {
        const raw = String(data.value ?? data.data ?? '').trim();
        netIface = raw || null;
        updateNetStatusDisplay();
    });

    dpManager.subscribe('system/net/ip', (data) => {
        const raw = String(data.value ?? data.data ?? '').trim();
        netIp = raw || null;
        updateNetStatusDisplay();
    });

    dpManager.subscribe('system/net/wifi/ssid', (data) => {
        const raw = String(data.value ?? data.data ?? '').trim();
        netWifiSsid = raw || null;
        updateNetStatusDisplay();
    });

    dpManager.subscribe('system/net/wifi/bssid', (data) => {
        const raw = String(data.value ?? data.data ?? '').trim();
        netWifiBssid = raw || null;
        updateNetStatusDisplay();
    });

    dpManager.subscribe('system/net/wifi/signal_dbm', (data) => {
        const raw = String(data.value ?? data.data ?? '').trim();
        if (raw === '') {
            netWifiSignalDbm = null;
        } else {
            const parsed = parseInt(raw, 10);
            netWifiSignalDbm = Number.isNaN(parsed) ? null : parsed;
        }
        updateNetStatusDisplay();
    });

    dpManager.subscribe('system/net/wifi/bars', (data) => {
        const raw = String(data.value ?? data.data ?? '').trim();
        if (raw === '') {
            netWifiBars = null;
        } else {
            const parsed = parseInt(raw, 10);
            netWifiBars = Number.isNaN(parsed) ? null : Math.max(0, Math.min(4, parsed));
        }
        updateNetStatusDisplay();
    });

    updateNetStatusDisplay();

    container.addEventListener('click', () => openNetStatusModal());
    container.setAttribute('role', 'button');
    container.tabIndex = 0;
    container.addEventListener('keydown', (e) => {
        if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            openNetStatusModal();
        }
    });

    log('Network status indicator initialized', 'info');
}

let netStatusModal = null;

/**
 * Open network status modal (interfaces; APs behind an explicit Scan)
 */
function openNetStatusModal() {
    if (!connection || !connection.connected) {
        log('Cannot open network status: not connected to dserv', 'error');
        return;
    }
    if (!netStatusModal) {
        netStatusModal = new NetStatusModal({ connection, dpManager, log });
    }
    netStatusModal.open();
}

let settingsModal = null;

/**
 * Open the rig settings gear (every knob this rig DECLARES, rendered from
 * its own schema — see SettingsModal.js). `section` opens straight at one
 * subsystem, so a panel with its own dialog can hand off here for the rest.
 */
function openSettingsModal(section = null) {
    if (!connection || !connection.connected) {
        log('Cannot open settings: not connected to dserv', 'error');
        return;
    }
    if (!settingsModal) {
        settingsModal = new SettingsModal({ connection, dpManager, log });
    }
    settingsModal.open(section);
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
 * Unpushed-changes badge on the Sync Tasks button, fed by scripts/dirty
 * (a purely local scan in the scripts subprocess: files differing from
 * their .sync_base entries plus canonical local-new files). Refreshed
 * after sync/push operations and every few minutes by the subprocess.
 */
function initSyncDirtyBadge() {
    const btn = document.getElementById('sync-btn');
    if (!btn || !dpManager) return;

    let badge = document.getElementById('sync-dirty-badge');
    if (!badge) {
        badge = document.createElement('span');
        badge.id = 'sync-dirty-badge';
        badge.hidden = true;
        btn.appendChild(badge);
    }

    dpManager.subscribe('scripts/dirty', (data) => {
        try {
            const d = JSON.parse(data.value !== undefined ? data.value : data.data);
            const n = d.count || 0;
            badge.textContent = n;
            badge.hidden = n === 0;
            btn.title = n === 0
                ? 'Sync tasks with the cloud (pull / push)'
                : `${n} unpushed local change(s):\n${(d.files || []).join('\n')}`;
        } catch (e) { /* ignore malformed */ }
    });

    // Subscriptions only fire on change — ask for the current value.
    if (connection?.evalAsync) {
        connection.evalAsync('catch {dservTouch scripts/dirty}').catch(() => {});
    }
}

/**
 * Open the clone dialog (the + next to the System / Protocol selectors):
 * local-first creation of a new protocol or system by cloning an
 * existing one with the identifier renamed.
 */
let cloneModal = null;
function cloneScriptDialog(mode) {
    if (!connection || !connection.connected) {
        log('Cannot clone: not connected to dserv', 'error');
        return;
    }
    if (!cloneModal) {
        cloneModal = new CloneModal({ connection, essControl, log });
    }
    cloneModal.open(mode);
}

/**
 * Open the option-list editor for one loader parameter (the ✎ next to
 * each dropdown in the Variant Options panel). Edits the variants file
 * surgically, tests via ess::test_loader, and reloads on save.
 */
let variantOptionsModal = null;
function editVariantOptions(argName) {
    if (!connection || !connection.connected) {
        log('Cannot edit options: not connected to dserv', 'error');
        return;
    }
    if (!variantOptionsModal) {
        variantOptionsModal = new VariantOptionsModal({ connection, dpManager, essControl, log });
    }
    variantOptionsModal.open(argName);
}

/**
 * Open the Cloud Sync dialog (SyncModal.js), backed by the `scripts`
 * subprocess: workgroup-wide pull/push previews with 3-way status,
 * per-file diffs, version history, and pull/push actions.
 */
let syncModal = null;
function syncScripts() {
    if (!connection || !connection.connected) {
        log('Cannot sync: not connected to dserv', 'error');
        return;
    }
    if (!syncModal) {
        syncModal = new SyncModal({ connection, dpManager, essControl, log });
    }
    syncModal.open();
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

let tclTerminal = null;
let terminalInterps = 'dserv';

/**
 * Track available subprocess interpreters for the terminal's /name
 * switching. Subscribed at startup (before the initial-data touch)
 * because the terminal itself is created lazily on first tab switch.
 */
function initInterpTracking() {
    dpManager.subscribe('dserv/interps', (data) => {
        terminalInterps = data.data !== undefined ? data.data : data.value;
        if (tclTerminal) {
            tclTerminal.updateAvailableInterps(terminalInterps);
        }
    });
}
let activeBottomTab = 'console';

// Initialize when DOM is ready
document.addEventListener('DOMContentLoaded', init);

/*
 * The mesh peer dropdown (a picker that opened another host's ess_control)
 * lived here. Removed from this page: peers are advertised with addresses that
 * are frequently unroutable from wherever the browser happens to be -- another
 * subnet, a private rig network, a different site -- so most entries led to a
 * page that could not load. A menu whose items are expected to fail is worse
 * than no menu. mesh/peers itself is still used, by ESSControl's remote-config
 * server list, and js/mesh_dropdown.js is still in the tree for other pages.
 */

// Initialize Tcl terminal
function initTerminal() {
    if (connection && !tclTerminal) {
        try {
            tclTerminal = new TclTerminal('tcl-terminal-container', connection, {
                interpreter: 'dserv',
                useLinkedSubprocess: false,
                showWelcome: true,
                welcomeMessage: 'dserv Terminal - Type "help" for assistance, /name to target a subprocess'
            });
            tclTerminal.updateAvailableInterps(terminalInterps);
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
