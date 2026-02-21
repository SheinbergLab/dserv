/**
 * ESS Workbench - Main Application Script
 * 
 * Handles:
 * - Plugin registration and lifecycle hooks
 * - WebSocket connection and snapshot subscription
 * - Tab navigation
 * - Dashboard component rendering
 * - Script editor integration
 * - State diagram visualization
 * - Variants editor
 *
 * Plugin Lifecycle Hooks (called in registration order):
 *   onInit(wb)                — after core init, before connect()
 *   onConnected(wb)           — WebSocket connected
 *   onDisconnected(wb)        — WebSocket disconnected
 *   onSnapshot(wb, snapshot)  — after snapshot is parsed and core UI updated
 *   onTabSwitch(wb, tabName)  — after tab switch completes
 *   onScriptSelect(wb, name)  — after script selected in Scripts tab
 *   onSaveScript(wb, type, content) — before a script save; return false to handle it
 *   onPullScript(wb, type)    — before a pull; return false to handle it
 *   onCommitScript(wb, type, comment) — before a commit; return false to handle it
 *   onShowCommitDialog(wb, type) — before commit dialog; return false to handle it
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
        this.registry = null;
        
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
        this.initVariantsEditor();
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
            
            // Variant args
            variantArgsContainer: document.getElementById('variant-args-container'),
            autoReloadCheck: document.getElementById('auto-reload-check'),
            reloadVariantBtn: document.getElementById('reload-variant-btn'),
            
            // Parameters (legacy)
            paramsContainer: document.getElementById('params-container'),
            paramCount: document.getElementById('param-count'),
            
            // Loaders (legacy)
            loadersList: document.getElementById('loaders-list'),
            loaderCount: document.getElementById('loader-count'),
            
            // States preview
            statesPreview: document.getElementById('states-preview'),
            
            // Scripts
            scriptsList: document.getElementById('scripts-list'),
            editorFilename: document.getElementById('editor-filename'),
            editorStatus: document.getElementById('editor-status'),
            editorContainer: document.getElementById('editor-container'),
            lintBtn: document.getElementById('lint-btn'),
            formatBtn: document.getElementById('format-btn'),
            
            // States diagram
            statesDiagram: document.getElementById('states-diagram'),
            statesList: document.getElementById('states-list'),
            transitionsList: document.getElementById('transitions-list'),
            stateDiagramSvg: document.getElementById('state-diagram-svg'),
            
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
        
        // Auto-reload toggle
        this.elements.autoReloadCheck?.addEventListener('change', (e) => {
            this.autoReload = e.target.checked;
        });
        
        // Reload variant button
        this.elements.reloadVariantBtn?.addEventListener('click', () => {
            this.reloadVariant();
        });
        
        // Script file selection
        this.elements.scriptsList?.querySelectorAll('.script-item').forEach(item => {
            item.addEventListener('click', () => {
                this.selectScript(item.dataset.script);
            });
        });
        
        // Editor actions
        this.elements.lintBtn?.addEventListener('click', () => this.lintCurrentScript());
        this.elements.formatBtn?.addEventListener('click', () => this.formatCurrentScript());
    }
    
    // ==========================================
    // Connection Management
    // ==========================================
    
    connect() {
        this.updateConnectionStatus('connecting');
        
        this.connection = new DservConnection({
            subprocess: 'ess',
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
            this.initRegistry();
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
    
    async initRegistry() {
        try {
            console.log('Initializing ESS Registry...');
            
            const registryUrlDp = await this.dpManager.get('ess/registry/url');
            const workgroupDp = await this.dpManager.get('ess/registry/workgroup');
            
            const registryUrl = registryUrlDp?.data || registryUrlDp?.value || '';
            const workgroup = workgroupDp?.data || workgroupDp?.value || 'brown-sheinberg';
            
            console.log('Registry config from dserv:', { registryUrl, workgroup });
            
            this.registry = new RegistryClient({
                baseUrl: registryUrl,
                workgroup: workgroup
            });
            
            if (this.registry.workgroup) {
                try {
                    const systems = await this.registry.getSystems();
                    console.log(`Registry connected: ${systems.length} systems available`);
                } catch (err) {
                    console.warn('Registry connection test failed:', err);
                }
            }
            
            // Let plugins know registry is ready
            await this._pluginHookAsync('onRegistryReady');
            
        } catch (err) {
            console.error('Failed to initialize registry from dserv config:', err);
            
            // Fallback: try URL parameters
            try {
                const urlParams = new URLSearchParams(window.location.search);
                const agentUrl = urlParams.get('agent') || '';
                const workgroup = urlParams.get('workgroup') || 'brown-sheinberg';
                
                this.registry = new RegistryClient({
                    baseUrl: agentUrl,
                    workgroup: workgroup
                });
                
                await this._pluginHookAsync('onRegistryReady');
            } catch (fallbackErr) {
                console.error('Registry initialization completely failed:', fallbackErr);
            }
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
            
            // Hide sidebar entries for scripts that don't exist
            this.updateScriptSidebarVisibility();
            
            // Update core displays
            this.updateConfigDisplay();
            this.updateVariantsEditor();
            this.updateLoadersEditor();
            this.updateScriptEditor();
            this.updateStatesDiagram();
            
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
        
        if (this.elements.variantArgsContainer) {
            this.elements.variantArgsContainer.innerHTML = this.getEmptyState('No variant loaded');
        }
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
    // Variant Args
    // ==========================================
    
    updateVariantArgs() {
        const container = this.elements.variantArgsContainer;
        if (!container) return;
        
        const loaderInfo = this.parseCurrentLoader(this.snapshot?.current_loader);
        const variantArgs = this.parseVariantArgs(this.snapshot?.variant_args || '');
        
        if (!loaderInfo || !loaderInfo.loader_arg_options) {
            container.innerHTML = this.getEmptyState('No variant options available');
            return;
        }
        
        const argNames = loaderInfo.loader_arg_names || [];
        const argOptions = loaderInfo.loader_arg_options || {};
        
        if (argNames.length === 0) {
            container.innerHTML = this.getEmptyState('No configurable options');
            return;
        }
        
        let html = '';
        
        argNames.forEach(argName => {
            const options = argOptions[argName] || [];
            const currentValue = variantArgs[argName] || '';
            
            if (Array.isArray(options) && options.length > 0) {
                html += `
                    <div class="variant-arg-row">
                        <span class="variant-arg-label">${this.escapeHtml(argName)}</span>
                        <select class="variant-arg-select" data-arg="${this.escapeHtml(argName)}">
                            ${options.map((opt, idx) => {
                                const label = typeof opt === 'object' ? opt.label : opt;
                                const value = typeof opt === 'object' ? opt.value : opt;
                                const normCurrent = this.normalizeWhitespace(currentValue);
                                const normValue = this.normalizeWhitespace(value);
                                const selected = normValue === normCurrent ? 'selected' : '';
                                return `<option value="${idx}" ${selected}>${this.escapeHtml(label)}</option>`;
                            }).join('')}
                        </select>
                    </div>
                `;
            } else {
                html += `
                    <div class="variant-arg-row">
                        <span class="variant-arg-label">${this.escapeHtml(argName)}</span>
                        <input type="text" class="variant-arg-select" data-arg="${this.escapeHtml(argName)}" 
                               value="${this.escapeHtml(currentValue)}" placeholder="Enter value">
                    </div>
                `;
            }
        });
        
        container.innerHTML = html;
        
        this._currentArgOptions = argOptions;
        
        container.querySelectorAll('.variant-arg-select').forEach(el => {
            el.addEventListener('change', (e) => {
                const argName = e.target.dataset.arg;
                let value = e.target.value;
                
                if (e.target.tagName === 'SELECT' && this._currentArgOptions[argName]) {
                    const idx = parseInt(value, 10);
                    const opt = this._currentArgOptions[argName][idx];
                    value = opt ? opt.value : value;
                }
                
                this.onVariantArgChange(argName, value);
            });
        });
    }
    
    normalizeWhitespace(str) {
        if (typeof str !== 'string') return String(str);
        return str.replace(/\s+/g, ' ').trim();
    }
    
    onVariantArgChange(argName, value) {
        if (!this.connection || !this.connection.connected) return;
        
        const cmd = `ess::set_variant_args ${argName} {${value}}`;
        this.connection.send(cmd).catch(err => {
            console.error('Failed to set variant arg:', err);
        });
        
        if (this.autoReload) {
            setTimeout(() => this.reloadVariant(), 100);
        }
    }
    
    reloadVariant() {
        if (!this.connection || !this.connection.connected) return;
        
        this.connection.send('ess::reload_variant').catch(err => {
            console.error('Failed to reload variant:', err);
        });
    }
    
    // ==========================================
    // Parameters
    // ==========================================
    
    updateParameters() {
        const container = this.elements.paramsContainer;
        if (!container) return;
        
        const params = this.parseParams(this.snapshot?.params || '');
        const paramNames = Object.keys(params);
        
        this.elements.paramCount.textContent = paramNames.length.toString();
        
        if (paramNames.length === 0) {
            container.innerHTML = this.getEmptyState('No parameters defined');
            return;
        }
        
        let html = '';
        
        paramNames.forEach(name => {
            const param = params[name];
            const value = param.value !== undefined ? param.value : param.default;
            const type = param.type || 'string';
            const isTime = param.flag === '1' || name.toLowerCase().includes('time');
            
            html += `
                <div class="param-row">
                    <span class="param-name ${isTime ? 'time' : ''}">${this.escapeHtml(name)}</span>
                    <input type="text" class="param-input" data-param="${this.escapeHtml(name)}" 
                           value="${this.escapeHtml(String(value))}">
                    <span class="param-type">${this.escapeHtml(type)}</span>
                </div>
            `;
        });
        
        container.innerHTML = html;
        
        container.querySelectorAll('.param-input').forEach(el => {
            el.addEventListener('change', (e) => {
                this.onParamChange(e.target.dataset.param, e.target.value);
            });
        });
    }
    
    onParamChange(paramName, value) {
        if (!this.connection || !this.connection.connected) return;
        
        const cmd = `ess::set_param ${paramName} {${value}}`;
        this.connection.send(cmd).catch(err => {
            console.error('Failed to set parameter:', err);
        });
    }
    
    // ==========================================
    // Loaders
    // ==========================================
    
    updateLoaders() {
        const container = this.elements.loadersList;
        if (!container) return;
        
        const loaders = this.parseLoaders(this.snapshot?.loaders || '');
        
        this.elements.loaderCount.textContent = loaders.length.toString();
        
        if (loaders.length === 0) {
            container.innerHTML = this.getEmptyState('No loaders registered');
            return;
        }
        
        let html = '';
        
        loaders.forEach(loader => {
            const name = loader.name || 'unnamed';
            const args = loader.args || [];
            const argsStr = args.length > 0 ? args.join(', ') : 'no arguments';
            
            html += `
                <div class="loader-item">
                    <div class="loader-name">${this.escapeHtml(name)}</div>
                    <div class="loader-args">(${this.escapeHtml(argsStr)})</div>
                </div>
            `;
        });
        
        container.innerHTML = html;
    }
    
    // ==========================================
    // States Preview
    // ==========================================
    
    updateStatesPreview() {
        const container = this.elements.statesPreview;
        if (!container) return;
        
        const states = this.parseStates(this.snapshot?.states || '');
        const stateNames = Object.keys(states);
        
        if (stateNames.length === 0) {
            container.innerHTML = this.getEmptyState('No states defined');
            return;
        }
        
        const startState = stateNames[0];
        const endStates = new Set();
        
        stateNames.forEach(name => {
            const state = states[name];
            if (!state.transitions || state.transitions.length === 0) {
                endStates.add(name);
            }
            if (name.toLowerCase() === 'end') {
                endStates.add(name);
            }
        });
        
        let html = '';
        
        stateNames.forEach(name => {
            let classes = 'state-node';
            if (name === startState) classes += ' start';
            if (endStates.has(name)) classes += ' end';
            
            html += `<div class="${classes}">${this.escapeHtml(name)}</div>`;
        });
        
        container.innerHTML = html;
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
        
        // Initialize tab-specific content
        if (tabName === 'scripts' && !this.editor) {
            this.initEditor();
        } else if (tabName === 'states') {
            this.updateStatesDiagram();
        } else if (tabName === 'variants') {
            this.initVariantsTab();
        } else if (tabName === 'loaders') {
            this.initLoadersTab();
        }
        
        // Notify plugins
        this._pluginHook('onTabSwitch', tabName);
    }
    
    async initVariantsTab() {
        if (this.variantEditorPending || !this.variantScriptEditor?.view) {
            const ready = await this.ensureVariantScriptEditor();
            if (ready) {
                this.loadVariantScriptContent();
            }
        }
    }
    
    loadVariantScriptContent() {
        if (!this.variantScriptEditor?.view) return;
        
        if (this.scripts.variants) {
            this.variantScriptEditor.setValue(this.scripts.variants);
        } else if (this.snapshot?.variants) {
            this.variantScriptEditor.setValue(this.snapshot.variants);
        }
    }

    // ==========================================
    // Script Editor
    // ==========================================
    
    async initEditor() {
        if (this.editor) return;
        
        try {
            this.editor = new TclEditor(this.elements.editorContainer, {
                theme: 'dark',
                fontSize: '13px',
                tabSize: 4,
                lineNumbers: true,
                keybindings: 'emacs'
            });
            
            await new Promise(resolve => {
                this.elements.editorContainer.addEventListener('editor-ready', resolve, { once: true });
            });
            
            await this.loadScript(this.currentScript);
            
            let checkTimeout = null;
            const checkModified = async () => {
                if (this.scriptOriginalHash) {
                    const currentContent = this.editor.getValue();
                    const currentHash = await sha256(currentContent);
                    const isModified = currentHash !== this.scriptOriginalHash;
                    
                    if (isModified !== this.scriptModified) {
                        this.scriptModified = isModified;
                        const saveBtn = document.getElementById('script-save-btn');
                        if (saveBtn) saveBtn.disabled = !isModified;
                    }
                }
            };
            
            const onEditorChange = () => {
                if (!this.scriptOriginalHash) return;
                
                if (checkTimeout) clearTimeout(checkTimeout);
                checkTimeout = setTimeout(checkModified, 300);
                
                if (this.elements.editorStatus.textContent) {
                    this.elements.editorStatus.textContent = '';
                    this.elements.editorStatus.style.color = '';
                    this.elements.editorStatus.onclick = null;
                }
            };
            
            this.editor.view.dom.addEventListener('input', onEditorChange);
            this.editor.view.dom.addEventListener('keyup', onEditorChange);
            
        } catch (e) {
            console.error('Failed to initialize editor:', e);
        }
    }

    async selectScript(scriptName) {
        this.currentScript = scriptName;
        
        this.elements.scriptsList?.querySelectorAll('.script-item').forEach(item => {
            item.classList.toggle('active', item.dataset.script === scriptName);
        });
        
        await this.loadScript(scriptName);
        
        // Notify plugins
        this._pluginHook('onScriptSelect', scriptName);
    }
    
    async loadScript(scriptName) {
        if (!this.editor) return;
        
        const content = this.scripts[scriptName] || '';
        this.editor.setValue(content);
        
        this.scriptOriginalHash = await sha256(content);
        this.scriptModified = false;
        
        const saveBtn = document.getElementById('script-save-btn');
        if (saveBtn) saveBtn.disabled = true;
        
        const sys = this.snapshot?.system || '';
        const proto = this.snapshot?.protocol || '';
        const filenames = {
            system: sys ? `${sys}.tcl` : 'system.tcl',
            protocol: proto ? `${sys}/${proto}.tcl` : 'protocol.tcl',
            loaders: proto ? `${sys}/${proto}_loaders.tcl` : 'loaders.tcl',
            variants: proto ? `${sys}/${proto}_variants.tcl` : 'variants.tcl',
            stim: proto ? `${sys}/${proto}_stim.tcl` : 'stim.tcl',
            sys_extract: sys ? `${sys}_extract.tcl` : 'sys_extract.tcl',
            proto_extract: proto ? `${sys}/${proto}_extract.tcl` : 'proto_extract.tcl',
            sys_analyze: sys ? `${sys}_analyze.tcl` : 'sys_analyze.tcl'
        };
        
        this.elements.editorFilename.textContent = filenames[scriptName] || `${scriptName}.tcl`;
        this.elements.editorStatus.textContent = '';
    }
    
    updateScriptSidebarVisibility() {
        const optionalTypes = ['sys_extract', 'proto_extract', 'sys_analyze'];
        
        const labels = {
            system: 'system',
            protocol: 'protocol',
            loaders: 'loaders',
            variants: 'variants',
            stim: 'stim',
            sys_extract: 'sys_extract',
            proto_extract: 'proto_extract',
            sys_analyze: 'sys_analyze'
        };
        
        const sys = this.snapshot?.system || '';
        const proto = this.snapshot?.protocol || '';
        const filenames = {
            system: sys ? `${sys}.tcl` : '',
            protocol: proto ? `${proto}.tcl` : '',
            loaders: proto ? `${proto}_loaders.tcl` : '',
            variants: proto ? `${proto}_variants.tcl` : '',
            stim: proto ? `${proto}_stim.tcl` : '',
            sys_extract: sys ? `${sys}_extract.tcl` : '',
            proto_extract: proto ? `${proto}_extract.tcl` : '',
            sys_analyze: sys ? `${sys}_analyze.tcl` : ''
        };
        
        this.elements.scriptsList?.querySelectorAll('.script-item').forEach(item => {
            const scriptType = item.dataset.script;
            
            const label = item.querySelector('span:not(.script-status-dot)');
            if (label && labels[scriptType]) {
                label.textContent = labels[scriptType];
            }
            
            if (filenames[scriptType]) {
                item.title = filenames[scriptType];
            }
            
            if (optionalTypes.includes(scriptType)) {
                const hasContent = this.scripts[scriptType] && this.scripts[scriptType].trim() !== '';
                item.style.display = hasContent ? '' : 'none';
                
                if (!hasContent && this.currentScript === scriptType) {
                    this.selectScript('system');
                }
            }
        });
    }

    updateScriptEditor() {
        if (this.editor && this.currentTab === 'scripts') {
            this.loadScript(this.currentScript);
        }
    }
    
    lintCurrentScript() {
        if (!this.editor) return;
        
        const result = this.editor.lint();
        
        if (result.isValid) {
            this.elements.editorStatus.textContent = '✓ No issues';
            this.elements.editorStatus.style.color = 'var(--wb-success)';
            
            setTimeout(() => {
                this.elements.editorStatus.textContent = '';
                this.elements.editorStatus.style.color = '';
            }, 3000);
        } else {
            const firstError = result.errors[0];
            const firstWarning = result.warnings[0];
            const issue = firstError || firstWarning;
            
            if (issue) {
                this.elements.editorStatus.innerHTML = 
                    `<span style="cursor:pointer" title="Click to see all issues">` +
                    `Line ${issue.line}: ${issue.message}` +
                    (result.errors.length + result.warnings.length > 1 
                     ? ` <small>(+${result.errors.length + result.warnings.length - 1} more)</small>` 
                     : '') +
                    `</span>`;
                this.elements.editorStatus.style.color = firstError ? 'var(--wb-error)' : 'var(--wb-warning)';
                
                this.elements.editorStatus.onclick = () => {
                    this.jumpToLine(issue.line);
                    console.log('Lint results:', result);
                };
            } else {
                this.elements.editorStatus.textContent = result.summary;
                this.elements.editorStatus.style.color = 'var(--wb-error)';
            }
        }
    }
    
    jumpToLine(lineNum) {
        if (!this.editor?.view) return;
        
        const doc = this.editor.view.state.doc;
        const line = doc.line(Math.min(lineNum, doc.lines));
        
        this.editor.view.dispatch({
            selection: { anchor: line.from },
            scrollIntoView: true
        });
        
        this.editor.view.focus();
    }
    
    formatCurrentScript() {
        if (!this.editor) return;
        
        this.editor.format();
        this.elements.editorStatus.textContent = 'Formatted';
        this.elements.editorStatus.style.color = 'var(--wb-success)';
        
        setTimeout(() => {
            this.elements.editorStatus.textContent = '';
            this.elements.editorStatus.style.color = '';
        }, 2000);
    }
    
    // ==========================================
    // Script Save / Pull / Commit (delegatable to plugins)
    // ==========================================
    
    /**
     * Save current script. Plugins can intercept via onSaveScript.
     */
    async saveCurrentScript() {
        const scriptType = this.currentScript;
        const content = this.editor?.getValue();
        
        // Let plugins handle it if they want
        const handled = await this._pluginHookAsync('onSaveScript', scriptType, content);
        if (handled === false) return;
        
        // Default: save via dserv eval
        if (!this.editor?.view) {
            alert('No editor available');
            return;
        }
        
        if (!content) {
            alert('No content to save');
            return;
        }
        
        const btn = document.getElementById('script-save-btn');
        const originalText = btn?.textContent;
        
        try {
            if (btn) {
                btn.disabled = true;
                btn.textContent = 'Saving...';
            }
            
            if (this.connection?.ws?.readyState === WebSocket.OPEN) {
                const saveCmd = `ess::save_script ${scriptType} {${content}}`;
                this.connection.ws.send(JSON.stringify({
                    cmd: 'eval',
                    script: saveCmd
                }));
                
                console.log(`Saved ${scriptType} script to local filesystem`);
                this.scripts[scriptType] = content;
                
                if (btn) {
                    btn.textContent = 'Saved!';
                    setTimeout(() => {
                        btn.textContent = originalText;
                        btn.disabled = false;
                    }, 1500);
                }
            } else {
                throw new Error('WebSocket not connected');
            }
        } catch (err) {
            alert(`Save failed: ${err.message}`);
            if (btn) {
                btn.textContent = originalText;
                btn.disabled = false;
            }
        }
    }
    
    /**
     * Pull script from registry. Plugins can intercept via onPullScript.
     */
    async pullScriptFromRegistry() {
        const handled = await this._pluginHookAsync('onPullScript', this.currentScript);
        if (handled === false) return;
        
        // Default: no-op (needs registry plugin)
        console.warn('pullScriptFromRegistry: no handler');
    }
    
    /**
     * Show commit dialog. Plugins can intercept via onShowCommitDialog.
     */
    showCommitDialog(scriptType) {
        const handled = this._pluginHook('onShowCommitDialog', scriptType);
        if (handled === false) return;
        
        // Default: show the modal
        const modal = document.getElementById('commit-modal');
        if (!modal) return;
        
        const fileEl = document.getElementById('commit-file');
        if (fileEl) {
            fileEl.textContent = `File: ${scriptType}.tcl`;
        }
        
        modal.dataset.scriptType = scriptType;
        document.getElementById('commit-comment').value = '';
        modal.style.display = 'flex';
    }
    
    /**
     * Commit to registry. Plugins can intercept via onCommitScript.
     */
    async commitToRegistry(scriptType, comment) {
        const handled = await this._pluginHookAsync('onCommitScript', scriptType, comment);
        if (handled === false) return;
        
        // Default: no-op (needs registry plugin)
        console.warn('commitToRegistry: no handler');
    }
    
    // ==========================================
    // States Diagram
    // ==========================================
    
    updateStatesDiagram() {
        this.updateStatesList();
        this.updateTransitionsList();
        this.renderStateDiagram();
    }
    
    updateStatesList() {
        const container = this.elements.statesList;
        if (!container) return;
        
        const states = this.parseStates(this.snapshot?.states || '');
        const stateNames = Object.keys(states);
        
        if (stateNames.length === 0) {
            container.innerHTML = '<div class="empty-state"><span>No states loaded</span></div>';
            return;
        }
        
        let html = '';
        stateNames.forEach(name => {
            html += `<div class="state-list-item">${this.escapeHtml(name)}</div>`;
        });
        
        container.innerHTML = html;
    }
    
    updateTransitionsList() {
        const container = this.elements.transitionsList;
        if (!container) return;
        
        const states = this.parseStates(this.snapshot?.states || '');
        const transitions = [];
        
        Object.entries(states).forEach(([fromState, stateData]) => {
            if (stateData.transitions && stateData.transitions.length > 0) {
                stateData.transitions.forEach(toState => {
                    transitions.push({ from: fromState, to: toState });
                });
            }
        });
        
        if (transitions.length === 0) {
            container.innerHTML = '<div class="empty-state"><span>No transitions loaded</span></div>';
            return;
        }
        
        let html = '';
        transitions.forEach(t => {
            html += `
                <div class="transition-item">
                    <div class="transition-from-to">
                        ${this.escapeHtml(t.from)}
                        <span class="transition-arrow">→</span>
                        ${this.escapeHtml(t.to)}
                    </div>
                </div>
            `;
        });
        
        container.innerHTML = html;
    }
    
    renderStateDiagram() {
        const svg = this.elements.stateDiagramSvg;
        if (!svg) return;
        
        const states = this.parseStates(this.snapshot?.states || '');
        const stateNames = Object.keys(states);
        
        if (stateNames.length === 0) {
            svg.innerHTML = '';
            return;
        }
        
        const nodeWidth = 120;
        const nodeHeight = 40;
        const hSpacing = 80;
        const vSpacing = 60;
        const padding = 40;
        
        const cols = Math.ceil(Math.sqrt(stateNames.length));
        const rows = Math.ceil(stateNames.length / cols);
        
        const positions = {};
        stateNames.forEach((name, i) => {
            const col = i % cols;
            const row = Math.floor(i / cols);
            positions[name] = {
                x: padding + col * (nodeWidth + hSpacing) + nodeWidth / 2,
                y: padding + row * (nodeHeight + vSpacing) + nodeHeight / 2
            };
        });
        
        const width = padding * 2 + cols * (nodeWidth + hSpacing) - hSpacing;
        const height = padding * 2 + rows * (nodeHeight + vSpacing) - vSpacing;
        svg.setAttribute('viewBox', `0 0 ${width} ${height}`);
        
        let svgContent = '';
        
        const startState = stateNames[0];
        const endStates = new Set();
        stateNames.forEach(name => {
            const state = states[name];
            if (!state.transitions || state.transitions.length === 0 || name.toLowerCase() === 'end') {
                endStates.add(name);
            }
        });
        
        // Edges
        Object.entries(states).forEach(([fromState, stateData]) => {
            if (stateData.transitions) {
                stateData.transitions.forEach(toState => {
                    if (positions[fromState] && positions[toState]) {
                        const from = positions[fromState];
                        const to = positions[toState];
                        
                        const dx = to.x - from.x;
                        const dy = to.y - from.y;
                        const len = Math.sqrt(dx * dx + dy * dy);
                        
                        if (len > 0) {
                            const ux = dx / len;
                            const uy = dy / len;
                            
                            const startX = from.x + ux * (nodeWidth / 2);
                            const startY = from.y + uy * (nodeHeight / 2);
                            const endX = to.x - ux * (nodeWidth / 2 + 8);
                            const endY = to.y - uy * (nodeHeight / 2 + 8);
                            
                            svgContent += `
                                <line x1="${startX}" y1="${startY}" x2="${endX}" y2="${endY}" 
                                      class="state-diagram-edge" marker-end="url(#arrowhead)"/>
                            `;
                        }
                    }
                });
            }
        });
        
        // Nodes
        stateNames.forEach(name => {
            const pos = positions[name];
            
            let nodeClass = 'state-diagram-node';
            if (name === startState) nodeClass += ' start';
            if (endStates.has(name)) nodeClass += ' end';
            
            svgContent += `
                <rect x="${pos.x - nodeWidth / 2}" y="${pos.y - nodeHeight / 2}" 
                      width="${nodeWidth}" height="${nodeHeight}" 
                      rx="6" class="${nodeClass}"/>
                <text x="${pos.x}" y="${pos.y}" class="state-diagram-text">
                    ${this.escapeHtml(name)}
                </text>
            `;
        });
        
        svg.innerHTML = `
            <defs>
                <marker id="arrowhead" markerWidth="10" markerHeight="7" refX="9" refY="3.5" orient="auto">
                    <polygon points="0 0, 10 3.5, 0 7" class="state-diagram-arrowhead"/>
                </marker>
            </defs>
            ${svgContent}
        `;
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
            
            const responseHandler = (data) => {
                if (data.name === `ess/cmd_response/${responseId}`) {
                    this.dpManager.unsubscribe(`ess/cmd_response/${responseId}`, responseHandler);
                    
                    const value = data.data !== undefined ? data.data : data.value;
                    if (value && value.error) {
                        reject(new Error(value.error));
                    } else {
                        resolve(value);
                    }
                }
            };
            
            this.dpManager.subscribe(`ess/cmd_response/${responseId}`, responseHandler);
            
            const wrappedCmd = `
                if {[catch {${cmd}} result]} {
                    dservSet ess/cmd_response/${responseId} [list error $result]
                } else {
                    dservSet ess/cmd_response/${responseId} $result
                }
            `;
            
            this.connection.ws.send(JSON.stringify({
                cmd: 'eval',
                script: wrappedCmd
            }));
            
            setTimeout(() => {
                this.dpManager.unsubscribe(`ess/cmd_response/${responseId}`, responseHandler);
                reject(new Error('Command timeout'));
            }, 30000);
        });
    }
    
    // Alias for backward compat (plugins used execRegistryCmd)
    async execRegistryCmd(cmd) {
        return this.execTclCmd(cmd);
    }
    
    // ==========================================
    // Variants Editor (large section, all core)
    // ==========================================
    
    initVariantsEditor() {
        this.variantElements = {
            scriptEditor: document.getElementById('variant-script-editor'),
            parsedVariantSelect: document.getElementById('parsed-variant-select'),
            parsedStatus: document.getElementById('parsed-status'),
            parsedContent: document.getElementById('parsed-content'),
            fileValidationStatus: document.getElementById('file-validation-status'),
            formatBtn: document.getElementById('variant-format-btn'),
            newBtn: document.getElementById('variant-new-btn'),
            duplicateBtn: document.getElementById('variant-duplicate-btn'),
            saveBtn: document.getElementById('variant-save-btn'),
            saveReloadBtn: document.getElementById('variant-save-reload-btn')
        };
        
        this.variantsOriginalHash = null;
        this.variantsModified = false;
        
        this.variantElements.formatBtn?.addEventListener('click', () => {
            this.formatVariantsScript();
        });
        
        this.variantElements.newBtn?.addEventListener('click', () => {
            this.addNewVariant();
        });
        
        this.variantElements.duplicateBtn?.addEventListener('click', () => {
            this.duplicateCurrentVariant();
        });
        
        this.variantElements.saveBtn?.addEventListener('click', () => {
            this.saveVariantsScript(false);
        });
        
        this.variantElements.saveReloadBtn?.addEventListener('click', () => {
            this.saveVariantsScript(true);
        });
        
        this.variantElements.parsedVariantSelect?.addEventListener('change', (e) => {
            this.jumpToVariant(e.target.value);
        });
        
        this.variantEditorPending = true;
    }
    
    async formatVariantsScript() {
        if (!this.variantScriptEditor?.view) return;
        
        if (typeof TclFormatter === 'undefined') {
            console.warn('TclFormatter not available');
            return;
        }
        
        const content = this.variantScriptEditor.getValue();
        
        try {
            const formatted = TclFormatter.formatTclCode(content, 4);
            const cursorPos = this.getCursorPosition();
            
            this.variantScriptEditor.setValue(formatted);
            
            const newLength = formatted.length;
            const newPos = Math.min(cursorPos, newLength);
            
            this.variantScriptEditor.view.dispatch({
                selection: { anchor: newPos }
            });

            if (this.variantsOriginalHash) {
                const currentHash = await sha256(formatted);
                this.variantsModified = currentHash !== this.variantsOriginalHash;
                this.updateSaveButton();
            }
            
            this.validateEntireFile();
            this.populateVariantDropdown();
            this.updateParsedView();
            
        } catch (e) {
            console.error('Format error:', e);
        }
    }
    
    initVariantScriptEditor() {
        console.log('initVariantScriptEditor called - deferring until tab visible');
        this.variantEditorPending = true;
    }
    
    async ensureVariantScriptEditor() {
        const container = this.variantElements.scriptEditor;
        
        if (!container) {
            console.warn('Variant script editor container not found');
            return false;
        }
        
        if (typeof TclEditor === 'undefined') {
            console.warn('TclEditor not defined');
            return false;
        }
        
        if (this.variantScriptEditor && this.variantScriptEditor.view) {
            return true;
        }
        
        this.variantScriptEditor = new TclEditor(container, {
            theme: 'dark',
            fontSize: '13px',
            tabSize: 4,
            lineNumbers: true,
            keybindings: 'emacs'
        });
        
        await new Promise(resolve => {
            container.addEventListener('editor-ready', resolve, { once: true });
        });
        
        this.variantEditorPending = false;
        
        this.setupCursorTracking();
        
        return this.variantScriptEditor.view !== null;
    }
    
    setupCursorTracking() {
        if (!this.variantScriptEditor?.view) return;
        
        let updateTimeout = null;
        const debouncedUpdate = () => {
            if (updateTimeout) clearTimeout(updateTimeout);
            updateTimeout = setTimeout(() => {
                this.updateParsedView();
            }, 150);
        };
        
        let validateTimeout = null;
        const debouncedValidate = () => {
            if (validateTimeout) clearTimeout(validateTimeout);
            validateTimeout = setTimeout(() => {
                this.validateEntireFile();
            }, 500);
        };
        
        const checkModified = async () => {
            if (this.variantsOriginalHash) {
                const currentContent = this.variantScriptEditor.getValue();
                const currentHash = await sha256(currentContent);
                const isModified = currentHash !== this.variantsOriginalHash;
                if (isModified !== this.variantsModified) {
                    this.variantsModified = isModified;
                    this.updateSaveButton();
                }
            }
        };
        
        const view = this.variantScriptEditor.view;
        if (view) {
            view.dom.addEventListener('keyup', () => {
                debouncedUpdate();
                debouncedValidate();
                checkModified();
            });
            view.dom.addEventListener('click', debouncedUpdate);
            view.dom.addEventListener('input', () => {
                debouncedUpdate();
                debouncedValidate();
                checkModified();
            });
            
            view.scrollDOM.addEventListener('scroll', () => {
                this.applyVariantHighlight();
            });
        }
    }
    
    updateSaveButton() {
        const btn = this.variantElements.saveBtn;
        const reloadBtn = this.variantElements.saveReloadBtn;
        
        if (btn) {
            if (this.variantsModified) {
                btn.disabled = false;
                btn.classList.add('modified');
            } else {
                btn.disabled = true;
                btn.classList.remove('modified');
            }
        }
        
        if (reloadBtn) {
            if (this.variantsModified) {
                reloadBtn.disabled = false;
                reloadBtn.classList.add('modified');
            } else {
                reloadBtn.disabled = true;
                reloadBtn.classList.remove('modified');
            }
        }
    }
    
    async saveVariantsScript(andReload = false) {
        if (!this.variantScriptEditor?.view || !this.variantsModified) return;
        
        const content = this.variantScriptEditor.getValue();
        const btn = andReload ? this.variantElements.saveReloadBtn : this.variantElements.saveBtn;
        
        const statusEl = this.variantElements.fileValidationStatus;
        if (statusEl?.classList.contains('error')) {
            const proceed = confirm('This file has validation errors. Save anyway?');
            if (!proceed) return;
        }
        
        const originalHtml = btn?.innerHTML;
        if (btn) {
            btn.disabled = true;
            btn.innerHTML = `
                <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="spin">
                    <circle cx="12" cy="12" r="10"></circle>
                </svg>
                Saving...
            `;
        }
        
        try {
            const saveCmd = `ess::save_script variants {${content}}`;
            
            if (this.connection?.ws?.readyState === WebSocket.OPEN) {
                this.connection.ws.send(JSON.stringify({
                    cmd: 'eval',
                    script: saveCmd
                }));
                
                this.variantsOriginalHash = await sha256(content);
                this.variantsModified = false;
                
                console.log('Variants script saved successfully');
                
                if (andReload) {
                    if (btn) {
                        btn.innerHTML = `
                            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="spin">
                                <circle cx="12" cy="12" r="10"></circle>
                            </svg>
                            Reloading...
                        `;
                    }
                    
                    await new Promise(resolve => setTimeout(resolve, 200));
                    
                    this.connection.ws.send(JSON.stringify({
                        cmd: 'eval',
                        script: 'ess::reload_system'
                    }));
                }
                
                // Notify plugins (e.g., registry tracks sync status)
                this._pluginHook('onVariantsSaved', content);
                
                if (btn) {
                    btn.innerHTML = `
                        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                            <polyline points="20 6 9 17 4 12"></polyline>
                        </svg>
                        ${andReload ? 'Reloaded!' : 'Saved!'}
                    `;
                    setTimeout(() => {
                        this.resetSaveButton();
                    }, 2000);
                }
            } else {
                throw new Error('WebSocket not connected');
            }
            
        } catch (error) {
            console.error('Failed to save variants script:', error);
            alert(`Failed to save: ${error.message}`);
            this.resetSaveButton();
        }
    }
    
    resetSaveButton() {
        const btn = this.variantElements.saveBtn;
        const reloadBtn = this.variantElements.saveReloadBtn;
        
        if (btn) {
            btn.innerHTML = `
                <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                    <path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"></path>
                    <polyline points="17 21 17 13 7 13 7 21"></polyline>
                    <polyline points="7 3 7 8 15 8"></polyline>
                </svg>
                Save
            `;
        }
        
        if (reloadBtn) {
            reloadBtn.innerHTML = `
                <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                    <polyline points="23 4 23 10 17 10"></polyline>
                    <path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"></path>
                </svg>
                Save & Reload
            `;
        }
        
        this.updateSaveButton();
    }
    
    validateEntireFile() {
        if (!this.variantScriptEditor?.view) return;
        
        const content = this.variantScriptEditor.getValue();
        const statusEl = this.variantElements.fileValidationStatus;
        if (!statusEl) return;
        
        const errors = [];
        const warnings = [];
        
        if (typeof TclLinter !== 'undefined') {
            try {
                const linter = new TclLinter();
                const result = linter.lint(content);
                if (result.errors?.length > 0) {
                    result.errors.forEach(e => {
                        errors.push(`Line ${e.line || '?'}: ${e.message}`);
                    });
                }
            } catch (e) {
                errors.push(`Linter error: ${e.message}`);
            }
        }
        
        const variantsMatch = content.match(/variable\s+variants\s*\{/);
        if (!variantsMatch) {
            errors.push('Missing "variable variants { ... }" block');
        }
        
        const variants = this.findAllVariants(content);
        if (variants.length === 0 && variantsMatch) {
            warnings.push('No variants defined');
        }
        
        for (const v of variants) {
            const variantInfo = this.findVariantAtPosition(content, v.start + 1);
            if (variantInfo) {
                try {
                    const parsed = this.parseVariantBlock(variantInfo.body);
                    if (!parsed.loader_proc) {
                        errors.push(`Variant "${v.name}": missing loader_proc`);
                    }
                } catch (e) {
                    errors.push(`Variant "${v.name}": parse error`);
                }
            }
        }
        
        if (errors.length > 0) {
            statusEl.className = 'file-validation-status error';
            statusEl.textContent = `✗ ${errors.length} error${errors.length > 1 ? 's' : ''}`;
            statusEl.title = errors.join('\n');
        } else if (warnings.length > 0) {
            statusEl.className = 'file-validation-status warning';
            statusEl.textContent = `⚠ ${warnings.length} warning${warnings.length > 1 ? 's' : ''}`;
            statusEl.title = warnings.join('\n');
        } else {
            statusEl.className = 'file-validation-status valid';
            statusEl.textContent = `✓ ${variants.length} variant${variants.length !== 1 ? 's' : ''}`;
            statusEl.title = 'File is valid';
        }
    }
    
    updateVariantsEditor() {
        if (!this.snapshot) return;
        
        const variantsStr = this.snapshot.variants || '';
        this.parsedVariants = this.parseVariantsDict(variantsStr);
        
        if (this.variantScriptEditor?.view) {
            this.loadVariantScriptContent();
        }
    }
    
    parseVariantsDict(str) {
        if (!str || typeof str !== 'string') return {};
        
        const variants = {};
        const list = TclParser.parseList(str.trim());
        
        for (let i = 0; i < list.length - 1; i += 2) {
            const name = list[i];
            const defStr = list[i + 1];
            const def = TclParser.parseDict(defStr);
            
            variants[name] = {
                description: def.description || '',
                loader_proc: def.loader_proc || '',
                loader_options: def.loader_options ? TclParser.parseDict(def.loader_options) : {},
                init: def.init || '',
                deinit: def.deinit || '',
                params: def.params ? TclParser.parseDict(def.params) : {}
            };
        }
        
        return variants;
    }
    
    async loadVariantScriptContent() {
        if (!this.variantScriptEditor?.view) return;
        
        let content = '';
        if (this.scripts.variants) {
            content = this.scripts.variants;
        } else if (this.snapshot?.variants) {
            content = this.snapshot.variants;
        }

        if (content) {
            this.variantScriptEditor.setValue(content);
            
            this.variantsOriginalHash = await sha256(content);
            this.variantsModified = false;
            this.updateSaveButton();
            
            setTimeout(() => {
                this.populateVariantDropdown();
                this.updateParsedView();
                this.validateEntireFile();
            }, 100);
        }
    }
    
    populateVariantDropdown() {
        const select = this.variantElements.parsedVariantSelect;
        if (!select || !this.variantScriptEditor?.view) return;
        
        const content = this.variantScriptEditor.getValue();
        const variants = this.findAllVariants(content);
        
        select.innerHTML = '<option value="">— Select variant —</option>';
        
        variants.forEach(v => {
            const option = document.createElement('option');
            option.value = v.name;
            option.textContent = v.name;
            select.appendChild(option);
        });
        
        this.allVariantPositions = variants;
    }
    
    findAllVariants(content) {
        const variants = [];
        
        const variantsMatch = content.match(/variable\s+variants\s*\{/);
        if (!variantsMatch) return variants;
        
        const variantsStart = variantsMatch.index + variantsMatch[0].length;
        
        let braceDepth = 1;
        let variantsEnd = variantsStart;
        for (let i = variantsStart; i < content.length && braceDepth > 0; i++) {
            if (content[i] === '{') braceDepth++;
            else if (content[i] === '}') braceDepth--;
            if (braceDepth === 0) variantsEnd = i;
        }
        
        const variantsBody = content.substring(variantsStart, variantsEnd);
        let searchPos = 0;
        
        while (searchPos < variantsBody.length) {
            while (searchPos < variantsBody.length && /\s/.test(variantsBody[searchPos])) {
                searchPos++;
            }
            
            if (searchPos >= variantsBody.length) break;
            
            let nameStart = searchPos;
            while (searchPos < variantsBody.length && /\w/.test(variantsBody[searchPos])) {
                searchPos++;
            }
            const variantName = variantsBody.substring(nameStart, searchPos);
            
            if (!variantName) break;
            
            while (searchPos < variantsBody.length && /\s/.test(variantsBody[searchPos])) {
                searchPos++;
            }
            
            if (variantsBody[searchPos] !== '{') break;
            
            let depth = 1;
            searchPos++;
            while (searchPos < variantsBody.length && depth > 0) {
                if (variantsBody[searchPos] === '{') depth++;
                else if (variantsBody[searchPos] === '}') depth--;
                searchPos++;
            }
            
            variants.push({
                name: variantName,
                start: variantsStart + nameStart
            });
        }
        
        return variants;
    }
    
    jumpToVariant(variantName) {
        if (!variantName || !this.variantScriptEditor?.view) return;
        
        const variant = this.allVariantPositions?.find(v => v.name === variantName);
        if (!variant) return;
        
        const view = this.variantScriptEditor.view;
        
        view.dispatch({
            selection: { anchor: variant.start }
        });
        
        const lineBlock = view.lineBlockAt(variant.start);
        const editorHeight = view.dom.clientHeight;
        const targetScroll = lineBlock.top - (editorHeight * 0.25);
        
        view.scrollDOM.scrollTop = Math.max(0, targetScroll);
        view.focus();
        
        setTimeout(() => this.updateParsedView(), 50);
    }
    
    updateParsedView() {
        if (!this.variantScriptEditor?.view) return;
        
        const content = this.variantScriptEditor.getValue();
        const cursorPos = this.getCursorPosition();
        
        const variantInfo = this.findVariantAtPosition(content, cursorPos);
        
        if (!variantInfo) {
            this.showNoParsedVariant();
            this.clearVariantHighlight();
            return;
        }
        
        this.highlightVariant(variantInfo.start, variantInfo.end);
        
        try {
            const parsed = this.parseVariantBlock(variantInfo.body);
            const validationErrors = this.validateVariant(parsed);
            this.showParsedVariant(variantInfo.name, parsed, validationErrors);
        } catch (e) {
            this.showParseError(variantInfo.name, e.message);
        }
    }
    
    highlightVariant(start, end) {
        if (!this.variantScriptEditor?.view) return;
        
        const view = this.variantScriptEditor.view;
        const doc = view.state.doc;
        
        const startLine = doc.lineAt(start);
        const endLine = doc.lineAt(end);
        
        this.currentHighlight = { start: startLine.number, end: endLine.number };
        this.applyVariantHighlight();
    }
    
    clearVariantHighlight() {
        this.currentHighlight = null;
        this.applyVariantHighlight();
    }
    
    applyVariantHighlight() {
        if (!this.variantScriptEditor?.view) return;
        
        const view = this.variantScriptEditor.view;
        const gutterEl = view.dom.querySelector('.cm-gutters');
        
        if (!gutterEl) return;
        
        view.dom.querySelectorAll('.variant-highlight-gutter').forEach(el => {
            el.classList.remove('variant-highlight-gutter');
        });
        
        if (!this.currentHighlight) return;
        
        const gutterElements = gutterEl.querySelectorAll('.cm-gutterElement');
        
        gutterElements.forEach(gutterEl => {
            const lineNum = parseInt(gutterEl.textContent);
            if (!isNaN(lineNum) && lineNum >= this.currentHighlight.start && lineNum <= this.currentHighlight.end) {
                gutterEl.classList.add('variant-highlight-gutter');
            }
        });
    }
    
    validateVariant(parsed) {
        const errors = [];
        const warnings = [];
        
        const availableLoaders = this.getAvailableLoaders();
        const availableParams = this.getAvailableParams();
        
        if (!parsed.loader_proc) {
            errors.push('Missing required field: loader_proc');
        } else if (availableLoaders.length > 0 && !availableLoaders.find(l => l.name === parsed.loader_proc)) {
            errors.push(`Unknown loader: "${parsed.loader_proc}". Available: ${availableLoaders.map(l => l.name).join(', ')}`);
        }
        
        if (parsed.loader_proc && parsed.loader_options) {
            const loader = availableLoaders.find(l => l.name === parsed.loader_proc);
            if (loader && loader.args) {
                const optionKeys = Object.keys(parsed.loader_options);
                optionKeys.forEach(key => {
                    if (!loader.args.includes(key)) {
                        warnings.push(`Loader option "${key}" not in ${parsed.loader_proc}'s args: ${loader.args.join(', ')}`);
                    }
                });
            }
        }
        
        if (parsed.params && availableParams.length > 0) {
            const paramKeys = Object.keys(parsed.params);
            paramKeys.forEach(key => {
                if (!availableParams.includes(key)) {
                    warnings.push(`Unknown param: "${key}"`);
                }
            });
        }
        
        if (parsed.init && typeof TclLinter !== 'undefined') {
            try {
                const linter = new TclLinter();
                const initLint = linter.lint(parsed.init);
                if (initLint.errors?.length > 0) {
                    errors.push(`Init script: ${initLint.errors[0].message}`);
                }
            } catch (e) { /* skip */ }
        }
        
        if (parsed.deinit && typeof TclLinter !== 'undefined') {
            try {
                const linter = new TclLinter();
                const deinitLint = linter.lint(parsed.deinit);
                if (deinitLint.errors?.length > 0) {
                    errors.push(`Deinit script: ${deinitLint.errors[0].message}`);
                }
            } catch (e) { /* skip */ }
        }
        
        return { errors, warnings };
    }
    
    getAvailableLoaders() {
        if (!this.snapshot?.loaders) return [];
        return this.parseLoaders(this.snapshot.loaders);
    }
    
    getAvailableParams() {
        if (!this.snapshot?.params) return [];
        const params = this.parseParams(this.snapshot.params);
        return Object.keys(params);
    }
    
    getCursorPosition() {
        if (!this.variantScriptEditor?.view) return 0;
        const state = this.variantScriptEditor.view.state;
        return state.selection.main.head;
    }
    
    findVariantAtPosition(content, pos) {
        const variantsMatch = content.match(/variable\s+variants\s*\{/);
        if (!variantsMatch) return null;
        
        const variantsStart = variantsMatch.index + variantsMatch[0].length;
        
        let braceDepth = 1;
        let variantsEnd = variantsStart;
        for (let i = variantsStart; i < content.length && braceDepth > 0; i++) {
            if (content[i] === '{') braceDepth++;
            else if (content[i] === '}') braceDepth--;
            if (braceDepth === 0) variantsEnd = i;
        }
        
        const variantsBody = content.substring(variantsStart, variantsEnd);
        let searchPos = 0;
        
        while (searchPos < variantsBody.length) {
            while (searchPos < variantsBody.length && /\s/.test(variantsBody[searchPos])) {
                searchPos++;
            }
            
            if (searchPos >= variantsBody.length) break;
            
            let nameStart = searchPos;
            while (searchPos < variantsBody.length && /\w/.test(variantsBody[searchPos])) {
                searchPos++;
            }
            const variantName = variantsBody.substring(nameStart, searchPos);
            
            if (!variantName) break;
            
            while (searchPos < variantsBody.length && /\s/.test(variantsBody[searchPos])) {
                searchPos++;
            }
            
            if (variantsBody[searchPos] !== '{') break;
            
            const bodyStart = searchPos + 1;
            let depth = 1;
            searchPos++;
            
            while (searchPos < variantsBody.length && depth > 0) {
                if (variantsBody[searchPos] === '{') depth++;
                else if (variantsBody[searchPos] === '}') depth--;
                searchPos++;
            }
            
            const bodyEnd = searchPos - 1;
            const body = variantsBody.substring(bodyStart, bodyEnd);
            
            const absStart = variantsStart + nameStart;
            const absEnd = variantsStart + searchPos;
            
            if (pos >= absStart && pos <= absEnd) {
                return {
                    name: variantName,
                    body: body,
                    start: absStart,
                    end: absEnd
                };
            }
        }
        
        return null;
    }
    
    parseVariantBlock(body) {
        const dict = TclParser.parseDict(body.trim());
        
        return {
            description: dict.description || '',
            loader_proc: dict.loader_proc || '',
            loader_options: dict.loader_options ? TclParser.parseDict(dict.loader_options) : {},
            init: dict.init || '',
            deinit: dict.deinit || '',
            params: dict.params ? TclParser.parseDict(dict.params) : {}
        };
    }
    
    showNoParsedVariant() {
        if (this.variantElements.parsedVariantSelect) {
            this.variantElements.parsedVariantSelect.value = '';
        }
        if (this.variantElements.parsedStatus) {
            this.variantElements.parsedStatus.textContent = '';
            this.variantElements.parsedStatus.className = 'parsed-status';
        }
        if (this.variantElements.parsedContent) {
            this.variantElements.parsedContent.innerHTML = `
                <div class="parsed-empty">
                    <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5">
                        <circle cx="12" cy="12" r="10"></circle>
                        <path d="M12 16v-4"></path>
                        <path d="M12 8h.01"></path>
                    </svg>
                    <span>Place cursor inside a variant or select from dropdown</span>
                </div>
            `;
        }
        this.currentVariantName = null;
    }
    
    showParsedVariant(name, parsed, validation = { errors: [], warnings: [] }) {
        this.currentVariantName = name;
        
        const hasErrors = validation.errors.length > 0;
        const hasWarnings = validation.warnings.length > 0;
        
        if (this.variantElements.parsedVariantSelect) {
            this.variantElements.parsedVariantSelect.value = name;
        }
        if (this.variantElements.parsedStatus) {
            if (hasErrors) {
                this.variantElements.parsedStatus.textContent = `✗ ${validation.errors.length} error${validation.errors.length > 1 ? 's' : ''}`;
                this.variantElements.parsedStatus.className = 'parsed-status error';
            } else if (hasWarnings) {
                this.variantElements.parsedStatus.textContent = `⚠ ${validation.warnings.length} warning${validation.warnings.length > 1 ? 's' : ''}`;
                this.variantElements.parsedStatus.className = 'parsed-status warning';
            } else {
                this.variantElements.parsedStatus.textContent = '✓ Valid';
                this.variantElements.parsedStatus.className = 'parsed-status valid';
            }
        }
        if (this.variantElements.parsedContent) {
            let html = '';
            
            if (hasErrors) {
                html += `
                    <div class="parsed-validation errors">
                        <div class="validation-header">
                            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                                <circle cx="12" cy="12" r="10"></circle>
                                <line x1="15" y1="9" x2="9" y2="15"></line>
                                <line x1="9" y1="9" x2="15" y2="15"></line>
                            </svg>
                            Errors
                        </div>
                        ${validation.errors.map(e => `<div class="validation-item">${this.escapeHtml(e)}</div>`).join('')}
                    </div>
                `;
            }
            
            if (hasWarnings) {
                html += `
                    <div class="parsed-validation warnings">
                        <div class="validation-header">
                            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                                <path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"></path>
                                <line x1="12" y1="9" x2="12" y2="13"></line>
                                <line x1="12" y1="17" x2="12.01" y2="17"></line>
                            </svg>
                            Warnings
                        </div>
                        ${validation.warnings.map(w => `<div class="validation-item">${this.escapeHtml(w)}</div>`).join('')}
                    </div>
                `;
            }
            
            // Description
            html += `
                <div class="parsed-section">
                    <div class="parsed-section-label">Description</div>
                    <div class="parsed-section-value ${parsed.description ? '' : 'empty'}">
                        ${parsed.description ? this.escapeHtml(parsed.description) : 'No description'}
                    </div>
                </div>
            `;
            
            // Loader
            const loaderValid = !validation.errors.some(e => e.includes('loader'));
            html += `
                <div class="parsed-section">
                    <div class="parsed-section-label">Loader</div>
                    <div class="parsed-section-value inline-mono ${parsed.loader_proc ? '' : 'empty'} ${!loaderValid ? 'invalid' : ''}">
                        ${parsed.loader_proc ? this.escapeHtml(parsed.loader_proc) : 'No loader specified'}
                    </div>
                </div>
            `;
            
            // Loader Options
            const optionKeys = Object.keys(parsed.loader_options);
            if (optionKeys.length > 0) {
                html += `
                    <div class="parsed-section">
                        <div class="parsed-section-label">Loader Options</div>
                        <div class="parsed-options">
                            ${optionKeys.map(key => {
                                const hasWarning = validation.warnings.some(w => w.includes(`"${key}"`));
                                const optionValues = this.parseLoaderOptionValues(parsed.loader_options[key]);
                                const firstValue = optionValues[0]?.value || '';
                                const displayValue = this.truncateValue(firstValue, 20);
                                const needsTooltip = firstValue.length > 20;
                                return `
                                <div class="parsed-option ${hasWarning ? 'has-warning' : ''}">
                                    <span class="parsed-option-name">${this.escapeHtml(key)}</span>
                                    <div class="parsed-option-control">
                                        <select class="parsed-option-preview" title="Preview: ${optionValues.length} option(s)" onchange="this.nextElementSibling.textContent = '→ ' + this.options[this.selectedIndex].dataset.display; this.nextElementSibling.title = this.value;">
                                            ${optionValues.map(opt => {
                                                const dispVal = this.truncateValue(opt.value, 20);
                                                return `<option value="${this.escapeHtml(opt.value)}" data-display="${this.escapeHtml(dispVal)}">${this.escapeHtml(opt.label)}</option>`;
                                            }).join('')}
                                        </select>
                                        <span class="parsed-option-value ${needsTooltip ? 'has-tooltip' : ''}" title="${needsTooltip ? this.escapeHtml(firstValue) : ''}">→ ${this.escapeHtml(displayValue)}</span>
                                    </div>
                                </div>
                            `}).join('')}
                        </div>
                    </div>
                `;
            }
            
            // Params
            const paramKeys = Object.keys(parsed.params);
            if (paramKeys.length > 0) {
                html += `
                    <div class="parsed-section">
                        <div class="parsed-section-label">Params Override</div>
                        <div class="parsed-params">
                            ${paramKeys.map(key => {
                                const hasWarning = validation.warnings.some(w => w.includes(`"${key}"`));
                                return `
                                <div class="parsed-param ${hasWarning ? 'has-warning' : ''}">
                                    <span class="parsed-param-name">${this.escapeHtml(key)}</span>
                                    <span class="parsed-param-value">${this.escapeHtml(parsed.params[key])}</span>
                                </div>
                            `}).join('')}
                        </div>
                    </div>
                `;
            }
            
            // Init
            if (parsed.init) {
                const hasInitError = validation.errors.some(e => e.includes('Init script'));
                html += `
                    <div class="parsed-section">
                        <div class="parsed-section-label">Init ${hasInitError ? '<span class="label-error">⚠</span>' : ''}</div>
                        <div class="parsed-section-value mono ${hasInitError ? 'has-error' : ''}">${this.escapeHtml(parsed.init)}</div>
                    </div>
                `;
            }
            
            // Deinit
            if (parsed.deinit) {
                const hasDeinitError = validation.errors.some(e => e.includes('Deinit script'));
                html += `
                    <div class="parsed-section">
                        <div class="parsed-section-label">Deinit ${hasDeinitError ? '<span class="label-error">⚠</span>' : ''}</div>
                        <div class="parsed-section-value mono ${hasDeinitError ? 'has-error' : ''}">${this.escapeHtml(parsed.deinit)}</div>
                    </div>
                `;
            }
            
            this.variantElements.parsedContent.innerHTML = html;
        }
    }
    
    formatOptionValue(value) {
        if (value.length > 50) {
            return value.substring(0, 47) + '...';
        }
        return value;
    }
    
    truncateValue(value, maxLen = 20) {
        if (value.length > maxLen) {
            return value.substring(0, maxLen - 3) + '...';
        }
        return value;
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
    
    showParseError(name, message) {
        this.currentVariantName = name;
        
        if (this.variantElements.parsedVariantName) {
            this.variantElements.parsedVariantName.textContent = name;
        }
        if (this.variantElements.parsedStatus) {
            this.variantElements.parsedStatus.textContent = '✗ Error';
            this.variantElements.parsedStatus.className = 'parsed-status error';
        }
        if (this.variantElements.parsedContent) {
            this.variantElements.parsedContent.innerHTML = `
                <div class="parsed-error">
                    <div class="parsed-error-title">Parse Error</div>
                    <div class="parsed-error-message">${this.escapeHtml(message)}</div>
                </div>
            `;
        }
    }
    
    addNewVariant() {
        if (!this.variantScriptEditor?.view) return;
        
        const template = `
    new_variant {
        description "New variant description"
        loader_proc your_loader
        loader_options {
        }
        params {
        }
    }
`;
        
        const content = this.variantScriptEditor.getValue();
        const lastBrace = content.lastIndexOf('}');
        
        if (lastBrace > 0) {
            const newContent = content.substring(0, lastBrace) + template + '\n}';
            this.variantScriptEditor.setValue(newContent);
        } else {
            this.variantScriptEditor.setValue(content + template);
        }
    }
    
    duplicateCurrentVariant() {
        if (!this.variantScriptEditor?.view || !this.currentVariantName) {
            alert('Place cursor inside a variant first');
            return;
        }
        
        const content = this.variantScriptEditor.getValue();
        const cursorPos = this.getCursorPosition();
        const variantInfo = this.findVariantAtPosition(content, cursorPos);
        
        if (!variantInfo) {
            alert('Could not find variant at cursor');
            return;
        }
        
        const newName = prompt('Enter name for duplicated variant:', variantInfo.name + '_copy');
        if (!newName || newName === variantInfo.name) return;
        
        const duplicate = `\n    ${newName} {${variantInfo.body}}\n`;
        
        const newContent = content.substring(0, variantInfo.end) + duplicate + content.substring(variantInfo.end);
        this.variantScriptEditor.setValue(newContent);
    }

    // ==========================================
    // Loaders Editor
    // ==========================================

    initLoadersEditor() {
        this.loaderElements = {
            scriptEditor: document.getElementById('loader-script-editor'),
            loaderSelect: document.getElementById('loader-select'),
            runBtn: document.getElementById('loader-run-btn'),
            runStatus: document.getElementById('loader-run-status'),
            formatBtn: document.getElementById('loader-format-btn'),
            saveBtn: document.getElementById('loader-save-btn'),
            saveReloadBtn: document.getElementById('loader-save-reload-btn'),
            validationStatus: document.getElementById('loader-validation-status'),
            argsBody: document.getElementById('loader-args-body'),
            argsResetBtn: document.getElementById('loader-args-reset-btn'),
            tableContainer: document.getElementById('loader-table-container'),
            tableInfo: document.getElementById('loader-table-info'),
            consoleOutput: document.getElementById('loader-console-output'),
            errorCount: document.getElementById('loader-error-count'),
            consoleClearBtn: document.getElementById('loader-console-clear-btn')
        };

        this.loadersOriginalHash = null;
        this.loadersModified = false;
        this.loaderEditorPending = true;
        this.loaderSandbox = null;         // DservConnection for isolated subprocess
        this.loaderSandboxPrefix = null;   // private prefix for datapoints
        this.loaderSandboxDpManager = null;
        this.loaderErrorCount = 0;
        this.currentLoaderName = null;
        this.parsedLoaderDefs = [];        // [{name, args, bodyStart, bodyEnd}, ...]

        // Bind events
        this.loaderElements.formatBtn?.addEventListener('click', () => this.formatLoadersScript());
        this.loaderElements.saveBtn?.addEventListener('click', () => this.saveLoadersScript(false));
        this.loaderElements.saveReloadBtn?.addEventListener('click', () => this.saveLoadersScript(true));
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
        if (this.loaderEditorPending || !this.loaderScriptEditor?.view) {
            const ready = await this.ensureLoaderScriptEditor();
            if (ready) {
                await this.loadLoaderScriptContent();
            }
        }
        if (!this.loaderSandbox) {
            await this.initLoaderSandbox();
        }
    }

    async ensureLoaderScriptEditor() {
        const container = this.loaderElements.scriptEditor;
        if (!container) return false;

        if (typeof TclEditor === 'undefined') return false;

        if (this.loaderScriptEditor && this.loaderScriptEditor.view) {
            return true;
        }

        this.loaderScriptEditor = new TclEditor(container, {
            theme: 'dark',
            fontSize: '13px',
            tabSize: 4,
            lineNumbers: true,
            keybindings: 'emacs'
        });

        await new Promise(resolve => {
            container.addEventListener('editor-ready', resolve, { once: true });
        });

        this.loaderEditorPending = false;

        // Setup modification tracking
        this.setupLoaderCursorTracking();

        return this.loaderScriptEditor.view !== null;
    }

    setupLoaderCursorTracking() {
        if (!this.loaderScriptEditor?.view) return;

        let validateTimeout = null;
        const debouncedValidate = () => {
            if (validateTimeout) clearTimeout(validateTimeout);
            validateTimeout = setTimeout(() => this.validateLoadersFile(), 500);
        };

        const checkModified = async () => {
            if (this.loadersOriginalHash) {
                const currentContent = this.loaderScriptEditor.getValue();
                const currentHash = await sha256(currentContent);
                const isModified = currentHash !== this.loadersOriginalHash;
                if (isModified !== this.loadersModified) {
                    this.loadersModified = isModified;
                    this.updateLoaderSaveButton();
                }
            }
        };

        let parseTimeout = null;
        const debouncedParse = () => {
            if (parseTimeout) clearTimeout(parseTimeout);
            parseTimeout = setTimeout(() => {
                this.parseAndUpdateLoaderDropdown();
            }, 300);
        };

        const view = this.loaderScriptEditor.view;
        if (view) {
            view.dom.addEventListener('keyup', () => {
                debouncedValidate();
                checkModified();
                debouncedParse();
            });
            view.dom.addEventListener('input', () => {
                debouncedValidate();
                checkModified();
                debouncedParse();
            });
        }
    }

    // ==========================================
    // Loader Sandbox (Isolated Subprocess)
    // ==========================================

    async initLoaderSandbox() {
        try {
            this.loaderSandbox = new DservConnection({
                subprocess: 'loader_dev',
                createLinkedSubprocess: true,
                autoReconnect: true,
                onStatus: (status, msg) => {
                    console.log(`Loader sandbox: ${status} - ${msg}`);
                },
                onError: (err) => {
                    console.error('Loader sandbox error:', err);
                }
            });

            await this.loaderSandbox.connect();

            this.loaderSandboxPrefix = this.loaderSandbox.getLinkedSubprocess();

            if (this.loaderSandboxPrefix) {
                // Initialize with dlsh
                await this.loaderSandbox.sendToLinked('package require dlsh');

                // Create dg_view proc for pushing tables
                await this.loaderSandbox.sendToLinked(
                    `proc dg_view {dg {name ""}} {
                        if {$name eq ""} { set name $dg }
                        dservSet ${this.loaderSandboxPrefix}/tables/$name [dg_toHybridJSON $dg]
                    }`
                );

                // Subscribe to table updates
                this.loaderSandboxDpManager = new DatapointManager(this.loaderSandbox, {
                    autoGetKeys: false
                });

                this.loaderSandboxDpManager.subscribe(
                    `${this.loaderSandboxPrefix}/tables/*`,
                    (dp) => this.handleLoaderTablePush(dp)
                );

                // Subscribe to errors
                this.loaderSandboxDpManager.subscribe(
                    `error/${this.loaderSandboxPrefix}`,
                    (dp) => this.handleLoaderError(dp)
                );

                console.log('Loader sandbox ready:', this.loaderSandboxPrefix);

                // Enable run button if a loader is selected
                if (this.currentLoaderName) {
                    this.loaderElements.runBtn.disabled = false;
                }
            }
        } catch (e) {
            console.error('Failed to initialize loader sandbox:', e);
            this.logToLoaderConsole(`Sandbox init failed: ${e.message}`, 'error');
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

    handleLoaderError(datapoint) {
        const value = datapoint.data !== undefined ? datapoint.data : datapoint.value;
        if (!value) return;

        let errorInfo;
        if (typeof value === 'string') {
            try {
                const parsed = JSON.parse(value);
                errorInfo = parsed.errorInfo || parsed.message || value;
            } catch {
                errorInfo = value;
            }
        } else if (typeof value === 'object') {
            errorInfo = value.errorInfo || value.message || JSON.stringify(value);
        } else {
            return;
        }

        this.logToLoaderConsole(errorInfo, 'error');
    }

    // ==========================================
    // Loader Script Content
    // ==========================================

    async loadLoaderScriptContent() {
        if (!this.loaderScriptEditor?.view) return;

        let content = '';
        if (this.scripts.loaders) {
            content = this.scripts.loaders;
        }

        if (content) {
            this.loaderScriptEditor.setValue(content);
            this.loadersOriginalHash = await sha256(content);
            this.loadersModified = false;
            this.updateLoaderSaveButton();

            setTimeout(() => {
                this.parseAndUpdateLoaderDropdown();
                this.validateLoadersFile();
            }, 100);
        }
    }

    updateLoadersEditor() {
        if (!this.snapshot) return;

        if (this.loaderScriptEditor?.view && !this.loadersModified) {
            this.loadLoaderScriptContent();
        }

        // Also update parsed variants for cross-referencing loader_options
        if (this.snapshot.variants) {
            this.parsedVariantsForLoaders = this.parseVariantsDict(this.snapshot.variants);
        }
    }

    // ==========================================
    // Loader Parsing
    // ==========================================

    parseLoadersFromScript(content) {
        // Find all $s add_loader <name> { <arglist> } { <body> } patterns
        const loaders = [];
        const lines = content.split('\n');

        let i = 0;
        while (i < lines.length) {
            const line = lines[i];
            // Match: $s add_loader <name> { <args> } {
            const match = line.match(/\$\w+\s+add_loader\s+(\w+)\s+\{([^}]*)\}\s*\{/);
            if (match) {
                const name = match[1];
                const args = match[2].trim().split(/\s+/).filter(a => a);

                // Find the body by counting braces
                let braceCount = 0;
                let bodyStartLine = i;
                let bodyEndLine = i;

                // Count braces from the match line
                for (const ch of line) {
                    if (ch === '{') braceCount++;
                    if (ch === '}') braceCount--;
                }

                // The first two opening braces are for arglist and body
                // We need to find where the body brace closes
                if (braceCount > 0) {
                    let j = i + 1;
                    while (j < lines.length && braceCount > 0) {
                        for (const ch of lines[j]) {
                            if (ch === '{') braceCount++;
                            if (ch === '}') braceCount--;
                        }
                        if (braceCount > 0) j++;
                        else bodyEndLine = j;
                    }
                }

                // Calculate character positions
                let bodyStart = 0;
                for (let k = 0; k < bodyStartLine; k++) {
                    bodyStart += lines[k].length + 1;
                }
                let bodyEnd = 0;
                for (let k = 0; k <= bodyEndLine; k++) {
                    bodyEnd += lines[k].length + 1;
                }

                loaders.push({
                    name,
                    args,
                    bodyStartLine,
                    bodyEndLine,
                    bodyStart,
                    bodyEnd
                });
            }
            i++;
        }

        return loaders;
    }

    parseAndUpdateLoaderDropdown() {
        if (!this.loaderScriptEditor?.view) return;

        const content = this.loaderScriptEditor.getValue();
        this.parsedLoaderDefs = this.parseLoadersFromScript(content);

        const select = this.loaderElements.loaderSelect;
        if (!select) return;

        const prevValue = select.value;

        select.innerHTML = '<option value="">— select loader —</option>';
        this.parsedLoaderDefs.forEach(loader => {
            const opt = document.createElement('option');
            opt.value = loader.name;
            opt.textContent = `${loader.name} (${loader.args.join(', ')})`;
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

            const input = document.createElement('input');
            input.className = 'loader-arg-input';
            input.type = 'text';
            input.dataset.argName = argName;
            input.placeholder = 'value';

            // Pre-populate with first option from variants if available
            const opts = variantOptions[argName];
            if (opts && opts.length > 0) {
                // Use the first option value as default
                input.value = opts[0];
            }

            row.appendChild(input);

            // Add dropdown for variant options if available
            if (opts && opts.length > 1) {
                const optDiv = document.createElement('div');
                optDiv.className = 'loader-arg-options';

                const optSelect = document.createElement('select');
                optSelect.title = 'Choose from variant options';
                const emptyOpt = document.createElement('option');
                emptyOpt.value = '';
                emptyOpt.textContent = 'opts...';
                optSelect.appendChild(emptyOpt);

                opts.forEach(optVal => {
                    const o = document.createElement('option');
                    o.value = optVal;
                    o.textContent = optVal.length > 20 ? optVal.substring(0, 17) + '...' : optVal;
                    optSelect.appendChild(o);
                });

                optSelect.addEventListener('change', () => {
                    if (optSelect.value) {
                        input.value = optSelect.value;
                    }
                });

                optDiv.appendChild(optSelect);
                row.appendChild(optDiv);
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
                    const optValues = TclParser.parseList(optStr);
                    optValues.forEach(v => {
                        if (!options[argName].includes(v)) {
                            options[argName].push(v);
                        }
                    });
                }
            }
        }

        return options;
    }

    getLoaderArgValues() {
        const args = {};
        const inputs = this.loaderElements.argsBody?.querySelectorAll('.loader-arg-input');
        if (inputs) {
            inputs.forEach(input => {
                args[input.dataset.argName] = input.value;
            });
        }
        return args;
    }

    resetLoaderArgs() {
        const inputs = this.loaderElements.argsBody?.querySelectorAll('.loader-arg-input');
        if (inputs) {
            inputs.forEach(input => { input.value = ''; });
        }
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

        // Build test script
        const content = this.loaderScriptEditor.getValue();

        // Extract package requirements from the system script
        // (e.g., "package require points" in search.tcl)
        let packageCmds = '';
        const systemScript = this.scripts?.system || '';
        if (systemScript) {
            const pkgPattern = /^\s*package\s+require\s+(.+)$/gm;
            let pkgMatch;
            while ((pkgMatch = pkgPattern.exec(systemScript)) !== null) {
                const pkg = pkgMatch[1].trim();
                // Skip ess itself (system framework, not needed in sandbox)
                if (pkg !== 'ess') {
                    packageCmds += `catch {package require ${pkg}}\n`;
                }
            }
        }
        // Also scan the loaders script itself for package requires
        const loaderPkgPattern = /^\s*package\s+require\s+(.+)$/gm;
        let loaderPkgMatch;
        while ((loaderPkgMatch = loaderPkgPattern.exec(content)) !== null) {
            const pkg = loaderPkgMatch[1].trim();
            if (pkg !== 'ess' && !packageCmds.includes(pkg)) {
                packageCmds += `catch {package require ${pkg}}\n`;
            }
        }

        // Detect free variables in the loader body (screen_halfx, targ_radius, etc.)
        // These are variables referenced but not in the arg list or locally set.
        // We fetch them from the ESS process via "send ess" at run time.
        const freeVars = this.detectFreeVariables(content, loaderDef);
        let fetchVarsCmds = '';
        if (freeVars.length > 0) {
            // Use ess::get_variable to fetch each from the system object
            // Wrap in catch so missing variables don't abort the whole run
            fetchVarsCmds = freeVars.map(v =>
                `catch {set ${v} [send ess {ess::get_variable ${v}}]}`
            ).join('\n') + '\n';
        }

        const testScript = `
# Load required packages from system
${packageCmds}
# Fetch free variables from ESS system object
${fetchVarsCmds}
# Clean previous stimdg
if { [dg_exists stimdg] } { dg_delete stimdg }

# Define test loader proc with access to globals
proc _test_loader_ { ${loaderDef.args.join(' ')} } {
${freeVars.map(v => `    upvar #0 ${v} ${v}`).join('\n')}
${this.extractLoaderBody(content, loaderDef)}
}

# Call with args
set _result_ [_test_loader_ ${loaderDef.args.map(a => `{${argValues[a]}}`).join(' ')}]

# Push to viewer
dg_view $_result_ stimdg

# Return summary
set _nrows_ [dl_length [lindex [dg_tclListnames stimdg] 0]]
set _ncols_ [llength [dg_tclListnames stimdg]]
return "$_nrows_ rows, $_ncols_ columns"
`;

        // Update UI
        this.loaderElements.runBtn.disabled = true;
        this.setLoaderRunStatus('running...', '');

        try {
            const response = await this.loaderSandbox.sendToLinked(testScript);
            this.setLoaderRunStatus(response || 'done', 'success');
            this.logToLoaderConsole(`${this.currentLoaderName}: ${response}`, 'success');
        } catch (e) {
            this.setLoaderRunStatus('error', 'error');
            this.logToLoaderConsole(`${this.currentLoaderName}: ${e.message}`, 'error');
        } finally {
            this.loaderElements.runBtn.disabled = false;
        }
    }

    extractLoaderBody(content, loaderDef) {
        // Extract just the body lines between bodyStartLine and bodyEndLine
        const lines = content.split('\n');
        // The body starts after the opening brace line
        // and ends before the closing brace
        const bodyLines = [];
        for (let i = loaderDef.bodyStartLine + 1; i < loaderDef.bodyEndLine; i++) {
            bodyLines.push(lines[i]);
        }
        return bodyLines.join('\n');
    }

    /**
     * Detect variables referenced in a loader body that aren't in its argument list.
     * These are "free variables" that need upvar to access globals in the sandbox.
     */
    detectFreeVariables(content, loaderDef) {
        const body = this.extractLoaderBody(content, loaderDef);
        const args = new Set(loaderDef.args);

        // Find $varname references (not ${expr} or $::namespace)
        const varRefs = new Set();
        const varPattern = /\$([a-zA-Z_][a-zA-Z0-9_]*)/g;
        let match;
        while ((match = varPattern.exec(body)) !== null) {
            varRefs.add(match[1]);
        }

        // Find local set assignments (set varname ...)
        const localVars = new Set();
        const setPattern = /^\s*set\s+([a-zA-Z_][a-zA-Z0-9_]*)\s/gm;
        while ((match = setPattern.exec(body)) !== null) {
            localVars.add(match[1]);
        }

        // Free variables: referenced but not in args and not locally assigned before use
        // Also exclude common Tcl built-ins and loop vars
        const builtins = new Set(['g', 'n_obs', 'i', 'j', 'n', 'result', '_result_']);
        const freeVars = [];
        for (const v of varRefs) {
            if (!args.has(v) && !localVars.has(v) && !builtins.has(v)) {
                freeVars.push(v);
            }
        }

        return freeVars;
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
    // Save Loaders Script
    // ==========================================

    async saveLoadersScript(andReload = false) {
        if (!this.loaderScriptEditor?.view || !this.loadersModified) return;

        const content = this.loaderScriptEditor.getValue();
        const btn = andReload ? this.loaderElements.saveReloadBtn : this.loaderElements.saveBtn;

        const originalHtml = btn?.innerHTML;
        if (btn) {
            btn.disabled = true;
            btn.innerHTML = `
                <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="spin">
                    <circle cx="12" cy="12" r="10"></circle>
                </svg>
                Saving...
            `;
        }

        try {
            const saveCmd = `ess::save_script loaders {${content}}`;

            if (this.connection?.ws?.readyState === WebSocket.OPEN) {
                this.connection.ws.send(JSON.stringify({
                    cmd: 'eval',
                    script: saveCmd
                }));

                this.loadersOriginalHash = await sha256(content);
                this.loadersModified = false;

                if (andReload) {
                    if (btn) {
                        btn.innerHTML = `
                            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="spin">
                                <circle cx="12" cy="12" r="10"></circle>
                            </svg>
                            Reloading...
                        `;
                    }

                    await new Promise(resolve => setTimeout(resolve, 200));

                    this.connection.ws.send(JSON.stringify({
                        cmd: 'eval',
                        script: 'ess::reload_system'
                    }));
                }

                this._pluginHook('onLoadersSaved', content);

                if (btn) {
                    btn.innerHTML = `
                        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                            <polyline points="20 6 9 17 4 12"></polyline>
                        </svg>
                        ${andReload ? 'Reloaded!' : 'Saved!'}
                    `;
                    setTimeout(() => this.resetLoaderSaveButton(), 2000);
                }
            } else {
                throw new Error('WebSocket not connected');
            }
        } catch (error) {
            console.error('Failed to save loaders script:', error);
            alert(`Failed to save: ${error.message}`);
            this.resetLoaderSaveButton();
        }
    }

    updateLoaderSaveButton() {
        const btn = this.loaderElements.saveBtn;
        const reloadBtn = this.loaderElements.saveReloadBtn;

        if (btn) {
            btn.disabled = !this.loadersModified;
            btn.classList.toggle('modified', this.loadersModified);
        }
        if (reloadBtn) {
            reloadBtn.disabled = !this.loadersModified;
            reloadBtn.classList.toggle('modified', this.loadersModified);
        }
    }

    resetLoaderSaveButton() {
        const btn = this.loaderElements.saveBtn;
        const reloadBtn = this.loaderElements.saveReloadBtn;

        if (btn) {
            btn.innerHTML = `
                <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                    <path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"></path>
                    <polyline points="17 21 17 13 7 13 7 21"></polyline>
                    <polyline points="7 3 7 8 15 8"></polyline>
                </svg>
                Save
            `;
        }
        if (reloadBtn) {
            reloadBtn.innerHTML = `
                <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                    <polyline points="23 4 23 10 17 10"></polyline>
                    <path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"></path>
                </svg>
                Save &amp; Reload
            `;
        }
        this.updateLoaderSaveButton();
    }

    // ==========================================
    // Loader Validation & Formatting
    // ==========================================

    validateLoadersFile() {
        if (!this.loaderScriptEditor?.view) return;

        const content = this.loaderScriptEditor.getValue();
        const statusEl = this.loaderElements.validationStatus;
        if (!statusEl) return;

        if (typeof TclLinter !== 'undefined') {
            try {
                const linter = new TclLinter();
                const result = linter.lint(content);
                if (result.errors?.length > 0) {
                    statusEl.textContent = `${result.errors.length} error(s)`;
                    statusEl.className = 'loaders-validation-status error';
                } else if (result.warnings?.length > 0) {
                    statusEl.textContent = `${result.warnings.length} warning(s)`;
                    statusEl.className = 'loaders-validation-status warning';
                } else {
                    statusEl.textContent = 'valid';
                    statusEl.className = 'loaders-validation-status valid';
                }
            } catch (e) {
                statusEl.textContent = 'lint error';
                statusEl.className = 'loaders-validation-status error';
            }
        }
    }

    async formatLoadersScript() {
        if (!this.loaderScriptEditor?.view) return;

        if (typeof TclFormatter === 'undefined') return;

        const content = this.loaderScriptEditor.getValue();

        try {
            const formatted = TclFormatter.formatTclCode(content, 4);
            this.loaderScriptEditor.setValue(formatted);

            if (this.loadersOriginalHash) {
                const currentHash = await sha256(formatted);
                this.loadersModified = currentHash !== this.loadersOriginalHash;
                this.updateLoaderSaveButton();
            }

            this.validateLoadersFile();
            this.parseAndUpdateLoaderDropdown();
        } catch (e) {
            console.error('Format error:', e);
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
