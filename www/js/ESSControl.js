/**
 * ESSControl.js
 * Experiment System State Control Panel Component
 * 
 * Provides UI for:
 * - Subject selection
 * - System/Protocol/Variant selection
 * - State control (Initialize, Go, Stop, Reset)
 * - Parameter viewing and editing
 * - Variant option selection
 */

class ESSControl {
    constructor(container, dpManager, options = {}) {
        this.container = typeof container === 'string' 
            ? document.getElementById(container) 
            : container;
        this.dpManager = dpManager;
        
        this.options = {
            autoReload: true,  // Auto-reload variant on setting change
            ...options
        };
        
        // State
        this.state = {
            subjects: [],
            systems: [],
            protocols: [],
            variants: [],
            currentSubject: '',
            currentSystem: '',
            currentProtocol: '',
            currentVariant: '',
            essStatus: 'stopped',  // running, stopped, loading
            params: {},
            variantInfo: null,
            currentDatafile: ''  // Currently open datafile path
        };
        
        // Event listeners
        this.listeners = new Map();
        
        // Build UI
        this.render();
        
        // Setup datapoint subscriptions
        this.setupSubscriptions();
    }
    
    /**
     * Render the control panel UI
     */
    render() {
        this.container.innerHTML = `
            <!-- State Controls -->
            <div class="ess-control-section">
                <div class="ess-state-controls">
                    <button id="ess-btn-go" class="ess-state-btn go" disabled>▶ Go</button>
                    <button id="ess-btn-stop" class="ess-state-btn stop" disabled>■ Stop</button>
                    <button id="ess-btn-reset" class="ess-state-btn reset" disabled>Reset</button>
                </div>
            </div>
            
            <!-- Subject and Juice - combined row -->
            <div class="ess-control-section">
                <div class="ess-subject-juice-row">
                    <span class="ess-control-label">Subject</span>
                    <div class="ess-control-input">
                        <select id="ess-subject-select">
                            <option value="">-- Select --</option>
                        </select>
                    </div>
                    <button id="ess-btn-juice" class="ess-state-btn juice">Juice</button>
                    <input type="number" id="ess-juice-amount" value="0.5" min="0.1" max="5" step="0.1" title="Juice amount (sec)">
                </div>
            </div>
            
            <!-- System Selection -->
            <div class="ess-control-section">
                <div class="ess-control-section-title-row">
                    <span class="ess-control-section-title">System Configuration</span>
                    <a href="stimdg_viewer.html" target="_blank" class="ess-stimdg-btn">StimDG</a>
                </div>
                <div class="ess-control-row-with-reload">
                    <span class="ess-control-label">System</span>
                    <div class="ess-control-input">
                        <select id="ess-system-select">
                            <option value="">-- Select System --</option>
                        </select>
                    </div>
                    <button id="ess-reload-system" class="ess-reload-btn" title="Reload System">↻</button>
                </div>
                <div class="ess-control-row-with-reload">
                    <span class="ess-control-label">Protocol</span>
                    <div class="ess-control-input">
                        <select id="ess-protocol-select">
                            <option value="">-- Select Protocol --</option>
                        </select>
                    </div>
                    <button id="ess-reload-protocol" class="ess-reload-btn" title="Reload Protocol">↻</button>
                </div>
                <div class="ess-control-row-with-reload">
                    <span class="ess-control-label">Variant</span>
                    <div class="ess-control-input">
                        <select id="ess-variant-select">
                            <option value="">-- Select Variant --</option>
                        </select>
                    </div>
                    <button id="ess-reload-variant" class="ess-reload-btn" title="Reload Variant">↻</button>
                </div>
                <!-- Datafile row -->
                <div class="ess-datafile-row" id="ess-datafile-row">
                    <span class="ess-control-label">Datafile</span>
                    <span class="ess-file-name" id="ess-current-file">No file</span>
                    <button id="ess-btn-file-open" class="ess-file-btn" title="Open datafile">Open</button>
                    <button id="ess-btn-file-close" class="ess-file-btn close" title="Close datafile" style="display: none;">Close</button>
                </div>
            </div>
            
            <!-- Variant Options -->
            <div class="ess-control-section" id="ess-options-section" style="display: none;">
                <div class="ess-control-section-title">Variant Options</div>
                <div class="ess-auto-reload">
                    <input type="checkbox" id="ess-auto-reload" checked>
                    <label for="ess-auto-reload">Auto-reload</label>
                    <button id="ess-reload-options-btn" class="ess-reload-variant-btn">Reload</button>
                    <button id="ess-btn-settings-save" class="ess-settings-btn">Save</button>
                    <button id="ess-btn-settings-reset" class="ess-settings-btn">Reset</button>
                </div>
                <div id="ess-options-container" class="ess-options-container"></div>
            </div>
            
            <!-- Parameters -->
            <div class="ess-control-section" id="ess-params-section" style="display: none;">
                <div class="ess-control-section-title">Parameters</div>
                <div id="ess-params-container" class="ess-params-container"></div>
            </div>
        `;
        
        // Cache element references
        this.elements = {
            subjectSelect: this.container.querySelector('#ess-subject-select'),
            systemSelect: this.container.querySelector('#ess-system-select'),
            protocolSelect: this.container.querySelector('#ess-protocol-select'),
            variantSelect: this.container.querySelector('#ess-variant-select'),
            btnReset: this.container.querySelector('#ess-btn-reset'),
            btnGo: this.container.querySelector('#ess-btn-go'),
            btnStop: this.container.querySelector('#ess-btn-stop'),
            btnJuice: this.container.querySelector('#ess-btn-juice'),
            juiceAmount: this.container.querySelector('#ess-juice-amount'),
            reloadSystem: this.container.querySelector('#ess-reload-system'),
            reloadProtocol: this.container.querySelector('#ess-reload-protocol'),
            reloadVariant: this.container.querySelector('#ess-reload-variant'),
            optionsSection: this.container.querySelector('#ess-options-section'),
            optionsContainer: this.container.querySelector('#ess-options-container'),
            autoReloadCheckbox: this.container.querySelector('#ess-auto-reload'),
            reloadOptionsBtn: this.container.querySelector('#ess-reload-options-btn'),
            paramsSection: this.container.querySelector('#ess-params-section'),
            paramsContainer: this.container.querySelector('#ess-params-container'),
            // File controls
            datafileRow: this.container.querySelector('#ess-datafile-row'),
            btnFileOpen: this.container.querySelector('#ess-btn-file-open'),
            btnFileClose: this.container.querySelector('#ess-btn-file-close'),
            currentFile: this.container.querySelector('#ess-current-file'),
            // Settings controls
            btnSettingsSave: this.container.querySelector('#ess-btn-settings-save'),
            btnSettingsReset: this.container.querySelector('#ess-btn-settings-reset')
        };
        
        // Bind event handlers
        this.bindEvents();
    }
    
    /**
     * Bind UI event handlers
     */
    bindEvents() {
        // Selection changes
        this.elements.subjectSelect.addEventListener('change', (e) => {
            this.setSubject(e.target.value);
        });
        
        this.elements.systemSelect.addEventListener('change', (e) => {
            this.setSystem(e.target.value);
        });
        
        this.elements.protocolSelect.addEventListener('change', (e) => {
            this.setProtocol(e.target.value);
        });
        
        this.elements.variantSelect.addEventListener('change', (e) => {
            this.setVariant(e.target.value);
        });
        
        // Reload buttons
        this.elements.reloadSystem.addEventListener('click', () => {
            this.reloadSystem();
        });
        
        this.elements.reloadProtocol.addEventListener('click', () => {
            this.reloadProtocol();
        });
        
        this.elements.reloadVariant.addEventListener('click', () => {
            this.reloadVariant();
        });
        
        // Auto-reload checkbox
        this.elements.autoReloadCheckbox.addEventListener('change', (e) => {
            this.options.autoReload = e.target.checked;
        });
        
        // Manual reload button for variant options
        this.elements.reloadOptionsBtn.addEventListener('click', () => {
            this.reloadVariant();
        });
        
        // State control buttons - send actual ESS commands
        this.elements.btnReset.addEventListener('click', () => {
            this.resetExperiment();
        });
        
        this.elements.btnGo.addEventListener('click', () => {
            this.startExperiment();
        });
        
        this.elements.btnStop.addEventListener('click', () => {
            this.stopExperiment();
        });
        
        // Juice button
        this.elements.btnJuice.addEventListener('click', () => {
            this.giveJuice();
        });
        
        // File open button (auto-suggests filename)
        this.elements.btnFileOpen.addEventListener('click', () => {
            this.openDatafile();
        });
        
        // File close button
        this.elements.btnFileClose.addEventListener('click', () => {
            this.closeDatafile();
        });
        
        // Settings buttons
        this.elements.btnSettingsSave.addEventListener('click', () => {
            this.saveSettings();
        });
        
        this.elements.btnSettingsReset.addEventListener('click', () => {
            this.resetSettings();
        });
    }
    
    /**
     * Setup datapoint subscriptions
     */
    setupSubscriptions() {
        // Subject list
        this.dpManager.subscribe('ess/subject_ids', (data) => {
            this.updateSubjects(data.value);
        });
        
        // Current subject
        this.dpManager.subscribe('ess/subject', (data) => {
            this.state.currentSubject = data.value;
            this.updateSelectValue(this.elements.subjectSelect, data.value);
        });
        
        // Systems list
        this.dpManager.subscribe('ess/systems', (data) => {
            this.updateSystems(data.value);
        });
        
        // Current system
        this.dpManager.subscribe('ess/system', (data) => {
            this.state.currentSystem = data.value;
            this.updateSelectValue(this.elements.systemSelect, data.value);
        });
        
        // Protocols list
        this.dpManager.subscribe('ess/protocols', (data) => {
            this.updateProtocols(data.value);
        });
        
        // Current protocol
        this.dpManager.subscribe('ess/protocol', (data) => {
            this.state.currentProtocol = data.value;
            this.updateSelectValue(this.elements.protocolSelect, data.value);
        });
        
        // Variants list
        this.dpManager.subscribe('ess/variants', (data) => {
            this.updateVariants(data.value);
        });
        
        // Current variant
        this.dpManager.subscribe('ess/variant', (data) => {
            this.state.currentVariant = data.value;
            this.updateSelectValue(this.elements.variantSelect, data.value);
        });
        
        // ESS status (running, stopped, loading)
        this.dpManager.subscribe('ess/status', (data) => {
            this.updateEssStatus(data.value);
        });
        
        // Variant info (JSON format with options)
        this.dpManager.subscribe('ess/variant_info_json', (data) => {
            this.updateVariantInfo(data.value);
        });
        
        // Parameter settings
        this.dpManager.subscribe('ess/param_settings', (data) => {
            this.updateParams(data.value);
        });
        
        // Individual parameter updates
        this.dpManager.subscribe('ess/param', (data) => {
            this.updateParamValue(data.value);
        });
        
        this.dpManager.subscribe('ess/params', (data) => {
            this.updateParamValue(data.value);
        });
        
        // Current datafile
        this.dpManager.subscribe('ess/datafile', (data) => {
            this.updateDatafileStatus(data.value);
        });
    }
    
    /**
     * Update subjects dropdown
     */
    updateSubjects(data) {
        const subjects = this.parseListData(data);
        this.state.subjects = subjects;
        
        this.populateSelect(this.elements.subjectSelect, subjects, '-- Select Subject --');
        
        // Restore current selection if set
        if (this.state.currentSubject) {
            this.updateSelectValue(this.elements.subjectSelect, this.state.currentSubject);
        }
    }
    
    /**
     * Update systems dropdown
     */
    updateSystems(data) {
        const systems = this.parseListData(data);
        this.state.systems = systems;
        
        this.populateSelect(this.elements.systemSelect, systems, '-- Select System --');
        
        if (this.state.currentSystem) {
            this.updateSelectValue(this.elements.systemSelect, this.state.currentSystem);
        }
    }
    
    /**
     * Update protocols dropdown
     */
    updateProtocols(data) {
        const protocols = this.parseListData(data);
        this.state.protocols = protocols;
        
        this.populateSelect(this.elements.protocolSelect, protocols, '-- Select Protocol --');
        
        if (this.state.currentProtocol) {
            this.updateSelectValue(this.elements.protocolSelect, this.state.currentProtocol);
        }
    }
    
    /**
     * Update variants dropdown
     */
    updateVariants(data) {
        const variants = this.parseListData(data);
        this.state.variants = variants;
        
        this.populateSelect(this.elements.variantSelect, variants, '-- Select Variant --');
        
        if (this.state.currentVariant) {
            this.updateSelectValue(this.elements.variantSelect, this.state.currentVariant);
        }
    }
    
    /**
     * Update ESS status and button states
     * Values: 'running', 'stopped', 'loading'
     */
    updateEssStatus(status) {
        this.state.essStatus = status;
        
        // Enable/disable selection controls based on status
        // Only allow changes when stopped
        const canChangeConfig = (status === 'stopped');
        this.setControlsEnabled(canChangeConfig);
        
        // Add/remove loading class for visual feedback
        if (status === 'loading') {
            this.container.classList.add('ess-loading');
        } else {
            this.container.classList.remove('ess-loading');
        }
        
        // Emit event for external listeners (e.g., status bar)
        this.emit('stateChange', { state: status });
        
        // Update button states based on current status
        this.updateButtonStates();
    }
    
    /**
     * Update button enabled states based on current ESS status
     */
    updateButtonStates() {
        const status = this.state.essStatus;
        
        // Reset all buttons to disabled
        this.elements.btnReset.disabled = true;
        this.elements.btnGo.disabled = true;
        this.elements.btnStop.disabled = true;
        
        switch (status) {
            case 'stopped':
                // When stopped: can Go or Reset
                this.elements.btnGo.disabled = false;
                this.elements.btnReset.disabled = false;
                break;
            case 'running':
                // When running: can only Stop
                this.elements.btnStop.disabled = false;
                break;
            case 'loading':
                // When loading: all buttons disabled
                break;
            default:
                // Unknown state - enable Go if we have a variant
                if (this.state.currentVariant) {
                    this.elements.btnGo.disabled = false;
                }
                break;
        }
    }
    
    /**
     * Enable/disable selection controls
     */
    setControlsEnabled(enabled) {
        this.elements.subjectSelect.disabled = !enabled;
        this.elements.systemSelect.disabled = !enabled;
        this.elements.protocolSelect.disabled = !enabled;
        this.elements.variantSelect.disabled = !enabled;
        
        // Reload buttons
        this.elements.reloadSystem.disabled = !enabled;
        this.elements.reloadProtocol.disabled = !enabled;
        this.elements.reloadVariant.disabled = !enabled;
        this.elements.reloadOptionsBtn.disabled = !enabled;
        
        // Also disable option selects in variant options
        const optionSelects = this.elements.optionsContainer.querySelectorAll('select');
        optionSelects.forEach(select => {
            select.disabled = !enabled;
        });
    }
    
    /**
     * Update variant info/options from JSON
     */
    updateVariantInfo(data) {
        try {
            const info = typeof data === 'string' ? JSON.parse(data) : data;
            this.state.variantInfo = info;
            
            if (!info || !info.options || !info.loader_arg_names) {
                this.elements.optionsSection.style.display = 'none';
                return;
            }
            
            this.renderVariantOptions(info);
            this.elements.optionsSection.style.display = 'block';
            
        } catch (e) {
            console.error('Failed to parse variant info:', e);
            this.elements.optionsSection.style.display = 'none';
        }
    }
    
    /**
     * Render variant options (dropdowns for loader arguments)
     * Matches Vue's parseVariantInfoJson logic
     */
    renderVariantOptions(info) {
        const container = this.elements.optionsContainer;
        container.innerHTML = '';
        
        const { options, loader_arg_names, loader_args } = info;
        
        loader_arg_names.forEach((argName, index) => {
            const argOptions = options[argName];
            if (!argOptions || !Array.isArray(argOptions)) return;
            
            // Get the current value from loader_args
            const currentValue = loader_args[index];
            
            const row = document.createElement('div');
            row.className = 'ess-option-row';
            
            const label = document.createElement('span');
            label.className = 'ess-option-label';
            label.textContent = argName;
            
            const select = document.createElement('select');
            select.className = 'ess-option-select';
            select.dataset.argName = argName;
            select.dataset.argIndex = index;
            
            // Find the selected value - check opt.selected first, then match against currentValue
            let selectedIndex = 0;
            let foundSelected = false;
            
            argOptions.forEach((opt, optIndex) => {
                const option = document.createElement('option');
                option.value = opt.value;
                option.textContent = opt.label;
                select.appendChild(option);
                
                // Check if this option is selected
                if (opt.selected === true) {
                    selectedIndex = optIndex;
                    foundSelected = true;
                } else if (!foundSelected && currentValue !== undefined) {
                    // Match against currentValue from loader_args
                    const optValue = String(opt.value).trim();
                    const curValue = String(currentValue).trim();
                    if (optValue === curValue) {
                        selectedIndex = optIndex;
                    }
                }
            });
            
            select.selectedIndex = selectedIndex;
            
            // Handle option change
            select.addEventListener('change', (e) => {
                this.onVariantOptionChange(argName, e.target.value);
            });
            
            row.appendChild(label);
            row.appendChild(select);
            container.appendChild(row);
        });
    }
    
    /**
     * Handle variant option change
     */
    onVariantOptionChange(argName, value) {
        // Send command to set variant option
        // Format matches Vue: ess::set_variant_args {optionName {value}}
        const cmd = `ess::set_variant_args {${argName} {${value}}}`;
        this.sendCommand(cmd);
        
        if (this.options.autoReload) {
            // Reload the variant to apply changes
            this.sendCommand('ess::reload_variant');
        }
    }
    
    /**
     * Update parameters display
     */
    updateParams(data) {
        try {
            const params = TclParser.parseParamSettings(data);
            this.state.params = params;
            
            if (Object.keys(params).length === 0) {
                this.elements.paramsSection.style.display = 'none';
                return;
            }
            
            this.renderParams(params);
            this.elements.paramsSection.style.display = 'block';
            
        } catch (e) {
            console.error('Failed to parse params:', e);
            this.elements.paramsSection.style.display = 'none';
        }
    }
    
    /**
     * Render parameters as input fields
     */
    renderParams(params) {
        const container = this.elements.paramsContainer;
        container.innerHTML = '';
        
        for (const [name, info] of Object.entries(params)) {
            const row = document.createElement('div');
            row.className = 'ess-param-row';
            
            const label = document.createElement('span');
            label.className = 'ess-param-label';
            label.textContent = name;
            label.title = name;
            
            // Color code based on type (1=time, 2=variable)
            if (info.varType === '1') {
                label.classList.add('time');
            } else {
                label.classList.add('variable');
            }
            
            row.appendChild(label);
            
            // Handle boolean as checkbox
            if (info.dataType === 'bool' || info.dataType === 'boolean') {
                const checkbox = document.createElement('input');
                checkbox.type = 'checkbox';
                checkbox.className = 'ess-param-checkbox';
                checkbox.checked = info.value === '1' || info.value === 'true' || info.value === true || info.value === 1;
                checkbox.dataset.paramName = name;
                checkbox.dataset.dataType = info.dataType;
                
                checkbox.addEventListener('change', (e) => {
                    this.onParamChange(name, e.target.checked ? '1' : '0');
                });
                
                row.appendChild(checkbox);
            } else {
                const input = document.createElement('input');
                input.type = 'text';
                input.className = 'ess-param-input';
                input.value = info.value || '';
                input.dataset.paramName = name;
                input.dataset.dataType = info.dataType;
                
                // Set input type and validation based on datatype
                if (info.dataType === 'int') {
                    input.type = 'text';
                    input.inputMode = 'numeric';
                    input.pattern = '-?[0-9]*';
                    // Filter non-integer input
                    input.addEventListener('input', (e) => {
                        e.target.value = e.target.value.replace(/[^0-9-]/g, '');
                        // Only allow one minus at the start
                        if (e.target.value.indexOf('-') > 0) {
                            e.target.value = e.target.value.replace(/-/g, '');
                        }
                    });
                } else if (info.dataType === 'float') {
                    input.type = 'text';
                    input.inputMode = 'decimal';
                    input.pattern = '-?[0-9]*\\.?[0-9]*';
                    // Filter non-float input
                    input.addEventListener('input', (e) => {
                        let val = e.target.value;
                        // Allow digits, one decimal point, and minus at start
                        val = val.replace(/[^0-9.-]/g, '');
                        // Only one decimal point
                        const parts = val.split('.');
                        if (parts.length > 2) {
                            val = parts[0] + '.' + parts.slice(1).join('');
                        }
                        // Only allow minus at start
                        if (val.indexOf('-') > 0) {
                            val = val.charAt(0) + val.slice(1).replace(/-/g, '');
                        }
                        e.target.value = val;
                    });
                }
                
                // Handle parameter change
                input.addEventListener('change', (e) => {
                    this.onParamChange(name, e.target.value);
                });
                
                input.addEventListener('keydown', (e) => {
                    if (e.key === 'Enter') {
                        this.onParamChange(name, e.target.value);
                        e.target.blur();
                    }
                });
                
                row.appendChild(input);
            }
            
            container.appendChild(row);
        }
    }
    
    /**
     * Handle parameter value change
     */
    onParamChange(name, value) {
        // Set the parameter value via ess::set_param command (like Vue)
        const cmd = `ess::set_param ${name} ${value}`;
        this.sendCommand(cmd);
        
        this.emit('paramChange', { name, value });
    }
    
    /**
     * Update a single parameter value
     */
    updateParamValue(data) {
        // Parse key-value pairs: "param1 val1 param2 val2"
        const updates = TclParser.parseKeyValue(data);
        
        for (const [name, value] of Object.entries(updates)) {
            // Update state
            if (this.state.params[name]) {
                this.state.params[name].value = value;
            }
            
            // Update UI
            const input = this.elements.paramsContainer.querySelector(
                `input[data-param-name="${name}"]`
            );
            if (input && document.activeElement !== input) {
                if (input.type === 'checkbox') {
                    input.checked = value === '1' || value === 'true' || value === true || value === 1;
                } else {
                    input.value = value;
                }
            }
        }
    }
    
    /**
     * Set subject
     */
    setSubject(subject) {
        if (!subject) return;
        this.sendCommand(`ess::set_subject ${subject}`);
    }
    
    /**
     * Set system (triggers protocol list update)
     */
    setSystem(system) {
        if (!system) return;
        this.sendCommand(`evalNoReply {ess::load_system ${system}}`);
    }
    
    /**
     * Set protocol (triggers variant list update)
     */
    setProtocol(protocol) {
        if (!protocol || !this.state.currentSystem) return;
        this.sendCommand(
            `evalNoReply {ess::load_system ${this.state.currentSystem} ${protocol}}`
        );
    }
    
    /**
     * Set variant
     */
    setVariant(variant) {
        if (!variant || !this.state.currentSystem || !this.state.currentProtocol) return;
        this.sendCommand(
            `evalNoReply {ess::load_system ${this.state.currentSystem} ${this.state.currentProtocol} ${variant}}`
        );
    }
    
    /**
     * Start the experiment
     */
    startExperiment() {
        this.sendCommand('ess::start');
    }
    
    /**
     * Stop the experiment
     */
    stopExperiment() {
        this.sendCommand('ess::stop');
    }
    
    /**
     * Reset the experiment
     */
    resetExperiment() {
        this.sendCommand('ess::reset');
    }
    
    /**
     * Reload the current system
     */
    reloadSystem() {
        this.sendCommand('ess::reload_system');
    }
    
    /**
     * Reload the current protocol
     */
    reloadProtocol() {
        this.sendCommand('ess::reload_protocol');
    }
    
    /**
     * Reload the current variant
     */
    reloadVariant() {
        this.sendCommand('ess::reload_variant');
    }
    
    /**
     * Give juice reward
     */
    giveJuice() {
        const amount = this.elements.juiceAmount.value || 50;
        this.sendCommand(`send juicer reward ${amount}`);
    }
    
    /**
     * Toggle datafile open/close
     */
    /**
     * Open a datafile using auto-suggested filename
     */
    openDatafile() {
        // Use file_suggest to get filename, then open it
        // The ess::file_suggest command returns the suggested filename
        // Then we call ess::file_open with that filename
        this.sendCommand('ess::file_open [ess::file_suggest]');
        this.emit('log', { message: 'Opening datafile...', level: 'info' });
    }
    
    /**
     * Close the current datafile
     */
    closeDatafile() {
        if (!this.state.currentDatafile) {
            this.emit('log', { message: 'No datafile is currently open', level: 'warning' });
            return;
        }
        
        // Send close command - result will come via ess/datafile subscription
        this.sendCommand('ess::file_close');
        this.emit('log', { message: 'Closing datafile...', level: 'info' });
    }
    
    /**
     * Update datafile status display
     * Called when ess/datafile subscription updates
     */
    updateDatafileStatus(filepath) {
        const wasOpen = !!this.state.currentDatafile;
        const isOpen = !!filepath;
        
        this.state.currentDatafile = filepath || '';
        
        if (filepath) {
            // Show just the filename without path and extension
            const filename = filepath.split('/').pop().replace('.ess', '');
            this.elements.currentFile.textContent = filename;
            this.elements.currentFile.classList.add('open');
            this.elements.btnFileOpen.style.display = 'none';
            this.elements.btnFileClose.style.display = '';
            
            // Log success if file was just opened
            if (!wasOpen && isOpen) {
                this.emit('log', { message: `Datafile opened: ${filename}`, level: 'info' });
            }
        } else {
            this.elements.currentFile.textContent = 'No file';
            this.elements.currentFile.classList.remove('open');
            this.elements.btnFileOpen.style.display = '';
            this.elements.btnFileClose.style.display = 'none';
            
            // Log if file was just closed
            if (wasOpen && !isOpen) {
                this.emit('log', { message: 'Datafile closed', level: 'info' });
            }
        }
    }
    
    /**
     * Save current settings
     */
    saveSettings() {
        this.sendCommand('ess::save_settings');
        this.emit('log', { message: 'Settings saved', level: 'info' });
    }
    
    /**
     * Reset settings to defaults
     */
    resetSettings() {
        this.sendCommand('ess::reset_settings');
        this.emit('log', { message: 'Settings reset to defaults', level: 'info' });
    }
    
    /**
     * Send a command and wait for response
     * Used for commands that return values (like file_suggest, file_open)
     * Uses the connection's sendRaw for commands that expect responses
     */
    async sendCommandWithResponse(cmd, timeout = 5000) {
        if (!this.dpManager.connection.ws || !this.dpManager.connection.connected) {
            throw new Error('Not connected');
        }
        
        // Use sendRaw which has built-in request/response handling
        // Wrap as eval command to ESS subprocess
        try {
            const result = await this.dpManager.connection.sendRaw(
                `send ess {${cmd}}`
            );
            return result;
        } catch (e) {
            console.error('Command failed:', e);
            throw e;
        }
    }
    
    /**
     * Send a command to the ESS subprocess using eval
     * This matches the Vue dserv.js essCommand format
     */
    sendCommand(cmd) {
        if (this.dpManager.connection.ws && this.dpManager.connection.connected) {
            // Use the same format as Vue: { cmd: 'eval', script: command }
            const message = { cmd: 'eval', script: cmd };
            this.dpManager.connection.ws.send(JSON.stringify(message));
        }
    }
    
    /**
     * Parse list data (handles both Tcl list and array formats)
     */
    parseListData(data) {
        if (Array.isArray(data)) {
            return data;
        }
        
        if (typeof data === 'string') {
            return TclParser.parseList(data);
        }
        
        return [];
    }
    
    /**
     * Populate a select element with options
     */
    populateSelect(select, items, placeholder = '') {
        select.innerHTML = '';
        
        if (placeholder) {
            const opt = document.createElement('option');
            opt.value = '';
            opt.textContent = placeholder;
            select.appendChild(opt);
        }
        
        items.forEach(item => {
            const opt = document.createElement('option');
            opt.value = item;
            opt.textContent = item;
            select.appendChild(opt);
        });
    }
    
    /**
     * Update select to show current value (only if value exists in options)
     */
    updateSelectValue(select, value) {
        if (!value) return;
        
        // Only set the value if it exists in options (like FLTK's find_index)
        const exists = Array.from(select.options).some(opt => opt.value === value);
        
        if (exists) {
            select.value = value;
        }
        // If value doesn't exist in options, don't add it - it's stale
    }
    
    /**
     * Clear a select element back to just the placeholder
     */
    clearSelect(select, placeholder = '') {
        select.innerHTML = '';
        if (placeholder) {
            const opt = document.createElement('option');
            opt.value = '';
            opt.textContent = placeholder;
            select.appendChild(opt);
        }
    }
    
    /**
     * Hide options and params sections (used when switching system/protocol)
     */
    hideOptionsAndParams() {
        this.elements.optionsSection.style.display = 'none';
        this.elements.paramsSection.style.display = 'none';
        this.elements.optionsContainer.innerHTML = '';
        this.elements.paramsContainer.innerHTML = '';
        this.state.variantOptions = {};
        this.state.params = {};
    }
    
    /**
     * Event emitter methods
     */
    on(event, callback) {
        if (!this.listeners.has(event)) {
            this.listeners.set(event, new Set());
        }
        this.listeners.get(event).add(callback);
        return () => this.listeners.get(event)?.delete(callback);
    }
    
    emit(event, data) {
        const callbacks = this.listeners.get(event);
        if (callbacks) {
            callbacks.forEach(cb => {
                try { cb(data); }
                catch (e) { console.error(`Error in ${event} handler:`, e); }
            });
        }
    }
    
    /**
     * Get current state
     */
    getState() {
        return { ...this.state };
    }
    
    /**
     * Cleanup
     */
    dispose() {
        // Unsubscribe from all datapoints
        this.dpManager.unsubscribe('ess/subject_ids');
        this.dpManager.unsubscribe('ess/subject');
        this.dpManager.unsubscribe('ess/systems');
        this.dpManager.unsubscribe('ess/system');
        this.dpManager.unsubscribe('ess/protocols');
        this.dpManager.unsubscribe('ess/protocol');
        this.dpManager.unsubscribe('ess/variants');
        this.dpManager.unsubscribe('ess/variant');
        this.dpManager.unsubscribe('ess/status');
        this.dpManager.unsubscribe('ess/variant_info_json');
        this.dpManager.unsubscribe('ess/param_settings');
        this.dpManager.unsubscribe('ess/param');
        this.dpManager.unsubscribe('ess/params');
        this.dpManager.unsubscribe('ess/datafile');
        
        this.listeners.clear();
        this.container.innerHTML = '';
    }
}

// Export
if (typeof window !== 'undefined') {
    window.ESSControl = ESSControl;
}