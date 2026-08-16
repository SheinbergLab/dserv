/**
 * ESS Workbench - Main Application Script
 *
 * The workbench is an INSPECTOR: it reads and explains the loaded
 * system (dashboard, state map, loader dry-runs, script browsing with
 * history/diffs) and edits nothing. Editing happens in files — usually
 * with an LLM in the loop — and syncs through ess_control's Sync modal.
 * (The in-page editors, per-user overlay, and old registry sync UI were
 * retired 2026-08.)
 *
 * Handles:
 * - Plugin registration and lifecycle hooks
 * - WebSocket connection and snapshot subscription
 * - Tab navigation and dashboard rendering
 * - The loader dry-run lab (ess::test_loader)
 *
 * Plugin Lifecycle Hooks (called in registration order):
 *   onInit(wb)                — after core init, before connect()
 *   onConnected(wb)           — WebSocket connected
 *   onDisconnected(wb)        — WebSocket disconnected
 *   onSnapshot(wb, snapshot)  — after snapshot is parsed and core UI updated
 *   onTabSwitch(wb, tabName)  — after tab switch completes
 */


// ==========================================
// SHA-256 Utility
// ==========================================

async function sha256(text) {
    const encoder = new TextEncoder();
    const data = encoder.encode(text);
    
    if (typeof crypto !== 'undefined' && crypto.subtle) {
        const hashBuffer = await crypto.subtle.digest('SHA-256', data);
        const hashArray = Array.from(new Uint8Array(hashBuffer));
        return hashArray.map(b => b.toString(16).padStart(2, '0')).join('');
    }
    
    return sha256_fallback(data);
}

function sha256_fallback(data) {
    const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
    
    const K = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    ];
    
    let h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
    let h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;
    
    const bitLen = bytes.length * 8;
    const padded = new Uint8Array(Math.ceil((bytes.length + 9) / 64) * 64);
    padded.set(bytes);
    padded[bytes.length] = 0x80;
    
    const view = new DataView(padded.buffer);
    view.setUint32(padded.length - 4, bitLen, false);
    
    const rotr = (x, n) => ((x >>> n) | (x << (32 - n))) >>> 0;
    const ch = (x, y, z) => ((x & y) ^ (~x & z)) >>> 0;
    const maj = (x, y, z) => ((x & y) ^ (x & z) ^ (y & z)) >>> 0;
    const sigma0 = x => (rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22)) >>> 0;
    const sigma1 = x => (rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25)) >>> 0;
    const gamma0 = x => (rotr(x, 7) ^ rotr(x, 18) ^ (x >>> 3)) >>> 0;
    const gamma1 = x => (rotr(x, 17) ^ rotr(x, 19) ^ (x >>> 10)) >>> 0;
    
    for (let i = 0; i < padded.length; i += 64) {
        const w = new Uint32Array(64);
        for (let j = 0; j < 16; j++) {
            w[j] = view.getUint32(i + j * 4, false);
        }
        for (let j = 16; j < 64; j++) {
            w[j] = (gamma1(w[j-2]) + w[j-7] + gamma0(w[j-15]) + w[j-16]) >>> 0;
        }
        
        let a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
        
        for (let j = 0; j < 64; j++) {
            const t1 = (h + sigma1(e) + ch(e, f, g) + K[j] + w[j]) >>> 0;
            const t2 = (sigma0(a) + maj(a, b, c)) >>> 0;
            h = g; g = f; f = e;
            e = (d + t1) >>> 0;
            d = c; c = b; b = a;
            a = (t1 + t2) >>> 0;
        }
        
        h0 = (h0 + a) >>> 0; h1 = (h1 + b) >>> 0;
        h2 = (h2 + c) >>> 0; h3 = (h3 + d) >>> 0;
        h4 = (h4 + e) >>> 0; h5 = (h5 + f) >>> 0;
        h6 = (h6 + g) >>> 0; h7 = (h7 + h) >>> 0;
    }
    
    const toHex = n => n.toString(16).padStart(8, '0');
    return toHex(h0) + toHex(h1) + toHex(h2) + toHex(h3) +
           toHex(h4) + toHex(h5) + toHex(h6) + toHex(h7);
}    


// ==========================================
// ESSWorkbench Class
// ==========================================

class ESSWorkbench {
    constructor() {
        // Plugin registry
        this._plugins = [];
        
        // Connection state
        this.connection = null;
        this.snapshot = null;
        this.autoReload = true;
        
        // UI state
        this.currentTab = 'dashboard';
        this.currentScript = 'system';
        this.editor = null;
        this.scripts = {};
        
        // DOM references
        this.elements = {};
        
        // Initialize
        this.init();
    }
    
    // ==========================================
    // Plugin System
    // ==========================================
    
    /**
     * Register a plugin. Call before instantiation (collected statically)
     * or during init. Plugins are objects with optional lifecycle methods.
     */
    static registerPlugin(plugin) {
        if (!ESSWorkbench._pendingPlugins) {
            ESSWorkbench._pendingPlugins = [];
        }
        ESSWorkbench._pendingPlugins.push(plugin);
    }
    
    /**
     * Call a lifecycle hook on all plugins.
     * If any plugin returns false, returns false (used for override hooks).
     */
    _pluginHook(hookName, ...args) {
        for (const plugin of this._plugins) {
            if (typeof plugin[hookName] === 'function') {
                const result = plugin[hookName](this, ...args);
                if (result === false) return false;
            }
        }
        return true;
    }
    
    /**
     * Async version of _pluginHook
     */
    async _pluginHookAsync(hookName, ...args) {
        for (const plugin of this._plugins) {
            if (typeof plugin[hookName] === 'function') {
                const result = await plugin[hookName](this, ...args);
                if (result === false) return false;
            }
        }
        return true;
    }
    
    // ==========================================
    // Initialization
    // ==========================================
    
    init() {
        // Collect statically-registered plugins
        if (ESSWorkbench._pendingPlugins) {
            this._plugins = [...ESSWorkbench._pendingPlugins];
        }
        
        this.cacheElements();
        this.bindEvents();
        this.startClock();
        this.initLoadersEditor();

        // Let plugins initialize (before connect, so they can bind events)
        this._pluginHook('onInit');
        
        this.connect();
    }
    
    cacheElements() {
        this.elements = {
            // Connection
            connectionIndicator: document.getElementById('connection-indicator'),
            
            // Config display
            configStatus: document.getElementById('config-status'),
            cfgProject: document.getElementById('cfg-project'),
            cfgSystem: document.getElementById('cfg-system'),
            cfgProtocol: document.getElementById('cfg-protocol'),
            cfgVariant: document.getElementById('cfg-variant'),
            cfgVersion: document.getElementById('cfg-version'),
            cfgSubject: document.getElementById('cfg-subject'),
            
            // Stats bar
            statStates: document.getElementById('stat-states'),
            statParams: document.getElementById('stat-params'),
            statLoaders: document.getElementById('stat-loaders'),
            statVariants: document.getElementById('stat-variants'),

            // Footer
            snapshotTime: document.getElementById('snapshot-time'),
            clock: document.getElementById('clock')
        };
    }
    
    bindEvents() {
        // Tab navigation
        document.querySelectorAll('.tab-btn').forEach(btn => {
            btn.addEventListener('click', () => {
                this.switchTab(btn.dataset.tab);
            });
        });
        
    }
    
    // ==========================================
    // Connection Management
    // ==========================================
    
    connect() {
        this.updateConnectionStatus('connecting');

        // Zero-deploy dev loop: ?host=<host[:port]> (and ?ssl=1|0 when the
        // page's protocol differs from the target dserv's) point a
        // statically served copy of this page at a live dserv.
        const qs = new URLSearchParams(location.search);
        const qsHost = qs.get('host');
        const qsSsl = qs.get('ssl');

        this.connection = new DservConnection({
            subprocess: 'ess',
            dservHost: qsHost || null,
            forceSecure: qsSsl !== null ? qsSsl === '1' : undefined,
            autoReconnect: true,
            onStatus: (status, message) => {
                this.updateConnectionStatus(status);
                console.log(`Connection: ${status} - ${message}`);
            },
            onError: (error) => {
                console.error('Connection error:', error);
            }
        });
        
        // Create DatapointManager
        this.dpManager = new DatapointManager(this.connection, {
            autoGetKeys: false
        });
        
        // Setup event handlers
        this.connection.on('connected', () => {
            this.updateConnectionStatus('connected');
            this.subscribeToSnapshot();
            this._pluginHook('onConnected');
        });
        
        this.connection.on('disconnected', () => {
            this.updateConnectionStatus('disconnected');
            this._pluginHook('onDisconnected');
        });
        
        // Connect
        this.connection.connect().catch(err => {
            console.error('Failed to connect:', err);
        });
    }
    
    updateConnectionStatus(status) {
        const indicator = this.elements.connectionIndicator;
        if (!indicator) return;
        
        indicator.className = 'connection-indicator';
        const text = indicator.querySelector('.indicator-text');
        
        switch (status) {
            case 'connected':
                indicator.classList.add('connected');
                if (text) text.textContent = 'Connected';
                break;
            case 'connecting':
                indicator.classList.add('connecting');
                if (text) text.textContent = 'Connecting...';
                break;
            default:
                if (text) text.textContent = 'Disconnected';
        }
    }
    
    subscribeToSnapshot() {
        if (!this.dpManager) {
            console.error('No dpManager available');
            return;
        }
        
        console.log('Subscribing to ess/snapshot...');
        
        this.dpManager.subscribe('ess/snapshot', (data) => {
            const value = data.data !== undefined ? data.data : data.value;
            this.handleSnapshot(value);
        });
        
        // Touch to trigger initial value
        if (this.connection?.ws) {
            this.connection.ws.send(JSON.stringify({
                cmd: 'touch',
                name: 'ess/snapshot'
            }));
        }
    }
    
    // ==========================================
    // Snapshot Handling
    // ==========================================
    
    handleSnapshot(data) {
        try {
            const snapshot = typeof data === 'string' ? JSON.parse(data) : data;
            
            if (!snapshot) {
                console.warn('Snapshot is null/undefined');
                return;
            }
            
            if (snapshot.error) {
                console.warn('Snapshot error:', snapshot.error);
                this.clearDashboard();
                return;
            }
            
            this.snapshot = snapshot;
            
            this.scripts = {
                system: snapshot.script_system || '',
                protocol: snapshot.script_protocol || '',
                loaders: snapshot.script_loaders || '',
                variants: snapshot.script_variants || '',
                stim: snapshot.script_stim || '',
                sys_extract: snapshot.script_sys_extract || '',
                proto_extract: snapshot.script_proto_extract || '',
                sys_analyze: snapshot.script_sys_analyze || ''
            };
            
            // Update config display (always visible/cheap)
            this.updateConfigDisplay();

            // Only update the active tab's content
            this._snapshotDirty = true;
            switch (this.currentTab) {
                case 'loaders':
                    this.updateLoadersEditor();
                    break;
            }
            
            // Update timestamp
            this.updateSnapshotTime(snapshot.timestamp);
            
            // Notify plugins
            this._pluginHook('onSnapshot', snapshot);
            
        } catch (e) {
            console.error('Failed to parse snapshot:', e);
        }
    }
    
    // ==========================================
    // Tcl Parsing Helpers
    // ==========================================
    
    parseScriptsDict(str) {
        if (!str || typeof str !== 'string') return {};
        return TclParser.parseDict(str);
    }
    
    parseCurrentLoader(str) {
        if (!str || typeof str !== 'string') return null;
        
        const dict = TclParser.parseDict(str);
        
        const argNames = TclParser.parseList(dict.loader_arg_names || '');
        
        const argOptions = {};
        if (dict.loader_arg_options) {
            const optDict = TclParser.parseDict(dict.loader_arg_options);
            for (const [name, optStr] of Object.entries(optDict)) {
                const opts = TclParser.parseList(optStr);
                argOptions[name] = opts.map(opt => {
                    const parts = TclParser.parseList(opt);
                    if (parts.length >= 2) {
                        return { label: parts[0], value: parts[1] };
                    } else if (parts.length === 1) {
                        return { label: parts[0], value: parts[0] };
                    }
                    return { label: opt, value: opt };
                });
            }
        }
        
        const loaderArgsRaw = dict.loader_args || '';
        const loaderArgs = TclParser.parseList(loaderArgsRaw);
        
        return {
            loader_proc: dict.loader_proc,
            loader_args: loaderArgs,
            loader_arg_names: argNames,
            loader_arg_options: argOptions
        };
    }
    
    parseParams(str) {
        if (!str || typeof str !== 'string') return {};
        
        const parsed = TclParser.parseParamSettings(str);
        const params = {};
        
        for (const [name, info] of Object.entries(parsed)) {
            params[name] = {
                default: info.value,
                value: info.value,
                flag: info.varType,
                type: info.dataType || 'string'
            };
        }
        
        return params;
    }
    
    parseStates(str) {
        if (!str || typeof str !== 'string') return {};
        
        const list = TclParser.parseList(str);
        const states = {};
        
        for (let i = 0; i < list.length - 1; i += 2) {
            const stateName = list[i];
            const nextStates = list[i + 1];
            const transitions = TclParser.parseList(nextStates);
            
            states[stateName] = {
                transitions: transitions.filter(t => t && t !== '{}')
            };
        }
        
        return states;
    }
    
    parseLoaders(str) {
        if (!str || typeof str !== 'string') return [];
        
        const items = TclParser.parseList(str);
        const loaders = [];
        
        if (items.length > 0 && items[0] === 'name') {
            const dict = TclParser.parseDict(str);
            loaders.push({
                name: dict.name,
                args: TclParser.parseList(dict.args || '')
            });
        } else {
            items.forEach(item => {
                const dict = TclParser.parseDict(item);
                if (dict.name) {
                    loaders.push({
                        name: dict.name,
                        args: TclParser.parseList(dict.args || '')
                    });
                }
            });
        }
        
        return loaders;
    }
    
    parseVariantArgs(str) {
        if (!str || typeof str !== 'string') return {};
        return TclParser.parseDict(str);
    }
    
    clearDashboard() {
        ['cfgProject', 'cfgSystem', 'cfgProtocol', 'cfgVariant', 'cfgVersion', 'cfgSubject'].forEach(key => {
            if (this.elements[key]) this.elements[key].textContent = '—';
        });
        
        ['statStates', 'statParams', 'statLoaders', 'statVariants'].forEach(key => {
            if (this.elements[key]) this.elements[key].textContent = '0';
        });
    }
    
    getEmptyState(message) {
        return `
            <div class="empty-state">
                <svg width="32" height="32" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5">
                    <circle cx="12" cy="12" r="10"></circle>
                    <line x1="12" y1="8" x2="12" y2="12"></line>
                    <line x1="12" y1="16" x2="12.01" y2="16"></line>
                </svg>
                <span>${message}</span>
            </div>
        `;
    }
    
        parseVariantsDict(str) {
        if (!str || typeof str !== 'string') return {};
        
        const variants = {};
        // Strip comment lines at each dict level, mirroring ess::normalize_variants
        // in lib/ess-2.0.tm — variant files now allow comments the server sanitizes.
        const list = TclParser.parseList(TclParser.stripComments(str.trim()));

        for (let i = 0; i < list.length - 1; i += 2) {
            const name = list[i];
            const defStr = list[i + 1];
            const def = TclParser.parseDict(TclParser.stripComments(defStr));

            variants[name] = {
                description: def.description || '',
                loader_proc: def.loader_proc || '',
                loader_options: def.loader_options ? TclParser.parseDict(TclParser.stripComments(def.loader_options)) : {},
                init: def.init || '',
                deinit: def.deinit || '',
                params: def.params ? TclParser.parseDict(TclParser.stripComments(def.params)) : {}
            };
        }
        
        return variants;
    }

        parseLoaderOptionValues(optionStr) {
        const values = [];
        const items = TclParser.parseList(optionStr);
        
        for (const item of items) {
            const parts = TclParser.parseList(item);
            
            if (parts.length === 2) {
                values.push({
                    label: parts[0],
                    value: parts[1]
                });
            } else {
                values.push({
                    label: item,
                    value: item
                });
            }
        }
        
        return values;
    }

    // ==========================================
    // Configuration Display
    // ==========================================
    
    updateConfigDisplay() {
        if (!this.snapshot) return;
        
        const s = this.snapshot;
        
        if (this.elements.cfgProject) this.elements.cfgProject.textContent = s.project || '—';
        if (this.elements.cfgSystem) this.elements.cfgSystem.textContent = s.system || '—';
        if (this.elements.cfgProtocol) this.elements.cfgProtocol.textContent = s.protocol || '—';
        if (this.elements.cfgVariant) this.elements.cfgVariant.textContent = s.variant || '—';
        if (this.elements.cfgVersion) this.elements.cfgVersion.textContent = s.version || '—';
        if (this.elements.cfgSubject) this.elements.cfgSubject.textContent = s.subject_id || '—';
        
        this.updateTabHeroes();
    }
    
    updateTabHeroes() {
        const s = this.snapshot;
        if (!s) return;
        
        const tabs = ['variants', 'loaders', 'scripts', 'states'];
        
        tabs.forEach(tab => {
            const project = document.getElementById(`${tab}-hero-project`);
            const system = document.getElementById(`${tab}-hero-system`);
            const protocol = document.getElementById(`${tab}-hero-protocol`);
            const variant = document.getElementById(`${tab}-hero-variant`);
            
            if (project) project.textContent = s.project || '—';
            if (system) system.textContent = s.system || '—';
            if (protocol) protocol.textContent = s.protocol || '—';
            if (variant) variant.textContent = s.variant || '—';
        });
    }
    
    updateStats() {
        const s = this.snapshot;
        if (!s) return;
        
        const states = this.parseStates(s.states || '');
        if (this.elements.statStates) this.elements.statStates.textContent = Object.keys(states).length;
        
        const params = this.parseParams(s.params || '');
        if (this.elements.statParams) this.elements.statParams.textContent = Object.keys(params).length;
        
        const loaders = this.parseLoaders(s.loaders || '');
        if (this.elements.statLoaders) this.elements.statLoaders.textContent = loaders.length;
        
        const variantCount = this.countVariants(s.variants || '');
        if (this.elements.statVariants) this.elements.statVariants.textContent = variantCount;
    }
    
    countVariants(str) {
        if (!str || typeof str !== 'string') return 0;
        const list = TclParser.parseList(str.trim());
        return Math.floor(list.length / 2);
    }
    
    // ==========================================
    // Tab Navigation
    // ==========================================
    
    switchTab(tabName) {
        this.currentTab = tabName;
        
        document.querySelectorAll('.tab-btn').forEach(btn => {
            btn.classList.toggle('active', btn.dataset.tab === tabName);
        });
        
        document.querySelectorAll('.tab-content').forEach(content => {
            content.classList.toggle('active', content.id === `tab-${tabName}`);
        });
        
        // Initialize tab-specific content and refresh if snapshot arrived while away
        if (tabName === 'loaders') {
            this.initLoadersTab();
        }
        this._snapshotDirty = false;
        
        // Notify plugins
        this._pluginHook('onTabSwitch', tabName);
    }
    
    // ==========================================
    // Utilities
    // ==========================================
    
    escapeHtml(str) {
        if (typeof str !== 'string') return str;
        return str
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;')
            .replace(/'/g, '&#039;');
    }
    
    updateSnapshotTime(timestamp) {
        if (!this.elements.snapshotTime) return;
        
        if (timestamp) {
            const date = new Date(timestamp * 1000);
            const timeStr = date.toLocaleTimeString();
            this.elements.snapshotTime.textContent = `Last updated: ${timeStr}`;
        } else {
            this.elements.snapshotTime.textContent = 'Last updated: --';
        }
    }
    
    startClock() {
        const updateClock = () => {
            if (this.elements.clock) {
                const now = new Date();
                this.elements.clock.textContent = now.toLocaleTimeString();
            }
        };
        
        updateClock();
        setInterval(updateClock, 1000);
    }
    
    // ==========================================
    // Notification (shared utility for plugins)
    // ==========================================
    
    showNotification(message, type = 'info') {
        const notification = document.createElement('div');
        notification.className = `notification notification-${type}`;
        notification.innerHTML = `
            <span class="notification-message">${this.escapeHtml(message)}</span>
            <button class="notification-close">&times;</button>
        `;
        
        Object.assign(notification.style, {
            position: 'fixed',
            bottom: '20px',
            right: '20px',
            padding: '12px 20px',
            borderRadius: '8px',
            backgroundColor: type === 'error' ? 'var(--wb-error)' : 
                            type === 'success' ? 'var(--wb-success)' : 
                            type === 'warning' ? 'var(--wb-warning)' : 'var(--wb-info)',
            color: 'white',
            boxShadow: 'var(--wb-shadow-lg)',
            zIndex: '9999',
            display: 'flex',
            alignItems: 'center',
            gap: '12px',
            animation: 'slideInRight 0.3s ease'
        });
        
        notification.querySelector('.notification-close').onclick = () => notification.remove();
        
        document.body.appendChild(notification);
        setTimeout(() => notification.remove(), 5000);
    }
    
    // ==========================================
    // Tcl Command Execution (shared utility for plugins)
    // ==========================================
    
    /**
     * Execute a Tcl command on the backend with response routing.
     * Returns a promise that resolves with the result.
     */
    async execTclCmd(cmd) {
        return new Promise((resolve, reject) => {
            if (this.connection?.ws?.readyState !== WebSocket.OPEN) {
                reject(new Error('WebSocket not connected'));
                return;
            }

            const responseId = `cmd_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;
            const dpName = `ess/cmd_response/${responseId}`;
            let settled = false;

            const cleanup = () => {
                this.dpManager.unsubscribe(dpName, responseHandler);
                // Queue for batched cleanup instead of sending an eval
                // per response (eval blocks the uWS event loop)
                this._queueDatapointCleanup(dpName);
            };

            const responseHandler = (data) => {
                if (data.name === dpName) {
                    if (settled) return;
                    settled = true;
                    cleanup();

                    const value = data.data !== undefined ? data.data : data.value;
                    if (value && value.error) {
                        reject(new Error(value.error));
                    } else {
                        resolve(value);
                    }
                }
            };

            this.dpManager.subscribe(dpName, responseHandler);

            const wrappedCmd = `
                if {[catch {${cmd}} result]} {
                    dservSet ${dpName} [list error $result]
                } else {
                    dservSet ${dpName} $result
                }
            `;

            this.connection.ws.send(JSON.stringify({
                cmd: 'eval',
                script: wrappedCmd
            }));

            setTimeout(() => {
                if (settled) return;
                settled = true;
                cleanup();
                reject(new Error('Command timeout'));
            }, 30000);
        });
    }
    
    /**
     * Queue a temporary datapoint name for batched cleanup.
     * Uses the native WebSocket 'clear' command which runs directly
     * on the dataserver without going through the Tcl interpreter,
     * so it never blocks the uWS event loop.
     */
    _queueDatapointCleanup(dpName) {
        if (!this._dpCleanupQueue) this._dpCleanupQueue = [];
        this._dpCleanupQueue.push(dpName);

        if (!this._dpCleanupTimer) {
            this._dpCleanupTimer = setTimeout(() => {
                this._dpCleanupTimer = null;
                const names = this._dpCleanupQueue;
                this._dpCleanupQueue = [];
                if (names.length > 0 && this.connection?.ws?.readyState === WebSocket.OPEN) {
                    this.connection.ws.send(JSON.stringify({
                        cmd: 'clear',
                        names: names
                    }));
                }
            }, 2000);
        }
    }

    // Alias for backward compat (plugins used execRegistryCmd)
    async execRegistryCmd(cmd) {
        return this.execTclCmd(cmd);
    }
    
    // ==========================================
    // Loaders Editor
    // ==========================================

    initLoadersEditor() {
        this.loaderElements = {
            loaderSelect: document.getElementById('loader-select'),
            runBtn: document.getElementById('loader-run-btn'),
            runStatus: document.getElementById('loader-run-status'),
            argsBody: document.getElementById('loader-args-body'),
            argsResetBtn: document.getElementById('loader-args-reset-btn'),
            tableContainer: document.getElementById('loader-table-container'),
            tableInfo: document.getElementById('loader-table-info'),
            consoleOutput: document.getElementById('loader-console-output'),
            errorCount: document.getElementById('loader-error-count'),
            consoleClearBtn: document.getElementById('loader-console-clear-btn')
        };

        this.loaderSandbox = null;         // DservConnection for isolated subprocess
        this.loaderErrorCount = 0;
        this.currentLoaderName = null;
        this.parsedLoaderDefs = [];        // [{name, args, bodyStart, bodyEnd}, ...]

        // Bind events
        this.loaderElements.runBtn?.addEventListener('click', () => this.runLoader());
        this.loaderElements.consoleClearBtn?.addEventListener('click', () => this.clearLoaderConsole());
        this.loaderElements.argsResetBtn?.addEventListener('click', () => this.resetLoaderArgs());

        this.loaderElements.loaderSelect?.addEventListener('change', (e) => {
            this.currentLoaderName = e.target.value || null;
            this.updateLoaderArgs();
            this.loaderElements.runBtn.disabled = !this.currentLoaderName || !this.loaderSandbox;
        });
    }

    async initLoadersTab() {
        // Full refresh every time the tab shows: defs from the snapshot's
        // loaders script AND the variants cross-reference that supplies
        // arg choices. Unconditional on purpose — gating this on
        // _snapshotDirty left the arg dropdowns empty whenever another
        // tab visit had already consumed the flag.
        this.updateLoadersEditor();
        if (!this.loaderSandbox) {
            await this.initLoaderSandbox();
        }
    }

    // ==========================================
    // Loader Sandbox (Isolated Subprocess)
    // ==========================================

    async initLoaderSandbox() {
        try {
            // Subscribe to loader test results via the main connection
            if (this.dpManager) {
                this.dpManager.subscribe(
                    'ess/loader_test/stimdg',
                    (dp) => this.handleLoaderTablePush(dp)
                );
            }

            // Mark sandbox as ready (no separate subprocess needed)
            this.loaderSandbox = true;
            console.log('Loader test environment ready (using ess::test_loader)');

            // Enable run button if a loader is selected
            if (this.currentLoaderName) {
                this.loaderElements.runBtn.disabled = false;
            }
        } catch (e) {
            console.error('Failed to initialize loader test environment:', e);
            this.logToLoaderConsole(`Init failed: ${e.message}`, 'error');
        }
    }

    handleLoaderTablePush(datapoint) {
        let data;
        try {
            data = typeof datapoint.data === 'string'
                ? JSON.parse(datapoint.data)
                : datapoint.data;
        } catch (e) {
            this.logToLoaderConsole(`Invalid table data: ${e.message}`, 'error');
            return;
        }

        // Render in DGTableViewer
        const container = this.loaderElements.tableContainer;
        if (container) {
            container.innerHTML = '';
            const viewer = new DGTableViewer('loader-table-container', data, {
                pageSize: 50,
                maxHeight: '100%',
                theme: 'dark',
                compactMode: true
            });
            viewer.render();

            // Update info
            const rowCount = data.rows ? data.rows.length : 0;
            const colCount = data.rows && data.rows.length > 0 ? Object.keys(data.rows[0]).length : 0;
            if (this.loaderElements.tableInfo) {
                this.loaderElements.tableInfo.textContent = `${rowCount} rows, ${colCount} columns`;
            }
        }
    }

    // ==========================================
    // Loader Defs (parsed from the snapshot's loaders script)
    // ==========================================

    refreshLoaderDefs() {
        this.parseAndUpdateLoaderDropdown();
    }

    updateLoadersEditor() {
        if (!this.snapshot) return;
        // Parsed variants cross-reference loader_options for arg choices
        if (this.snapshot.variants) {
            this.parsedVariantsForLoaders = this.parseVariantsDict(this.snapshot.variants);
        }
        this.refreshLoaderDefs();
    }

    // ==========================================
    // Loader Parsing
    // ==========================================

    // Starting at index `from`, skip whitespace and backslash-newline line
    // continuations, then read a brace-balanced { ... } group. Returns
    // { content, start, end } (start = index of the opening brace, end = one
    // past the closing brace) or null if no balanced group is found.
    readBraceGroup(content, from) {
        let i = from;
        while (i < content.length) {
            const c = content[i];
            if (c === '\\' && content[i + 1] === '\n') { i += 2; continue; }
            if (/\s/.test(c)) { i++; continue; }
            break;
        }
        if (content[i] !== '{') return null;

        const start = i;
        let depth = 0;
        for (; i < content.length; i++) {
            const c = content[i];
            if (c === '\\') { i++; continue; } // skip escaped char (\{ \} \newline)
            if (c === '{') depth++;
            else if (c === '}') {
                depth--;
                if (depth === 0) return { content: content.slice(start + 1, i), start, end: i + 1 };
            }
        }
        return null; // unbalanced
    }

    parseLoadersFromScript(content) {
        // Find all `<$s> add_loader <name> { <arglist> } { <body> }` definitions.
        // The name immediately follows add_loader, but the arglist and body brace
        // groups may span multiple lines and use backslash continuations, so we
        // scan the whole content and read balanced brace groups rather than
        // matching a single line.
        const loaders = [];
        const re = /(?:\$\w+|\w+)\s+add_loader\s+(\w+)/g;
        let m;

        while ((m = re.exec(content)) !== null) {
            const name = m[1];

            // Arglist brace group.
            const argsGroup = this.readBraceGroup(content, m.index + m[0].length);
            if (!argsGroup) continue;
            // Tokens are whitespace-separated; drop stray '\' continuation markers.
            const args = argsGroup.content.trim().split(/\s+/).filter(a => a && a !== '\\');

            // Body brace group.
            const bodyGroup = this.readBraceGroup(content, argsGroup.end);
            if (!bodyGroup) continue;

            const bodyStartLine = content.slice(0, bodyGroup.start).split('\n').length - 1;
            const bodyEndLine = content.slice(0, bodyGroup.end).split('\n').length - 1;

            loaders.push({
                name,
                args,
                bodyStartLine,
                bodyEndLine,
                bodyStart: bodyGroup.start,
                bodyEnd: bodyGroup.end
            });

            // Resume scanning after the body so we don't re-match inside it.
            re.lastIndex = bodyGroup.end;
        }

        return loaders;
    }

    parseAndUpdateLoaderDropdown() {
        const content = this.scripts?.loaders || '';
        this.parsedLoaderDefs = this.parseLoadersFromScript(content);

        const select = this.loaderElements.loaderSelect;
        if (!select) return;

        const prevValue = select.value;

        select.innerHTML = '<option value="">— select loader —</option>';
        this.parsedLoaderDefs.forEach(loader => {
            const opt = document.createElement('option');
            opt.value = loader.name;
            opt.textContent = loader.name;
            select.appendChild(opt);
        });

        // Restore selection
        if (prevValue && this.parsedLoaderDefs.some(l => l.name === prevValue)) {
            select.value = prevValue;
            this.currentLoaderName = prevValue;
        } else if (this.parsedLoaderDefs.length > 0) {
            // Auto-select first if nothing selected
            if (!this.currentLoaderName) {
                select.value = this.parsedLoaderDefs[0].name;
                this.currentLoaderName = this.parsedLoaderDefs[0].name;
            }
        }

        this.updateLoaderArgs();
        this.loaderElements.runBtn.disabled = !this.currentLoaderName || !this.loaderSandbox;
    }

    // ==========================================
    // Loader Arguments UI
    // ==========================================

    updateLoaderArgs() {
        const body = this.loaderElements.argsBody;
        if (!body) return;

        if (!this.currentLoaderName) {
            body.innerHTML = '<div class="loaders-args-empty">Select a loader to see its arguments</div>';
            return;
        }

        const loaderDef = this.parsedLoaderDefs.find(l => l.name === this.currentLoaderName);
        if (!loaderDef || loaderDef.args.length === 0) {
            body.innerHTML = '<div class="loaders-args-empty">This loader has no arguments</div>';
            return;
        }

        // Get variant options for this loader (cross-reference from parsed variants)
        const variantOptions = this.getVariantOptionsForLoader(this.currentLoaderName);

        body.innerHTML = '';
        loaderDef.args.forEach(argName => {
            const row = document.createElement('div');
            row.className = 'loader-arg-row';

            const nameEl = document.createElement('span');
            nameEl.className = 'loader-arg-name';
            nameEl.textContent = argName;
            row.appendChild(nameEl);

            const opts = variantOptions[argName];

            if (opts && opts.length > 0) {
                // Variant options exist — show a select dropdown
                const optSelect = document.createElement('select');
                optSelect.className = 'loader-arg-select';
                optSelect.dataset.argName = argName;

                opts.forEach((opt, idx) => {
                    const o = document.createElement('option');
                    o.value = idx;
                    o.textContent = opt.label;
                    optSelect.appendChild(o);
                });

                row.appendChild(optSelect);
            } else {
                // No variant options — text input for manual entry
                const input = document.createElement('input');
                input.className = 'loader-arg-input';
                input.type = 'text';
                input.dataset.argName = argName;
                input.placeholder = 'value';
                row.appendChild(input);
            }

            body.appendChild(row);
        });
    }

    getVariantOptionsForLoader(loaderName) {
        const options = {};
        const variants = this.parsedVariantsForLoaders || this.parsedVariants || {};

        // Gather options from all variants that use this loader
        for (const [, variant] of Object.entries(variants)) {
            if (variant.loader_proc === loaderName && variant.loader_options) {
                for (const [argName, optStr] of Object.entries(variant.loader_options)) {
                    if (!options[argName]) {
                        options[argName] = [];
                    }
                    // Parse the option values from the Tcl list
                    // Options can be {label value} pairs or plain values
                    const optValues = TclParser.parseList(optStr);
                    optValues.forEach(v => {
                        const parts = TclParser.parseList(v);
                        let label, value;
                        if (parts.length === 2) {
                            // {label value} pair — display label, use value
                            label = parts[0];
                            value = parts[1];
                        } else {
                            // plain value — label and value are the same
                            label = v;
                            value = v;
                        }
                        // Store as {label, value} objects
                        const existing = options[argName].find(o => o.value === value && o.label === label);
                        if (!existing) {
                            options[argName].push({ label, value });
                        }
                    });
                }
            }
        }

        return options;
    }

    getLoaderArgValues() {
        const args = {};
        const body = this.loaderElements.argsBody;
        if (!body) return args;

        // Read text inputs
        body.querySelectorAll('.loader-arg-input').forEach(input => {
            args[input.dataset.argName] = input.value;
        });

        // Read select dropdowns (named options like dict params)
        const variantOptions = this.getVariantOptionsForLoader(this.currentLoaderName);
        body.querySelectorAll('.loader-arg-select').forEach(select => {
            const argName = select.dataset.argName;
            const opts = variantOptions[argName];
            if (opts && opts[select.selectedIndex]) {
                args[argName] = opts[select.selectedIndex].value;
            }
        });

        return args;
    }

    resetLoaderArgs() {
        const body = this.loaderElements.argsBody;
        if (!body) return;
        body.querySelectorAll('.loader-arg-input').forEach(input => { input.value = ''; });
        body.querySelectorAll('.loader-arg-select').forEach(select => { select.selectedIndex = 0; });
        // Re-populate from variant defaults
        this.updateLoaderArgs();
    }

    // ==========================================
    // Run Loader
    // ==========================================

    async runLoader() {
        if (!this.loaderSandbox || !this.currentLoaderName) return;

        const loaderDef = this.parsedLoaderDefs.find(l => l.name === this.currentLoaderName);
        if (!loaderDef) return;

        const argValues = this.getLoaderArgValues();

        // Check all args have values
        const missingArgs = loaderDef.args.filter(a => !argValues[a] && argValues[a] !== '0');
        if (missingArgs.length > 0) {
            this.logToLoaderConsole(`Missing argument values: ${missingArgs.join(', ')}`, 'error');
            return;
        }

        // Build args dict for ess::test_loader
        const argPairs = loaderDef.args.map(a => `${a} {${argValues[a]}}`).join(' ');
        const cmd = `ess::test_loader ${this.currentLoaderName} [dict create ${argPairs}]`;

        // Update UI
        this.loaderElements.runBtn.disabled = true;
        this.setLoaderRunStatus('running...', '');

        try {
            const response = await this.connection.send(cmd, 'ess');
            this.setLoaderRunStatus(response || 'done', 'success');
            this.logToLoaderConsole(`${this.currentLoaderName}: ${response}`, 'success');
        } catch (e) {
            this.setLoaderRunStatus('error', 'error');
            this.logToLoaderConsole(`${this.currentLoaderName}: ${e.message}`, 'error');
        } finally {
            this.loaderElements.runBtn.disabled = false;
        }
    }

    setLoaderRunStatus(text, type) {
        const el = this.loaderElements.runStatus;
        if (el) {
            el.textContent = text;
            el.className = 'loaders-run-status';
            if (type) el.classList.add(type);

            if (type === 'success' || type === 'error') {
                setTimeout(() => {
                    el.textContent = '';
                    el.className = 'loaders-run-status';
                }, 5000);
            }
        }
    }

    // ==========================================
    // Loader Console
    // ==========================================

    logToLoaderConsole(message, type = 'info') {
        const output = this.loaderElements.consoleOutput;
        if (!output) return;

        const entry = document.createElement('div');
        entry.className = 'loader-console-entry';

        const time = document.createElement('span');
        time.className = 'time';
        time.textContent = new Date().toLocaleTimeString();
        entry.appendChild(time);

        const msg = document.createElement('span');
        msg.className = `message ${type}`;
        msg.textContent = message;
        entry.appendChild(msg);

        output.appendChild(entry);
        output.scrollTop = output.scrollHeight;

        if (type === 'error') {
            this.loaderErrorCount++;
            if (this.loaderElements.errorCount) {
                this.loaderElements.errorCount.textContent =
                    `${this.loaderErrorCount} error${this.loaderErrorCount > 1 ? 's' : ''}`;
            }
        }
    }

    clearLoaderConsole() {
        if (this.loaderElements.consoleOutput) {
            this.loaderElements.consoleOutput.innerHTML = '';
        }
        this.loaderErrorCount = 0;
        if (this.loaderElements.errorCount) {
            this.loaderElements.errorCount.textContent = '';
        }
    }
}

// Global tab switch function for onclick handlers
function switchTab(tabName) {
    if (window.workbench) {
        window.workbench.switchTab(tabName);
    }
}

// Initialize on DOM ready
document.addEventListener('DOMContentLoaded', () => {
    window.workbench = new ESSWorkbench();
});
