/**
 * ESS Workbench - Main Application Script
 * 
 * Handles:
 * - WebSocket connection and snapshot subscription
 * - Tab navigation
 * - Dashboard component rendering
 * - Script editor integration
 * - State diagram visualization
 */


/**
 * Compute SHA256 hash of a string
 * Uses Web Crypto API when available, falls back to pure JS
 * Returns lowercase hex string (64 chars)
 */
async function sha256(text) {
    const encoder = new TextEncoder();
    const data = encoder.encode(text);
    
    // Try Web Crypto API first (only works in secure contexts)
    if (typeof crypto !== 'undefined' && crypto.subtle) {
        const hashBuffer = await crypto.subtle.digest('SHA-256', data);
        const hashArray = Array.from(new Uint8Array(hashBuffer));
        return hashArray.map(b => b.toString(16).padStart(2, '0')).join('');
    }
    
    // Fallback: pure JS SHA256 implementation
    return sha256_fallback(data);
}

/**
 * Pure JS SHA256 implementation (for non-secure contexts)
 */
function sha256_fallback(data) {
    // Convert Uint8Array to array if needed
    const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
    
    // SHA256 constants
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
    
    // Initial hash values
    let h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
    let h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;
    
    // Pre-processing: adding padding bits
    const bitLen = bytes.length * 8;
    const padded = new Uint8Array(Math.ceil((bytes.length + 9) / 64) * 64);
    padded.set(bytes);
    padded[bytes.length] = 0x80;
    
    // Append length in bits as 64-bit big-endian
    const view = new DataView(padded.buffer);
    view.setUint32(padded.length - 4, bitLen, false);
    
    // Helper functions
    const rotr = (x, n) => ((x >>> n) | (x << (32 - n))) >>> 0;
    const ch = (x, y, z) => ((x & y) ^ (~x & z)) >>> 0;
    const maj = (x, y, z) => ((x & y) ^ (x & z) ^ (y & z)) >>> 0;
    const sigma0 = x => (rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22)) >>> 0;
    const sigma1 = x => (rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25)) >>> 0;
    const gamma0 = x => (rotr(x, 7) ^ rotr(x, 18) ^ (x >>> 3)) >>> 0;
    const gamma1 = x => (rotr(x, 17) ^ rotr(x, 19) ^ (x >>> 10)) >>> 0;
    
    // Process each 64-byte chunk
    for (let i = 0; i < padded.length; i += 64) {
        const w = new Uint32Array(64);
        
        // Copy chunk into first 16 words
        for (let j = 0; j < 16; j++) {
            w[j] = view.getUint32(i + j * 4, false);
        }
        
        // Extend to 64 words
        for (let j = 16; j < 64; j++) {
            w[j] = (gamma1(w[j-2]) + w[j-7] + gamma0(w[j-15]) + w[j-16]) >>> 0;
        }
        
        // Initialize working variables
        let a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
        
        // Main loop
        for (let j = 0; j < 64; j++) {
            const t1 = (h + sigma1(e) + ch(e, f, g) + K[j] + w[j]) >>> 0;
            const t2 = (sigma0(a) + maj(a, b, c)) >>> 0;
            h = g; g = f; f = e;
            e = (d + t1) >>> 0;
            d = c; c = b; b = a;
            a = (t1 + t2) >>> 0;
        }
        
        // Add to hash
        h0 = (h0 + a) >>> 0; h1 = (h1 + b) >>> 0;
        h2 = (h2 + c) >>> 0; h3 = (h3 + d) >>> 0;
        h4 = (h4 + e) >>> 0; h5 = (h5 + f) >>> 0;
        h6 = (h6 + g) >>> 0; h7 = (h7 + h) >>> 0;
    }
    
    // Convert to hex string
    const toHex = n => n.toString(16).padStart(8, '0');
    return toHex(h0) + toHex(h1) + toHex(h2) + toHex(h3) +
           toHex(h4) + toHex(h5) + toHex(h6) + toHex(h7);
}    

class ESSWorkbench {
    constructor() {
        // Connection state
        this.connection = null;
        this.snapshot = null;
        this.autoReload = true;
        this.registry = null;  // ESS Registry client
        
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
    
    init() {
        this.cacheElements();
        this.bindEvents();
        this.startClock();
        this.initVariantsEditor();
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
            
            // Parameters (legacy - removed from dashboard but may be used elsewhere)
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
            this.initRegistry();  // Initialize registry after connection
        });
        
        this.connection.on('disconnected', () => {
            this.updateConnectionStatus('disconnected');
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
        
        // Subscribe using DatapointManager
        this.dpManager.subscribe('ess/snapshot', (data) => {
            console.log('Snapshot callback fired:', data);
            const value = data.data !== undefined ? data.data : data.value;
            console.log('Snapshot value:', value);
            this.handleSnapshot(value);
        });
        
        // Touch the datapoint to trigger initial value
        if (this.connection && this.connection.ws) {
            this.connection.ws.send(JSON.stringify({
                cmd: 'touch',
                name: 'ess/snapshot'
            }));
            console.log('Touch sent for ess/snapshot');
        }
        
        console.log('Subscription sent');
    }
    
    async initRegistry() {
        try {
            console.log('Initializing ESS Registry...');
            
            // Query dserv for registry configuration
            const registryUrlDp = await this.dpManager.get('ess/registry/url');
            const workgroupDp = await this.dpManager.get('ess/registry/workgroup');
            
            // Extract values from datapoint objects
            const registryUrl = registryUrlDp?.data || registryUrlDp?.value || '';
            const workgroup = workgroupDp?.data || workgroupDp?.value || 'brown-sheinberg';
            
            console.log('Registry config from dserv:', { registryUrl, workgroup });
            
            // Create registry client
            this.registry = new RegistryClient({
                baseUrl: registryUrl,
                workgroup: workgroup
            });
            
            console.log('Registry client initialized');
            
            // Test connection and load available systems
            if (this.registry.workgroup) {
                try {
                    const systems = await this.registry.getSystems();
                    console.log(`Registry connected: ${systems.length} systems available`);
                } catch (err) {
                    console.warn('Registry connection test failed:', err);
                }
            }
            
            // Set up registry UI features if the integration module is loaded
            if (typeof this.setupRegistryUI === 'function') {
                await this.setupRegistryUI();
            }
            
        } catch (err) {
            console.error('Failed to initialize registry from dserv config:', err);
            
            // Fallback: try URL parameters
            try {
                const urlParams = new URLSearchParams(window.location.search);
                const agentUrl = urlParams.get('agent') || '';
                const workgroup = urlParams.get('workgroup') || 'brown-sheinberg';
                
                console.log('Falling back to URL params:', { agentUrl, workgroup });
                
                this.registry = new RegistryClient({
                    baseUrl: agentUrl,
                    workgroup: workgroup
                });
                
                // Set up UI even with fallback
                if (typeof this.setupRegistryUI === 'function') {
                    await this.setupRegistryUI();
                }
            } catch (fallbackErr) {
                console.error('Registry initialization completely failed:', fallbackErr);
            }
        }
    }
    
    // ==========================================
    // Snapshot Handling
    // ==========================================
    
    handleSnapshot(data) {
        console.log('handleSnapshot called with:', typeof data, data);
        try {
            // Parse JSON if string
            const snapshot = typeof data === 'string' ? JSON.parse(data) : data;
            console.log('Parsed snapshot:', snapshot);
            
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
	    
            console.log('Parsed scripts:', Object.keys(this.scripts));
            
            // Hide sidebar entries for scripts that don't exist
            this.updateScriptSidebarVisibility();
            
            // Update displays
            console.log('Updating displays...');
            this.updateConfigDisplay();
            this.updateVariantsEditor();
            this.updateScriptEditor();
            this.updateStatesDiagram();
            
            // Update timestamp
            this.updateSnapshotTime(snapshot.timestamp);
            console.log('Display update complete');
            
        } catch (e) {
            console.error('Failed to parse snapshot:', e);
        }
    }
    
    // ==========================================
    // Tcl Parsing Helpers (using TclParser.js)
    // ==========================================
    
    /**
     * Parse scripts dict: "system {...} protocol {...} loaders {...} variants {...} stim {...}"
     */
    parseScriptsDict(str) {
        if (!str || typeof str !== 'string') return {};
        return TclParser.parseDict(str);
    }
    
    /**
     * Parse current_loader string into structured object
     * Format: "loader_proc name loader_args {...} loader_arg_names {...} loader_arg_options {...}"
     */
    parseCurrentLoader(str) {
        if (!str || typeof str !== 'string') return null;
        
        const dict = TclParser.parseDict(str);
        
        // Parse loader_arg_names as list
        const argNames = TclParser.parseList(dict.loader_arg_names || '');
        
        // Parse loader_arg_options - each option is "name {{label1 val1} {label2 val2}}"
        // Format is {label value} where label is display name, value is the actual value
        const argOptions = {};
        if (dict.loader_arg_options) {
            const optDict = TclParser.parseDict(dict.loader_arg_options);
            for (const [name, optStr] of Object.entries(optDict)) {
                // optStr is like "{{4 4} {6 6}}" or "{{ jittered {...} }}" 
                const opts = TclParser.parseList(optStr);
                argOptions[name] = opts.map(opt => {
                    const parts = TclParser.parseList(opt);
                    if (parts.length >= 2) {
                        // Format: {label value} - label is first, value is second
                        return { label: parts[0], value: parts[1] };
                    } else if (parts.length === 1) {
                        // Single value, use as both label and value
                        return { label: parts[0], value: parts[0] };
                    }
                    return { label: opt, value: opt };
                });
            }
        }
        
        // Parse loader_args as list (could be single bare value or braced list)
        const loaderArgsRaw = dict.loader_args || '';
        const loaderArgs = TclParser.parseList(loaderArgsRaw);
        
        return {
            loader_proc: dict.loader_proc,
            loader_args: loaderArgs,
            loader_arg_names: argNames,
            loader_arg_options: argOptions
        };
    }
    
    /**
     * Parse params using TclParser's parseParamSettings
     */
    parseParams(str) {
        if (!str || typeof str !== 'string') return {};
        
        const parsed = TclParser.parseParamSettings(str);
        const params = {};
        
        // Convert to our expected format
        for (const [name, info] of Object.entries(parsed)) {
            params[name] = {
                default: info.value,
                value: info.value,
                flag: info.varType,   // 1=time, 2=variable, etc.
                type: info.dataType || 'string'
            };
        }
        
        return params;
    }
    
    /**
     * Parse states string: "state1 next1 state2 {next2a next2b} state3 next3"
     * Returns object with transitions
     */
    parseStates(str) {
        if (!str || typeof str !== 'string') return {};
        
        const list = TclParser.parseList(str);
        const states = {};
        
        for (let i = 0; i < list.length - 1; i += 2) {
            const stateName = list[i];
            const nextStates = list[i + 1];
            
            // Parse next states (could be single or braced list)
            const transitions = TclParser.parseList(nextStates);
            
            states[stateName] = {
                transitions: transitions.filter(t => t && t !== '{}')
            };
        }
        
        return states;
    }
    
    /**
     * Parse loaders string: "{name loader1 args {arg1 arg2}} {name loader2 args {...}}"
     */
    parseLoaders(str) {
        if (!str || typeof str !== 'string') return [];
        
        // Could be single loader or list of loaders
        const items = TclParser.parseList(str);
        const loaders = [];
        
        // Check if it's a single loader dict or multiple
        if (items.length > 0 && items[0] === 'name') {
            // Single loader as flat dict
            const dict = TclParser.parseDict(str);
            loaders.push({
                name: dict.name,
                args: TclParser.parseList(dict.args || '')
            });
        } else {
            // Multiple loaders
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
    
    /**
     * Parse variant_args: "arg1 val1 arg2 val2"
     */
    parseVariantArgs(str) {
        if (!str || typeof str !== 'string') return {};
        return TclParser.parseDict(str);
    }
    
    clearDashboard() {
        // Reset config display
        ['cfgProject', 'cfgSystem', 'cfgProtocol', 'cfgVariant', 'cfgVersion', 'cfgSubject'].forEach(key => {
            if (this.elements[key]) this.elements[key].textContent = '—';
        });
        
        // Reset stats
        ['statStates', 'statParams', 'statLoaders', 'statVariants'].forEach(key => {
            if (this.elements[key]) this.elements[key].textContent = '0';
        });
        
        // Clear variant args
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
        
        // Update config hero values
        if (this.elements.cfgProject) this.elements.cfgProject.textContent = s.project || '—';
        if (this.elements.cfgSystem) this.elements.cfgSystem.textContent = s.system || '—';
        if (this.elements.cfgProtocol) this.elements.cfgProtocol.textContent = s.protocol || '—';
        if (this.elements.cfgVariant) this.elements.cfgVariant.textContent = s.variant || '—';
        if (this.elements.cfgVersion) this.elements.cfgVersion.textContent = s.version || '—';
        if (this.elements.cfgSubject) this.elements.cfgSubject.textContent = s.subject_id || '—';
        
        // Update tab mini-heroes
        this.updateTabHeroes();
    }
    
    updateTabHeroes() {
        const s = this.snapshot;
        if (!s) return;
        
        // Update all tab heroes
        const tabs = ['variants', 'scripts', 'states'];
        
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
        
        // Count states
        const states = this.parseStates(s.states || '');
        const stateCount = Object.keys(states).length;
        if (this.elements.statStates) this.elements.statStates.textContent = stateCount;
        
        // Count params
        const params = this.parseParams(s.params || '');
        const paramCount = Object.keys(params).length;
        if (this.elements.statParams) this.elements.statParams.textContent = paramCount;
        
        // Count loaders
        const loaders = this.parseLoaders(s.loaders || '');
        const loaderCount = loaders.length;
        if (this.elements.statLoaders) this.elements.statLoaders.textContent = loaderCount;
        
        // Count variants from the variants string
        const variantCount = this.countVariants(s.variants || '');
        if (this.elements.statVariants) this.elements.statVariants.textContent = variantCount;
    }
    
    countVariants(str) {
        if (!str || typeof str !== 'string') return 0;
        // Variants dict has format: "variant1 {...} variant2 {...}"
        // Count top-level keys
        const list = TclParser.parseList(str.trim());
        // Every pair is a variant name + definition
        return Math.floor(list.length / 2);
    }
    
    // ==========================================
    // Variant Args
    // ==========================================
    
    updateVariantArgs() {
        const container = this.elements.variantArgsContainer;
        if (!container) return;
        
        // Parse the current_loader string
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
                // Dropdown for options with choices
                html += `
                    <div class="variant-arg-row">
                        <span class="variant-arg-label">${this.escapeHtml(argName)}</span>
                        <select class="variant-arg-select" data-arg="${this.escapeHtml(argName)}">
                            ${options.map((opt, idx) => {
                                const label = typeof opt === 'object' ? opt.label : opt;
                                const value = typeof opt === 'object' ? opt.value : opt;
                                // Normalize whitespace for comparison
                                const normCurrent = this.normalizeWhitespace(currentValue);
                                const normValue = this.normalizeWhitespace(value);
                                const selected = normValue === normCurrent ? 'selected' : '';
                                // Use index as value to avoid issues with complex dict values
                                return `<option value="${idx}" ${selected}>${this.escapeHtml(label)}</option>`;
                            }).join('')}
                        </select>
                    </div>
                `;
            } else {
                // Text input for free-form values
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
        
        // Store options for lookup when sending commands
        this._currentArgOptions = argOptions;
        
        // Bind change events
        container.querySelectorAll('.variant-arg-select').forEach(el => {
            el.addEventListener('change', (e) => {
                const argName = e.target.dataset.arg;
                let value = e.target.value;
                
                // If it's a select (index-based), look up the actual value
                if (e.target.tagName === 'SELECT' && this._currentArgOptions[argName]) {
                    const idx = parseInt(value, 10);
                    const opt = this._currentArgOptions[argName][idx];
                    value = opt ? opt.value : value;
                }
                
                this.onVariantArgChange(argName, value);
            });
        });
    }
    
    /**
     * Normalize whitespace for comparison (collapse multiple spaces/newlines)
     */
    normalizeWhitespace(str) {
        if (typeof str !== 'string') return String(str);
        return str.replace(/\s+/g, ' ').trim();
    }
    
    onVariantArgChange(argName, value) {
        if (!this.connection || !this.connection.connected) return;
        
        // Send the command to update variant args
        const cmd = `ess::set_variant_args ${argName} {${value}}`;
        this.connection.send(cmd).catch(err => {
            console.error('Failed to set variant arg:', err);
        });
        
        // Auto-reload if enabled
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
            // flag 1 = time parameter
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
        
        // Bind change events
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
        
        // Determine start and end states
        // First state is typically start, state with empty transitions is end
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
        
        // Update tab buttons
        document.querySelectorAll('.tab-btn').forEach(btn => {
            btn.classList.toggle('active', btn.dataset.tab === tabName);
        });
        
        // Update tab content
        document.querySelectorAll('.tab-content').forEach(content => {
            content.classList.toggle('active', content.id === `tab-${tabName}`);
        });
        
        // Initialize tab-specific content
        if (tabName === 'scripts' && !this.editor) {
            this.initEditor();
        } else if (tabName === 'states') {
            this.updateStatesDiagram();
        } else if (tabName === 'variants') {
            // Initialize variant editor now that tab is visible
            this.initVariantsTab();
        }
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
            console.log('Loading variant script content:', this.scripts.variants.length, 'chars');
            this.variantScriptEditor.setValue(this.scripts.variants);
        } else if (this.snapshot?.variants) {
            console.log('Loading variant content from snapshot');
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
            
            // Wait for editor to be ready
            await new Promise(resolve => {
		this.elements.editorContainer.addEventListener('editor-ready', resolve, { once: true });
            });
            
            // Load initial script
            await this.loadScript(this.currentScript);
            
            // Set up debounced change detection
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
            
	    // Listen for changes (debounced)
	    const onEditorChange = () => {
		if (!this.scriptOriginalHash) return;
		
		if (checkTimeout) clearTimeout(checkTimeout);
		checkTimeout = setTimeout(checkModified, 300);
		
		// Clear lint status when editing (user will re-lint manually)
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
	
	// Update selection UI
	this.elements.scriptsList?.querySelectorAll('.script-item').forEach(item => {
            item.classList.toggle('active', item.dataset.script === scriptName);
	});
	
	await this.loadScript(scriptName);
    }
    
    async loadScript(scriptName) {
	if (!this.editor) return;
	
	const content = this.scripts[scriptName] || '';
	this.editor.setValue(content);
	
	// Compute and store hash for change detection
	this.scriptOriginalHash = await sha256(content);
	this.scriptModified = false;
	
	// Disable save button (no changes yet)
	const saveBtn = document.getElementById('script-save-btn');
	if (saveBtn) saveBtn.disabled = true;
	
	// Update filename display with real filename
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
    
    // Hide sidebar entries for optional scripts that don't exist on disk
    // Also update labels to show clean type names
    updateScriptSidebarVisibility() {
        // Optional script types — hide when empty, show when present
        const optionalTypes = ['sys_extract', 'proto_extract', 'sys_analyze'];
        
        // Clean display labels (no .tcl)
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
        
        // Real filenames for tooltips
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
            
            // Update label text
            const label = item.querySelector('span:not(.script-status-dot)');
            if (label && labels[scriptType]) {
                label.textContent = labels[scriptType];
            }
            
            // Set tooltip to real filename
            if (filenames[scriptType]) {
                item.title = filenames[scriptType];
            }
            
            // Hide optional types when empty
            if (optionalTypes.includes(scriptType)) {
                const hasContent = this.scripts[scriptType] && this.scripts[scriptType].trim() !== '';
                item.style.display = hasContent ? '' : 'none';
                
                // If we're currently viewing a hidden script, switch to system
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
            // Show first error with line number
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
		
		// Jump to error line on click
		this.elements.editorStatus.onclick = () => {
                    this.jumpToLine(issue.line);
                    // Log all issues to console for reference
                    console.log('Lint results:', result);
		};
            } else {
		this.elements.editorStatus.textContent = result.summary;
		this.elements.editorStatus.style.color = 'var(--wb-error)';
            }
            
            // Don't auto-clear errors - let user see them
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
        
        // Extract transitions from state data
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
        
        // Simple layout algorithm
        const nodeWidth = 120;
        const nodeHeight = 40;
        const hSpacing = 80;
        const vSpacing = 60;
        const padding = 40;
        
        // Calculate grid layout
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
        
        // Set SVG size
        const width = padding * 2 + cols * (nodeWidth + hSpacing) - hSpacing;
        const height = padding * 2 + rows * (nodeHeight + vSpacing) - vSpacing;
        svg.setAttribute('viewBox', `0 0 ${width} ${height}`);
        
        let svgContent = '';
        
        // Determine start/end states
        const startState = stateNames[0];
        const endStates = new Set();
        stateNames.forEach(name => {
            const state = states[name];
            if (!state.transitions || state.transitions.length === 0 || name.toLowerCase() === 'end') {
                endStates.add(name);
            }
        });
        
        // Draw edges first (so they appear behind nodes)
        Object.entries(states).forEach(([fromState, stateData]) => {
            if (stateData.transitions) {
                stateData.transitions.forEach(toState => {
                    if (positions[fromState] && positions[toState]) {
                        const from = positions[fromState];
                        const to = positions[toState];
                        
                        // Calculate edge points
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
                            
                            // Draw line
                            svgContent += `
                                <line x1="${startX}" y1="${startY}" x2="${endX}" y2="${endY}" 
                                      class="state-diagram-edge" marker-end="url(#arrowhead)"/>
                            `;
                        }
                    }
                });
            }
        });
        
        // Draw nodes
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
        
        // Wrap with defs for arrowhead marker
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
    // Variants Editor
    // ==========================================
    
    initVariantsEditor() {
        // Cache variant editor elements
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
        
        // Track original content for modified detection
        this.variantsOriginalHash = null;
        this.variantsModified = false;
        
        // Bind button events
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
        
        // Bind variant selector dropdown
        this.variantElements.parsedVariantSelect?.addEventListener('change', (e) => {
            this.jumpToVariant(e.target.value);
        });
        
        // Defer editor init
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
            
            // Preserve cursor position roughly
            const cursorPos = this.getCursorPosition();
            
            this.variantScriptEditor.setValue(formatted);
            
            // Try to restore cursor near original position
            const newLength = formatted.length;
            const newPos = Math.min(cursorPos, newLength);
            
            this.variantScriptEditor.view.dispatch({
                selection: { anchor: newPos }
            });

	    // Check if modified and update save button
	    if (this.variantsOriginalHash) {
		const currentHash = await sha256(formatted);
		this.variantsModified = currentHash !== this.variantsOriginalHash;
		this.updateSaveButton();
	    }
            
            // Re-validate
            this.validateEntireFile();
            this.populateVariantDropdown();
            this.updateParsedView();
            
        } catch (e) {
            console.error('Format error:', e);
        }
    }
    
    initVariantScriptEditor() {
        // Defer until tab is visible - CodeMirror needs visible container
        console.log('initVariantScriptEditor called - deferring until tab visible');
        this.variantEditorPending = true;
    }
    
    async ensureVariantScriptEditor() {
        const container = this.variantElements.scriptEditor;
        console.log('ensureVariantScriptEditor - container:', container, 'pending:', this.variantEditorPending);
        
        if (!container) {
            console.warn('Variant script editor container not found');
            return false;
        }
        
        if (typeof TclEditor === 'undefined') {
            console.warn('TclEditor not defined');
            return false;
        }
        
        // If already have a working editor, return true
        if (this.variantScriptEditor && this.variantScriptEditor.view) {
            return true;
        }
        
        // Create the editor now that tab should be visible
        console.log('Creating variant script editor now');
        this.variantScriptEditor = new TclEditor(container, {
            theme: 'dark',
            fontSize: '13px',
            tabSize: 4,
            lineNumbers: true,
            keybindings: 'emacs'
        });
        
        // Wait for editor to be ready (same pattern as Scripts tab)
        await new Promise(resolve => {
            container.addEventListener('editor-ready', resolve, { once: true });
        });
        
        this.variantEditorPending = false;
        console.log('Variant script editor ready, view:', this.variantScriptEditor.view);
        
        // Setup cursor change listener for tracking current variant
        this.setupCursorTracking();
        
        return this.variantScriptEditor.view !== null;
    }
    
    setupCursorTracking() {
        if (!this.variantScriptEditor?.view) return;
        
        // Debounced update on cursor/content change
        let updateTimeout = null;
        const debouncedUpdate = () => {
            if (updateTimeout) clearTimeout(updateTimeout);
            updateTimeout = setTimeout(() => {
                this.updateParsedView();
            }, 150);
        };
        
        // Debounced file validation (longer delay)
        let validateTimeout = null;
        const debouncedValidate = () => {
            if (validateTimeout) clearTimeout(validateTimeout);
            validateTimeout = setTimeout(() => {
                this.validateEntireFile();
            }, 500);
        };
        
        // Check for modifications
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
        
        // Listen for cursor position changes via CodeMirror
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
            
            // Refresh highlight on scroll (since CM virtualizes lines)
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
        
        // Check for errors first
        const statusEl = this.variantElements.fileValidationStatus;
        if (statusEl?.classList.contains('error')) {
            const proceed = confirm('This file has validation errors. Save anyway?');
            if (!proceed) return;
        }
        
        // Update button to show saving
        const originalHtml = btn?.innerHTML;
        if (btn) {
            btn.disabled = true;
            btn.innerHTML = `
                <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="spin">
                    <circle cx="12" cy="12" r="10"></circle>
                </svg>
                ${andReload ? 'Saving...' : 'Saving...'}
            `;
        }
        
        try {
            // Send save command via WebSocket
            const saveCmd = `ess::save_script variants {${content}}`;
            
            if (this.connection?.ws?.readyState === WebSocket.OPEN) {
                this.connection.ws.send(JSON.stringify({
                    cmd: 'eval',
                    script: saveCmd
                }));
                
                // Mark as saved
		this.variantsOriginalHash = await sha256(content);
                this.variantsModified = false;
                
                console.log('Variants script saved successfully');
                
                // If reload requested, send reload command
                if (andReload) {
                    if (btn) {
                        btn.innerHTML = `
                            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="spin">
                                <circle cx="12" cy="12" r="10"></circle>
                            </svg>
                            Reloading...
                        `;
                    }
                    
                    // Small delay then reload
                    await new Promise(resolve => setTimeout(resolve, 200));
                    
                    this.connection.ws.send(JSON.stringify({
                        cmd: 'eval',
                        script: 'ess::reload_system'
                    }));
                    
                    console.log('System reload triggered');
                }
                
                // Show success briefly
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
        
        // 1. Run TclLinter on entire file
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
        
        // 2. Check that we can find the variants block
        const variantsMatch = content.match(/variable\s+variants\s*\{/);
        if (!variantsMatch) {
            errors.push('Missing "variable variants { ... }" block');
        }
        
        // 3. Find all variants and validate each
        const variants = this.findAllVariants(content);
        if (variants.length === 0 && variantsMatch) {
            warnings.push('No variants defined');
        }
        
        // 4. Check each variant has required fields
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
        
        // Update status display
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
        
        // Parse variants from snapshot (for reference)
        const variantsStr = this.snapshot.variants || '';
        this.parsedVariants = this.parseVariantsDict(variantsStr);
        console.log('Parsed variants:', Object.keys(this.parsedVariants));
        
        // Update script editor if it's ready (tab visible)
        if (this.variantScriptEditor?.view) {
            this.loadVariantScriptContent();
        }
    }
    
    parseVariantsDict(str) {
        if (!str || typeof str !== 'string') return {};
        
        const variants = {};
        const list = TclParser.parseList(str.trim());
        
        // Parse as key-value pairs: variant_name {definition}
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
            console.log('Loading variant script content:', this.scripts.variants.length, 'chars');
            content = this.scripts.variants;
        } else if (this.snapshot?.variants) {
            console.log('Loading variant content from snapshot');
            content = this.snapshot.variants;
        }

	if (content) {
	    this.variantScriptEditor.setValue(content);
	    
	    // Track original hash for modified detection
	    this.variantsOriginalHash = await sha256(content);
	    this.variantsModified = false;
	    this.updateSaveButton();
	    
	    // Populate dropdown, trigger initial parse, and validate file
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
        
        // Find "variable variants {" section
        const variantsMatch = content.match(/variable\s+variants\s*\{/);
        if (!variantsMatch) return variants;
        
        const variantsStart = variantsMatch.index + variantsMatch[0].length;
        
        // Find matching closing brace
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
            // Skip whitespace
            while (searchPos < variantsBody.length && /\s/.test(variantsBody[searchPos])) {
                searchPos++;
            }
            
            if (searchPos >= variantsBody.length) break;
            
            // Read variant name
            let nameStart = searchPos;
            while (searchPos < variantsBody.length && /\w/.test(variantsBody[searchPos])) {
                searchPos++;
            }
            const variantName = variantsBody.substring(nameStart, searchPos);
            
            if (!variantName) break;
            
            // Skip to opening brace
            while (searchPos < variantsBody.length && /\s/.test(variantsBody[searchPos])) {
                searchPos++;
            }
            
            if (variantsBody[searchPos] !== '{') break;
            
            // Find matching close brace
            let depth = 1;
            searchPos++;
            while (searchPos < variantsBody.length && depth > 0) {
                if (variantsBody[searchPos] === '{') depth++;
                else if (variantsBody[searchPos] === '}') depth--;
                searchPos++;
            }
            
            // Store variant with absolute position
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
        
        // Set cursor position to start of variant
        view.dispatch({
            selection: { anchor: variant.start }
        });
        
        // Scroll so the line is ~25% from top
        const line = view.state.doc.lineAt(variant.start);
        const lineBlock = view.lineBlockAt(variant.start);
        const editorHeight = view.dom.clientHeight;
        const targetScroll = lineBlock.top - (editorHeight * 0.25);
        
        view.scrollDOM.scrollTop = Math.max(0, targetScroll);
        
        // Focus the editor
        view.focus();
        
        // Trigger parse update
        setTimeout(() => this.updateParsedView(), 50);
    }
    
    updateParsedView() {
        if (!this.variantScriptEditor?.view) return;
        
        const content = this.variantScriptEditor.getValue();
        const cursorPos = this.getCursorPosition();
        
        // Find which variant the cursor is in
        const variantInfo = this.findVariantAtPosition(content, cursorPos);
        
        if (!variantInfo) {
            this.showNoParsedVariant();
            this.clearVariantHighlight();
            return;
        }
        
        // Highlight the variant in the editor
        this.highlightVariant(variantInfo.start, variantInfo.end);
        
        // Parse the variant
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
        
        // Get line numbers for start and end
        const startLine = doc.lineAt(start);
        const endLine = doc.lineAt(end);
        
        // Remove existing highlight and add new one
        // We'll use a CSS class on the gutter/lines
        this.currentHighlight = { start: startLine.number, end: endLine.number };
        
        // Apply highlight via DOM manipulation (simpler than CM extensions)
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
        
        // Remove all existing highlights
        view.dom.querySelectorAll('.variant-highlight-gutter').forEach(el => {
            el.classList.remove('variant-highlight-gutter');
        });
        
        if (!this.currentHighlight) return;
        
        // Highlight gutter line numbers
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
        
        // Get available loaders from snapshot
        const availableLoaders = this.getAvailableLoaders();
        const availableParams = this.getAvailableParams();
        
        // 1. Check loader_proc exists
        if (!parsed.loader_proc) {
            errors.push('Missing required field: loader_proc');
        } else if (availableLoaders.length > 0 && !availableLoaders.find(l => l.name === parsed.loader_proc)) {
            errors.push(`Unknown loader: "${parsed.loader_proc}". Available: ${availableLoaders.map(l => l.name).join(', ')}`);
        }
        
        // 2. Check loader_options match loader's expected args
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
        
        // 3. Check params exist in system params
        if (parsed.params && availableParams.length > 0) {
            const paramKeys = Object.keys(parsed.params);
            paramKeys.forEach(key => {
                if (!availableParams.includes(key)) {
                    warnings.push(`Unknown param: "${key}"`);
                }
            });
        }
        
        // 4. Run Tcl linter on init/deinit scripts
        if (parsed.init && typeof TclLinter !== 'undefined') {
            try {
                const linter = new TclLinter();
                const initLint = linter.lint(parsed.init);
                if (initLint.errors?.length > 0) {
                    errors.push(`Init script: ${initLint.errors[0].message}`);
                }
            } catch (e) {
                // Linter failed, skip
            }
        }
        
        if (parsed.deinit && typeof TclLinter !== 'undefined') {
            try {
                const linter = new TclLinter();
                const deinitLint = linter.lint(parsed.deinit);
                if (deinitLint.errors?.length > 0) {
                    errors.push(`Deinit script: ${deinitLint.errors[0].message}`);
                }
            } catch (e) {
                // Linter failed, skip
            }
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
        // Structure: namespace eval X::Y { variable variants { variant1 {...} variant2 {...} } }
        // We need to find variant blocks inside "variable variants { ... }"
        
        // First, find the "variable variants {" section
        const variantsMatch = content.match(/variable\s+variants\s*\{/);
        if (!variantsMatch) {
            return null;
        }
        
        const variantsStart = variantsMatch.index + variantsMatch[0].length;
        
        // Find the matching closing brace for the variants block
        let braceDepth = 1;
        let variantsEnd = variantsStart;
        for (let i = variantsStart; i < content.length && braceDepth > 0; i++) {
            if (content[i] === '{') braceDepth++;
            else if (content[i] === '}') braceDepth--;
            if (braceDepth === 0) variantsEnd = i;
        }
        
        const variantsBody = content.substring(variantsStart, variantsEnd);
        
        // Now find individual variant blocks within the variants body
        // Pattern: variant_name { ... }
        let searchPos = 0;
        
        while (searchPos < variantsBody.length) {
            // Skip whitespace
            while (searchPos < variantsBody.length && /\s/.test(variantsBody[searchPos])) {
                searchPos++;
            }
            
            if (searchPos >= variantsBody.length) break;
            
            // Read variant name (identifier)
            let nameStart = searchPos;
            while (searchPos < variantsBody.length && /\w/.test(variantsBody[searchPos])) {
                searchPos++;
            }
            const variantName = variantsBody.substring(nameStart, searchPos);
            
            if (!variantName) break;
            
            // Skip whitespace to opening brace
            while (searchPos < variantsBody.length && /\s/.test(variantsBody[searchPos])) {
                searchPos++;
            }
            
            if (variantsBody[searchPos] !== '{') {
                break;
            }
            
            // Find matching close brace
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
            
            // Calculate absolute positions
            const absStart = variantsStart + nameStart;
            const absEnd = variantsStart + searchPos;
            
            // Check if cursor is in this variant
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
        // Parse the variant body as a Tcl dict
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
        // Reset dropdown selection
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
        
        // Update dropdown selection
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
            
            // Show errors first
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
            
            // Show warnings
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
            
            // Loader Options - show as dropdown previews with value sent
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
        // Truncate long values
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
        // Parse loader option values:
        // 1. Bare values: { 2 1 4 } -> each item is label AND value
        // 2. Label/value pairs: { {Small 0.2} {Large 0.3} } -> exactly 2 parts = {label, value}
        // Rule: if item splits into exactly 2 parts, it's a pair; otherwise it's a bare value
        
        const values = [];
        const items = TclParser.parseList(optionStr);
        
        for (const item of items) {
            const parts = TclParser.parseList(item);
            
            if (parts.length === 2) {
                // Exactly 2 parts = label/value pair
                values.push({
                    label: parts[0],
                    value: parts[1]
                });
            } else {
                // Anything else = bare value (use whole item as both label and value)
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
        
        // Append to end of file (before final closing brace if in namespace)
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
        
        // Prompt for new name
        const newName = prompt('Enter name for duplicated variant:', variantInfo.name + '_copy');
        if (!newName || newName === variantInfo.name) return;
        
        // Create duplicate
        const duplicate = `\n    ${newName} {${variantInfo.body}}\n`;
        
        // Insert after current variant
        const newContent = content.substring(0, variantInfo.end) + duplicate + content.substring(variantInfo.end);
        this.variantScriptEditor.setValue(newContent);
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
