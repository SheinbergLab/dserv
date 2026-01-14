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
            activeTab: 'setup',  // 'setup', 'configs', or 'queue'
            
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
            editForm: {},         // form field values during edit
            
            // Remote servers (manually entered during this session)
            remoteServers: [],        // [{address: 'hostname', label: 'hostname'}]
            backendRemoteServers: [],  // from configs/remote_servers datapoint
            
            // Queue state
            queues: [],               // list of available queues
            selectedQueue: '',        // currently selected queue name
            queueState: {             // current playback state from queues/state
                status: 'idle',
                queue_name: '',
                position: 0,
                total_items: 0,
                current_config: '',
                run_count: 0,
                repeat_total: 1,
                pause_remaining: 0,
                auto_start: 1,
                auto_advance: 1
            },
            queueItems: []            // items of selected/active queue
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
                        <button class="ess-tab-btn" data-tab="queue">Queue</button>
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
                            
                            <!-- Short Name -->
                            <div class="ess-edit-row">
                                <label class="ess-edit-label">Short Name</label>
                                <input type="text" id="ess-edit-short-name" class="ess-edit-input" 
                                       placeholder="Brief label for filenames">
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
                
                <!-- Queue Tab Content -->
                <div class="ess-tab-content" id="ess-tab-queue">
                    <!-- Queue Selection Row -->
                    <div class="ess-queue-select-row">
                        <select id="ess-queue-select" class="ess-queue-select">
                            <option value="">-- Select Queue --</option>
                        </select>
                        <div class="ess-queue-menu-wrapper">
                            <button id="ess-queue-menu-btn" class="ess-queue-menu-btn" title="Queue actions">⋮</button>
                            <div class="ess-queue-menu" id="ess-queue-menu">
                                <button class="ess-queue-menu-action" data-action="edit">Edit</button>
                                <button class="ess-queue-menu-action" data-action="new">New</button>
                                <div class="ess-queue-menu-divider"></div>
                                <button class="ess-queue-menu-action" data-action="push">Push to...</button>
                                <button class="ess-queue-menu-action" data-action="import">Import from...</button>
                            </div>
                        </div>
                    </div>
                    
                    <!-- Queue Status Display -->
                    <div class="ess-queue-status" id="ess-queue-status">
                        <div class="ess-queue-status-row">
                            <span class="ess-queue-status-label">Status:</span>
                            <span class="ess-queue-status-value" id="ess-queue-status-text">idle</span>
                        </div>
                        <div class="ess-queue-status-row">
                            <span class="ess-queue-status-label">Progress:</span>
                            <span class="ess-queue-status-value" id="ess-queue-progress">--/--</span>
                        </div>
                        <div class="ess-queue-status-row" id="ess-queue-run-row" style="display: none;">
                            <span class="ess-queue-status-label">Run:</span>
                            <span class="ess-queue-status-value" id="ess-queue-run-count">--/--</span>
                        </div>
                        <div class="ess-queue-status-row" id="ess-queue-countdown-row" style="display: none;">
                            <span class="ess-queue-status-label">Next in:</span>
                            <span class="ess-queue-status-value" id="ess-queue-countdown">--</span>
                        </div>
                    </div>
                    
                    <!-- Queue Playback Controls -->
                    <div class="ess-queue-controls">
                        <button id="ess-queue-start" class="ess-state-btn go" disabled>▶ Start</button>
                        <button id="ess-queue-stop" class="ess-state-btn stop" disabled>■ Stop</button>
                        <button id="ess-queue-pause" class="ess-state-btn reset" disabled>⏸ Pause</button>
                    </div>
                    
                    <!-- Queue Secondary Controls -->
                    <div class="ess-queue-controls-secondary">
                        <button id="ess-queue-skip" class="ess-mini-btn" disabled title="Skip to next item">Skip →</button>
                        <button id="ess-queue-retry" class="ess-mini-btn" disabled title="Retry current item">↺ Retry</button>
                        <button id="ess-queue-force" class="ess-mini-btn" disabled title="Force complete current run">Force Done</button>
                    </div>
                    
                    <!-- Playlist (Queue Items) -->
                    <div class="ess-queue-playlist-header">
                        <span class="ess-queue-playlist-title">Playlist</span>
                        <span class="ess-queue-playlist-count" id="ess-queue-item-count">0 items</span>
                    </div>
                    <div class="ess-queue-playlist" id="ess-queue-playlist">
                        <div class="ess-queue-empty">No queue selected</div>
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
            tabQueue: this.container.querySelector('#ess-tab-queue'),
            
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
            editShortName: this.container.querySelector('#ess-edit-short-name'),
            editSubject: this.container.querySelector('#ess-edit-subject'),
            editTags: this.container.querySelector('#ess-edit-tags'),
            editTagInput: this.container.querySelector('#ess-edit-tag-input'),
            editVariantArgsSection: this.container.querySelector('#ess-edit-variant-args-section'),
            editVariantArgs: this.container.querySelector('#ess-edit-variant-args'),
            editParamsSection: this.container.querySelector('#ess-edit-params-section'),
            editParams: this.container.querySelector('#ess-edit-params'),
            editPath: this.container.querySelector('#ess-edit-path'),
            editCreated: this.container.querySelector('#ess-edit-created'),
            
            // Queue tab elements
            queueSelect: this.container.querySelector('#ess-queue-select'),
            queueMenuBtn: this.container.querySelector('#ess-queue-menu-btn'),
            queueMenu: this.container.querySelector('#ess-queue-menu'),
            queueStatusText: this.container.querySelector('#ess-queue-status-text'),
            queueProgress: this.container.querySelector('#ess-queue-progress'),
            queueRunRow: this.container.querySelector('#ess-queue-run-row'),
            queueRunCount: this.container.querySelector('#ess-queue-run-count'),
            queueCountdownRow: this.container.querySelector('#ess-queue-countdown-row'),
            queueCountdown: this.container.querySelector('#ess-queue-countdown'),
            queueBtnStart: this.container.querySelector('#ess-queue-start'),
            queueBtnStop: this.container.querySelector('#ess-queue-stop'),
            queueBtnPause: this.container.querySelector('#ess-queue-pause'),
            queueBtnSkip: this.container.querySelector('#ess-queue-skip'),
            queueBtnRetry: this.container.querySelector('#ess-queue-retry'),
            queueBtnForce: this.container.querySelector('#ess-queue-force'),
            queueItemCount: this.container.querySelector('#ess-queue-item-count'),
            queuePlaylist: this.container.querySelector('#ess-queue-playlist')
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
        
        // Queue tab events
        this.elements.queueSelect.addEventListener('change', (e) => {
            this.selectQueue(e.target.value);
        });
        
        // Queue menu
        this.elements.queueMenuBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            this.toggleQueueMenu();
        });
        
        this.elements.queueMenu.querySelectorAll('.ess-queue-menu-action').forEach(btn => {
            btn.addEventListener('click', (e) => {
                e.stopPropagation();
                const action = btn.dataset.action;
                this.closeQueueMenu();
                
                switch (action) {
                    case 'edit':
                        if (this.state.selectedQueue) {
                            this.showQueueBuilderModal(this.state.selectedQueue);
                        }
                        break;
                    case 'new':
                        this.showQueueBuilderModal();
                        break;
                    case 'push':
                        if (this.state.selectedQueue) {
                            this.showQueuePushDialog(this.state.selectedQueue);
                        }
                        break;
                    case 'import':
                        this.showQueueImportDialog();
                        break;
                }
            });
        });
        
        // Close menu when clicking elsewhere
        document.addEventListener('click', () => this.closeQueueMenu());
        
        // Queue playback controls
        this.elements.queueBtnStart.addEventListener('click', () => this.queueStart());
        this.elements.queueBtnStop.addEventListener('click', () => this.queueStop());
        this.elements.queueBtnPause.addEventListener('click', () => this.queuePauseResume());
        this.elements.queueBtnSkip.addEventListener('click', () => this.queueSkip());
        this.elements.queueBtnRetry.addEventListener('click', () => this.queueRetry());
        this.elements.queueBtnForce.addEventListener('click', () => this.queueForceComplete());
    }
    
    /**
     * Toggle queue menu visibility
     */
    toggleQueueMenu() {
        const isOpen = this.elements.queueMenu.classList.toggle('open');
        if (isOpen) {
            this.updateQueueMenuState();
        }
    }
    
    /**
     * Close queue menu
     */
    closeQueueMenu() {
        this.elements.queueMenu.classList.remove('open');
    }
    
    /**
     * Update queue menu item enabled/disabled state
     */
    updateQueueMenuState() {
        const hasQueue = !!this.state.selectedQueue;
        const qs = this.state.queueState;
        const isActive = qs.status !== 'idle' && qs.status !== 'finished';
        
        this.elements.queueMenu.querySelectorAll('.ess-queue-menu-action').forEach(btn => {
            const action = btn.dataset.action;
            switch (action) {
                case 'edit':
                    btn.disabled = !hasQueue || isActive;
                    break;
                case 'push':
                    btn.disabled = !hasQueue;
                    break;
                // 'new' and 'import' are always enabled
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
        this.elements.tabQueue.classList.toggle('active', tabName === 'queue');
        
        // Hide config "New" button when on Queue tab (status bar still shows current config)
        this.elements.btnConfigNew.style.display = (tabName === 'queue') ? 'none' : '';
        
        // If switching to configs, refresh the list
        if (tabName === 'configs') {
            this.refreshConfigList();
        }
        
        // If switching to queue, refresh queue list
        if (tabName === 'queue') {
            this.refreshQueueList();
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
        
        // Remote servers subscription (optional - if backend provides known servers)
        this.dpManager.subscribe('configs/remote_servers', (data) => {
            this.updateRemoteServers(data.value);
        });
        
        // Queue subscriptions
        this.dpManager.subscribe('queues/list', (data) => {
            this.updateQueueList(data.value);
        });
        
        this.dpManager.subscribe('queues/state', (data) => {
            this.updateQueueState(data.value);
        });
        
        this.dpManager.subscribe('queues/items', (data) => {
            this.updateQueueItems(data.value);
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
            short_name: '',
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
                short_name: config.short_name || '',
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
        this.elements.editShortName.value = form.short_name || '';
        
        // Subject dropdown - populate with available subjects
        this.populateSelect(this.elements.editSubject, this.state.subjects, '-- None --');
        this.elements.editSubject.value = form.subject;
        
        // Apply view mode (read-only) to form fields
        this.elements.editName.disabled = viewMode;
        this.elements.editDescription.disabled = viewMode;
        this.elements.editShortName.disabled = viewMode;
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
        const newShortName = this.elements.editShortName.value.trim();
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
                if (newShortName) {
                    optArgs.push(`-short_name {${newShortName}}`);
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
        if (newShortName !== (original.short_name || '')) {
            updateArgs.push(`-short_name {${newShortName}}`);
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
     * Update remote servers from backend datapoint
     * Expected format: [{address: "hostname", label: "Display Name"}, ...]
     * or simple array of strings: ["hostname1", "hostname2", ...]
     */
    updateRemoteServers(data) {
        try {
            let servers = typeof data === 'string' ? JSON.parse(data) : data;
            if (!Array.isArray(servers)) {
                servers = [];
            }
            
            // Normalize to {address, label} format
            const normalized = servers.map(s => {
                if (typeof s === 'string') {
                    return { address: s, label: s };
                }
                return { address: s.address || s.host || s, label: s.label || s.name || s.address || s };
            });
            
            // Merge with session servers (session servers take priority, no duplicates)
            const sessionAddresses = new Set(this.state.remoteServers.map(s => s.address));
            const backendServers = normalized.filter(s => !sessionAddresses.has(s.address));
            
            // Store backend servers separately so session additions appear first
            this.state.backendRemoteServers = backendServers;
        } catch (e) {
            console.error('Failed to parse remote servers:', e);
            this.state.backendRemoteServers = [];
        }
    }
    
    /**
     * Get combined list of remote servers (session + backend)
     */
    getRemoteServers() {
        const session = this.state.remoteServers || [];
        const backend = this.state.backendRemoteServers || [];
        return [...session, ...backend];
    }
    
    /**
     * Show import dialog - select peer, then select configs to import
     */
    async showImportDialog() {
        const peers = this.getMeshPeers();
        const recentServers = this.getRemoteServers();
        
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
                            <option value="">-- Select source --</option>
                            <option value="__custom__">Enter IP address...</option>
                            ${recentServers.length > 0 ? `<optgroup label="Recent Servers">
                                ${recentServers.map(s => `<option value="${this.escapeAttr(s.address)}">${this.escapeHtml(s.label)}</option>`).join('')}
                            </optgroup>` : ''}
                            ${peers.length > 0 ? `<optgroup label="Mesh Peers">
                                ${peers.map(p => `<option value="${p.ipAddress}">${this.escapeHtml(p.name)} (${p.ipAddress})</option>`).join('')}
                            </optgroup>` : ''}
                        </select>
                    </div>
                    <div class="ess-modal-section" id="ess-import-custom-ip-section" style="display: none;">
                        <label class="ess-modal-label">Remote server address:</label>
                        <input type="text" class="ess-modal-input" id="ess-import-custom-ip" 
                               placeholder="hostname or IP address">
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
        const customIpSection = modal.querySelector('#ess-import-custom-ip-section');
        const customIpInput = modal.querySelector('#ess-import-custom-ip');
        const configsSection = modal.querySelector('#ess-import-configs-section');
        const configList = modal.querySelector('#ess-import-config-list');
        const confirmBtn = modal.querySelector('#ess-import-confirm');
        const cancelBtn = modal.querySelector('#ess-import-cancel');
        const closeBtn = modal.querySelector('.ess-modal-close');
        
        let selectedConfigs = new Set();
        let remoteConfigs = [];
        let currentIp = '';
        
        // Helper to fetch and display configs from an IP
        const fetchConfigsFromIp = async (ip) => {
            if (!ip) return;
            
            currentIp = ip;
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
        };
        
        // Peer selection change
        peerSelect.addEventListener('change', async () => {
            const value = peerSelect.value;
            
            if (value === '__custom__') {
                // Show custom IP input
                customIpSection.style.display = 'block';
                configsSection.style.display = 'none';
                customIpInput.focus();
                return;
            }
            
            customIpSection.style.display = 'none';
            
            if (!value) {
                configsSection.style.display = 'none';
                return;
            }
            
            await fetchConfigsFromIp(value);
        });
        
        // Custom IP input - fetch on Enter
        customIpInput.addEventListener('keydown', async (e) => {
            if (e.key === 'Enter') {
                const ip = customIpInput.value.trim();
                if (ip) {
                    await fetchConfigsFromIp(ip);
                }
            }
        });
        
        // Also add a "Connect" button behavior on blur
        customIpInput.addEventListener('blur', async () => {
            const ip = customIpInput.value.trim();
            if (ip && configsSection.style.display === 'none') {
                await fetchConfigsFromIp(ip);
            }
        });
        
        // Confirm import
        confirmBtn.addEventListener('click', async () => {
            if (!currentIp || selectedConfigs.size === 0) return;
            
            // If this was a custom IP, save it to session list
            if (peerSelect.value === '__custom__' && customIpInput.value.trim()) {
                this.addRemoteServer(customIpInput.value.trim());
            }
            
            confirmBtn.disabled = true;
            confirmBtn.textContent = 'Importing...';
            
            let imported = 0;
            let failed = 0;
            
            for (const name of selectedConfigs) {
                try {
                    // Get export JSON from remote
                    const exportJson = await this.dpManager.connection.sendRaw(
                        `remoteSend ${currentIp} {send configs {config_export {${name}}}}`
                    );
                    
                    // Import locally
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
                this.emit('log', { message: `Imported ${imported} config(s) from ${currentIp}`, level: 'info' });
            } else {
                this.emit('log', { message: `Imported ${imported}, failed ${failed} from ${currentIp}`, level: 'warning' });
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
        const recentServers = this.getRemoteServers();
        
        // Create modal for destination selection
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
                            <option value="__custom__">Enter IP address...</option>
                            ${recentServers.length > 0 ? `<optgroup label="Recent Servers">
                                ${recentServers.map(s => `<option value="${this.escapeAttr(s.address)}">${this.escapeHtml(s.label)}</option>`).join('')}
                            </optgroup>` : ''}
                            ${peers.length > 0 ? `<optgroup label="Mesh Peers">
                                ${peers.map(p => `<option value="${p.ipAddress}">${this.escapeHtml(p.name)} (${p.ipAddress})</option>`).join('')}
                            </optgroup>` : ''}
                        </select>
                    </div>
                    <div class="ess-modal-section" id="ess-export-custom-ip-section" style="display: none;">
                        <label class="ess-modal-label">Remote server address:</label>
                        <input type="text" class="ess-modal-input" id="ess-export-custom-ip" 
                               placeholder="hostname or IP address">
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
        const customIpSection = modal.querySelector('#ess-export-custom-ip-section');
        const customIpInput = modal.querySelector('#ess-export-custom-ip');
        const confirmBtn = modal.querySelector('#ess-export-confirm');
        const cancelBtn = modal.querySelector('#ess-export-cancel');
        const closeBtn = modal.querySelector('.ess-modal-close');
        
        // Helper to get current target IP
        const getTargetIp = () => {
            if (peerSelect.value === '__custom__') {
                return customIpInput.value.trim();
            }
            return peerSelect.value;
        };
        
        // Update confirm button state
        const updateConfirmState = () => {
            confirmBtn.disabled = !getTargetIp();
        };
        
        peerSelect.addEventListener('change', () => {
            if (peerSelect.value === '__custom__') {
                customIpSection.style.display = 'block';
                customIpInput.focus();
                confirmBtn.disabled = true;
            } else {
                customIpSection.style.display = 'none';
                updateConfirmState();
            }
        });
        
        customIpInput.addEventListener('input', updateConfirmState);
        
        confirmBtn.addEventListener('click', async () => {
            const ip = getTargetIp();
            if (!ip) return;
            
            // If this was a custom IP, save it to session list
            if (peerSelect.value === '__custom__' && customIpInput.value.trim()) {
                this.addRemoteServer(customIpInput.value.trim());
            }
            
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
                this.emit('log', { message: `Exported "${configName}" to ${ip}`, level: 'info' });
                
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
    
    /**
     * Add a remote server to the session list
     */
    addRemoteServer(address) {
        // Don't add duplicates
        if (this.state.remoteServers.some(s => s.address === address)) {
            return;
        }
        
        // Add to front of list
        this.state.remoteServers.unshift({
            address: address,
            label: address
        });
        
        // Keep only most recent 5
        if (this.state.remoteServers.length > 5) {
            this.state.remoteServers.pop();
        }
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
            'configs/list', 'configs/tags', 'configs/current',
            'queues/list', 'queues/state', 'queues/items'
        ];
        dps.forEach(dp => this.dpManager.unsubscribe(dp));
        
        this.listeners.clear();
        this.container.innerHTML = '';
    }
    
    // =========================================================================
    // SECTION 9: QUEUE TAB - Queue Management
    // =========================================================================
    
    /**
     * Update queue list from queues/list datapoint
     */
    updateQueueList(data) {
        try {
            const queues = typeof data === 'string' ? JSON.parse(data) : data;
            this.state.queues = Array.isArray(queues) ? queues : [];
            this.renderQueueSelect();
        } catch (e) {
            console.error('Failed to parse queue list:', e);
            this.state.queues = [];
            this.renderQueueSelect();
        }
    }
    
    /**
     * Update queue state from queues/state datapoint
     */
    updateQueueState(data) {
        try {
            const state = typeof data === 'string' ? JSON.parse(data) : data;
            if (state && typeof state === 'object') {
                this.state.queueState = {
                    status: state.status || 'idle',
                    queue_name: state.queue_name || '',
                    position: parseInt(state.position) || 0,
                    total_items: parseInt(state.total_items) || 0,
                    current_config: state.current_config || '',
                    run_count: parseInt(state.run_count) || 0,
                    repeat_total: parseInt(state.repeat_total) || 1,
                    pause_remaining: parseInt(state.pause_remaining) || 0,
                    auto_start: parseInt(state.auto_start) || 0,
                    auto_advance: parseInt(state.auto_advance) || 0
                };
                
                // If queue is active, sync selection
                if (this.state.queueState.queue_name && 
                    this.state.queueState.status !== 'idle' &&
                    this.state.selectedQueue !== this.state.queueState.queue_name) {
                    this.state.selectedQueue = this.state.queueState.queue_name;
                    this.elements.queueSelect.value = this.state.queueState.queue_name;
                }
                
                this.renderQueueStatus();
                this.updateQueueControls();
                this.renderQueuePlaylist();
            }
        } catch (e) {
            console.error('Failed to parse queue state:', e);
        }
    }
    
    /**
     * Update queue items from queues/items datapoint
     */
    updateQueueItems(data) {
        try {
            const items = typeof data === 'string' ? JSON.parse(data) : data;
            this.state.queueItems = Array.isArray(items) ? items : [];
            this.renderQueuePlaylist();
        } catch (e) {
            console.error('Failed to parse queue items:', e);
            this.state.queueItems = [];
            this.renderQueuePlaylist();
        }
    }
    
    /**
     * Render queue dropdown
     */
    renderQueueSelect() {
        const select = this.elements.queueSelect;
        const queues = this.state.queues;
        
        select.innerHTML = '<option value="">-- Select Queue --</option>';
        queues.forEach(q => {
            const opt = document.createElement('option');
            opt.value = q.name;
            opt.textContent = `${q.name} (${q.item_count} items)`;
            select.appendChild(opt);
        });
        
        // Restore selection
        if (this.state.selectedQueue) {
            select.value = this.state.selectedQueue;
        }
    }
    
    /**
     * Render queue status display
     */
    renderQueueStatus() {
        const qs = this.state.queueState;
        
        // Status text with styling
        const statusEl = this.elements.queueStatusText;
        statusEl.textContent = qs.status;
        statusEl.className = 'ess-queue-status-value';
        if (qs.status === 'running') {
            statusEl.classList.add('running');
        } else if (qs.status === 'paused') {
            statusEl.classList.add('paused');
        } else if (qs.status === 'finished') {
            statusEl.classList.add('finished');
        }
        
        // Progress - cap at total_items when finished
        if (qs.total_items > 0) {
            const displayPos = Math.min(qs.position + 1, qs.total_items);
            this.elements.queueProgress.textContent = `${displayPos}/${qs.total_items}`;
        } else {
            this.elements.queueProgress.textContent = '--/--';
        }
        
        // Run count (for repeats)
        if (qs.repeat_total > 1) {
            this.elements.queueRunRow.style.display = '';
            this.elements.queueRunCount.textContent = `${qs.run_count}/${qs.repeat_total}`;
        } else {
            this.elements.queueRunRow.style.display = 'none';
        }
        
        // Countdown (for pause_after delays)
        if (qs.pause_remaining > 0) {
            this.elements.queueCountdownRow.style.display = '';
            this.elements.queueCountdown.textContent = `${qs.pause_remaining}s`;
        } else {
            this.elements.queueCountdownRow.style.display = 'none';
        }
    }
    
    /**
     * Update queue control button states
     */
    updateQueueControls() {
        const qs = this.state.queueState;
        const hasQueue = !!this.state.selectedQueue;
        const isIdle = qs.status === 'idle' || qs.status === 'finished';
        const isRunning = qs.status === 'running';
        const isPaused = qs.status === 'paused';
        const isActive = !isIdle;  // Any non-idle state
        
        // Start button - enabled when queue selected and idle/finished
        this.elements.queueBtnStart.disabled = !hasQueue || isActive;
        
        // Stop button - enabled when active
        this.elements.queueBtnStop.disabled = !isActive;
        
        // Pause button - changes text based on state
        this.elements.queueBtnPause.disabled = !isActive;
        if (isPaused) {
            this.elements.queueBtnPause.textContent = '▶ Resume';
            this.elements.queueBtnPause.classList.remove('reset');
            this.elements.queueBtnPause.classList.add('go');
        } else {
            this.elements.queueBtnPause.textContent = '⏸ Pause';
            this.elements.queueBtnPause.classList.remove('go');
            this.elements.queueBtnPause.classList.add('reset');
        }
        
        // Secondary controls - only when active
        this.elements.queueBtnSkip.disabled = !isActive;
        this.elements.queueBtnRetry.disabled = !isActive;
        this.elements.queueBtnForce.disabled = !isRunning;
    }
    
    /**
     * Render queue playlist
     */
    renderQueuePlaylist() {
        const items = this.state.queueItems;
        const qs = this.state.queueState;
        const playlist = this.elements.queuePlaylist;
        
        // Update item count
        this.elements.queueItemCount.textContent = `${items.length} item${items.length !== 1 ? 's' : ''}`;
        
        if (items.length === 0) {
            if (this.state.selectedQueue) {
                playlist.innerHTML = '<div class="ess-queue-empty">Queue is empty</div>';
            } else {
                playlist.innerHTML = '<div class="ess-queue-empty">No queue selected</div>';
            }
            return;
        }
        
        playlist.innerHTML = items.map((item, idx) => {
            const isCurrent = qs.status !== 'idle' && qs.position === idx;
            const isPast = qs.status !== 'idle' && idx < qs.position;
            const repeatInfo = item.repeat_count > 1 ? `×${item.repeat_count}` : '';
            const pauseInfo = item.pause_after > 0 ? `⏱${item.pause_after}s` : '';
            
            let statusClass = '';
            if (isCurrent) statusClass = 'current';
            else if (isPast) statusClass = 'completed';
            
            return `
                <div class="ess-queue-item ${statusClass}" data-position="${idx}">
                    <span class="ess-queue-item-position">${idx + 1}</span>
                    <span class="ess-queue-item-name">${this.escapeHtml(item.config_name)}</span>
                    <span class="ess-queue-item-info">
                        ${repeatInfo ? `<span class="ess-queue-item-repeat">${repeatInfo}</span>` : ''}
                        ${pauseInfo ? `<span class="ess-queue-item-pause">${pauseInfo}</span>` : ''}
                    </span>
                    ${isCurrent && qs.run_count > 0 ? `<span class="ess-queue-item-run">run ${qs.run_count}</span>` : ''}
                </div>
            `;
        }).join('');
        
        // Add click handlers for jumping to position
        playlist.querySelectorAll('.ess-queue-item').forEach(el => {
            el.addEventListener('click', () => {
                const pos = parseInt(el.dataset.position);
                this.queueJumpTo(pos);
            });
        });
    }
    
    /**
     * Refresh queue list from backend
     */
    refreshQueueList() {
        this.sendConfigCommand('queue_publish_list');
    }
    
    /**
     * Select a queue
     */
    async selectQueue(name) {
        this.state.selectedQueue = name;
        
        if (!name) {
            this.state.queueItems = [];
            this.renderQueuePlaylist();
            this.updateQueueControls();
            return;
        }
        
        // Load queue items
        try {
            const response = await this.sendConfigCommandAsync(`queue_get_json {${name}}`);
            console.log('queue_get_json response:', response);  // Debug
            const queue = typeof response === 'string' ? JSON.parse(response) : response;
            console.log('parsed queue:', queue);  // Debug
            
            if (queue && queue.items) {
                let items = queue.items;
                // Handle case where items might be a string (Tcl list)
                if (typeof items === 'string') {
                    try {
                        items = JSON.parse(items);
                    } catch (e) {
                        console.warn('items is string but not JSON, trying TclParser');
                        // Try parsing as Tcl list of dicts
                        items = TclParser.parseList(items).map(itemStr => {
                            return TclParser.parseDict(itemStr);
                        });
                    }
                }
                this.state.queueItems = Array.isArray(items) ? items : [];
                console.log('final queueItems:', this.state.queueItems);  // Debug
            } else {
                this.state.queueItems = [];
            }
            this.renderQueuePlaylist();
        } catch (e) {
            console.error('Failed to load queue:', e);
            this.state.queueItems = [];
            this.renderQueuePlaylist();
        }
        
        this.updateQueueControls();
    }
    
    // Queue playback commands
    async queueStart() {
        if (!this.state.selectedQueue) return;
        
        try {
            this.emit('log', { message: `Starting queue: ${this.state.selectedQueue}`, level: 'info' });
            await this.sendConfigCommandAsync(`queue_start {${this.state.selectedQueue}}`);
        } catch (e) {
            this.emit('log', { message: `Failed to start queue: ${e.message}`, level: 'error' });
        }
    }
    
    async queueStop() {
        try {
            this.emit('log', { message: 'Stopping queue', level: 'info' });
            await this.sendConfigCommandAsync('queue_stop');
        } catch (e) {
            this.emit('log', { message: `Failed to stop queue: ${e.message}`, level: 'error' });
        }
    }
    
    async queuePauseResume() {
        const isPaused = this.state.queueState.status === 'paused';
        try {
            if (isPaused) {
                this.emit('log', { message: 'Resuming queue', level: 'info' });
                await this.sendConfigCommandAsync('queue_resume');
            } else {
                this.emit('log', { message: 'Pausing queue', level: 'info' });
                await this.sendConfigCommandAsync('queue_pause');
            }
        } catch (e) {
            this.emit('log', { message: `Failed to ${isPaused ? 'resume' : 'pause'} queue: ${e.message}`, level: 'error' });
        }
    }
    
    async queueSkip() {
        try {
            this.emit('log', { message: 'Skipping to next item', level: 'info' });
            await this.sendConfigCommandAsync('queue_skip');
        } catch (e) {
            this.emit('log', { message: `Failed to skip: ${e.message}`, level: 'error' });
        }
    }
    
    async queueRetry() {
        try {
            this.emit('log', { message: 'Retrying current item', level: 'info' });
            await this.sendConfigCommandAsync('queue_retry');
        } catch (e) {
            this.emit('log', { message: `Failed to retry: ${e.message}`, level: 'error' });
        }
    }
    
    async queueForceComplete() {
        try {
            this.emit('log', { message: 'Force completing current run', level: 'info' });
            await this.sendConfigCommandAsync('queue_force_complete');
        } catch (e) {
            this.emit('log', { message: `Failed to force complete: ${e.message}`, level: 'error' });
        }
    }
    
    async queueJumpTo(position) {
        // Only allow jumping when queue is paused or stopped
        const qs = this.state.queueState;
        if (qs.status === 'running') {
            this.emit('log', { message: 'Stop or pause the queue before jumping to a position', level: 'warning' });
            return;
        }
        
        if (!this.state.selectedQueue) return;
        
        try {
            this.emit('log', { message: `Jumping to position ${position + 1}`, level: 'info' });
            await this.sendConfigCommandAsync(`queue_start {${this.state.selectedQueue}} -position ${position}`);
        } catch (e) {
            this.emit('log', { message: `Failed to jump: ${e.message}`, level: 'error' });
        }
    }
    
    // =========================================================================
    // SECTION 10: QUEUE BUILDER MODAL
    // =========================================================================
    
    /**
     * Show queue builder modal for creating or editing a queue
     */
    async showQueueBuilderModal(queueName = null) {
        // Initialize builder state
        this.queueBuilder = {
            isEditing: !!queueName,
            originalName: queueName,
            name: '',
            description: '',
            auto_start: 1,
            auto_advance: 1,
            auto_datafile: 1,
            datafile_template: '{suggest}',
            items: []
        };
        
        // If editing, load existing queue data
        if (queueName) {
            try {
                const response = await this.sendConfigCommandAsync(`queue_get_json {${queueName}}`);
                const queue = typeof response === 'string' ? JSON.parse(response) : response;
                
                this.queueBuilder.name = queue.name || queueName;
                this.queueBuilder.description = queue.description || '';
                this.queueBuilder.auto_start = queue.auto_start ?? 1;
                this.queueBuilder.auto_advance = queue.auto_advance ?? 1;
                this.queueBuilder.auto_datafile = queue.auto_datafile ?? 1;
                this.queueBuilder.datafile_template = queue.datafile_template || '{suggest}';
                
                // Parse items
                let items = queue.items || [];
                if (typeof items === 'string') {
                    try {
                        items = JSON.parse(items);
                    } catch (e) {
                        items = [];
                    }
                }
                this.queueBuilder.items = Array.isArray(items) ? items : [];
            } catch (e) {
                console.error('Failed to load queue for editing:', e);
                this.emit('log', { message: `Failed to load queue: ${e.message}`, level: 'error' });
                return;
            }
        }
        
        // Create modal HTML
        const modal = document.createElement('div');
        modal.className = 'ess-modal-overlay';
        modal.id = 'ess-queue-builder-modal';
        modal.innerHTML = `
            <div class="ess-modal ess-queue-builder-modal">
                <div class="ess-modal-header">
                    <span class="ess-modal-title">${queueName ? `Edit: ${this.escapeHtml(queueName)}` : 'Create Queue'}</span>
                    <button class="ess-modal-close" id="ess-qb-close">&times;</button>
                </div>
                <div class="ess-modal-body">
                    <!-- Name & Description -->
                    <div class="ess-modal-section">
                        <label class="ess-modal-label">Name</label>
                        <input type="text" class="ess-modal-input" id="ess-qb-name" 
                               value="${this.escapeHtml(this.queueBuilder.name)}" 
                               placeholder="Queue name (required)">
                    </div>
                    <div class="ess-modal-section">
                        <label class="ess-modal-label">Description</label>
                        <input type="text" class="ess-modal-input" id="ess-qb-description" 
                               value="${this.escapeHtml(this.queueBuilder.description)}" 
                               placeholder="Optional description">
                    </div>
                    
                    <!-- Settings -->
                    <div class="ess-modal-section">
                        <label class="ess-modal-label">Settings</label>
                        <div class="ess-qb-settings">
                            <label class="ess-qb-checkbox">
                                <input type="checkbox" id="ess-qb-auto-start" ${this.queueBuilder.auto_start ? 'checked' : ''}>
                                Auto-start
                            </label>
                            <label class="ess-qb-checkbox">
                                <input type="checkbox" id="ess-qb-auto-advance" ${this.queueBuilder.auto_advance ? 'checked' : ''}>
                                Auto-advance
                            </label>
                            <label class="ess-qb-checkbox">
                                <input type="checkbox" id="ess-qb-auto-datafile" ${this.queueBuilder.auto_datafile ? 'checked' : ''}>
                                Auto-datafile
                            </label>
                        </div>
                        <div class="ess-qb-template-row">
                            <span class="ess-qb-template-label">Datafile template:</span>
                            <input type="text" class="ess-modal-input" id="ess-qb-template" 
                                   value="${this.escapeHtml(this.queueBuilder.datafile_template)}"
                                   placeholder="{suggest}">
                            <button type="button" class="ess-qb-help-btn" id="ess-qb-template-help">?</button>
                        </div>
                        <div class="ess-qb-template-preview" id="ess-qb-template-preview">
                            Preview: <span id="ess-qb-preview-text">--</span>
                        </div>
                        <div class="ess-qb-template-help-popup" id="ess-qb-help-popup">
                            <div class="ess-qb-help-title">Template Variables</div>
                            <div class="ess-qb-help-content">
                                <div class="ess-qb-help-item"><code>{suggest}</code> Use ESS default naming</div>
                                <div class="ess-qb-help-section">ESS State</div>
                                <div class="ess-qb-help-item"><code>{subject}</code> Current subject</div>
                                <div class="ess-qb-help-item"><code>{system}</code> System name</div>
                                <div class="ess-qb-help-item"><code>{protocol}</code> Protocol name</div>
                                <div class="ess-qb-help-item"><code>{variant}</code> Variant name</div>
                                <div class="ess-qb-help-item"><code>{config}</code> Config name (or short_name)</div>
                                <div class="ess-qb-help-section">Queue Info</div>
                                <div class="ess-qb-help-item"><code>{queue}</code> Queue name</div>
                                <div class="ess-qb-help-item"><code>{position}</code> Position in queue (0-based)</div>
                                <div class="ess-qb-help-item"><code>{run}</code> Run number within item (1-based)</div>
                                <div class="ess-qb-help-item"><code>{global}</code> Global run counter (0-based)</div>
                                <div class="ess-qb-help-section">Zero-Padding</div>
                                <div class="ess-qb-help-item"><code>{position:02}</code> → 00, 01, 02...</div>
                                <div class="ess-qb-help-item"><code>{global:03}</code> → 000, 001, 002...</div>
                                <div class="ess-qb-help-section">Date/Time</div>
                                <div class="ess-qb-help-item"><code>{date}</code> YYYYMMDD</div>
                                <div class="ess-qb-help-item"><code>{date_short}</code> YYMMDD</div>
                                <div class="ess-qb-help-item"><code>{time}</code> HHMMSS</div>
                                <div class="ess-qb-help-item"><code>{time_short}</code> HHMM</div>
                                <div class="ess-qb-help-item"><code>{timestamp}</code> Unix timestamp</div>
                            </div>
                        </div>
                    </div>
                    
                    <!-- Items -->
                    <div class="ess-modal-section">
                        <label class="ess-modal-label">Items</label>
                        <div class="ess-qb-items-list" id="ess-qb-items-list">
                            <!-- Items rendered here -->
                        </div>
                        <div class="ess-qb-add-row">
                            <select id="ess-qb-config-select" class="ess-qb-config-select">
                                <option value="">-- Select Config --</option>
                            </select>
                            <button class="ess-mini-btn" id="ess-qb-add-btn" disabled>+ Add</button>
                        </div>
                    </div>
                </div>
                <div class="ess-modal-footer">
                    ${queueName ? '<button class="ess-modal-btn delete" id="ess-qb-delete">Delete</button>' : ''}
                    <div class="ess-modal-footer-spacer"></div>
                    <button class="ess-modal-btn cancel" id="ess-qb-cancel">Cancel</button>
                    <button class="ess-modal-btn primary" id="ess-qb-save">Save</button>
                </div>
            </div>
        `;
        
        document.body.appendChild(modal);
        
        // Populate config dropdown
        this.populateQueueBuilderConfigSelect();
        
        // Render items
        this.renderQueueBuilderItems();
        
        // Bind events
        modal.querySelector('#ess-qb-close').addEventListener('click', () => this.closeQueueBuilderModal());
        modal.querySelector('#ess-qb-cancel').addEventListener('click', () => this.closeQueueBuilderModal());
        modal.querySelector('#ess-qb-save').addEventListener('click', () => this.queueBuilderSave());
        modal.querySelector('#ess-qb-add-btn').addEventListener('click', () => this.queueBuilderAddItem());
        
        // Enable/disable add button based on config selection
        const configSelect = modal.querySelector('#ess-qb-config-select');
        const addBtn = modal.querySelector('#ess-qb-add-btn');
        configSelect.addEventListener('change', () => {
            addBtn.disabled = !configSelect.value;
        });
        
        // Template help popup toggle
        const helpBtn = modal.querySelector('#ess-qb-template-help');
        const helpPopup = modal.querySelector('#ess-qb-help-popup');
        helpBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            const isVisible = helpPopup.classList.toggle('visible');
            if (isVisible) {
                // Position popup below the button
                const rect = helpBtn.getBoundingClientRect();
                helpPopup.style.top = (rect.bottom + 8) + 'px';
                helpPopup.style.left = Math.max(10, rect.right - 280) + 'px';
            }
        });
        
        // Close help popup when clicking elsewhere
        document.addEventListener('click', (e) => {
            if (!helpBtn.contains(e.target) && !helpPopup.contains(e.target)) {
                helpPopup.classList.remove('visible');
            }
        });
        
        // Template preview - update on input
        const templateInput = modal.querySelector('#ess-qb-template');
        templateInput.addEventListener('input', () => this.updateTemplatePreview());
        
        // Initial preview update
        this.updateTemplatePreview();
        
        if (queueName) {
            modal.querySelector('#ess-qb-delete').addEventListener('click', () => this.queueBuilderDelete());
        }
        
        // Close on overlay click
        modal.addEventListener('click', (e) => {
            if (e.target === modal) this.closeQueueBuilderModal();
        });
        
        // Focus name field
        modal.querySelector('#ess-qb-name').focus();
    }
    
    /**
     * Update the template preview with current values
     */
    updateTemplatePreview() {
        const templateInput = document.querySelector('#ess-qb-template');
        const previewText = document.querySelector('#ess-qb-preview-text');
        if (!templateInput || !previewText) return;
        
        const template = templateInput.value.trim();
        
        // Handle special {suggest} case
        if (template === '{suggest}' || template === '') {
            previewText.textContent = '(ESS default naming)';
            previewText.classList.remove('error');
            return;
        }
        
        // Get current ESS state for preview
        const subject = this.state.currentSubject || 'subject';
        const system = this.state.currentSystem || 'system';
        const protocol = this.state.currentProtocol || 'protocol';
        const variant = this.state.currentVariant || 'variant';
        
        // Use first item's config name or placeholder
        const configName = this.queueBuilder.items.length > 0 
            ? this.queueBuilder.items[0].config_name 
            : 'config';
        
        // Queue name from the form
        const queueName = document.querySelector('#ess-qb-name')?.value.trim() || 'queue';
        
        // Date/time values
        const now = new Date();
        const date = now.toISOString().slice(0, 10).replace(/-/g, '');
        const dateShort = date.slice(2);
        const time = now.toTimeString().slice(0, 8).replace(/:/g, '');
        const timeShort = time.slice(0, 4);
        const timestamp = Math.floor(now.getTime() / 1000);
        
        // Build substitution map for string variables
        const stringSubstitutions = {
            '{subject}': subject,
            '{system}': system,
            '{protocol}': protocol,
            '{variant}': variant,
            '{config}': configName,
            '{queue}': queueName,
            '{date}': date,
            '{date_short}': dateShort,
            '{time}': time,
            '{time_short}': timeShort,
            '{timestamp}': timestamp.toString()
        };
        
        // Numeric variables that support format specifiers
        const numericVars = {
            'position': 0,
            'run': 1,
            'global': 0
        };
        
        let preview = template;
        
        // Handle numeric variables with optional format specifier {var:NN}
        for (const [varName, value] of Object.entries(numericVars)) {
            // Handle formatted version {var:NN} - zero-padded to NN digits
            const formatRegex = new RegExp(`\\{${varName}:(\\d+)\\}`, 'g');
            preview = preview.replace(formatRegex, (match, width) => {
                return String(value).padStart(parseInt(width), '0');
            });
            // Handle plain version {var}
            preview = preview.split(`{${varName}}`).join(String(value));
        }
        
        // Apply string substitutions
        for (const [key, value] of Object.entries(stringSubstitutions)) {
            preview = preview.split(key).join(value);
        }
        
        // Check for unrecognized variables
        const remaining = preview.match(/\{[^}]+\}/g);
        if (remaining) {
            previewText.textContent = `Unknown: ${remaining.join(', ')}`;
            previewText.classList.add('error');
        } else {
            previewText.textContent = preview;
            previewText.classList.remove('error');
        }
    }
    
    /**
     * Populate the config select dropdown in the queue builder
     */
    populateQueueBuilderConfigSelect() {
        const select = document.querySelector('#ess-qb-config-select');
        if (!select) return;
        
        select.innerHTML = '<option value="">-- Select Config --</option>';
        
        // Use configs from state (non-archived only)
        const configs = this.state.configs.filter(c => !c.archived);
        configs.forEach(config => {
            const opt = document.createElement('option');
            opt.value = config.name;
            opt.textContent = config.name;
            select.appendChild(opt);
        });
    }
    
    /**
     * Render the items list in the queue builder
     */
    renderQueueBuilderItems() {
        const container = document.querySelector('#ess-qb-items-list');
        if (!container) return;
        
        const items = this.queueBuilder.items;
        
        if (items.length === 0) {
            container.innerHTML = '<div class="ess-qb-empty">No items - add configs below</div>';
            return;
        }
        
        container.innerHTML = items.map((item, idx) => `
            <div class="ess-qb-item" data-index="${idx}">
                <span class="ess-qb-item-pos">${idx + 1}</span>
                <span class="ess-qb-item-name">${this.escapeHtml(item.config_name)}</span>
                <div class="ess-qb-item-fields">
                    <label class="ess-qb-item-field">
                        <span>×</span>
                        <input type="number" class="ess-qb-repeat" value="${item.repeat_count || 1}" min="1" max="99" data-index="${idx}">
                    </label>
                    <label class="ess-qb-item-field">
                        <span>⏱</span>
                        <input type="number" class="ess-qb-pause" value="${item.pause_after || 0}" min="0" max="999" data-index="${idx}">
                        <span>s</span>
                    </label>
                </div>
                <div class="ess-qb-item-actions">
                    <button class="ess-qb-move-btn" data-index="${idx}" data-dir="up" ${idx === 0 ? 'disabled' : ''}>↑</button>
                    <button class="ess-qb-move-btn" data-index="${idx}" data-dir="down" ${idx === items.length - 1 ? 'disabled' : ''}>↓</button>
                    <button class="ess-qb-remove-btn" data-index="${idx}">×</button>
                </div>
            </div>
        `).join('');
        
        // Bind item events
        container.querySelectorAll('.ess-qb-repeat').forEach(input => {
            input.addEventListener('change', (e) => {
                const idx = parseInt(e.target.dataset.index);
                this.queueBuilder.items[idx].repeat_count = parseInt(e.target.value) || 1;
            });
        });
        
        container.querySelectorAll('.ess-qb-pause').forEach(input => {
            input.addEventListener('change', (e) => {
                const idx = parseInt(e.target.dataset.index);
                this.queueBuilder.items[idx].pause_after = parseInt(e.target.value) || 0;
            });
        });
        
        container.querySelectorAll('.ess-qb-move-btn').forEach(btn => {
            btn.addEventListener('click', (e) => {
                const idx = parseInt(e.target.dataset.index);
                const dir = e.target.dataset.dir;
                this.queueBuilderMoveItem(idx, dir);
            });
        });
        
        container.querySelectorAll('.ess-qb-remove-btn').forEach(btn => {
            btn.addEventListener('click', (e) => {
                const idx = parseInt(e.target.dataset.index);
                this.queueBuilderRemoveItem(idx);
            });
        });
    }
    
    /**
     * Add a config to the queue builder items
     */
    queueBuilderAddItem() {
        const select = document.querySelector('#ess-qb-config-select');
        const configName = select?.value;
        
        if (!configName) {
            this.emit('log', { message: 'Select a config to add', level: 'warning' });
            return;
        }
        
        this.queueBuilder.items.push({
            config_name: configName,
            repeat_count: 1,
            pause_after: 0,
            notes: ''
        });
        
        select.value = '';
        this.renderQueueBuilderItems();
    }
    
    /**
     * Remove an item from the queue builder
     */
    queueBuilderRemoveItem(index) {
        this.queueBuilder.items.splice(index, 1);
        this.renderQueueBuilderItems();
    }
    
    /**
     * Move an item up or down in the queue builder
     */
    queueBuilderMoveItem(index, direction) {
        const items = this.queueBuilder.items;
        const newIndex = direction === 'up' ? index - 1 : index + 1;
        
        if (newIndex < 0 || newIndex >= items.length) return;
        
        // Swap items
        [items[index], items[newIndex]] = [items[newIndex], items[index]];
        this.renderQueueBuilderItems();
    }
    
    /**
     * Save the queue (create or update)
     */
    async queueBuilderSave() {
        // Get values from form
        const name = document.querySelector('#ess-qb-name')?.value.trim();
        const description = document.querySelector('#ess-qb-description')?.value.trim() || '';
        const auto_start = document.querySelector('#ess-qb-auto-start')?.checked ? 1 : 0;
        const auto_advance = document.querySelector('#ess-qb-auto-advance')?.checked ? 1 : 0;
        const auto_datafile = document.querySelector('#ess-qb-auto-datafile')?.checked ? 1 : 0;
        const datafile_template = document.querySelector('#ess-qb-template')?.value.trim() || '{suggest}';
        
        console.log('queueBuilderSave - items to save:', this.queueBuilder.items);
        
        // Validate
        if (!name) {
            this.emit('log', { message: 'Queue name is required', level: 'error' });
            document.querySelector('#ess-qb-name')?.focus();
            return;
        }
        
        if (this.queueBuilder.items.length === 0) {
            this.emit('log', { message: 'Queue must have at least one item', level: 'error' });
            return;
        }
        
        try {
            if (this.queueBuilder.isEditing) {
                // Update existing queue
                const originalName = this.queueBuilder.originalName;
                const isRenaming = name !== originalName;
                
                // Clear items first (using original name, before any rename)
                console.log(`Clearing items from queue: ${originalName}`);
                await this.sendConfigCommandAsync(`queue_clear {${originalName}}`);
                
                // Update settings (including rename if applicable)
                let updateCmd = `queue_update {${originalName}}`;
                if (isRenaming) {
                    updateCmd += ` -name {${name}}`;
                }
                updateCmd += ` -description {${description}}`;
                updateCmd += ` -auto_start ${auto_start}`;
                updateCmd += ` -auto_advance ${auto_advance}`;
                updateCmd += ` -auto_datafile ${auto_datafile}`;
                updateCmd += ` -datafile_template {${datafile_template}}`;
                
                console.log('Update command:', updateCmd);
                await this.sendConfigCommandAsync(updateCmd);
                
                // Add items (using new name if renamed)
                for (const item of this.queueBuilder.items) {
                    let addCmd = `queue_add {${name}} {${item.config_name}}`;
                    if (item.repeat_count > 1) addCmd += ` -repeat ${item.repeat_count}`;
                    if (item.pause_after > 0) addCmd += ` -pause_after ${item.pause_after}`;
                    if (item.notes) addCmd += ` -notes {${item.notes}}`;
                    console.log('Add item command:', addCmd);
                    await this.sendConfigCommandAsync(addCmd);
                }
                
                this.emit('log', { message: `Queue "${name}" updated`, level: 'info' });
            } else {
                // Create new queue
                let createCmd = `queue_create {${name}}`;
                createCmd += ` -description {${description}}`;
                createCmd += ` -auto_start ${auto_start}`;
                createCmd += ` -auto_advance ${auto_advance}`;
                createCmd += ` -auto_datafile ${auto_datafile}`;
                createCmd += ` -datafile_template {${datafile_template}}`;
                
                await this.sendConfigCommandAsync(createCmd);
                
                // Add items
                for (const item of this.queueBuilder.items) {
                    let addCmd = `queue_add {${name}} {${item.config_name}}`;
                    if (item.repeat_count > 1) addCmd += ` -repeat ${item.repeat_count}`;
                    if (item.pause_after > 0) addCmd += ` -pause_after ${item.pause_after}`;
                    if (item.notes) addCmd += ` -notes {${item.notes}}`;
                    await this.sendConfigCommandAsync(addCmd);
                }
                
                this.emit('log', { message: `Queue "${name}" created`, level: 'info' });
            }
            
            // Refresh queue list and close modal
            this.refreshQueueList();
            this.state.selectedQueue = name;
            this.closeQueueBuilderModal();
            
            // Reload the newly saved queue
            setTimeout(() => this.selectQueue(name), 100);
            
        } catch (e) {
            console.error('Failed to save queue:', e);
            this.emit('log', { message: `Failed to save queue: ${e.message}`, level: 'error' });
        }
    }
    
    /**
     * Delete the queue being edited
     */
    async queueBuilderDelete() {
        const name = this.queueBuilder.originalName;
        if (!name) return;
        
        // Confirm deletion
        if (!confirm(`Delete queue "${name}"? This cannot be undone.`)) {
            return;
        }
        
        try {
            await this.sendConfigCommandAsync(`queue_delete {${name}}`);
            this.emit('log', { message: `Queue "${name}" deleted`, level: 'info' });
            
            // Clear selection if this was selected
            if (this.state.selectedQueue === name) {
                this.state.selectedQueue = '';
                this.state.queueItems = [];
            }
            
            this.refreshQueueList();
            this.closeQueueBuilderModal();
            this.renderQueuePlaylist();
            
        } catch (e) {
            console.error('Failed to delete queue:', e);
            this.emit('log', { message: `Failed to delete queue: ${e.message}`, level: 'error' });
        }
    }
    
    /**
     * Close the queue builder modal
     */
    closeQueueBuilderModal() {
        const modal = document.querySelector('#ess-queue-builder-modal');
        if (modal) {
            modal.remove();
        }
        this.queueBuilder = null;
    }
    
    // =========================================================================
    // SECTION 11: QUEUE IMPORT/EXPORT
    // =========================================================================
    
    /**
     * Show queue import dialog - select peer and queue to import
     */
    async showQueueImportDialog() {
        const peers = this.getMeshPeers();
        const recentServers = this.getRemoteServers();
        
        const modal = document.createElement('div');
        modal.className = 'ess-modal-overlay';
        modal.innerHTML = `
            <div class="ess-modal">
                <div class="ess-modal-header">
                    <span class="ess-modal-title">Import Queue</span>
                    <button class="ess-modal-close">×</button>
                </div>
                <div class="ess-modal-body">
                    <div class="ess-modal-section">
                        <label class="ess-modal-label">Import from:</label>
                        <select class="ess-modal-select" id="ess-queue-import-peer-select">
                            <option value="">-- Select source --</option>
                            <option value="__custom__">Enter IP address...</option>
                            ${recentServers.length > 0 ? `<optgroup label="Recent Servers">
                                ${recentServers.map(s => `<option value="${this.escapeAttr(s.address)}">${this.escapeHtml(s.label)}</option>`).join('')}
                            </optgroup>` : ''}
                            ${peers.length > 0 ? `<optgroup label="Mesh Peers">
                                ${peers.map(p => `<option value="${p.ipAddress}">${this.escapeHtml(p.name)} (${p.ipAddress})</option>`).join('')}
                            </optgroup>` : ''}
                        </select>
                    </div>
                    <div class="ess-modal-section" id="ess-queue-import-custom-ip-section" style="display: none;">
                        <label class="ess-modal-label">Remote server address:</label>
                        <input type="text" class="ess-modal-input" id="ess-queue-import-custom-ip" 
                               placeholder="hostname or IP address">
                    </div>
                    <div class="ess-modal-section" id="ess-queue-import-list-section" style="display: none;">
                        <label class="ess-modal-label">Select queue to import:</label>
                        <div class="ess-modal-config-list" id="ess-queue-import-list">
                            <!-- Populated dynamically -->
                        </div>
                    </div>
                    <div class="ess-modal-section" id="ess-queue-import-options-section" style="display: none;">
                        <label class="ess-qb-checkbox">
                            <input type="checkbox" id="ess-queue-import-configs" checked>
                            Also import referenced configs (if missing)
                        </label>
                    </div>
                </div>
                <div class="ess-modal-footer">
                    <button class="ess-modal-btn cancel" id="ess-queue-import-cancel">Cancel</button>
                    <button class="ess-modal-btn primary" id="ess-queue-import-confirm" disabled>Import</button>
                </div>
            </div>
        `;
        
        document.body.appendChild(modal);
        
        const peerSelect = modal.querySelector('#ess-queue-import-peer-select');
        const customIpSection = modal.querySelector('#ess-queue-import-custom-ip-section');
        const customIpInput = modal.querySelector('#ess-queue-import-custom-ip');
        const listSection = modal.querySelector('#ess-queue-import-list-section');
        const queueList = modal.querySelector('#ess-queue-import-list');
        const optionsSection = modal.querySelector('#ess-queue-import-options-section');
        const confirmBtn = modal.querySelector('#ess-queue-import-confirm');
        const cancelBtn = modal.querySelector('#ess-queue-import-cancel');
        const closeBtn = modal.querySelector('.ess-modal-close');
        
        let currentIp = '';
        let selectedQueue = '';
        
        const loadQueuesFromPeer = async (ip) => {
            currentIp = ip;
            queueList.innerHTML = '<div class="ess-loading">Loading queues...</div>';
            listSection.style.display = 'block';
            optionsSection.style.display = 'none';
            confirmBtn.disabled = true;
            selectedQueue = '';
            
            try {
                const response = await this.dpManager.connection.sendRaw(
                    `remoteSend ${ip} {send configs {queue_list}}`
                );
                
                // Parse the response - it's a Tcl list of dicts
                let queues = [];
                if (response) {
                    queues = TclParser.parseList(response).map(q => {
                        if (typeof q === 'string') {
                            return TclParser.parseDict(q);
                        }
                        return q;
                    });
                }
                
                if (queues.length === 0) {
                    queueList.innerHTML = '<div class="ess-empty">No queues found</div>';
                    return;
                }
                
                queueList.innerHTML = queues.map(q => `
                    <div class="ess-import-queue-item" data-name="${this.escapeAttr(q.name)}">
                        <span class="ess-import-queue-name">${this.escapeHtml(q.name)}</span>
                        <span class="ess-import-queue-count">${q.item_count || 0} items</span>
                    </div>
                `).join('');
                
                // Add click handlers for selection
                queueList.querySelectorAll('.ess-import-queue-item').forEach(item => {
                    item.addEventListener('click', () => {
                        queueList.querySelectorAll('.ess-import-queue-item').forEach(i => i.classList.remove('selected'));
                        item.classList.add('selected');
                        selectedQueue = item.dataset.name;
                        optionsSection.style.display = 'block';
                        confirmBtn.disabled = false;
                    });
                });
                
            } catch (e) {
                console.error('Failed to load queues from peer:', e);
                queueList.innerHTML = `<div class="ess-error">Failed to connect: ${e.message}</div>`;
            }
        };
        
        peerSelect.addEventListener('change', () => {
            if (peerSelect.value === '__custom__') {
                customIpSection.style.display = 'block';
                listSection.style.display = 'none';
                optionsSection.style.display = 'none';
                customIpInput.focus();
            } else if (peerSelect.value) {
                customIpSection.style.display = 'none';
                loadQueuesFromPeer(peerSelect.value);
            } else {
                customIpSection.style.display = 'none';
                listSection.style.display = 'none';
                optionsSection.style.display = 'none';
            }
        });
        
        customIpInput.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' && customIpInput.value.trim()) {
                this.addRemoteServer(customIpInput.value.trim());
                loadQueuesFromPeer(customIpInput.value.trim());
            }
        });
        
        confirmBtn.addEventListener('click', async () => {
            if (!selectedQueue || !currentIp) return;
            
            const includeConfigs = modal.querySelector('#ess-queue-import-configs').checked;
            
            confirmBtn.disabled = true;
            confirmBtn.textContent = 'Importing...';
            
            try {
                // Get queue export JSON from remote
                const exportFlag = includeConfigs ? ' -include_configs' : '';
                const exportJson = await this.dpManager.connection.sendRaw(
                    `remoteSend ${currentIp} {send configs {queue_export {${selectedQueue}}${exportFlag}}}`
                );
                
                // Import locally
                await this.sendConfigCommandAsync(`queue_import {${exportJson}} -skip_existing_configs -overwrite_queue`);
                
                modal.remove();
                this.emit('log', { message: `Imported queue "${selectedQueue}" from ${currentIp}`, level: 'info' });
                this.refreshQueueList();
                
                // Select the imported queue
                setTimeout(() => this.selectQueue(selectedQueue), 100);
                
            } catch (e) {
                modal.remove();
                this.emit('log', { message: `Import failed: ${e.message}`, level: 'error' });
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
     * Show queue push dialog - select destination peer
     */
    async showQueuePushDialog(queueName) {
        const peers = this.getMeshPeers();
        const recentServers = this.getRemoteServers();
        
        const modal = document.createElement('div');
        modal.className = 'ess-modal-overlay';
        modal.innerHTML = `
            <div class="ess-modal">
                <div class="ess-modal-header">
                    <span class="ess-modal-title">Push Queue</span>
                    <button class="ess-modal-close">×</button>
                </div>
                <div class="ess-modal-body">
                    <div class="ess-modal-section">
                        <label class="ess-modal-label">Queue:</label>
                        <div class="ess-modal-value">${this.escapeHtml(queueName)}</div>
                    </div>
                    <div class="ess-modal-section">
                        <label class="ess-modal-label">Push to:</label>
                        <select class="ess-modal-select" id="ess-queue-push-peer-select">
                            <option value="">-- Select destination --</option>
                            <option value="__custom__">Enter IP address...</option>
                            ${recentServers.length > 0 ? `<optgroup label="Recent Servers">
                                ${recentServers.map(s => `<option value="${this.escapeAttr(s.address)}">${this.escapeHtml(s.label)}</option>`).join('')}
                            </optgroup>` : ''}
                            ${peers.length > 0 ? `<optgroup label="Mesh Peers">
                                ${peers.map(p => `<option value="${p.ipAddress}">${this.escapeHtml(p.name)} (${p.ipAddress})</option>`).join('')}
                            </optgroup>` : ''}
                        </select>
                    </div>
                    <div class="ess-modal-section" id="ess-queue-push-custom-ip-section" style="display: none;">
                        <label class="ess-modal-label">Remote server address:</label>
                        <input type="text" class="ess-modal-input" id="ess-queue-push-custom-ip" 
                               placeholder="hostname or IP address">
                    </div>
                    <div class="ess-modal-section">
                        <label class="ess-qb-checkbox">
                            <input type="checkbox" id="ess-queue-push-configs" checked>
                            Include referenced configs
                        </label>
                        <label class="ess-qb-checkbox">
                            <input type="checkbox" id="ess-queue-push-overwrite">
                            Overwrite if queue exists on destination
                        </label>
                    </div>
                </div>
                <div class="ess-modal-footer">
                    <button class="ess-modal-btn cancel" id="ess-queue-push-cancel">Cancel</button>
                    <button class="ess-modal-btn primary" id="ess-queue-push-confirm" disabled>Push</button>
                </div>
            </div>
        `;
        
        document.body.appendChild(modal);
        
        const peerSelect = modal.querySelector('#ess-queue-push-peer-select');
        const customIpSection = modal.querySelector('#ess-queue-push-custom-ip-section');
        const customIpInput = modal.querySelector('#ess-queue-push-custom-ip');
        const confirmBtn = modal.querySelector('#ess-queue-push-confirm');
        const cancelBtn = modal.querySelector('#ess-queue-push-cancel');
        const closeBtn = modal.querySelector('.ess-modal-close');
        
        const getTargetIp = () => {
            if (peerSelect.value === '__custom__') {
                return customIpInput.value.trim();
            }
            return peerSelect.value;
        };
        
        const updateConfirmState = () => {
            confirmBtn.disabled = !getTargetIp();
        };
        
        peerSelect.addEventListener('change', () => {
            if (peerSelect.value === '__custom__') {
                customIpSection.style.display = 'block';
                customIpInput.focus();
                confirmBtn.disabled = true;
            } else {
                customIpSection.style.display = 'none';
                updateConfirmState();
            }
        });
        
        customIpInput.addEventListener('input', updateConfirmState);
        
        confirmBtn.addEventListener('click', async () => {
            const ip = getTargetIp();
            if (!ip) return;
            
            // Save custom IP if used
            if (peerSelect.value === '__custom__' && customIpInput.value.trim()) {
                this.addRemoteServer(customIpInput.value.trim());
            }
            
            const includeConfigs = modal.querySelector('#ess-queue-push-configs').checked;
            const overwrite = modal.querySelector('#ess-queue-push-overwrite').checked;
            
            confirmBtn.disabled = true;
            confirmBtn.textContent = 'Pushing...';
            
            try {
                // Get queue export JSON locally
                const exportFlag = includeConfigs ? ' -include_configs' : '';
                const exportJson = await this.sendConfigCommandAsync(`queue_export {${queueName}}${exportFlag}`);
                
                // Build import flags
                let importFlags = ' -skip_existing_configs';
                if (overwrite) {
                    importFlags += ' -overwrite_queue';
                }
                
                // Send to remote
                await this.dpManager.connection.sendRaw(
                    `remoteSend ${ip} {send configs {queue_import {${exportJson}}${importFlags}}}`
                );
                
                modal.remove();
                this.emit('log', { message: `Pushed queue "${queueName}" to ${ip}`, level: 'info' });
                
            } catch (e) {
                modal.remove();
                this.emit('log', { message: `Push failed: ${e.message}`, level: 'error' });
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
}

// Export
if (typeof window !== 'undefined') {
    window.ESSControl = ESSControl;
}
