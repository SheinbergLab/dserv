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
 *   - Run tab: Session/run management with Start Run, Close Run controls
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
 * SECTION 9: RUN TAB - Session Management
 *   - updateQueueList(), updateQueueState(), updateQueueItems()
 *   - renderQueueSelect(), renderQueueStatus(), renderQueuePlaylist()
 *   - updateQueueControls(), updateConfigPlayButtons()
 *   - Run Commands: startRun(), closeRun(), resetSession()
 * 
 * SECTION 10: QUEUE BUILDER MODAL
 *   - showQueueBuilderModal(), queueBuilderSave()
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
            essRunning: false,     // true when ESS state machine is actively running
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

            // Project filtering state
	    activeProject: '',
	    activeProjectDetail: null,
	    
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
                    <div class="ess-file-actions">
                        <button id="ess-file-menu-btn" class="ess-file-menu-btn" title="File actions">⋮</button>
                        <div class="ess-file-menu" id="ess-file-menu">
                            <button id="ess-btn-file-open" class="ess-file-menu-action">Open File</button>
                            <button id="ess-btn-file-close" class="ess-file-menu-action" style="display: none;">Close File</button>
                        </div>
                    </div>
                </div>
            </div>
            
            <!-- Scrollable Middle: Tabbed Section -->
            <div class="ess-control-section ess-tabbed-section">
                <!-- Tab Headers (fixed, outside scrollable area) -->
                <div class="ess-tab-header">
                    <button class="ess-tab-btn active" data-tab="setup">Setup</button>
                    <button class="ess-tab-btn" data-tab="configs">Configs</button>
                    <button class="ess-tab-btn" data-tab="queue">Queues</button>
                </div>
                
                <!-- Scrollable Tab Content -->
                <div class="ess-control-scrollable">
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
                    <!-- No Project Overlay -->
                    <div class="ess-no-project-overlay" id="ess-configs-no-project">
                        <div class="ess-no-project-icon">📁</div>
                        <div class="ess-no-project-title">No Project Selected</div>
                        <div class="ess-no-project-message">
                            Select a project from the dropdown above, or click <strong>+</strong> to create one.
                        </div>
                    </div>
                    
                    <!-- List View (default) -->
                    <div class="ess-config-list-view" id="ess-config-list-view">
                        <!-- Search Row -->
                        <div class="ess-config-search-row">
                            <button id="ess-config-trash-back" class="ess-config-trash-back" style="display: none;">← Back</button>
                            <input type="text" id="ess-config-search" class="ess-config-search" 
                                   placeholder="Search configs...">
                            <div class="ess-config-menu-wrapper">
                                <button id="ess-config-menu-btn" class="ess-config-menu-btn" title="Config actions">⋮</button>
                                <div class="ess-config-menu" id="ess-config-menu">
                                    <button class="ess-config-menu-action" data-action="trash">
                                        🗑 View Trash <span id="ess-config-trash-count"></span>
                                    </button>
                                </div>
                            </div>
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
                            
                            <!-- File Template (NEW - moved from queue) -->
                            <div class="ess-edit-row">
                                <label class="ess-edit-label">File Template</label>
                                <div class="ess-edit-template-wrapper">
                                    <input type="text" id="ess-edit-file-template" class="ess-edit-input" 
                                           placeholder="e.g. {subject}_{config}_{date}">
                                    <button type="button" class="ess-edit-template-help-btn" id="ess-edit-template-help">?</button>
                                </div>
                                <div class="ess-edit-template-preview" id="ess-edit-template-preview">
                                    Preview: <span id="ess-edit-template-preview-text">(default)</span>
                                </div>
                                <div class="ess-edit-template-help-popup" id="ess-edit-template-help-popup">
                                    <div class="ess-edit-help-title">Template Variables <span class="ess-edit-help-hint">(click to insert)</span></div>
                                    <div class="ess-edit-help-grid">
                                        <span class="ess-edit-help-var" data-var="{subject}">{subject}</span>
                                        <span class="ess-edit-help-var" data-var="{config}">{config}</span>
                                        <span class="ess-edit-help-var" data-var="{system}">{system}</span>
                                        <span class="ess-edit-help-var" data-var="{protocol}">{protocol}</span>
                                        <span class="ess-edit-help-var" data-var="{variant}">{variant}</span>
                                        <span class="ess-edit-help-var" data-var="{project}">{project}</span>
                                        <span class="ess-edit-help-var" data-var="{date}">{date}</span>
                                        <span class="ess-edit-help-var" data-var="{date_short}">{date_short}</span>
                                        <span class="ess-edit-help-var" data-var="{time}">{time}</span>
                                        <span class="ess-edit-help-var" data-var="{time_short}">{time_short}</span>
                                    </div>
                                    <div class="ess-edit-help-note">Leave empty for system default naming</div>
                                </div>
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
                </div><!-- /.ess-tab-configs -->
                
                <!-- Run Tab Content (internally still uses queue infrastructure) -->
                <div class="ess-tab-content" id="ess-tab-queue">
                    <!-- No Project Overlay -->
                    <div class="ess-no-project-overlay" id="ess-queues-no-project">
                        <div class="ess-no-project-icon">📁</div>
                        <div class="ess-no-project-title">No Project Selected</div>
                        <div class="ess-no-project-message">
                            Select a project from the dropdown above, or click <strong>+</strong> to create one.
                        </div>
                    </div>
                    
                    <!-- Queue Selection Row -->
                    <div class="ess-queue-select-row">
                        <select id="ess-queue-select" class="ess-queue-select">
                            <option value="">-- Select Queue --</option>
                        </select>
                        <button id="ess-queue-reset" class="ess-reload-btn" title="Reset queue to beginning" disabled>↻</button>
                        <div class="ess-queue-menu-wrapper">
                            <button id="ess-queue-menu-btn" class="ess-queue-menu-btn" title="Queue actions">⋮</button>
                            <div class="ess-queue-menu" id="ess-queue-menu">
                                <button class="ess-queue-menu-action" data-action="edit">Edit</button>
                                <button class="ess-queue-menu-action" data-action="new">New</button>
                                <div class="ess-queue-menu-divider"></div>
                                <button class="ess-queue-menu-action" data-action="delete">Delete</button>
                            </div>
                        </div>
                    </div>
                    
                    <!-- Run Status Display -->
                    <div class="ess-queue-status" id="ess-queue-status">
                        <div class="ess-queue-status-row">
                            <span class="ess-queue-status-label">Status:</span>
                            <span class="ess-queue-status-value" id="ess-queue-status-text">idle</span>
                        </div>
                        <div class="ess-queue-status-row" id="ess-queue-progress-row">
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
                    
                    <!-- Run Controls -->
                    <div class="ess-queue-controls">
                        <button id="ess-queue-start-run" class="ess-state-btn go" disabled>▶ Start Run</button>
                        <button id="ess-queue-close-run" class="ess-state-btn stop" disabled>Close Run</button>
                    </div>
                    
                    <!-- Config List (shown when multiple configs) -->
                    <div class="ess-queue-playlist-header" id="ess-queue-playlist-header">
                        <span class="ess-queue-playlist-title">Configs</span>
                        <span class="ess-queue-playlist-count" id="ess-queue-item-count">0 items</span>
                    </div>
                    <div class="ess-queue-playlist" id="ess-queue-playlist">
                        <div class="ess-queue-empty">No queue selected</div>
                    </div>
                </div>
                </div><!-- /.ess-control-scrollable -->
            </div><!-- /.ess-tabbed-section -->
            
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
            fileMenuBtn: this.container.querySelector('#ess-file-menu-btn'),
            fileMenu: this.container.querySelector('#ess-file-menu'),
            currentFile: this.container.querySelector('#ess-current-file'),
            
            // Configs tab elements
            configSearch: this.container.querySelector('#ess-config-search'),
            configTagsRow: this.container.querySelector('#ess-config-tags-row'),
            configTagsList: this.container.querySelector('#ess-config-tags-list'),
            configMenuBtn: this.container.querySelector('#ess-config-menu-btn'),
            configMenu: this.container.querySelector('#ess-config-menu'),
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
            configsNoProject: this.container.querySelector('#ess-configs-no-project'),
            
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
            editCreated: this.container.querySelector('#ess-edit-created'),

	    // Edit form elements
            editFileTemplate: this.container.querySelector('#ess-edit-file-template'),
            editFileTemplatePreview: this.container.querySelector('#ess-edit-template-preview-text'),
            editFileTemplateHelp: this.container.querySelector('#ess-edit-template-help'),
            editFileTemplateHelpPopup: this.container.querySelector('#ess-edit-template-help-popup'),

            // Queue tab elements
            queuesNoProject: this.container.querySelector('#ess-queues-no-project'),
            queueSelect: this.container.querySelector('#ess-queue-select'),
            queueMenuBtn: this.container.querySelector('#ess-queue-menu-btn'),
            queueMenu: this.container.querySelector('#ess-queue-menu'),
            queueStatusText: this.container.querySelector('#ess-queue-status-text'),
            queueProgressRow: this.container.querySelector('#ess-queue-progress-row'),
            queueProgress: this.container.querySelector('#ess-queue-progress'),
            queueRunRow: this.container.querySelector('#ess-queue-run-row'),
            queueRunCount: this.container.querySelector('#ess-queue-run-count'),
            queueCountdownRow: this.container.querySelector('#ess-queue-countdown-row'),
            queueCountdown: this.container.querySelector('#ess-queue-countdown'),
            queueResetBtn: this.container.querySelector('#ess-queue-reset'),
            queueBtnStartRun: this.container.querySelector('#ess-queue-start-run'),
            queueBtnCloseRun: this.container.querySelector('#ess-queue-close-run'),
            queuePlaylistHeader: this.container.querySelector('#ess-queue-playlist-header'),
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
        this.elements.btnFileOpen.addEventListener('click', () => {
            this.openDatafile();
            this.elements.fileMenu.classList.remove('open');
        });
        this.elements.btnFileClose.addEventListener('click', () => {
            this.closeDatafile();
            this.elements.fileMenu.classList.remove('open');
        });
        this.elements.fileMenuBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            this.elements.fileMenu.classList.toggle('open');
        });
        // Close file menu on outside click
        document.addEventListener('click', () => {
            this.elements.fileMenu?.classList.remove('open');
        });
        
        // Configs tab - Search
        this.elements.configSearch.addEventListener('input', (e) => {
            this.state.configSearch = e.target.value;
            this.renderConfigList();
        });
        
        // Config menu button
        this.elements.configMenuBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            this.toggleConfigMenu();
        });
        
        // Config menu actions
        this.elements.configMenu.querySelectorAll('.ess-config-menu-action').forEach(btn => {
            btn.addEventListener('click', (e) => {
                e.stopPropagation();
                const action = btn.dataset.action;
                this.closeConfigMenu();
                
                switch (action) {
                    case 'trash':
                        this.toggleTrashView();
                        break;
                }
            });
        });
        
        // Close config menu when clicking elsewhere
        document.addEventListener('click', () => this.closeConfigMenu());
        
        // Trash back button
        this.elements.configTrashBack.addEventListener('click', () => {
            this.toggleTrashView();  // Toggle back to normal view
        });
        
        // Config New button - opens edit view in create mode
        this.elements.btnConfigNew.addEventListener('click', () => this.startCreateConfig());
        
        // Config Save/Cancel buttons in status bar (for edit mode)
        this.elements.btnConfigSaveEdit.addEventListener('click', () => this.saveEdit());
        this.elements.btnConfigCancelEdit.addEventListener('click', () => this.cancelEdit());
        
        // Edit form - back button
        this.elements.editBack.addEventListener('click', () => this.cancelEdit());

        // File template help popup
        this.elements.editFileTemplateHelp?.addEventListener('click', (e) => {
            e.stopPropagation();
            this.elements.editFileTemplateHelpPopup?.classList.toggle('visible');
        });
        
        // Close help popup when clicking elsewhere
        document.addEventListener('click', (e) => {
            if (this.elements.editFileTemplateHelpPopup && 
                !this.elements.editFileTemplateHelp?.contains(e.target) && 
                !this.elements.editFileTemplateHelpPopup.contains(e.target)) {
                this.elements.editFileTemplateHelpPopup.classList.remove('visible');
            }
        });
        
        // Click variable to insert into template
        this.container.querySelectorAll('.ess-edit-help-var').forEach(el => {
            el.addEventListener('click', () => {
                const varText = el.dataset.var;
                if (this.elements.editFileTemplate) {
                    this.insertAtCursor(this.elements.editFileTemplate, varText);
                    this.updateEditTemplatePreview();
                }
                this.elements.editFileTemplateHelpPopup?.classList.remove('visible');
            });
        });
        
        // Update preview on template input
        this.elements.editFileTemplate?.addEventListener('input', () => {
            this.updateEditTemplatePreview();
        });
	
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
                case 'delete':
                    if (this.state.selectedQueue) {
                        this.showDeleteQueueDialog(this.state.selectedQueue);
                    }
                    break;
		}
            });
        });
        
        // Close menu when clicking elsewhere
        document.addEventListener('click', () => this.closeQueueMenu());
        
        // Run controls
        this.elements.queueResetBtn.addEventListener('click', () => this.resetSession());
        this.elements.queueBtnStartRun.addEventListener('click', () => this.startRun());
        this.elements.queueBtnCloseRun.addEventListener('click', () => this.closeRun());
        
        // Initial state for no-project overlays
        this.updateNoProjectOverlays();
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
     * Toggle config menu visibility
     */
    toggleConfigMenu() {
        this.elements.configMenu.classList.toggle('open');
    }
    
    /**
     * Close config menu
     */
    closeConfigMenu() {
        this.elements.configMenu.classList.remove('open');
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
                case 'delete':
                    btn.disabled = !hasQueue || isActive;
                    break;
                // 'new' is always enabled
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

	// Project subscriptions for filtering
        this.dpManager.subscribe('projects/active', (data) => {
            this.updateActiveProject(data.value);
        });
        
        this.dpManager.subscribe('projects/active_detail', (data) => {
            this.updateActiveProjectDetail(data.value);
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
        
        // If project is active, mark systems that are in the project
        if (this.state.activeProject && this.state.activeProjectDetail) {
            const projectSystems = this.state.activeProjectDetail.systems || [];
            this.populateSelectWithProjectIndicator(
                this.elements.systemSelect, 
                systems, 
                '-- Select System --',
                projectSystems
            );
        } else {
            this.populateSelect(this.elements.systemSelect, systems, '-- Select System --');
        }
        
        if (this.state.currentSystem) {
            this.updateSelectValue(this.elements.systemSelect, this.state.currentSystem);
        }
    }

  /**
     * Populate select with project membership indicator
     * Systems in project shown normally, others shown grayed with indicator
     */
    populateSelectWithProjectIndicator(select, items, placeholder, projectItems) {
        select.innerHTML = '';
        
        if (placeholder) {
            const opt = document.createElement('option');
            opt.value = '';
            opt.textContent = placeholder;
            select.appendChild(opt);
        }
        
        // Separate into in-project and not-in-project
        const inProject = items.filter(item => projectItems.includes(item));
        const notInProject = items.filter(item => !projectItems.includes(item));
        
        // Add in-project items first
        inProject.forEach(item => {
            const opt = document.createElement('option');
            opt.value = item;
            opt.textContent = item;
            select.appendChild(opt);
        });
        
        // Add separator and not-in-project items if any
        if (notInProject.length > 0 && inProject.length > 0) {
            const sep = document.createElement('option');
            sep.disabled = true;
            sep.textContent = '── other systems ──';
            select.appendChild(sep);
        }
        
        notInProject.forEach(item => {
            const opt = document.createElement('option');
            opt.value = item;
            opt.textContent = `${item} ○`;  // Circle indicates not in project
            opt.style.color = '#666';
            select.appendChild(opt);
        });
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
        this.state.essRunning = (status === 'running');
        const canChangeConfig = (status === 'stopped');
        this.setControlsEnabled(canChangeConfig);
        
        if (status === 'loading') {
            this.container.classList.add('ess-loading');
        } else {
            this.container.classList.remove('ess-loading');
        }
        
        this.emit('stateChange', { state: status });
        this.updateButtonStates();
        
        // Update queue controls and status display when ESS state changes
        this.updateQueueControls();
        this.renderQueueStatus();
        this.updateConfigRunButtons();
    }    

/**
     * Update config run button appearance based on queue state
     */
updateConfigRunButtons() {
        const currentConfig = this.state.queueState?.current_config || '';
        const queueStatus = this.state.queueState?.status || 'idle';
        
        this.container.querySelectorAll('.ess-config-item').forEach(item => {
            const btn = item.querySelector('.ess-config-item-run');
            const loadBtn = item.querySelector('.ess-config-item-load');
            if (!btn) return;
            
            const configName = item.dataset.name;
            const isThisConfig = configName === currentConfig;
            const isActive = isThisConfig && queueStatus !== 'idle';
            
            if (isActive) {
                btn.textContent = '✕';
                btn.title = 'Close';
                btn.classList.add('closeable');
            } else {
                btn.textContent = '▶';
                btn.title = 'Load, open file, and start';
                btn.classList.remove('closeable');
            }
            
            // Disable while ESS is running
            btn.disabled = this.state.essRunning;
            if (loadBtn) {
                loadBtn.disabled = this.state.essRunning;
            }
        });
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
        this.elements.configTrashCount.textContent = count > 0 ? `(${count})` : '';
    }
    
    toggleTrashView() {
        this.state.showingTrash = !this.state.showingTrash;
        this.elements.configList.classList.toggle('trash-view', this.state.showingTrash);
        
        // Show/hide back button and menu button
        this.elements.configTrashBack.style.display = this.state.showingTrash ? '' : 'none';
        this.elements.configMenuBtn.parentElement.style.display = this.state.showingTrash ? 'none' : '';
        
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

   /**
     * Update active project name
     */
    updateActiveProject(data) {
        this.state.activeProject = data || '';
        // Update no-project overlay visibility
        this.updateNoProjectOverlays();
        // Re-render lists to apply filter
        this.renderConfigList();
        this.renderQueueSelect();
    }
    
    /**
     * Show/hide the "no project" overlays based on active project state
     */
    updateNoProjectOverlays() {
        const hasProject = !!this.state.activeProject;
        
        // Configs tab overlay
        if (this.elements.configsNoProject) {
            this.elements.configsNoProject.style.display = hasProject ? 'none' : 'flex';
        }
        if (this.elements.configListView) {
            this.elements.configListView.style.display = hasProject ? '' : 'none';
        }
        
        // Queues tab overlay
        if (this.elements.queuesNoProject) {
            this.elements.queuesNoProject.style.display = hasProject ? 'none' : 'flex';
        }
        // Hide all queue content when no project
        const queueContent = this.container.querySelectorAll('#ess-tab-queue > :not(.ess-no-project-overlay)');
        queueContent.forEach(el => {
            el.style.display = hasProject ? '' : 'none';
        });
    }
    
    /**
     * Update active project detail (with configs/queues membership lists)
     */
    updateActiveProjectDetail(data) {
        try {
            if (!data || data === '{}') {
                this.state.activeProjectDetail = null;
            } else {
                const detail = typeof data === 'string' ? JSON.parse(data) : data;
                this.state.activeProjectDetail = detail;
            }
            // Re-render lists with new filter data
            this.renderConfigList();
            this.renderQueueSelect();
        } catch (e) {
            console.error('Failed to parse project detail:', e);
            this.state.activeProjectDetail = null;
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
                
                return `
                    <div class="ess-config-item ${isActive ? 'active' : ''}" data-name="${this.escapeAttr(cfg.name)}">
                        <div class="ess-config-item-name-row">
                            <span class="ess-config-item-name">${this.escapeHtml(cfg.name)}</span>
                            <div class="ess-config-item-actions">
                                <button class="ess-config-item-menu-btn" title="More actions">⋮</button>
                                <div class="ess-config-item-menu">
                                    <button class="ess-config-menu-action" data-action="view">View Details</button>
                                    <button class="ess-config-menu-action" data-action="edit">Edit</button>
                                    <button class="ess-config-menu-action" data-action="clone">Clone</button>
                                    <button class="ess-config-menu-action" data-action="export">Export...</button>
                                    <div class="ess-config-menu-divider"></div>
                                    <button class="ess-config-menu-action danger" data-action="delete">Delete</button>
                                </div>
                            </div>
                        </div>
                        <div class="ess-config-item-bottom">
                            <div class="ess-config-item-meta">
                                ${cfg.subject ? `<span class="ess-config-item-subject">${this.escapeHtml(cfg.subject)}</span>` : ''}
                                <span class="ess-config-item-path">${this.escapeHtml(cfg.system || '')}/${this.escapeHtml(cfg.protocol || '')}/${this.escapeHtml(cfg.variant || '')}</span>
                            </div>
                            <div class="ess-config-item-buttons">
                                <button class="ess-config-item-run" title="Load, open file, and ready to go">▶</button>
                                <button class="ess-config-item-load" title="Load setup only">Load</button>
                            </div>
                        </div>
                    </div>
                `;
           }).join('');	    
            
            // Bind normal item handlers
            list.querySelectorAll('.ess-config-item').forEach(item => {
                const name = item.dataset.name;
                
                // Run button - run or close file
                item.querySelector('.ess-config-item-run')?.addEventListener('click', (e) => {
                    e.stopPropagation();
                    const isThisConfig = this.state.queueState?.current_config === name;
                    const status = this.state.queueState?.status;
                    
                    if (isThisConfig && status !== 'idle') {
                        // This config is active (running or file open) - close it
                        this.closeRun();
                    } else {
                        // Idle - start run
                        this.runConfig(name);
                    }
                });
                
                // Menu button toggle
                const menuBtn = item.querySelector('.ess-config-item-menu-btn');
                const menu = item.querySelector('.ess-config-item-menu');
                
                menuBtn?.addEventListener('click', (e) => {
                    e.stopPropagation();
                    const wasOpen = menu.classList.contains('open');
                    this.closeAllConfigMenus();
                    if (!wasOpen) {
                        menu.classList.add('open');
                        this.positionConfigMenu(menuBtn, menu);
                    }
                });
                
                // Menu action handlers
                menu?.querySelectorAll('.ess-config-menu-action').forEach(btn => {
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
                
                // Click on row loads (but not if clicking button area)
                item.addEventListener('click', (e) => {
                    if (!e.target.closest('.ess-config-item-actions') && 
                        !e.target.closest('.ess-config-item-run') &&
                        !e.target.closest('.ess-config-item-load')) {
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
     * Run a config directly (load + open datafile, ready for Go)
     * Experimenter presses Go when ready to start
     */
    async runConfig(name) {
	try {
            this.emit('log', { message: `Preparing config: ${name}...`, level: 'info' });
            this.updateEssStatus('loading');
            
            const result = await this.sendConfigCommandAsync(`queue_run_config {${name}}`);
            this.emit('log', { message: `Ready: ${name} (press Go to start)`, level: 'info' });
	} catch (e) {
            this.emit('log', { message: `Failed to run config: ${e.message}`, level: 'error' });
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
	    file_template: '{subject}_{config}_{date_short}{time}',
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
            // Check if any queues reference this config
            const result = await this.sendConfigCommandAsync(`config_queues_using {${name}}`);
            const queueNames = this.parseListData(result);
            
            if (queueNames.length > 0) {
                const queueList = queueNames.join(', ');
                const action = confirm(
                    `"${name}" is used by ${queueNames.length === 1 ? 'queue' : 'queues'}: ${queueList}\n\n` +
                    `Delete ${queueNames.length === 1 ? 'this queue' : 'these queues'} and then move the config to trash?\n\n` +
                    `Cancel to keep everything as-is.`
                );
                
                if (!action) return;
                
                // Delete the referencing queues first
                for (const qName of queueNames) {
                    this.emit('log', { message: `Deleting queue: ${qName}...`, level: 'info' });
                    await this.sendConfigCommandAsync(`queue_delete {${qName}}`);
                }
                this.emit('log', { message: `Deleted ${queueNames.length} queue${queueNames.length !== 1 ? 's' : ''}`, level: 'info' });
                this.refreshQueueList();
            }
            
            this.emit('log', { message: `Moving to trash: ${name}...`, level: 'info' });
            await this.sendConfigCommandAsync(`config_archive {${name}}`);
            this.emit('log', { message: `Moved to trash: ${name}`, level: 'info' });
            
            // Clear current config if it was the archived one
            if (this.state.currentConfig?.name === name) {
                this.state.currentConfig = null;
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
        try {
            // Check if any queues reference this config
            const result = await this.sendConfigCommandAsync(`config_queues_using {${name}}`);
            const queueNames = this.parseListData(result);
            
            let msg = `Permanently delete "${name}"?\n\nThis cannot be undone.`;
            if (queueNames.length > 0) {
                const queueList = queueNames.join(', ');
                msg = `"${name}" is used by ${queueNames.length === 1 ? 'queue' : 'queues'}: ${queueList}\n\n` +
                      `${queueNames.length === 1 ? 'This queue' : 'These queues'} will also be deleted.\n\nThis cannot be undone.`;
            }
            
            if (!confirm(msg)) return;
            
            // Delete referencing queues first
            for (const qName of queueNames) {
                this.emit('log', { message: `Deleting queue: ${qName}...`, level: 'info' });
                await this.sendConfigCommandAsync(`queue_delete {${qName}}`);
            }
            if (queueNames.length > 0) {
                this.refreshQueueList();
            }
            
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
                file_template: config.file_template || '',  // NEW
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

        // File template
        if (this.elements.editFileTemplate) {
            this.elements.editFileTemplate.value = form.file_template || '';
            this.elements.editFileTemplate.disabled = viewMode;
        }
        this.updateEditTemplatePreview();
	
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

       /**
     * Update file template preview in config editor
     */
    updateEditTemplatePreview() {
        const template = this.elements.editFileTemplate?.value?.trim() || '';
        const previewEl = this.elements.editFileTemplatePreview;
        if (!previewEl) return;
        
	let displayTemplate = template;
        if (!displayTemplate) {
	    displayTemplate = '{subject}_{config}_{date_short}{time}';
        }
        
        // Get current values for preview
        const config = this.state.editingConfig || {};
        const subject = this.elements.editSubject?.value || config.subject || 'subject';
        const system = config.system || this.state.currentSystem || 'system';
        const protocol = config.protocol || this.state.currentProtocol || 'protocol';
        const variant = config.variant || this.state.currentVariant || 'variant';
        const configName = this.elements.editName?.value || config.name || 'config';
        const projectName = this.state.activeProject || 'project';
        
        const now = new Date();
        const date = now.toISOString().slice(0, 10).replace(/-/g, '');
        const dateShort = date.slice(2);
        const time = now.toTimeString().slice(0, 8).replace(/:/g, '');
        const timeShort = time.slice(0, 4);
        
        let preview = displayTemplate
            .replace(/\{subject\}/g, subject)
            .replace(/\{config\}/g, configName)
            .replace(/\{system\}/g, system)
            .replace(/\{protocol\}/g, protocol)
            .replace(/\{variant\}/g, variant)
            .replace(/\{project\}/g, projectName)
            .replace(/\{date\}/g, date)
            .replace(/\{date_short\}/g, dateShort)
            .replace(/\{time\}/g, time)
            .replace(/\{time_short\}/g, timeShort);
        
        // Check for unrecognized variables
        const remaining = preview.match(/\{[^}]+\}/g);
        if (remaining) {
            previewEl.textContent = `Unknown: ${remaining.join(', ')}`;
            previewEl.classList.add('error');
        } else {
            previewEl.textContent = preview;
            previewEl.classList.remove('error');
        }
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
	const newFileTemplate = this.elements.editFileTemplate?.value?.trim() || '';
        
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
		if (newFileTemplate) {
		    optArgs.push(`-file_template {${newFileTemplate}}`);
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
	// File template - compare
	if (newFileTemplate !== (original.file_template || '')) {
	    updateArgs.push(`-file_template {${newFileTemplate}}`);
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
    
    /**
     * Insert text at cursor position in an input/textarea element
     */
    insertAtCursor(input, text) {
        const start = input.selectionStart;
        const end = input.selectionEnd;
        const before = input.value.substring(0, start);
        const after = input.value.substring(end);
        
        input.value = before + text + after;
        
        // Move cursor to end of inserted text
        const newPos = start + text.length;
        input.setSelectionRange(newPos, newPos);
        input.focus();
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
            'queues/list', 'queues/state', 'queues/items',
	    'projects/active', 'projects/active_detail'	    
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
                
                // Handle (single) pseudo-queue specially - this is a single config run
                // Don't sync to dropdown, but do show the current config in playlist
                const isSingleRun = this.state.queueState.queue_name === '(single)';
                
                if (isSingleRun) {
                    // For single config runs, create a synthetic item from the state
                    if (this.state.queueState.current_config && this.state.queueState.status !== 'idle') {
                        this.state.queueItems = [{
                            config_name: this.state.queueState.current_config,
                            position: 0,
                            repeat_count: this.state.queueState.repeat_total,
                            pause_after: 0
                        }];
                        // Don't change selectedQueue - leave it showing the dropdown selection
                    } else {
                        // Single run finished/idle - clear items if no real queue selected
                        if (!this.state.selectedQueue || this.state.selectedQueue === '(single)') {
                            this.state.queueItems = [];
                            this.state.selectedQueue = '';
                        }
                    }
                } else {
                    // Regular queue - sync selection to backend state if:
                    // 1. Backend has a queue_name AND
                    // 2. Either: session is active (not idle), OR we don't have a selection yet
                    const hasBackendQueue = !!this.state.queueState.queue_name;
                    const isActive = this.state.queueState.status !== 'idle';
                    const noCurrentSelection = !this.state.selectedQueue;
                    
                    if (hasBackendQueue && (isActive || noCurrentSelection)) {
                        const needsLoad = this.state.selectedQueue !== this.state.queueState.queue_name;
                        this.state.selectedQueue = this.state.queueState.queue_name;
                        this.elements.queueSelect.value = this.state.queueState.queue_name;
                        
                        // Fetch items if we just synced to a different queue or have no items
                        if (needsLoad || this.state.queueItems.length === 0) {
                            // Load items async - renderQueuePlaylist will be called again when items arrive
                            this.loadQueueItems(this.state.queueState.queue_name);
                        }
                    }
                }
                
                this.renderQueueStatus();
                this.updateQueueControls();
                this.renderQueuePlaylist();
                this.updateConfigRunButtons();		
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
        let queues = this.state.queues;
        
        // Apply project filter
        if (this.state.activeProject && this.state.activeProjectDetail) {
            const projectQueues = this.state.activeProjectDetail.queues || [];
            queues = queues.filter(q => projectQueues.includes(q.name));
        }
        
        select.innerHTML = '<option value="">-- Select Queue --</option>';
        queues.forEach(q => {
            const itemLabel = q.item_count === 1 ? '1 config' : `${q.item_count} configs`;
            const opt = document.createElement('option');
            opt.value = q.name;
            opt.textContent = `${q.name} (${itemLabel})`;
            select.appendChild(opt);
        });
        
        // Restore selection if still valid
        if (this.state.selectedQueue) {
            const stillExists = queues.some(q => q.name === this.state.selectedQueue);
            if (stillExists) {
                select.value = this.state.selectedQueue;
            } else {
                // Selected queue no longer exists, clear selection and playlist
                this.state.selectedQueue = '';
                this.state.queueItems = [];
                select.value = '';
                this.renderQueuePlaylist();
            }
        }
    }    
    
    /**
     * Render queue status display
     */
    renderQueueStatus() {
        const qs = this.state.queueState;
        const essLoading = this.state.essStatus === 'loading';
        
        // Status text with styling
        const statusEl = this.elements.queueStatusText;
        statusEl.className = 'ess-queue-status-value';
        
        // Show ESS loading state prominently
        if (essLoading && (qs.status === 'loading' || qs.status === 'ready')) {
            statusEl.textContent = 'loading...';
            statusEl.classList.add('loading');
        } else if (qs.status === 'between_runs') {
            statusEl.textContent = 'waiting';
            statusEl.classList.add('waiting');
        } else {
            statusEl.textContent = qs.status;
            if (qs.status === 'running') {
                statusEl.classList.add('running');
            } else if (qs.status === 'paused') {
                statusEl.classList.add('paused');
            } else if (qs.status === 'finished') {
                statusEl.classList.add('finished');
            } else if (qs.status === 'ready') {
                statusEl.classList.add('ready');
            }
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
    /**
     * Update queue control button states
     * All run controls require ESS to be stopped (not running, not loading)
     */
    updateQueueControls() {
        const qs = this.state.queueState;
        const isSingleRun = qs.queue_name === '(single)';
        const hasQueue = !!this.state.selectedQueue || isSingleRun;
        const hasItems = this.state.queueItems.length > 0;
        const essRunning = this.state.essRunning;
        const essLoading = this.state.essStatus === 'loading';
        const essBusy = essRunning || essLoading;
        
        // Session/queue states
        const isIdle = qs.status === 'idle';
        const isFinished = qs.status === 'finished';
        const isReady = qs.status === 'ready';
        const isBetweenRuns = qs.status === 'between_runs';
        const isRunActive = qs.status === 'running' || qs.status === 'ready';
        
        // Reset button: enabled when session selected and ESS not busy (not for single runs)
        this.elements.queueResetBtn.disabled = !this.state.selectedQueue || essBusy || isSingleRun;
        
        // Start Run: enabled when session selected (real queue), has items, idle/finished/between_runs, ESS not busy
        // For single runs, Start Run is not applicable (use config run button instead)
        const canStartRun = this.state.selectedQueue && hasItems && (isIdle || isFinished || isBetweenRuns) && !essBusy;
        this.elements.queueBtnStartRun.disabled = !canStartRun;
        
        // Close Run: enabled when run is active (file open) and ESS is stopped (not running, not loading)
        // This works for both queue runs and single runs
        const canCloseRun = isRunActive && !essBusy;
        this.elements.queueBtnCloseRun.disabled = !canCloseRun;
        
        // Update config row play buttons
        this.updateConfigPlayButtons();
    }
    
    /**
     * Update play/close button states in config playlist
     * - Play buttons: enabled when ESS stopped and session is in a "selectable" state
     * - Close button: enabled when ESS stopped (can close the current run)
     */
    updateConfigPlayButtons() {
        const qs = this.state.queueState;
        const essRunning = this.state.essRunning;
        const essLoading = this.state.essStatus === 'loading';
        const essBusy = essRunning || essLoading;
        
        // Can play when ESS is stopped and session is in a "selectable" state
        const canPlay = !essBusy && 
            (qs.status === 'idle' || qs.status === 'finished' || qs.status === 'between_runs');
        
        // Can close when ESS is stopped and there's an active run
        const canClose = !essBusy && (qs.status === 'running' || qs.status === 'ready');
        
        this.elements.queuePlaylist.querySelectorAll('.ess-queue-item-play').forEach(btn => {
            btn.disabled = !canPlay;
        });
        
        this.elements.queuePlaylist.querySelectorAll('.ess-queue-item-close').forEach(btn => {
            btn.disabled = !canClose;
        });
    }
    
    /**
     * Render queue playlist
     */
    renderQueuePlaylist() {
        const qs = this.state.queueState;
        const playlist = this.elements.queuePlaylist;
        
        // If no queue selected and no active single run, ensure items are cleared
        const isSingleRun = qs.queue_name === '(single)' && qs.status !== 'idle';
        if (!this.state.selectedQueue && !isSingleRun) {
            this.state.queueItems = [];
        }
        
        const items = this.state.queueItems;
        const isSingleConfig = items.length === 1;
        
        // Adapt UI based on single vs multiple configs
        this.adaptUIForSessionSize(items.length);
        
        // Update item count
        this.elements.queueItemCount.textContent = `${items.length} config${items.length !== 1 ? 's' : ''}`;
        
        if (items.length === 0) {
            if (this.state.selectedQueue) {
                playlist.innerHTML = '<div class="ess-queue-empty">Queue is empty</div>';
            } else {
                playlist.innerHTML = '<div class="ess-queue-empty">No queue selected</div>';
            }
            return;
        }
        
        playlist.innerHTML = items.map((item, idx) => {
            const isCurrent = qs.status !== 'idle' && qs.status !== 'finished' && qs.position === idx;
            const repeatInfo = item.repeat_count > 1 ? `×${item.repeat_count}` : '';
            const pauseInfo = item.pause_after > 0 ? `⏱${item.pause_after}s` : '';
            
            let statusClass = '';
            if (isCurrent) statusClass = 'current';
            
            // Current config gets a close button, others get play button
            const buttonHtml = isCurrent
                ? `<button class="ess-queue-item-close" data-position="${idx}" title="Close run">■</button>`
                : `<button class="ess-queue-item-play" data-position="${idx}" title="Start run at this config">▶</button>`;
            
            // For single config, show simpler display
            if (isSingleConfig) {
                return `
                    <div class="ess-queue-item single ${statusClass}" data-position="${idx}">
                        ${buttonHtml}
                        <span class="ess-queue-item-name">${this.escapeHtml(item.config_name)}</span>
                        ${repeatInfo ? `<span class="ess-queue-item-repeat">${repeatInfo}</span>` : ''}
                        ${isCurrent && qs.run_count > 0 ? `<span class="ess-queue-item-run">run ${qs.run_count}</span>` : ''}
                    </div>
                `;
            }
            
            return `
                <div class="ess-queue-item ${statusClass}" data-position="${idx}">
                    ${buttonHtml}
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
        
        // Bind play button clicks
        playlist.querySelectorAll('.ess-queue-item-play').forEach(btn => {
            btn.addEventListener('click', (e) => {
                e.stopPropagation();
                const pos = parseInt(btn.dataset.position);
                this.startRun(pos);
            });
        });
        
        // Bind close button clicks (for current/active config)
        playlist.querySelectorAll('.ess-queue-item-close').forEach(btn => {
            btn.addEventListener('click', (e) => {
                e.stopPropagation();
                this.closeRun();
            });
        });
        
        // Update button enabled/disabled states
        this.updateConfigPlayButtons();
    }
    
    /**
     * Adapt UI elements based on session size (single config vs multiple)
     */
    adaptUIForSessionSize(itemCount) {
        const isSingleConfig = itemCount === 1;
        const isMultiConfig = itemCount > 1;
        
        // Hide progress row for single config (it's always 1/1)
        if (this.elements.queueProgressRow) {
            this.elements.queueProgressRow.style.display = isSingleConfig ? 'none' : '';
        }
        
        // Rename playlist header based on count
        const headerTitle = this.elements.queuePlaylistHeader?.querySelector('.ess-queue-playlist-title');
        if (headerTitle) {
            headerTitle.textContent = isSingleConfig ? 'Config' : 'Configs';
        }
    }
    
    /**
     * Refresh queue list from backend
     */
    refreshQueueList() {
        this.sendConfigCommand('queue_publish_list');
    }
    
    /**
     * Load items for a queue (without changing selection)
     */
    async loadQueueItems(name) {
        if (!name) {
            this.state.queueItems = [];
            this.renderQueuePlaylist();
            return;
        }
        
        try {
            const response = await this.sendConfigCommandAsync(`queue_get_json {${name}}`);
            const queue = typeof response === 'string' ? JSON.parse(response) : response;
            
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
            } else {
                this.state.queueItems = [];
            }
            this.renderQueuePlaylist();
        } catch (e) {
            console.error('Failed to load queue items:', e);
            this.state.queueItems = [];
            this.renderQueuePlaylist();
        }
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
        
        await this.loadQueueItems(name);
        this.updateQueueControls();
    }
    
    // =========================================================================
    // Run Commands
    // =========================================================================
    
    /**
     * Start a run at the current (or specified) position
     * - If queue is between_runs: advance to next item via queue_next
     * - If queue is idle/finished: start fresh via queue_start
     * @param {number|null} position - Position to start at, or null for current position
     */
    async startRun(position = null) {
        if (!this.state.selectedQueue) return;
        if (this.state.essRunning) {
            this.emit('log', { message: 'Stop ESS before starting a new run', level: 'warn' });
            return;
        }
        
        const qs = this.state.queueState;
        
        try {
            if (qs.status === 'between_runs') {
                // Queue is active, advance to next item
                this.emit('log', { message: `Advancing to next run`, level: 'info' });
                await this.sendConfigCommandAsync('queue_next');
            } else {
                // Fresh start (idle or finished)
                let pos = position;
                if (pos === null) {
                    pos = (qs.status === 'finished' || qs.status === 'idle') ? 0 : qs.position;
                }
                
                this.emit('log', { message: `Starting run: ${this.state.selectedQueue} at position ${pos}`, level: 'info' });
                await this.sendConfigCommandAsync(`queue_start {${this.state.selectedQueue}} -position ${pos}`);
            }
        } catch (e) {
            this.emit('log', { message: `Failed to start run: ${e.message}`, level: 'error' });
        }
    }
    
    /**
     * Close the current run - closes datafile and advances to next position
     * Only available when ESS is stopped
     */
    async closeRun() {
        if (this.state.essRunning) {
            this.emit('log', { message: 'Stop ESS before closing run', level: 'warn' });
            return;
        }
        
        try {
            this.emit('log', { message: 'Closing run', level: 'info' });
            await this.sendConfigCommandAsync('run_close');
        } catch (e) {
            this.emit('log', { message: `Failed to close run: ${e.message}`, level: 'error' });
        }
    }
    
    /**
     * Reset/reload session
     * - If session is active: close any open files, return to position 0, idle state
     * - If session is idle: just reload the items list
     * Only available when ESS is stopped
     */
    async resetSession() {
        if (!this.state.selectedQueue) return;
        if (this.state.essRunning) {
            this.emit('log', { message: 'Stop ESS before resetting session', level: 'warn' });
            return;
        }
        
        const qs = this.state.queueState;
        const isIdle = qs.status === 'idle' || qs.status === 'finished' || qs.queue_name !== this.state.selectedQueue;
        
        if (isIdle) {
            // Just reload items
            this.emit('log', { message: `Reloading session: ${this.state.selectedQueue}`, level: 'info' });
            await this.loadQueueItems(this.state.selectedQueue);
            this.updateQueueControls();
        } else {
            // Active session - reset to beginning
            try {
                this.emit('log', { message: `Resetting session: ${this.state.selectedQueue}`, level: 'info' });
                await this.sendConfigCommandAsync('queue_reset');
                // Reload items after reset
                await this.loadQueueItems(this.state.selectedQueue);
            } catch (e) {
                this.emit('log', { message: `Failed to reset session: ${e.message}`, level: 'error' });
            }
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
                    <span class="ess-modal-title">${queueName ? `Edit: ${this.escapeHtml(queueName)}` : 'New Session'}</span>
                    <button class="ess-modal-close" id="ess-qb-close">&times;</button>
                </div>
                <div class="ess-modal-body">
                    <!-- Name & Description -->
                    <div class="ess-modal-section">
                        <label class="ess-modal-label">Name</label>
                        <input type="text" class="ess-modal-input" id="ess-qb-name" 
                               value="${this.escapeHtml(this.queueBuilder.name)}" 
                               placeholder="Session name (required)">
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
                    </div>
                    
                    <!-- Configs -->
                    <div class="ess-modal-section">
                        <label class="ess-modal-label">Configs</label>
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
        
        // Refresh config list and populate dropdown
        await this.refreshConfigListForQueueBuilder();
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
     * Refresh config list for queue builder dropdown
     * Triggers a publish which will update state via subscription
     */
    async refreshConfigListForQueueBuilder() {
        try {
            // Trigger a publish - this will update state.configs via subscription
            await this.sendConfigCommandAsync('config_publish_all');
            // Small delay to let subscription update state
            await new Promise(resolve => setTimeout(resolve, 50));
        } catch (e) {
            console.warn('Failed to refresh config list for queue builder:', e);
        }
    }

    /**
     * Show confirmation dialog to delete a queue
     */
    showDeleteQueueDialog(queueName) {
        const modal = document.createElement('div');
        modal.className = 'ess-modal-overlay';
        modal.innerHTML = `
            <div class="ess-modal">
                <div class="ess-modal-header">
                    <span class="ess-modal-title">Delete Queue</span>
                    <button class="ess-modal-close">×</button>
                </div>
                <div class="ess-modal-body">
                    <p>Are you sure you want to delete the queue "<strong>${this.escapeHtml(queueName)}</strong>"?</p>
                    <p class="ess-modal-warning">This action cannot be undone. The configs in this queue will not be deleted.</p>
                </div>
                <div class="ess-modal-footer">
                    <button class="ess-modal-btn cancel">Cancel</button>
                    <button class="ess-modal-btn danger">Delete Queue</button>
                </div>
            </div>
        `;
        
        document.body.appendChild(modal);
        
        const closeBtn = modal.querySelector('.ess-modal-close');
        const cancelBtn = modal.querySelector('.ess-modal-btn.cancel');
        const deleteBtn = modal.querySelector('.ess-modal-btn.danger');
        
        const closeModal = () => modal.remove();
        
        closeBtn.addEventListener('click', closeModal);
        cancelBtn.addEventListener('click', closeModal);
        modal.addEventListener('click', (e) => {
            if (e.target === modal) closeModal();
        });
        
        deleteBtn.addEventListener('click', async () => {
            deleteBtn.disabled = true;
            deleteBtn.textContent = 'Deleting...';
            
            try {
                await this.sendConfigCommandAsync(`queue_delete {${queueName}}`);
                this.emit('log', { message: `Deleted queue "${queueName}"`, level: 'info' });
                
                // Clear selection and refresh
                this.state.selectedQueue = '';
                this.elements.queueSelect.value = '';
                this.refreshQueueList();
                this.renderQueuePlaylist();
                
                closeModal();
            } catch (e) {
                this.emit('log', { message: `Failed to delete queue: ${e.message}`, level: 'error' });
                deleteBtn.disabled = false;
                deleteBtn.textContent = 'Delete Queue';
            }
        });
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
                await this.sendConfigCommandAsync(`queue_clear_items {${originalName}}`);
                
                // Update settings (including rename if applicable)
                let updateCmd = `queue_update {${originalName}}`;
                if (isRenaming) {
                    updateCmd += ` -name {${name}}`;
                }
                updateCmd += ` -description {${description}}`;
                updateCmd += ` -auto_start ${auto_start}`;
                updateCmd += ` -auto_advance ${auto_advance}`;
                updateCmd += ` -auto_datafile ${auto_datafile}`;
                
                console.log('Update command:', updateCmd);
                await this.sendConfigCommandAsync(updateCmd);
                
                // Add items (using new name if renamed)
                for (const item of this.queueBuilder.items) {
                    let addCmd = `queue_add_item {${name}} {${item.config_name}}`;
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
                
                await this.sendConfigCommandAsync(createCmd);
                
                // Add items
                for (const item of this.queueBuilder.items) {
                    let addCmd = `queue_add_item {${name}} {${item.config_name}}`;
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
}

// Export
if (typeof window !== 'undefined') {
    window.ESSControl = ESSControl;
}
