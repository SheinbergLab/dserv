/**
 * ESSControl.js
 * Experiment System State Control Panel Component
 * 
 * Provides UI for:
 * - State control (Go, Stop, Reset, Juice)
 * - Observation status indicator (in_obs sync line)
 * - Tabbed interface:
 *   - Setup tab: Subject, System/Protocol/Variant selection, options, params
 *   - Configs tab: Search/browse saved configurations
 * - Configuration snapshot save/load
 * 
 * =============================================================================
 * TABLE OF CONTENTS
 * =============================================================================
 * 
 * SECTION 1: CONSTRUCTOR & STATE          (~line 35)
 *   - constructor()
 *   - state initialization
 * 
 * SECTION 2: RENDER                        (~line 60)
 *   - render() - Main HTML template
 *   - Element caching
 * 
 * SECTION 3: EVENT BINDING                 (~line 250)
 *   - bindEvents() - All UI event handlers
 *   - switchTab()
 * 
 * SECTION 4: DATAPOINT SUBSCRIPTIONS       (~line 330)
 *   - setupSubscriptions() - All dpManager subscriptions
 * 
 * SECTION 5: SETUP TAB - ESS STATE         (~line 450)
 *   - updateSubjects/Systems/Protocols/Variants()
 *   - updateEssStatus(), updateButtonStates()
 *   - updateObsDisplay(), updateInObsIndicator()
 *   - updateVariantInfo(), renderVariantOptions()
 *   - updateParams(), renderParams()
 *   - ESS Commands: setSubject, setSystem, start, stop, etc.
 *   - Datafile handling
 * 
 * SECTION 6: CONFIGS TAB                   (~line 850)
 *   - updateConfigList/Tags/CurrentConfig()
 *   - renderTagFilter(), renderConfigList()
 *   - updateConfigStatusBar()
 *   - loadConfig(), saveConfig(), openSaveDialog()
 * 
 * SECTION 7: HELPERS                       (~line 1020)
 *   - sendCommand(), sendConfigCommand()
 *   - parseListData(), parseTags()
 *   - populateSelect(), updateSelectValue()
 *   - escapeHtml(), escapeAttr()
 * 
 * SECTION 8: EVENT EMITTER & CLEANUP       (~line 1100)
 *   - on(), emit()
 *   - getState(), dispose()
 * 
 * =============================================================================
 */

class ESSControl {
    // =========================================================================
    // SECTION 1: CONSTRUCTOR & STATE
    // =========================================================================
    
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
            // ESS state
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
            currentDatafile: '',
            obsId: 0,
            obsTotal: 0,
            inObs: false,
            
            // Tab state
            activeTab: 'setup',  // 'setup' or 'configs'
            
            // Config state
            configs: [],
            archivedConfigs: [],
            configTags: [],
            currentConfig: null,  // {id, name} or null
            configSearch: '',
            configTagFilter: '',
            showingTrash: false,
            
            // Edit state
            editingConfig: null,  // full config object being edited, or null
            editForm: {}          // form field values during edit
        };
        
        // Event listeners
        this.listeners = new Map();
        
        // Build UI
        this.render();
        
        // Setup datapoint subscriptions
        this.setupSubscriptions();
    }
    
    // =========================================================================
    // SECTION 2: RENDER
    // =========================================================================
    
    /**
     * Render the control panel UI
     */
    render() {
        this.container.innerHTML = `
            <!-- Fixed Top: Identity + State Controls -->
            <div class="ess-control-fixed-top">
                <!-- Identity Header -->
                <div class="ess-identity-header">
                    <span class="ess-identity-subject" id="ess-identity-subject"></span>
                    <span class="ess-identity-path" id="ess-identity-path">(no system loaded)</span>
                </div>
                
                <!-- State Controls -->
                <div class="ess-control-section ess-state-section">
                    <div class="ess-state-controls">
                        <button id="ess-btn-go" class="ess-state-btn go" disabled>▶ Go</button>
                        <button id="ess-btn-stop" class="ess-state-btn stop" disabled>■ Stop</button>
                        <button id="ess-btn-reset" class="ess-state-btn reset" disabled>Reset</button>
                    </div>
                    <div class="ess-status-row">
                        <span class="ess-in-obs-indicator" id="ess-in-obs-indicator"></span>
                        <span class="ess-obs-label">Obs:</span>
                        <span class="ess-obs-value" id="ess-obs-display">–/–</span>
                        <div class="ess-status-spacer"></div>
                        <button id="ess-btn-juice" class="ess-mini-btn juice">Juice</button>
                        <input type="number" id="ess-juice-amount" class="ess-juice-input" value="0.5" min="0.1" max="5" step="0.1" title="Juice amount (sec)">
                    </div>
                </div>
                
                <!-- File Controls -->
                <div class="ess-file-header">
                    <span class="ess-file-label">File:</span>
                    <span class="ess-file-name" id="ess-current-file">(none)</span>
                    <div class="ess-file-spacer"></div>
                    <button id="ess-btn-file-open" class="ess-mini-btn">Open</button>
                    <button id="ess-btn-file-close" class="ess-mini-btn close" style="display: none;">Close</button>
                </div>
            </div>
            
            <!-- Scrollable Middle: Tabbed Section -->
            <div class="ess-control-scrollable">
                <div class="ess-control-section ess-tabbed-section">
                    <!-- Tab Headers -->
                    <div class="ess-tab-header">
                        <button class="ess-tab-btn active" data-tab="setup">Setup</button>
                        <button class="ess-tab-btn" data-tab="configs">Configs</button>
                        <div class="ess-tab-spacer"></div>
                        <a href="stimdg_viewer.html" target="_blank" class="ess-stimdg-btn">StimDG</a>
                    </div>
                    
                    <!-- Setup Tab Content -->
                    <div class="ess-tab-content active" id="ess-tab-setup">
                        <!-- Subject -->
                        <div class="ess-setup-row">
                            <span class="ess-control-label">Subject</span>
                            <div class="ess-control-input">
                                <select id="ess-subject-select">
                                    <option value="">-- Select --</option>
                                </select>
                            </div>
                        </div>
                        
                        <!-- System -->
                        <div class="ess-setup-row">
                            <span class="ess-control-label">System</span>
                            <div class="ess-control-input">
                                <select id="ess-system-select">
                                    <option value="">-- Select System --</option>
                            </select>
                        </div>
                        <button id="ess-reload-system" class="ess-reload-btn" title="Reload System">↻</button>
                    </div>
                    
                    <!-- Protocol -->
                    <div class="ess-setup-row">
                        <span class="ess-control-label">Protocol</span>
                        <div class="ess-control-input">
                            <select id="ess-protocol-select">
                                <option value="">-- Select Protocol --</option>
                            </select>
                        </div>
                        <button id="ess-reload-protocol" class="ess-reload-btn" title="Reload Protocol">↻</button>
                    </div>
                    
                    <!-- Variant -->
                    <div class="ess-setup-row">
                        <span class="ess-control-label">Variant</span>
                        <div class="ess-control-input">
                            <select id="ess-variant-select">
                                <option value="">-- Select Variant --</option>
                            </select>
                        </div>
                        <button id="ess-reload-variant" class="ess-reload-btn" title="Reload Variant">↻</button>
                    </div>
                    
                    <!-- Variant Options (collapsible) -->
                    <div class="ess-setup-subsection" id="ess-options-section" style="display: none;">
                        <div class="ess-subsection-header">
                            <span class="ess-subsection-title">Variant Options</span>
                            <label class="ess-auto-reload-label">
                                <input type="checkbox" id="ess-auto-reload" checked>
                                Auto
                            </label>
                            <button id="ess-reload-options-btn" class="ess-mini-btn">Reload</button>
                        </div>
                        <div id="ess-options-container" class="ess-options-container"></div>
                    </div>
                    
                    <!-- Parameters (collapsible) -->
                    <div class="ess-setup-subsection" id="ess-params-section" style="display: none;">
                        <div class="ess-subsection-header">
                            <span class="ess-subsection-title">Parameters</span>
                        </div>
                        <div id="ess-params-container" class="ess-params-container"></div>
                    </div>
                </div>
                
                <!-- Configs Tab Content -->
                <div class="ess-tab-content" id="ess-tab-configs">
                    <!-- List View (default) -->
                    <div class="ess-config-list-view" id="ess-config-list-view">
                        <!-- Search Row -->
                        <div class="ess-config-search-row">
                            <button id="ess-config-trash-back" class="ess-config-trash-back" style="display: none;">← Back</button>
                            <input type="text" id="ess-config-search" class="ess-config-search" 
                                   placeholder="Search configs...">
                            <button id="ess-config-import-btn" class="ess-config-import-btn" title="Import from peer">
                                ↓ Import
                            </button>
                            <button id="ess-config-trash-toggle" class="ess-config-trash-btn" title="View trash">
                                🗑 <span id="ess-config-trash-count"></span>
                            </button>
                        </div>
                        
                        <!-- Tag Filter Row -->
                        <div class="ess-config-tags-row" id="ess-config-tags-row">
                            <span class="ess-config-tags-label">Tags:</span>
                            <div class="ess-config-tags-list" id="ess-config-tags-list">
                                <!-- Tag chips rendered here -->
                            </div>
                        </div>
                        
                        <!-- Config List -->
                        <div class="ess-config-list" id="ess-config-list">
                            <div class="ess-config-empty">No saved configurations</div>
                        </div>
                    </div>
                    
                    <!-- Edit View (shown when editing) -->
                    <div class="ess-config-edit-view" id="ess-config-edit-view" style="display: none;">
                        <div class="ess-config-edit-header">
                            <button class="ess-config-edit-back" id="ess-config-edit-back">← Back</button>
                            <span class="ess-config-edit-title">Editing: <span id="ess-config-edit-name">—</span></span>
                        </div>
                        
                        <div class="ess-config-edit-form" id="ess-config-edit-form">
                            <!-- Name -->
                            <div class="ess-edit-row">
                                <label class="ess-edit-label">Name</label>
                                <input type="text" id="ess-edit-name" class="ess-edit-input">
                            </div>
                            
                            <!-- Description -->
                            <div class="ess-edit-row">
                                <label class="ess-edit-label">Description</label>
                                <textarea id="ess-edit-description" class="ess-edit-textarea" rows="2"></textarea>
                            </div>
                            
                            <!-- Subject -->
                            <div class="ess-edit-row">
                                <label class="ess-edit-label">Subject</label>
                                <select id="ess-edit-subject" class="ess-edit-select">
                                    <option value="">-- None --</option>
                                </select>
                            </div>
                            
                            <!-- Tags -->
                            <div class="ess-edit-row">
                                <label class="ess-edit-label">Tags</label>
                                <div class="ess-edit-tags-container">
                                    <div class="ess-edit-tags" id="ess-edit-tags"></div>
                                    <input type="text" id="ess-edit-tag-input" class="ess-edit-tag-input" 
                                           placeholder="Add tag...">
                                </div>
                            </div>
                            
                            <!-- Variant Args (dynamic) -->
                            <div class="ess-edit-section" id="ess-edit-variant-args-section" style="display: none;">
                                <div class="ess-edit-section-title">Variant Arguments</div>
                                <div id="ess-edit-variant-args"></div>
                            </div>
                            
                            <!-- Params (dynamic) -->
                            <div class="ess-edit-section" id="ess-edit-params-section" style="display: none;">
                                <div class="ess-edit-section-title">Parameters</div>
                                <div id="ess-edit-params"></div>
                            </div>
                            
                            <!-- Read-only info -->
                            <div class="ess-edit-readonly">
                                <div class="ess-edit-readonly-row">
                                    <span class="ess-edit-readonly-label">Path:</span>
                                    <span class="ess-edit-readonly-value" id="ess-edit-path">—</span>
                                </div>
                                <div class="ess-edit-readonly-row">
                                    <span class="ess-edit-readonly-label">Created:</span>
                                    <span class="ess-edit-readonly-value" id="ess-edit-created">—</span>
                                </div>
                            </div>
                        </div>
                    </div>
                </div>
            </div>
            </div>
            
            <!-- Fixed Bottom: Config Status Bar -->
            <div class="ess-control-fixed-bottom">
                <div class="ess-control-section ess-config-status">
                    <span class="ess-config-status-label">Config:</span>
                    <span class="ess-config-status-name" id="ess-config-status-name">(unsaved)</span>
                    <div class="ess-config-status-spacer"></div>
                    <button id="ess-btn-config-new" class="ess-mini-btn save">New</button>
                    <button id="ess-btn-config-save-edit" class="ess-mini-btn save" style="display: none;">Save</button>
                    <button id="ess-btn-config-cancel-edit" class="ess-mini-btn" style="display: none;">Cancel</button>
                </div>
            </div>
        `;
        
        // Cache element references
        this.elements = {
            // Identity header
            identitySubject: this.container.querySelector('#ess-identity-subject'),
            identityPath: this.container.querySelector('#ess-identity-path'),
            
            // Tab elements
            tabBtns: this.container.querySelectorAll('.ess-tab-btn'),
            tabSetup: this.container.querySelector('#ess-tab-setup'),
            tabConfigs: this.container.querySelector('#ess-tab-configs'),
            
            // State controls
            btnReset: this.container.querySelector('#ess-btn-reset'),
            btnGo: this.container.querySelector('#ess-btn-go'),
            btnStop: this.container.querySelector('#ess-btn-stop'),
            btnJuice: this.container.querySelector('#ess-btn-juice'),
            juiceAmount: this.container.querySelector('#ess-juice-amount'),
            inObsIndicator: this.container.querySelector('#ess-in-obs-indicator'),
            obsDisplay: this.container.querySelector('#ess-obs-display'),
            
            // Setup tab elements
            subjectSelect: this.container.querySelector('#ess-subject-select'),
            systemSelect: this.container.querySelector('#ess-system-select'),
            protocolSelect: this.container.querySelector('#ess-protocol-select'),
            variantSelect: this.container.querySelector('#ess-variant-select'),
            reloadSystem: this.container.querySelector('#ess-reload-system'),
            reloadProtocol: this.container.querySelector('#ess-reload-protocol'),
            reloadVariant: this.container.querySelector('#ess-reload-variant'),
            optionsSection: this.container.querySelector('#ess-options-section'),
            optionsContainer: this.container.querySelector('#ess-options-container'),
            autoReloadCheckbox: this.container.querySelector('#ess-auto-reload'),
            reloadOptionsBtn: this.container.querySelector('#ess-reload-options-btn'),
            paramsSection: this.container.querySelector('#ess-params-section'),
            paramsContainer: this.container.querySelector('#ess-params-container'),
            btnFileOpen: this.container.querySelector('#ess-btn-file-open'),
            btnFileClose: this.container.querySelector('#ess-btn-file-close'),
            currentFile: this.container.querySelector('#ess-current-file'),
            
            // Configs tab elements
            configSearch: this.container.querySelector('#ess-config-search'),
            configTagsRow: this.container.querySelector('#ess-config-tags-row'),
            configTagsList: this.container.querySelector('#ess-config-tags-list'),
            configImportBtn: this.container.querySelector('#ess-config-import-btn'),
            configTrashToggle: this.container.querySelector('#ess-config-trash-toggle'),
            configTrashBack: this.container.querySelector('#ess-config-trash-back'),
            configTrashCount: this.container.querySelector('#ess-config-trash-count'),
            configList: this.container.querySelector('#ess-config-list'),
            
            // Config status bar
            configStatusName: this.container.querySelector('#ess-config-status-name'),
            btnConfigNew: this.container.querySelector('#ess-btn-config-new'),
            btnConfigSaveEdit: this.container.querySelector('#ess-btn-config-save-edit'),
            btnConfigCancelEdit: this.container.querySelector('#ess-btn-config-cancel-edit'),
            
            // Config list/edit views
            configListView: this.container.querySelector('#ess-config-list-view'),
            configEditView: this.container.querySelector('#ess-config-edit-view'),
            
            // Edit form elements
            editBack: this.container.querySelector('#ess-config-edit-back'),
            editName: this.container.querySelector('#ess-edit-name'),
            editNameDisplay: this.container.querySelector('#ess-config-edit-name'),
            editDescription: this.container.querySelector('#ess-edit-description'),
            editSubject: this.container.querySelector('#ess-edit-subject'),
            editTags: this.container.querySelector('#ess-edit-tags'),
            editTagInput: this.container.querySelector('#ess-edit-tag-input'),
            editVariantArgsSection: this.container.querySelector('#ess-edit-variant-args-section'),
            editVariantArgs: this.container.querySelector('#ess-edit-variant-args'),
            editParamsSection: this.container.querySelector('#ess-edit-params-section'),
            editParams: this.container.querySelector('#ess-edit-params'),
            editPath: this.container.querySelector('#ess-edit-path'),
            editCreated: this.container.querySelector('#ess-edit-created')
        };
        
        // Bind event handlers
        this.bindEvents();
    }
    
    // =========================================================================
    // SECTION 3: EVENT BINDING
    // =========================================================================
    
    /**
     * Bind UI event handlers
     */
    bindEvents() {
        // Tab switching
        this.elements.tabBtns.forEach(btn => {
            btn.addEventListener('click', () => this.switchTab(btn.dataset.tab));
        });
        
        // Setup tab - Selection changes
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
        this.elements.reloadSystem.addEventListener('click', () => this.reloadSystem());
        this.elements.reloadProtocol.addEventListener('click', () => this.reloadProtocol());
        this.elements.reloadVariant.addEventListener('click', () => this.reloadVariant());
        
        // Auto-reload checkbox
        this.elements.autoReloadCheckbox.addEventListener('change', (e) => {
            this.options.autoReload = e.target.checked;
        });
        
        // Manual reload button for variant options
        this.elements.reloadOptionsBtn.addEventListener('click', () => this.reloadVariant());
        
        // State control buttons
        this.elements.btnReset.addEventListener('click', () => this.resetExperiment());
        this.elements.btnGo.addEventListener('click', () => this.startExperiment());
        this.elements.btnStop.addEventListener('click', () => this.stopExperiment());
        
        // Juice button
        this.elements.btnJuice.addEventListener('click', () => this.giveJuice());
        
        // File buttons
        this.elements.btnFileOpen.addEventListener('click', () => this.openDatafile());
        this.elements.btnFileClose.addEventListener('click', () => this.closeDatafile());
        
        // Configs tab - Search
        this.elements.configSearch.addEventListener('input', (e) => {
            this.state.configSearch = e.target.value;
            this.renderConfigList();
        });
        
        // Trash toggle
        this.elements.configTrashToggle.addEventListener('click', () => {
            this.toggleTrashView();
        });
        
        // Trash back button
        this.elements.configTrashBack.addEventListener('click', () => {
            this.toggleTrashView();  // Toggle back to normal view
        });
        
        // Import button
        this.elements.configImportBtn.addEventListener('click', () => {
            this.showImportDialog();
        });
        
        // Config New button - opens edit view in create mode
        this.elements.btnConfigNew.addEventListener('click', () => this.startCreateConfig());
        
        // Config Save/Cancel buttons in status bar (for edit mode)
        this.elements.btnConfigSaveEdit.addEventListener('click', () => this.saveEdit());
        this.elements.btnConfigCancelEdit.addEventListener('click', () => this.cancelEdit());
        
        // Edit form - back button
        this.elements.editBack.addEventListener('click', () => this.cancelEdit());
        
        // Tag input - add tag on Enter
        this.elements.editTagInput.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') {
                e.preventDefault();
                this.addEditTag(e.target.value.trim());
                e.target.value = '';
            }
        });
        
        // Close config menus when clicking outside
        document.addEventListener('click', (e) => {
            if (!e.target.closest('.ess-config-item-actions')) {
                this.closeAllConfigMenus();
            }
        });
    }
    
    /**
     * Switch between tabs
     */
    switchTab(tabName) {
        this.state.activeTab = tabName;
        
        // Update tab buttons
        this.elements.tabBtns.forEach(btn => {
            btn.classList.toggle('active', btn.dataset.tab === tabName);
        });
        
        // Update tab content
        this.elements.tabSetup.classList.toggle('active', tabName === 'setup');
        this.elements.tabConfigs.classList.toggle('active', tabName === 'configs');
        
        // If switching to configs, refresh the list
        if (tabName === 'configs') {
            this.refreshConfigList();
        }
    }
    
    // =========================================================================
    // SECTION 4: DATAPOINT SUBSCRIPTIONS
    // =========================================================================
    
    /**
     * Setup datapoint subscriptions
     */
    setupSubscriptions() {
        // ESS subscriptions (existing)
        this.dpManager.subscribe('ess/subject_ids', (data) => {
            this.updateSubjects(data.value);
        });
        
        this.dpManager.subscribe('ess/subject', (data) => {
            this.state.currentSubject = data.value;
            this.updateSelectValue(this.elements.subjectSelect, data.value);
            this.updateIdentityHeader();
        });
        
        this.dpManager.subscribe('ess/systems', (data) => {
            this.updateSystems(data.value);
        });
        
        this.dpManager.subscribe('ess/system', (data) => {
            this.state.currentSystem = data.value;
            this.updateSelectValue(this.elements.systemSelect, data.value);
            this.updateIdentityHeader();
            this.updateConfigStatusBar();
        });
        
        this.dpManager.subscribe('ess/protocols', (data) => {
            this.updateProtocols(data.value);
        });
        
        this.dpManager.subscribe('ess/protocol', (data) => {
            this.state.currentProtocol = data.value;
            this.updateSelectValue(this.elements.protocolSelect, data.value);
            this.updateIdentityHeader();
            this.updateConfigStatusBar();
        });
        
        this.dpManager.subscribe('ess/variants', (data) => {
            this.updateVariants(data.value);
        });
        
        this.dpManager.subscribe('ess/variant', (data) => {
            this.state.currentVariant = data.value;
            this.updateSelectValue(this.elements.variantSelect, data.value);
            this.updateIdentityHeader();
            this.updateConfigStatusBar();
        });
        
        this.dpManager.subscribe('ess/status', (data) => {
            this.updateEssStatus(data.value);
        });
        
        this.dpManager.subscribe('ess/variant_info_json', (data) => {
            this.updateVariantInfo(data.value);
        });
        
        this.dpManager.subscribe('ess/param_settings', (data) => {
            this.updateParams(data.value);
        });
        
        this.dpManager.subscribe('ess/param', (data) => {
            this.updateParamValue(data.value);
        });
        
        this.dpManager.subscribe('ess/params', (data) => {
            this.updateParamValue(data.value);
        });
        
        this.dpManager.subscribe('ess/datafile', (data) => {
            this.updateDatafileStatus(data.value);
        });
        
        // Observation tracking
        this.dpManager.subscribe('ess/obs_id', (data) => {
            this.state.obsId = parseInt(data.value) || 0;
            this.updateObsDisplay();
        });
        
        this.dpManager.subscribe('ess/obs_total', (data) => {
            this.state.obsTotal = parseInt(data.value) || 0;
            this.updateObsDisplay();
        });
        
        this.dpManager.subscribe('ess/in_obs', (data) => {
            this.state.inObs = data.value === '1' || data.value === 1 || data.value === true;
            this.updateInObsIndicator();
        });
        
        // Config subscriptions
        this.dpManager.subscribe('configs/list', (data) => {
            this.updateConfigList(data.value);
        });
        
        this.dpManager.subscribe('configs/archived', (data) => {
            this.updateArchivedList(data.value);
        });
        
        this.dpManager.subscribe('configs/tags', (data) => {
            this.updateConfigTags(data.value);
        });
        
        this.dpManager.subscribe('configs/current', (data) => {
            this.updateCurrentConfig(data.value);
        });
        
        // Mesh peers subscription (published by dserv-agent)
        this.dpManager.subscribe('mesh/peers', (data) => {
            this.updateMeshPeers(data.value);
        });
    }
    
    // =========================================================================
    // SECTION 5: SETUP TAB - ESS State Management
    // =========================================================================
    
    updateSubjects(data) {
        const subjects = this.parseListData(data);
        this.state.subjects = subjects;
        this.populateSelect(this.elements.subjectSelect, subjects, '-- Select Subject --');
        if (this.state.currentSubject) {
            this.updateSelectValue(this.elements.subjectSelect, this.state.currentSubject);
        }
    }
    
    updateSystems(data) {
        const systems = this.parseListData(data);
        this.state.systems = systems;
        this.populateSelect(this.elements.systemSelect, systems, '-- Select System --');
        if (this.state.currentSystem) {
            this.updateSelectValue(this.elements.systemSelect, this.state.currentSystem);
        }
    }
    
    updateProtocols(data) {
        const protocols = this.parseListData(data);
        this.state.protocols = protocols;
        this.populateSelect(this.elements.protocolSelect, protocols, '-- Select Protocol --');
        if (this.state.currentProtocol) {
            this.updateSelectValue(this.elements.protocolSelect, this.state.currentProtocol);
        }
    }
    
    updateVariants(data) {
        const variants = this.parseListData(data);
        this.state.variants = variants;
        this.populateSelect(this.elements.variantSelect, variants, '-- Select Variant --');
        if (this.state.currentVariant) {
            this.updateSelectValue(this.elements.variantSelect, this.state.currentVariant);
        }
    }
    
    updateEssStatus(status) {
        this.state.essStatus = status;
        const canChangeConfig = (status === 'stopped');
        this.setControlsEnabled(canChangeConfig);
        
        if (status === 'loading') {
            this.container.classList.add('ess-loading');
        } else {
            this.container.classList.remove('ess-loading');
        }
        
        this.emit('stateChange', { state: status });
        this.updateButtonStates();
    }
    
    updateButtonStates() {
        const status = this.state.essStatus;
        
        this.elements.btnReset.disabled = true;
        this.elements.btnGo.disabled = true;
        this.elements.btnStop.disabled = true;
        
        switch (status) {
            case 'stopped':
                this.elements.btnGo.disabled = false;
                this.elements.btnReset.disabled = false;
                break;
            case 'running':
                this.elements.btnStop.disabled = false;
                break;
            case 'loading':
                break;
            default:
                if (this.state.currentVariant) {
                    this.elements.btnGo.disabled = false;
                }
                break;
        }
    }
    
    updateObsDisplay() {
        const id = this.state.obsId + 1;  // Display 1-indexed
        const total = this.state.obsTotal;
        this.elements.obsDisplay.textContent = total > 0 ? `${id}/${total}` : '–/–';
    }
    
    updateInObsIndicator() {
        this.elements.inObsIndicator.classList.toggle('active', this.state.inObs);
    }
    
    setControlsEnabled(enabled) {
        this.elements.subjectSelect.disabled = !enabled;
        this.elements.systemSelect.disabled = !enabled;
        this.elements.protocolSelect.disabled = !enabled;
        this.elements.variantSelect.disabled = !enabled;
        this.elements.reloadSystem.disabled = !enabled;
        this.elements.reloadProtocol.disabled = !enabled;
        this.elements.reloadVariant.disabled = !enabled;
        this.elements.reloadOptionsBtn.disabled = !enabled;
        
        const optionSelects = this.elements.optionsContainer.querySelectorAll('select');
        optionSelects.forEach(select => select.disabled = !enabled);
    }
    
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
    
    renderVariantOptions(info) {
        const container = this.elements.optionsContainer;
        container.innerHTML = '';
        
        const { options, loader_arg_names, loader_args } = info;
        
        loader_arg_names.forEach((argName, index) => {
            const argOptions = options[argName];
            if (!argOptions || !Array.isArray(argOptions)) return;
            
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
            
            let selectedIndex = 0;
            let foundSelected = false;
            
            argOptions.forEach((opt, optIndex) => {
                const option = document.createElement('option');
                option.value = opt.value;
                option.textContent = opt.label;
                select.appendChild(option);
                
                if (opt.selected === true) {
                    selectedIndex = optIndex;
                    foundSelected = true;
                } else if (!foundSelected && currentValue !== undefined) {
                    const optValue = String(opt.value).trim();
                    const curValue = String(currentValue).trim();
                    if (optValue === curValue) {
                        selectedIndex = optIndex;
                    }
                }
            });
            
            select.selectedIndex = selectedIndex;
            select.addEventListener('change', (e) => {
                this.onVariantOptionChange(argName, e.target.value);
            });
            
            row.appendChild(label);
            row.appendChild(select);
            container.appendChild(row);
        });
    }
    
    onVariantOptionChange(argName, value) {
        const cmd = `ess::set_variant_args {${argName} {${value}}}`;
        this.sendCommand(cmd);
        
        if (this.options.autoReload) {
            this.sendCommand('ess::reload_variant');
        }
    }
    
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
            
            if (info.varType === '1') {
                label.classList.add('time');
            } else {
                label.classList.add('variable');
            }
            
            row.appendChild(label);
            
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
                
                if (info.dataType === 'int') {
                    input.inputMode = 'numeric';
                    input.addEventListener('input', (e) => {
                        e.target.value = e.target.value.replace(/[^0-9-]/g, '');
                        if (e.target.value.indexOf('-') > 0) {
                            e.target.value = e.target.value.replace(/-/g, '');
                        }
                    });
                } else if (info.dataType === 'float') {
                    input.inputMode = 'decimal';
                    input.addEventListener('input', (e) => {
                        let val = e.target.value.replace(/[^0-9.-]/g, '');
                        const parts = val.split('.');
                        if (parts.length > 2) {
                            val = parts[0] + '.' + parts.slice(1).join('');
                        }
                        if (val.indexOf('-') > 0) {
                            val = val.charAt(0) + val.slice(1).replace(/-/g, '');
                        }
                        e.target.value = val;
                    });
                }
                
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
    
    onParamChange(name, value) {
        const cmd = `ess::set_param ${name} ${value}`;
        this.sendCommand(cmd);
        this.emit('paramChange', { name, value });
    }
    
    updateParamValue(data) {
        const updates = TclParser.parseKeyValue(data);
        
        for (const [name, value] of Object.entries(updates)) {
            if (this.state.params[name]) {
                this.state.params[name].value = value;
            }
            
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
    
    // ESS Commands
    setSubject(subject) {
        if (!subject) return;
        this.sendCommand(`ess::set_subject ${subject}`);
    }
    
    setSystem(system) {
        if (!system) return;
        this.sendCommand(`evalNoReply {ess::load_system ${system}}`);
    }
    
    setProtocol(protocol) {
        if (!protocol || !this.state.currentSystem) return;
        this.sendCommand(`evalNoReply {ess::load_system ${this.state.currentSystem} ${protocol}}`);
    }
    
    setVariant(variant) {
        if (!variant || !this.state.currentSystem || !this.state.currentProtocol) return;
        this.sendCommand(`evalNoReply {ess::load_system ${this.state.currentSystem} ${this.state.currentProtocol} ${variant}}`);
    }
    
    startExperiment() {
        this.sendCommand('ess::start');
    }
    
    stopExperiment() {
        this.sendCommand('ess::stop');
    }
    
    resetExperiment() {
        this.sendCommand('ess::reset');
    }
    
    reloadSystem() {
        this.sendCommand('ess::reload_system');
    }
    
    reloadProtocol() {
        this.sendCommand('ess::reload_protocol');
    }
    
    reloadVariant() {
        this.sendCommand('ess::reload_variant');
    }
    
    giveJuice() {
        const amount = this.elements.juiceAmount.value || 50;
        this.sendCommand(`send juicer reward ${amount}`);
    }
    
    openDatafile() {
        this.sendCommand('ess::file_open [ess::file_suggest]');
        this.emit('log', { message: 'Opening datafile...', level: 'info' });
    }
    
    closeDatafile() {
        if (!this.state.currentDatafile) {
            this.emit('log', { message: 'No datafile is currently open', level: 'warning' });
            return;
        }
        this.sendCommand('ess::file_close');
        this.emit('log', { message: 'Closing datafile...', level: 'info' });
    }
    
    updateDatafileStatus(filepath) {
        const wasOpen = !!this.state.currentDatafile;
        const isOpen = !!filepath;
        
        this.state.currentDatafile = filepath || '';
        
        if (filepath) {
            const filename = filepath.split('/').pop().replace('.ess', '');
            this.elements.currentFile.textContent = filename;
            this.elements.currentFile.classList.add('open');
            this.elements.btnFileOpen.style.display = 'none';
            this.elements.btnFileClose.style.display = '';
            
            if (!wasOpen && isOpen) {
                this.emit('log', { message: `Datafile opened: ${filename}`, level: 'info' });
            }
        } else {
            this.elements.currentFile.textContent = 'No file';
            this.elements.currentFile.classList.remove('open');
            this.elements.btnFileOpen.style.display = '';
            this.elements.btnFileClose.style.display = 'none';
            
            if (wasOpen && !isOpen) {
                this.emit('log', { message: 'Datafile closed', level: 'info' });
            }
        }
    }
    
    // =========================================================================
    // SECTION 6: CONFIGS TAB - Configuration Management
    // =========================================================================
    
    updateConfigList(data) {
        try {
            const configs = typeof data === 'string' ? JSON.parse(data) : data;
            this.state.configs = Array.isArray(configs) ? configs : [];
            if (!this.state.showingTrash) {
                this.renderConfigList();
            }
        } catch (e) {
            console.error('Failed to parse config list:', e);
            this.state.configs = [];
            if (!this.state.showingTrash) {
                this.renderConfigList();
            }
        }
    }
    
    updateArchivedList(data) {
        try {
            const configs = typeof data === 'string' ? JSON.parse(data) : data;
            this.state.archivedConfigs = Array.isArray(configs) ? configs : [];
            this.updateTrashCount();
            if (this.state.showingTrash) {
                this.renderConfigList();
            }
        } catch (e) {
            console.error('Failed to parse archived config list:', e);
            this.state.archivedConfigs = [];
            this.updateTrashCount();
        }
    }
    
    updateTrashCount() {
        const count = this.state.archivedConfigs.length;
        this.elements.configTrashCount.textContent = count > 0 ? count : '';
        this.elements.configTrashToggle.classList.toggle('has-items', count > 0);
    }
    
    toggleTrashView() {
        this.state.showingTrash = !this.state.showingTrash;
        this.elements.configTrashToggle.classList.toggle('active', this.state.showingTrash);
        this.elements.configList.classList.toggle('trash-view', this.state.showingTrash);
        
        // Show/hide back button and trash toggle
        this.elements.configTrashBack.style.display = this.state.showingTrash ? '' : 'none';
        this.elements.configTrashToggle.style.display = this.state.showingTrash ? 'none' : '';
        
        // Update search placeholder
        this.elements.configSearch.placeholder = this.state.showingTrash 
            ? 'Search trash...' 
            : 'Search configs...';
        
        this.renderConfigList();
    }
    
    updateConfigTags(data) {
        try {
            const tags = typeof data === 'string' ? JSON.parse(data) : data;
            this.state.configTags = Array.isArray(tags) ? tags : [];
            this.renderTagFilter();
        } catch (e) {
            console.error('Failed to parse config tags:', e);
            this.state.configTags = [];
        }
    }
    
    updateCurrentConfig(data) {
        try {
            const current = typeof data === 'string' && data ? JSON.parse(data) : data;
            this.state.currentConfig = current;
            this.updateConfigStatusBar();
            this.renderConfigList(); // Re-render to highlight active
        } catch (e) {
            this.state.currentConfig = null;
            this.updateConfigStatusBar();
        }
    }
    
    refreshConfigList() {
        // Request fresh data from configs subprocess
        this.sendConfigCommand('config_publish_all');
    }
    
    renderTagFilter() {
        const tags = this.state.configTags;
        const currentFilter = this.state.configTagFilter;
        
        // Hide row if no tags
        if (tags.length === 0) {
            this.elements.configTagsRow.style.display = 'none';
            return;
        }
        
        this.elements.configTagsRow.style.display = 'flex';
        
        // Render "All" chip plus tag chips
        let html = `<span class="ess-filter-tag ${!currentFilter ? 'active' : ''}" data-tag="">All</span>`;
        html += tags.map(tag => 
            `<span class="ess-filter-tag ${currentFilter === tag ? 'active' : ''}" data-tag="${this.escapeAttr(tag)}">${this.escapeHtml(tag)}</span>`
        ).join('');
        
        this.elements.configTagsList.innerHTML = html;
        
        // Bind click handlers
        this.elements.configTagsList.querySelectorAll('.ess-filter-tag').forEach(chip => {
            chip.addEventListener('click', () => {
                this.state.configTagFilter = chip.dataset.tag;
                this.renderTagFilter();
                this.renderConfigList();
            });
        });
    }
    
    renderConfigList() {
        // Choose source based on view mode
        let configs = this.state.showingTrash 
            ? this.state.archivedConfigs 
            : this.state.configs;
        
        // Helper to safely get lowercase string
        const toLower = (val) => {
            if (val == null) return '';
            if (typeof val === 'string') return val.toLowerCase();
            return String(val).toLowerCase();
        };
        
        // Apply search filter
        if (this.state.configSearch) {
            const q = this.state.configSearch.toLowerCase();
            configs = configs.filter(cfg => {
                return toLower(cfg.name).includes(q) || 
                       toLower(cfg.description).includes(q) || 
                       toLower(cfg.system).includes(q) || 
                       toLower(cfg.variant).includes(q) || 
                       toLower(cfg.subject).includes(q);
            });
        }
        
        // Apply tag filter (only for normal view)
        if (!this.state.showingTrash && this.state.configTagFilter) {
            configs = configs.filter(cfg => {
                const tags = this.parseTags(cfg.tags);
                return tags.includes(this.state.configTagFilter);
            });
        }
        
        const list = this.elements.configList;
        
        if (configs.length === 0) {
            const emptyMsg = this.state.showingTrash 
                ? 'Trash is empty' 
                : 'No matching configurations';
            list.innerHTML = `<div class="ess-config-empty">${emptyMsg}</div>`;
            return;
        }
        
        if (this.state.showingTrash) {
            // Render trash view
            list.innerHTML = configs.map(cfg => {
                const tags = this.parseTags(cfg.tags);
                
                return `
                    <div class="ess-config-item archived" data-name="${this.escapeAttr(cfg.name)}">
                        <div class="ess-config-item-main">
                            <span class="ess-config-item-name">${this.escapeHtml(cfg.name)}</span>
                            <button class="ess-config-item-restore">Restore</button>
                            <button class="ess-config-item-delete-permanent">Delete</button>
                        </div>
                        <div class="ess-config-item-meta">
                            ${cfg.subject ? `<span class="ess-config-item-subject">${this.escapeHtml(cfg.subject)}</span>` : ''}
                            <span class="ess-config-item-path">${this.escapeHtml(cfg.system || '')}/${this.escapeHtml(cfg.protocol || '')}/${this.escapeHtml(cfg.variant || '')}</span>
                        </div>
                        ${tags.length > 0 ? `
                            <div class="ess-config-item-tags">
                                ${tags.map(t => `<span class="ess-config-tag">${this.escapeHtml(t)}</span>`).join('')}
                            </div>
                        ` : ''}
                    </div>
                `;
            }).join('');
            
            // Bind trash item handlers
            list.querySelectorAll('.ess-config-item').forEach(item => {
                const name = item.dataset.name;
                
                item.querySelector('.ess-config-item-restore')?.addEventListener('click', (e) => {
                    e.stopPropagation();
                    this.restoreConfig(name);
                });
                
                item.querySelector('.ess-config-item-delete-permanent')?.addEventListener('click', (e) => {
                    e.stopPropagation();
                    this.permanentlyDeleteConfig(name);
                });
            });
        } else {
            // Render normal view
            list.innerHTML = configs.map(cfg => {
                const isActive = this.state.currentConfig?.name === cfg.name;
                const tags = this.parseTags(cfg.tags);
                
                return `
                    <div class="ess-config-item ${isActive ? 'active' : ''}" data-name="${this.escapeAttr(cfg.name)}">
                        <div class="ess-config-item-main">
                            <span class="ess-config-item-name">${this.escapeHtml(cfg.name)}</span>
                            <button class="ess-config-item-load">Load</button>
                            <div class="ess-config-item-actions">
                                <button class="ess-config-item-menu-btn" title="More actions">⋮</button>
                                <div class="ess-config-item-menu">
                                    <button class="ess-config-menu-action" data-action="view">View</button>
                                    <button class="ess-config-menu-action" data-action="edit">Edit</button>
                                    <button class="ess-config-menu-action" data-action="clone">Clone</button>
                                    <button class="ess-config-menu-action" data-action="export">Export to...</button>
                                    <button class="ess-config-menu-action delete" data-action="delete">Delete</button>
                                </div>
                            </div>
                        </div>
                        <div class="ess-config-item-meta">
                            ${cfg.subject ? `<span class="ess-config-item-subject">${this.escapeHtml(cfg.subject)}</span>` : ''}
                            <span class="ess-config-item-path">${this.escapeHtml(cfg.system || '')}/${this.escapeHtml(cfg.protocol || '')}/${this.escapeHtml(cfg.variant || '')}</span>
                        </div>
                        ${tags.length > 0 ? `
                            <div class="ess-config-item-tags">
                                ${tags.map(t => `<span class="ess-config-tag">${this.escapeHtml(t)}</span>`).join('')}
                            </div>
                        ` : ''}
                    </div>
                `;
            }).join('');
            
            // Bind normal item handlers
            list.querySelectorAll('.ess-config-item').forEach(item => {
                const name = item.dataset.name;
                
                item.querySelector('.ess-config-item-load').addEventListener('click', (e) => {
                    e.stopPropagation();
                    this.loadConfig(name);
                });
                
                // Menu button toggle
                const menuBtn = item.querySelector('.ess-config-item-menu-btn');
                const menu = item.querySelector('.ess-config-item-menu');
                
                menuBtn.addEventListener('click', (e) => {
                    e.stopPropagation();
                    const wasOpen = menu.classList.contains('open');
                    
                    // Close all menus first
                    this.closeAllConfigMenus();
                    
                    // If it wasn't open, open it and position it
                    if (!wasOpen) {
                        menu.classList.add('open');
                        this.positionConfigMenu(menuBtn, menu);
                    }
                });
                
                // Menu action handlers
                menu.querySelectorAll('.ess-config-menu-action').forEach(btn => {
                    btn.addEventListener('click', (e) => {
                        e.stopPropagation();
                        const action = btn.dataset.action;
                        menu.classList.remove('open');
                        
                        switch (action) {
                            case 'view':
                                this.viewConfig(name);
                                break;
                            case 'clone':
                                this.cloneConfig(name);
                                break;
                            case 'edit':
                                this.editConfig(name);
                                break;
                            case 'export':
                                this.showExportDialog(name);
                                break;
                            case 'delete':
                                this.archiveConfig(name);
                                break;
                        }
                    });
                });
                
                // Click on row loads (but not if clicking menu area)
                item.addEventListener('click', (e) => {
                    if (!e.target.closest('.ess-config-item-actions')) {
                        this.loadConfig(name);
                    }
                });
            });
        }
    }
    
    closeAllConfigMenus() {
        this.elements.configList.querySelectorAll('.ess-config-item-menu.open').forEach(menu => {
            menu.classList.remove('open');
        });
    }
    
    positionConfigMenu(button, menu) {
        // Position the menu using fixed positioning relative to viewport
        const btnRect = button.getBoundingClientRect();
        const menuHeight = menu.offsetHeight || 100; // estimate if not yet rendered
        const viewportHeight = window.innerHeight;
        
        // Position below the button, aligned to right edge
        let top = btnRect.bottom + 4;
        let left = btnRect.right - (menu.offsetWidth || 90);
        
        // If menu would go below viewport, position above the button
        if (top + menuHeight > viewportHeight - 10) {
            top = btnRect.top - menuHeight - 4;
        }
        
        menu.style.top = `${top}px`;
        menu.style.left = `${left}px`;
    }
    
    updateIdentityHeader() {
        const subject = this.state.currentSubject || '';
        const system = this.state.currentSystem || '';
        const protocol = this.state.currentProtocol || '';
        const variant = this.state.currentVariant || '';
        
        // Update subject display
        if (subject) {
            this.elements.identitySubject.textContent = subject;
            this.elements.identitySubject.style.display = '';
        } else {
            this.elements.identitySubject.textContent = '';
            this.elements.identitySubject.style.display = 'none';
        }
        
        // Update path display
        if (system) {
            const path = [system, protocol, variant].filter(Boolean).join('/');
            this.elements.identityPath.textContent = path;
            this.elements.identityPath.classList.remove('empty');
        } else {
            this.elements.identityPath.textContent = '(no system loaded)';
            this.elements.identityPath.classList.add('empty');
        }
    }
    
    updateConfigStatusBar() {
        const current = this.state.currentConfig;
        
        // Config name display
        if (current && current.name) {
            this.elements.configStatusName.textContent = current.name;
            this.elements.configStatusName.classList.add('active');
        } else {
            this.elements.configStatusName.textContent = '(unsaved)';
            this.elements.configStatusName.classList.remove('active');
        }
    }
    
    async loadConfig(name) {
        try {
            this.emit('log', { message: `Loading config: ${name}...`, level: 'info' });
            
            // Set loading state
            this.updateEssStatus('loading');
            
            await this.sendConfigCommandAsync(`config_load {${name}}`);
            this.emit('log', { message: `Loaded config: ${name}`, level: 'info' });
        } catch (e) {
            this.emit('log', { message: `Failed to load config: ${e.message}`, level: 'error' });
            // Restore stopped state on error
            this.updateEssStatus('stopped');
        }
    }
    
    /**
     * Start creating a new config - opens edit view with current ESS state
     */
    async startCreateConfig() {
        // Check if system is loaded
        if (!this.state.currentSystem) {
            this.emit('log', { message: 'Load a system first before saving a config', level: 'warning' });
            return;
        }
        
        // Get current params from ESS
        const currentParams = { ...this.state.params };
        // Convert param objects to just values
        const paramsDict = {};
        for (const [name, info] of Object.entries(currentParams)) {
            paramsDict[name] = info.value || '';
        }
        
        // Get variant_args from variant_info
        let variantArgs = {};
        if (this.state.variantInfo) {
            const info = this.state.variantInfo;
            if (info.loader_arg_names && info.loader_args) {
                const names = Array.isArray(info.loader_arg_names) ? info.loader_arg_names : [];
                const values = Array.isArray(info.loader_args) ? info.loader_args : [];
                names.forEach((n, i) => {
                    variantArgs[n] = values[i] || '';
                });
            }
        }
        
        // Fetch variant options for dropdowns
        this.state.editVariantOptions = {};
        if (this.state.currentSystem && this.state.currentProtocol && this.state.currentVariant) {
            try {
                const optResponse = await this.sendConfigCommandAsync(
                    `config_get_variant_options {${this.state.currentSystem}} {${this.state.currentProtocol}} {${this.state.currentVariant}}`
                );
                const optData = typeof optResponse === 'string' ? JSON.parse(optResponse) : optResponse;
                if (optData && optData.loader_options) {
                    this.state.editVariantOptions = optData.loader_options;
                }
            } catch (optErr) {
                console.warn('Could not fetch variant options:', optErr);
            }
        }
        
        // Build a pseudo-config object for create mode
        const subject = this.state.currentSubject || '';
        this.state.editingConfig = {
            name: '',  // Empty - user must provide
            description: '',
            subject: subject,
            system: this.state.currentSystem,
            protocol: this.state.currentProtocol,
            variant: this.state.currentVariant,
            variant_args: variantArgs,
            params: paramsDict,
            tags: subject ? [subject] : [],  // Auto-tag with subject
            created_at: null,
            created_by: ''
        };
        
        // Mark as create mode (not editing existing)
        this.state.createMode = true;
        this.state.viewMode = false;
        
        // Initialize form
        this.state.editForm = {
            name: '',
            description: '',
            subject: subject,
            tags: subject ? [subject] : [],
            variant_args: { ...variantArgs },
            params: { ...paramsDict }
        };
        
        // Populate and show edit form
        this.populateEditForm();
        this.showEditView();
        
        // Switch to Configs tab if not already there
        this.switchTab('configs');
        
        // Focus the name input
        setTimeout(() => this.elements.editName.focus(), 100);
    }
    
    async saveConfig(name) {
        try {
            this.emit('log', { message: `Saving config: ${name}...`, level: 'info' });
            await this.sendConfigCommandAsync(`config_save {${name}}`);
            this.emit('log', { message: `Saved config: ${name}`, level: 'info' });
            
            // Refresh list
            this.refreshConfigList();
        } catch (e) {
            this.emit('log', { message: `Failed to save config: ${e.message}`, level: 'error' });
        }
    }
    
    async archiveConfig(name) {
        try {
            this.emit('log', { message: `Moving to trash: ${name}...`, level: 'info' });
            await this.sendConfigCommandAsync(`config_archive {${name}}`);
            this.emit('log', { message: `Moved to trash: ${name}`, level: 'info' });
            
            // Clear current config if it was the archived one
            if (this.state.currentConfig?.name === name) {
                this.state.currentConfig = null;
                this.updateConfigStatusBar();
                this.updateConfigStatusBar();
            }
            
            // Refresh list
            this.refreshConfigList();
        } catch (e) {
            this.emit('log', { message: `Failed to archive config: ${e.message}`, level: 'error' });
        }
    }
    
    async restoreConfig(name) {
        try {
            this.emit('log', { message: `Restoring config: ${name}...`, level: 'info' });
            await this.sendConfigCommandAsync(`config_restore {${name}}`);
            this.emit('log', { message: `Restored config: ${name}`, level: 'info' });
            
            // Refresh list
            this.refreshConfigList();
        } catch (e) {
            this.emit('log', { message: `Failed to restore config: ${e.message}`, level: 'error' });
        }
    }
    
    async permanentlyDeleteConfig(name) {
        const confirmed = confirm(`Permanently delete "${name}"?\n\nThis cannot be undone.`);
        if (!confirmed) return;
        
        try {
            this.emit('log', { message: `Permanently deleting: ${name}...`, level: 'info' });
            await this.sendConfigCommandAsync(`config_delete {${name}}`);
            this.emit('log', { message: `Permanently deleted: ${name}`, level: 'info' });
            
            // Refresh list
            this.refreshConfigList();
        } catch (e) {
            this.emit('log', { message: `Failed to delete config: ${e.message}`, level: 'error' });
        }
    }
    
    async cloneConfig(name) {
        // Backend clone with auto-generated name
        const newName = `${name}_copy`;
        
        try {
            this.emit('log', { message: `Cloning config: ${name} → ${newName}...`, level: 'info' });
            await this.sendConfigCommandAsync(`config_clone {${name}} {${newName}}`);
            this.emit('log', { message: `Cloned config: ${newName} (use Edit to rename)`, level: 'info' });
            
            // Refresh list
            this.refreshConfigList();
        } catch (e) {
            this.emit('log', { message: `Failed to clone config: ${e.message}`, level: 'error' });
        }
    }
    
    async viewConfig(name) {
        // View mode - same as edit but read-only
        this.state.viewMode = true;
        await this.editConfig(name);
    }
    
    async editConfig(name) {
        // Edit mode (unless viewConfig set viewMode)
        if (!this.state.viewMode) {
            this.state.viewMode = false;
        }
        
        try {
            // Fetch full config details as JSON
            const response = await this.sendConfigCommandAsync(`config_get_json {${name}}`);
            let config;
            try {
                config = typeof response === 'string' ? JSON.parse(response) : response;
            } catch (parseErr) {
                this.emit('log', { message: `Failed to parse config data`, level: 'error' });
                return;
            }
            
            if (!config || !config.name) {
                this.emit('log', { message: `Config not found: ${name}`, level: 'error' });
                return;
            }
            
            // Store config being edited
            this.state.editingConfig = config;
            this.state.createMode = false;  // Editing existing config
            
            // Fetch variant options for this config's system/protocol/variant
            this.state.editVariantOptions = {};
            if (config.system && config.protocol && config.variant) {
                try {
                    const optResponse = await this.sendConfigCommandAsync(
                        `config_get_variant_options {${config.system}} {${config.protocol}} {${config.variant}}`
                    );
                    const optData = typeof optResponse === 'string' ? JSON.parse(optResponse) : optResponse;
                    if (optData && optData.loader_options) {
                        this.state.editVariantOptions = optData.loader_options;
                    }
                } catch (optErr) {
                    console.warn('Could not fetch variant options:', optErr);
                }
            }
            
            // Initialize form values - handle tags that might be string or array
            let tags = config.tags || [];
            if (typeof tags === 'string') {
                try { tags = JSON.parse(tags); } catch (e) { tags = []; }
            }
            
            this.state.editForm = {
                name: config.name || '',
                description: config.description || '',
                subject: config.subject || '',
                tags: Array.isArray(tags) ? [...tags] : [],
                variant_args: typeof config.variant_args === 'string' 
                    ? TclParser.parseDict(config.variant_args) 
                    : (config.variant_args || {}),
                params: typeof config.params === 'string'
                    ? TclParser.parseDict(config.params)
                    : (config.params || {})
            };
            
            // Populate the edit form
            this.populateEditForm();
            
            // Show edit view
            this.showEditView();
            
        } catch (e) {
            this.emit('log', { message: `Failed to load config for editing: ${e.message}`, level: 'error' });
        }
    }
    
    showEditView() {
        this.elements.configListView.style.display = 'none';
        this.elements.configEditView.style.display = 'block';
        
        // Show Save/Cancel in status bar, hide New
        this.elements.btnConfigNew.style.display = 'none';
        this.elements.btnConfigSaveEdit.style.display = '';
        this.elements.btnConfigCancelEdit.style.display = '';
    }
    
    hideEditView() {
        this.elements.configEditView.style.display = 'none';
        this.elements.configListView.style.display = 'block';
        this.state.viewMode = false;
        this.state.createMode = false;
        
        // Show New in status bar, hide Save/Cancel
        this.elements.btnConfigNew.style.display = '';
        this.elements.btnConfigSaveEdit.style.display = 'none';
        this.elements.btnConfigCancelEdit.style.display = 'none';
    }
    
    populateEditForm() {
        const config = this.state.editingConfig;
        const form = this.state.editForm;
        const viewMode = this.state.viewMode;
        const createMode = this.state.createMode;
        
        // Basic fields
        this.elements.editNameDisplay.textContent = createMode ? 'New Config' : config.name;
        this.elements.editName.value = form.name;
        this.elements.editDescription.value = form.description;
        
        // Subject dropdown - populate with available subjects
        this.populateSelect(this.elements.editSubject, this.state.subjects, '-- None --');
        this.elements.editSubject.value = form.subject;
        
        // Apply view mode (read-only) to form fields
        this.elements.editName.disabled = viewMode;
        this.elements.editDescription.disabled = viewMode;
        this.elements.editSubject.disabled = viewMode;
        this.elements.editTagInput.disabled = viewMode;
        this.elements.editTagInput.style.display = viewMode ? 'none' : '';
        
        // Update status bar buttons for view vs edit mode
        if (viewMode) {
            this.elements.btnConfigSaveEdit.style.display = 'none';
            this.elements.btnConfigCancelEdit.textContent = 'Close';
        } else {
            this.elements.btnConfigSaveEdit.style.display = '';
            this.elements.btnConfigCancelEdit.textContent = 'Cancel';
        }
        
        // Tags
        this.renderEditTags();
        
        // Variant args
        const variantArgs = form.variant_args;
        if (variantArgs && Object.keys(variantArgs).length > 0) {
            this.elements.editVariantArgsSection.style.display = 'block';
            this.renderEditKeyValues(this.elements.editVariantArgs, variantArgs, 'variant_args');
        } else {
            this.elements.editVariantArgsSection.style.display = 'none';
        }
        
        // Params
        const params = form.params;
        if (params && Object.keys(params).length > 0) {
            this.elements.editParamsSection.style.display = 'block';
            this.renderEditKeyValues(this.elements.editParams, params, 'params');
        } else {
            this.elements.editParamsSection.style.display = 'none';
        }
        
        // Read-only info
        const path = `${config.system || ''}/${config.protocol || ''}/${config.variant || ''}`;
        this.elements.editPath.textContent = path;
        
        const createdDate = config.created_at ? new Date(config.created_at * 1000).toLocaleDateString() : '—';
        const createdBy = config.created_by || 'unknown';
        this.elements.editCreated.textContent = `${createdBy} @ ${createdDate}`;
    }
    
    renderEditTags() {
        const tags = this.state.editForm.tags || [];
        const viewMode = this.state.viewMode;
        
        this.elements.editTags.innerHTML = tags.map(tag => `
            <span class="ess-edit-tag">
                ${this.escapeHtml(tag)}
                ${viewMode ? '' : `<button class="ess-edit-tag-remove" data-tag="${this.escapeAttr(tag)}">×</button>`}
            </span>
        `).join('');
        
        // Bind remove handlers (only in edit mode)
        if (!viewMode) {
            this.elements.editTags.querySelectorAll('.ess-edit-tag-remove').forEach(btn => {
                btn.addEventListener('click', (e) => {
                    e.preventDefault();
                    this.removeEditTag(btn.dataset.tag);
                });
            });
        }
    }
    
    addEditTag(tag) {
        if (!tag) return;
        const tags = this.state.editForm.tags;
        if (!tags.includes(tag)) {
            tags.push(tag);
            this.renderEditTags();
        }
    }
    
    removeEditTag(tag) {
        const tags = this.state.editForm.tags;
        const idx = tags.indexOf(tag);
        if (idx >= 0) {
            tags.splice(idx, 1);
            this.renderEditTags();
        }
    }
    
    renderEditKeyValues(container, obj, prefix) {
        const variantOptions = this.state.editVariantOptions || {};
        const viewMode = this.state.viewMode;
        const disabledAttr = viewMode ? 'disabled' : '';
        
        container.innerHTML = Object.entries(obj).map(([key, value]) => {
            const options = (prefix === 'variant_args') ? variantOptions[key] : null;
            
            if (options && Array.isArray(options) && options.length > 0) {
                // Render dropdown for variant_args with defined options
                const optionsHtml = options.map(opt => {
                    const optLabel = opt.label || opt.value || opt;
                    const optValue = opt.value || opt;
                    const selected = String(optValue) === String(value) ? 'selected' : '';
                    return `<option value="${this.escapeAttr(String(optValue))}" ${selected}>${this.escapeHtml(String(optLabel))}</option>`;
                }).join('');
                
                return `
                    <div class="ess-edit-kv-row">
                        <label class="ess-edit-kv-label">${this.escapeHtml(key)}</label>
                        <select class="ess-edit-kv-select" data-prefix="${prefix}" data-key="${this.escapeAttr(key)}" ${disabledAttr}>
                            ${optionsHtml}
                        </select>
                    </div>
                `;
            } else {
                // Render text input (for params or variant_args without options)
                return `
                    <div class="ess-edit-kv-row">
                        <label class="ess-edit-kv-label">${this.escapeHtml(key)}</label>
                        <input type="text" class="ess-edit-kv-input" 
                               data-prefix="${prefix}" data-key="${this.escapeAttr(key)}"
                               value="${this.escapeAttr(String(value))}" ${disabledAttr}>
                    </div>
                `;
            }
        }).join('');
    }
    
    cancelEdit() {
        this.state.editingConfig = null;
        this.state.editForm = {};
        this.state.createMode = false;
        this.hideEditView();
    }
    
    async saveEdit() {
        const original = this.state.editingConfig;
        if (!original) return;
        
        // Gather current form values
        const newName = this.elements.editName.value.trim();
        const newDescription = this.elements.editDescription.value.trim();
        const newSubject = this.elements.editSubject.value;
        const newTags = this.state.editForm.tags;
        
        // Gather variant_args from inputs and selects
        const newVariantArgs = {};
        this.elements.editVariantArgs.querySelectorAll('.ess-edit-kv-input, .ess-edit-kv-select').forEach(el => {
            if (el.dataset.prefix === 'variant_args') {
                newVariantArgs[el.dataset.key] = el.value;
            }
        });
        
        // Gather params from inputs and selects
        const newParams = {};
        this.elements.editParams.querySelectorAll('.ess-edit-kv-input, .ess-edit-kv-select').forEach(el => {
            if (el.dataset.prefix === 'params') {
                newParams[el.dataset.key] = el.value;
            }
        });
        
        // Validate name
        if (!newName) {
            this.emit('log', { message: 'Name cannot be empty', level: 'error' });
            return;
        }
        if (!/^[\w\-\.]+$/.test(newName)) {
            this.emit('log', { message: 'Invalid name: use only letters, numbers, underscores, dashes, dots', level: 'error' });
            return;
        }
        
        // Handle CREATE mode vs UPDATE mode
        if (this.state.createMode) {
            // Create new config using config_create
            try {
                // Build optional args
                let optArgs = [];
                if (newDescription) {
                    optArgs.push(`-description {${newDescription}}`);
                }
                if (newSubject) {
                    optArgs.push(`-subject {${newSubject}}`);
                }
                if (newTags && newTags.length > 0) {
                    optArgs.push(`-tags {${newTags.join(' ')}}`);
                }
                if (Object.keys(newVariantArgs).length > 0) {
                    const vargsStr = Object.entries(newVariantArgs).map(([k, v]) => `${k} {${v}}`).join(' ');
                    optArgs.push(`-variant_args {${vargsStr}}`);
                }
                if (Object.keys(newParams).length > 0) {
                    const paramsStr = Object.entries(newParams).map(([k, v]) => `${k} {${v}}`).join(' ');
                    optArgs.push(`-params {${paramsStr}}`);
                }
                
                const cmd = `config_create {${newName}} {${original.system}} {${original.protocol}} {${original.variant}} ${optArgs.join(' ')}`;
                
                this.emit('log', { message: `Creating config: ${newName}...`, level: 'info' });
                await this.sendConfigCommandAsync(cmd);
                this.emit('log', { message: `Created config: ${newName}`, level: 'info' });
                
                // Refresh and close edit view
                this.refreshConfigList();
                this.cancelEdit();
            } catch (e) {
                this.emit('log', { message: `Failed to create config: ${e.message}`, level: 'error' });
            }
            return;
        }
        
        // UPDATE mode - build update command with changed fields
        let updateArgs = [];
        
        if (newName !== original.name) {
            updateArgs.push(`-name {${newName}}`);
        }
        if (newDescription !== (original.description || '')) {
            updateArgs.push(`-description {${newDescription}}`);
        }
        if (newSubject !== (original.subject || '')) {
            updateArgs.push(`-subject {${newSubject}}`);
        }
        
        // Tags - compare as sorted strings
        const origTags = Array.isArray(original.tags) ? [...original.tags].sort() : [];
        const formTags = [...newTags].sort();
        if (JSON.stringify(origTags) !== JSON.stringify(formTags)) {
            updateArgs.push(`-tags {${newTags.join(' ')}}`);
        }
        
        // Variant args - compare
        if (JSON.stringify(newVariantArgs) !== JSON.stringify(original.variant_args || {})) {
            const vargsStr = Object.entries(newVariantArgs).map(([k, v]) => `${k} {${v}}`).join(' ');
            updateArgs.push(`-variant_args {${vargsStr}}`);
        }
        
        // Params - compare
        if (JSON.stringify(newParams) !== JSON.stringify(original.params || {})) {
            const paramsStr = Object.entries(newParams).map(([k, v]) => `${k} {${v}}`).join(' ');
            updateArgs.push(`-params {${paramsStr}}`);
        }
        
        if (updateArgs.length === 0) {
            this.emit('log', { message: 'No changes to save', level: 'info' });
            this.cancelEdit();
            return;
        }
        
        try {
            this.emit('log', { message: `Updating config: ${original.name}...`, level: 'info' });
            await this.sendConfigCommandAsync(`config_update {${original.name}} ${updateArgs.join(' ')}`);
            this.emit('log', { message: `Updated config: ${original.name}`, level: 'info' });
            
            // Refresh and close edit view
            this.refreshConfigList();
            this.cancelEdit();
        } catch (e) {
            this.emit('log', { message: `Failed to update config: ${e.message}`, level: 'error' });
        }
    }
    
    // =========================================================================
    // SECTION 6b: IMPORT/EXPORT - Peer Config Sharing
    // =========================================================================
    
    /**
     * Get mesh peers from subscribed datapoint (populated by dserv-agent)
     * Falls back to empty array if not yet available
     */
    getMeshPeers() {
        const peers = this.state.meshPeers || [];
        // Filter out local peer for import/export
        return peers.filter(p => !p.isLocal);
    }
    
    /**
     * Update mesh peers from datapoint subscription
     */
    updateMeshPeers(data) {
        try {
            const peers = typeof data === 'string' ? JSON.parse(data) : data;
            this.state.meshPeers = Array.isArray(peers) ? peers : [];
        } catch (e) {
            console.error('Failed to parse mesh peers:', e);
            this.state.meshPeers = [];
        }
    }
    
    /**
     * Show import dialog - select peer, then select configs to import
     */
    async showImportDialog() {
        const peers = this.getMeshPeers();
        
        if (peers.length === 0) {
            this.emit('log', { message: 'No remote peers available', level: 'warning' });
            return;
        }
        
        // Create modal for peer selection
        const modal = document.createElement('div');
        modal.className = 'ess-modal-overlay';
        modal.innerHTML = `
            <div class="ess-modal">
                <div class="ess-modal-header">
                    <span class="ess-modal-title">Import Config</span>
                    <button class="ess-modal-close">×</button>
                </div>
                <div class="ess-modal-body">
                    <div class="ess-modal-section">
                        <label class="ess-modal-label">Select source system:</label>
                        <select class="ess-modal-select" id="ess-import-peer-select">
                            <option value="">-- Select peer --</option>
                            ${peers.map(p => `<option value="${p.ipAddress}" data-port="${p.webPort}">${this.escapeHtml(p.name)} (${p.ipAddress})</option>`).join('')}
                        </select>
                    </div>
                    <div class="ess-modal-section" id="ess-import-configs-section" style="display: none;">
                        <label class="ess-modal-label">Select configs to import:</label>
                        <div class="ess-modal-config-list" id="ess-import-config-list">
                            <div class="ess-modal-loading">Loading configs...</div>
                        </div>
                    </div>
                </div>
                <div class="ess-modal-footer">
                    <button class="ess-modal-btn cancel" id="ess-import-cancel">Cancel</button>
                    <button class="ess-modal-btn primary" id="ess-import-confirm" disabled>Import Selected</button>
                </div>
            </div>
        `;
        
        document.body.appendChild(modal);
        
        const peerSelect = modal.querySelector('#ess-import-peer-select');
        const configsSection = modal.querySelector('#ess-import-configs-section');
        const configList = modal.querySelector('#ess-import-config-list');
        const confirmBtn = modal.querySelector('#ess-import-confirm');
        const cancelBtn = modal.querySelector('#ess-import-cancel');
        const closeBtn = modal.querySelector('.ess-modal-close');
        
        let selectedConfigs = new Set();
        let remoteConfigs = [];
        
        // Peer selection change
        peerSelect.addEventListener('change', async () => {
            const ip = peerSelect.value;
            if (!ip) {
                configsSection.style.display = 'none';
                return;
            }
            
            configsSection.style.display = 'block';
            configList.innerHTML = '<div class="ess-modal-loading">Loading configs...</div>';
            selectedConfigs.clear();
            confirmBtn.disabled = true;
            
            try {
                // Fetch remote config list via remoteSend
                const result = await this.dpManager.connection.sendRaw(`remoteSend ${ip} {dservGet configs/list}`);
                remoteConfigs = typeof result === 'string' ? JSON.parse(result) : result;
                
                if (!Array.isArray(remoteConfigs) || remoteConfigs.length === 0) {
                    configList.innerHTML = '<div class="ess-modal-empty">No configs on remote system</div>';
                    return;
                }
                
                configList.innerHTML = remoteConfigs.map(cfg => `
                    <label class="ess-modal-config-item">
                        <input type="checkbox" value="${this.escapeAttr(cfg.name)}" class="ess-import-checkbox">
                        <span class="ess-modal-config-name">${this.escapeHtml(cfg.name)}</span>
                        <span class="ess-modal-config-path">${this.escapeHtml(cfg.system)}/${this.escapeHtml(cfg.protocol)}/${this.escapeHtml(cfg.variant)}</span>
                    </label>
                `).join('');
                
                // Bind checkbox changes
                configList.querySelectorAll('.ess-import-checkbox').forEach(cb => {
                    cb.addEventListener('change', () => {
                        if (cb.checked) {
                            selectedConfigs.add(cb.value);
                        } else {
                            selectedConfigs.delete(cb.value);
                        }
                        confirmBtn.disabled = selectedConfigs.size === 0;
                    });
                });
                
            } catch (e) {
                configList.innerHTML = `<div class="ess-modal-error">Failed to fetch configs: ${this.escapeHtml(e.message)}</div>`;
            }
        });
        
        // Confirm import
        confirmBtn.addEventListener('click', async () => {
            const ip = peerSelect.value;
            if (!ip || selectedConfigs.size === 0) return;
            
            confirmBtn.disabled = true;
            confirmBtn.textContent = 'Importing...';
            
            let imported = 0;
            let failed = 0;
            
            for (const name of selectedConfigs) {
                try {
                    // Get export JSON from remote
                    const exportJson = await this.dpManager.connection.sendRaw(
                        `remoteSend ${ip} {send configs {config_export {${name}}}}`
                    );
                    
                    // Import locally - need to escape the JSON properly for Tcl
                    const escapedJson = exportJson.replace(/\\/g, '\\\\').replace(/\{/g, '\\{').replace(/\}/g, '\\}');
                    await this.sendConfigCommandAsync(`config_import {${exportJson}}`);
                    imported++;
                    
                } catch (e) {
                    console.error(`Failed to import ${name}:`, e);
                    failed++;
                }
            }
            
            modal.remove();
            this.refreshConfigList();
            
            if (failed === 0) {
                this.emit('log', { message: `Imported ${imported} config(s)`, level: 'info' });
            } else {
                this.emit('log', { message: `Imported ${imported}, failed ${failed}`, level: 'warning' });
            }
        });
        
        // Cancel/close
        const closeModal = () => modal.remove();
        cancelBtn.addEventListener('click', closeModal);
        closeBtn.addEventListener('click', closeModal);
        modal.addEventListener('click', (e) => {
            if (e.target === modal) closeModal();
        });
    }
    
    /**
     * Show export dialog - select peer to export to
     */
    async showExportDialog(configName) {
        const peers = this.getMeshPeers();
        
        if (peers.length === 0) {
            this.emit('log', { message: 'No remote peers available', level: 'warning' });
            return;
        }
        
        // Create modal for peer selection
        const modal = document.createElement('div');
        modal.className = 'ess-modal-overlay';
        modal.innerHTML = `
            <div class="ess-modal">
                <div class="ess-modal-header">
                    <span class="ess-modal-title">Export Config</span>
                    <button class="ess-modal-close">×</button>
                </div>
                <div class="ess-modal-body">
                    <div class="ess-modal-section">
                        <label class="ess-modal-label">Config:</label>
                        <div class="ess-modal-value">${this.escapeHtml(configName)}</div>
                    </div>
                    <div class="ess-modal-section">
                        <label class="ess-modal-label">Export to:</label>
                        <select class="ess-modal-select" id="ess-export-peer-select">
                            <option value="">-- Select destination --</option>
                            ${peers.map(p => `<option value="${p.ipAddress}">${this.escapeHtml(p.name)} (${p.ipAddress})</option>`).join('')}
                        </select>
                    </div>
                </div>
                <div class="ess-modal-footer">
                    <button class="ess-modal-btn cancel" id="ess-export-cancel">Cancel</button>
                    <button class="ess-modal-btn primary" id="ess-export-confirm" disabled>Export</button>
                </div>
            </div>
        `;
        
        document.body.appendChild(modal);
        
        const peerSelect = modal.querySelector('#ess-export-peer-select');
        const confirmBtn = modal.querySelector('#ess-export-confirm');
        const cancelBtn = modal.querySelector('#ess-export-cancel');
        const closeBtn = modal.querySelector('.ess-modal-close');
        
        peerSelect.addEventListener('change', () => {
            confirmBtn.disabled = !peerSelect.value;
        });
        
        confirmBtn.addEventListener('click', async () => {
            const ip = peerSelect.value;
            if (!ip) return;
            
            confirmBtn.disabled = true;
            confirmBtn.textContent = 'Exporting...';
            
            try {
                // Get export JSON locally
                const exportJson = await this.sendConfigCommandAsync(`config_export {${configName}}`);
                
                // Send to remote via remoteSend
                await this.dpManager.connection.sendRaw(
                    `remoteSend ${ip} {send configs {config_import {${exportJson}}}}`
                );
                
                modal.remove();
                this.emit('log', { message: `Exported "${configName}" to ${peerSelect.options[peerSelect.selectedIndex].text}`, level: 'info' });
                
            } catch (e) {
                modal.remove();
                this.emit('log', { message: `Export failed: ${e.message}`, level: 'error' });
            }
        });
        
        // Cancel/close
        const closeModal = () => modal.remove();
        cancelBtn.addEventListener('click', closeModal);
        closeBtn.addEventListener('click', closeModal);
        modal.addEventListener('click', (e) => {
            if (e.target === modal) closeModal();
        });
    }
    
    // =========================================================================
    // SECTION 7: HELPERS
    // =========================================================================
    
    sendCommand(cmd) {
        if (this.dpManager.connection.ws && this.dpManager.connection.connected) {
            const message = { cmd: 'eval', script: cmd };
            this.dpManager.connection.ws.send(JSON.stringify(message));
        }
    }
    
    sendConfigCommand(cmd) {
        if (this.dpManager.connection.ws && this.dpManager.connection.connected) {
            const script = `send configs {${cmd}}`;
            const message = { cmd: 'eval', script: script };
            this.dpManager.connection.ws.send(JSON.stringify(message));
        }
    }
    
    async sendConfigCommandAsync(cmd) {
        if (!this.dpManager.connection.ws || !this.dpManager.connection.connected) {
            throw new Error('Not connected');
        }
        return await this.dpManager.connection.sendRaw(`send configs {${cmd}}`);
    }
    
    async sendEssCommandAsync(cmd) {
        if (!this.dpManager.connection.ws || !this.dpManager.connection.connected) {
            throw new Error('Not connected');
        }
        return await this.dpManager.connection.sendRaw(`send ess {${cmd}}`);
    }
    
    parseListData(data) {
        if (Array.isArray(data)) return data;
        if (typeof data === 'string') return TclParser.parseList(data);
        return [];
    }
    
    parseTags(tags) {
        if (!tags) return [];
        if (Array.isArray(tags)) return tags;
        if (typeof tags === 'string') {
            try {
                const parsed = JSON.parse(tags);
                return Array.isArray(parsed) ? parsed : [];
            } catch (e) {
                return [];
            }
        }
        return [];
    }
    
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
    
    updateSelectValue(select, value) {
        if (!value) return;
        const exists = Array.from(select.options).some(opt => opt.value === value);
        if (exists) select.value = value;
    }
    
    escapeHtml(text) {
        if (!text) return '';
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }
    
    escapeAttr(text) {
        if (!text) return '';
        return text.replace(/"/g, '&quot;').replace(/'/g, '&#39;');
    }
    
    // =========================================================================
    // SECTION 8: EVENT EMITTER & CLEANUP
    // =========================================================================
    
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
    
    getState() {
        return { ...this.state };
    }
    
    dispose() {
        // Unsubscribe from all datapoints
        const dps = [
            'ess/subject_ids', 'ess/subject', 'ess/systems', 'ess/system',
            'ess/protocols', 'ess/protocol', 'ess/variants', 'ess/variant',
            'ess/status', 'ess/variant_info_json', 'ess/param_settings',
            'ess/param', 'ess/params', 'ess/datafile',
            'ess/obs_id', 'ess/obs_total', 'ess/in_obs',
            'configs/list', 'configs/tags', 'configs/current'
        ];
        dps.forEach(dp => this.dpManager.unsubscribe(dp));
        
        this.listeners.clear();
        this.container.innerHTML = '';
    }
}

// Export
if (typeof window !== 'undefined') {
    window.ESSControl = ESSControl;
}
