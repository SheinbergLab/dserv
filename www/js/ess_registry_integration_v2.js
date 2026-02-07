/**
 * ESS Registry Integration v2
 * 
 * Enhanced integration with:
 * - System browser for exploring available systems
 * - Direct Tcl command execution for registry operations
 * - Improved sync status with backend validation
 * - Support for opening systems from workbench
 */

(function() {
    'use strict';
    
    // Extend ESSWorkbench with enhanced registry features
    const originalInit = ESSWorkbench.prototype.init;
    
    ESSWorkbench.prototype.init = function() {
        originalInit.call(this);
        
        // Enhanced registry state
        this.registryState = {
            connected: false,
            workgroup: null,
            user: null,
            version: 'main',
            systems: [],
            currentSystemLocked: false
        };
        
        this.initRegistryV2();
    };
    
    // ==========================================
    // Registry v2 Initialization
    // ==========================================
    
    ESSWorkbench.prototype.initRegistryV2 = function() {
	// Bind UI events that don't need connection
	this.bindRegistryV2Events();
	
	// Defer datapoint subscriptions until connected
	// (setupRegistryDatapoints requires an active WebSocket)
	if (this.connection) {
            this.connection.on('connected', () => {
		this.setupRegistryDatapoints();
            });
            
            // If already connected (e.g., reconnect scenario), set up now
            if (this.connection.ws?.readyState === WebSocket.OPEN) {
		this.setupRegistryDatapoints();
            }
	}
    };
    
    ESSWorkbench.prototype.setupRegistryDatapoints = function() {
        if (!this.dpManager) return;
        
        // Subscribe to registry status updates from Tcl backend
        const registryDatapoints = [
            'ess/registry/url',
            'ess/registry/workgroup', 
            'ess/registry/user',
            'ess/registry/version',
            'ess/registry/last_pull',
            'ess/registry/last_push',
            'ess/registry/sync_status'
        ];
        
        registryDatapoints.forEach(dp => {
            this.dpManager.subscribe(dp, (data) => {
                this.handleRegistryDatapoint(dp, data);
            });
        });
    };
    
    ESSWorkbench.prototype.handleRegistryDatapoint = function(name, data) {
        const value = data.data !== undefined ? data.data : data.value;
        
        switch(name) {
            case 'ess/registry/url':
                if (value && this.registry) {
                    this.registry.baseUrl = value;
                }
                break;
                
            case 'ess/registry/workgroup':
                this.registryState.workgroup = value;
                if (this.registry) {
                    this.registry.setWorkgroup(value);
                }
                this.updateWorkgroupDisplay(value);
                break;
                
            case 'ess/registry/user':
                this.registryState.user = value;
                if (this.registry) {
                    this.registry.setUser(value);
                }
                break;
                
            case 'ess/registry/version':
                this.registryState.version = value;
                this.updateVersionSelector(value);
                break;
                
            case 'ess/registry/sync_status':
                if (value) {
                    try {
                        const status = typeof value === 'string' ? JSON.parse(value) : value;
                        this.updateSyncStatusFromBackend(status);
                    } catch(e) {
                        console.warn('Could not parse sync status:', e);
                    }
                }
                break;
        }
    };
    
    // ==========================================
    // Tcl Command Execution for Registry Ops
    // ==========================================
    
    ESSWorkbench.prototype.execRegistryCmd = async function(cmd) {
        return new Promise((resolve, reject) => {
	    if (this.connection?.ws?.readyState !== WebSocket.OPEN) {	    
                reject(new Error('WebSocket not connected'));
                return;
            }
            
            // Generate unique response ID
            const responseId = `reg_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;
            
            // Set up response handler
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
            
            // Subscribe to response
            this.dpManager.subscribe(`ess/cmd_response/${responseId}`, responseHandler);
            
            // Send command with response routing
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
            
            // Timeout after 30 seconds
            setTimeout(() => {
                this.dpManager.unsubscribe(`ess/cmd_response/${responseId}`, responseHandler);
                reject(new Error('Registry command timeout'));
            }, 30000);
        });
    };
    
    // ==========================================
    // Registry Operations via Tcl Backend
    // ==========================================
    
    ESSWorkbench.prototype.registryPull = async function(type, options = {}) {
        const version = options.version || this.registryState.version;
        const apply = options.apply !== false;
        
        const cmd = `ess::registry::pull ${type} -version ${version} -apply ${apply ? 1 : 0}`;
        
        try {
            const result = await this.execRegistryCmd(cmd);
            this.showNotification(`Pulled ${type} from registry`, 'success');
            
            // Refresh editor if this is the current script
            if (type === 'variants' && this.variantScriptEditor) {
                const content = await this.execRegistryCmd(`ess::variants_script`);
                this.variantScriptEditor.setValue(content);
            } else if (this.currentScript === type && this.editor) {
                const content = await this.execRegistryCmd(`ess::${type}_script`);
                this.editor.setValue(content);
            }
            
            // Update sync status
            this.checkSyncStatus(type);
            
            return result;
        } catch(err) {
            this.showNotification(`Pull failed: ${err.message}`, 'error');
            throw err;
        }
    };
    
    ESSWorkbench.prototype.registryPush = async function(type, options = {}) {
        const version = options.version || this.registryState.version;
        const user = options.user || this.registryState.user || 'unknown';
        const comment = options.comment || '';
        
        const cmd = `ess::registry::push ${type} -version {${version}} -user {${user}} -comment {${comment}}`;
        
        try {
            const result = await this.execRegistryCmd(cmd);
            this.showNotification(`Pushed ${type} to registry`, 'success');
            
            // Update sync status
            this.scriptSyncStatus[type] = 'synced';
            this.updateSyncStatusUI();
            
            return result;
        } catch(err) {
            if (err.message.includes('conflict')) {
                this.showNotification('Conflict: someone else modified this script. Pull first.', 'error');
                this.scriptSyncStatus[type] = 'conflict';
                this.updateSyncStatusUI();
            } else {
                this.showNotification(`Push failed: ${err.message}`, 'error');
            }
            throw err;
        }
    };
    
    ESSWorkbench.prototype.registryStatus = async function(type) {
        const version = this.registryState.version;
        const cmd = `ess::registry::status ${type} ${version}`;
        
        try {
            return await this.execRegistryCmd(cmd);
        } catch(err) {
            console.warn(`Status check failed for ${type}:`, err);
            return { status: 'unknown', error: err.message };
        }
    };
    
    // ==========================================
    // System Browser
    // ==========================================
    
    ESSWorkbench.prototype.showSystemBrowser = async function() {
        // Create or show system browser modal
        let modal = document.getElementById('system-browser-modal');
        
        if (!modal) {
            modal = this.createSystemBrowserModal();
            document.body.appendChild(modal);
        }
        
        modal.style.display = 'flex';
        await this.loadSystemBrowserData();
    };
    
    ESSWorkbench.prototype.createSystemBrowserModal = function() {
        const modal = document.createElement('div');
        modal.id = 'system-browser-modal';
        modal.className = 'modal-overlay';
        modal.innerHTML = `
            <div class="modal" style="width: 700px; max-height: 80vh;">
                <div class="modal-header">
                    <h3>System Browser</h3>
                    <button class="modal-close" onclick="this.closest('.modal-overlay').style.display='none'">&times;</button>
                </div>
                <div class="modal-body" style="max-height: 60vh; overflow-y: auto;">
                    <div class="system-browser-tabs">
                        <button class="tab-btn active" data-browser-tab="workgroup">Workgroup Systems</button>
                        <button class="tab-btn" data-browser-tab="templates">Templates (Zoo)</button>
                    </div>
                    
                    <div class="browser-content" id="browser-workgroup">
                        <div class="loading-state">Loading systems...</div>
                    </div>
                    
                    <div class="browser-content" id="browser-templates" style="display:none;">
                        <div class="loading-state">Loading templates...</div>
                    </div>
                </div>
            </div>
        `;
        
        // Tab switching
        modal.querySelectorAll('.tab-btn').forEach(btn => {
            btn.addEventListener('click', () => {
                modal.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
                btn.classList.add('active');
                
                modal.querySelectorAll('.browser-content').forEach(c => c.style.display = 'none');
                document.getElementById(`browser-${btn.dataset.browserTab}`).style.display = 'block';
            });
        });
        
        return modal;
    };
    
    ESSWorkbench.prototype.loadSystemBrowserData = async function() {
        // Load workgroup systems
        try {
            const systemsResult = await this.execRegistryCmd('ess::registry::list_systems');
            this.renderWorkgroupSystems(systemsResult);
        } catch(err) {
            document.getElementById('browser-workgroup').innerHTML = 
                `<div class="empty-state-small">Failed to load: ${err.message}</div>`;
        }
        
        // Load templates
        try {
            const templatesResult = await this.execRegistryCmd('ess::registry::list_templates');
            this.renderTemplates(templatesResult);
        } catch(err) {
            document.getElementById('browser-templates').innerHTML = 
                `<div class="empty-state-small">Failed to load: ${err.message}</div>`;
        }
    };
    
    ESSWorkbench.prototype.renderWorkgroupSystems = function(data) {
        const container = document.getElementById('browser-workgroup');
        const systems = data.systems || [];
        
        if (systems.length === 0) {
            container.innerHTML = '<div class="empty-state-small">No systems in workgroup</div>';
            return;
        }
        
        // Group systems by name (may have multiple versions)
        const grouped = {};
        systems.forEach(sys => {
            if (!grouped[sys.name]) {
                grouped[sys.name] = [];
            }
            grouped[sys.name].push(sys);
        });
        
        let html = '<div class="system-list">';
        
        for (const [name, versions] of Object.entries(grouped)) {
            const latest = versions[0]; // Assume sorted
            const protocols = latest.protocols || [];
            
            html += `
                <div class="system-browser-item" data-system="${this.escapeHtml(name)}">
                    <div class="system-item-header">
                        <span class="system-name">${this.escapeHtml(name)}</span>
                        <span class="system-meta">${protocols.length} protocol(s)</span>
                    </div>
                    <div class="system-protocols">
                        ${protocols.map(p => `
                            <button class="protocol-chip" 
                                    data-system="${this.escapeHtml(name)}" 
                                    data-protocol="${this.escapeHtml(p)}"
                                    onclick="workbench.openSystemProtocol('${this.escapeHtml(name)}', '${this.escapeHtml(p)}')">
                                ${this.escapeHtml(p)}
                            </button>
                        `).join('')}
                    </div>
                    ${latest.description ? `<div class="system-description">${this.escapeHtml(latest.description)}</div>` : ''}
                </div>
            `;
        }
        
        html += '</div>';
        container.innerHTML = html;
    };
    
    ESSWorkbench.prototype.renderTemplates = function(data) {
        const container = document.getElementById('browser-templates');
        const templates = data.systems || [];
        
        if (templates.length === 0) {
            container.innerHTML = '<div class="empty-state-small">No templates available</div>';
            return;
        }
        
        let html = '<div class="template-list">';
        
        templates.forEach(tmpl => {
            // Check if already added to workgroup
            const alreadyAdded = this.registryState.systems.some(s => s.name === tmpl.name);
            
            html += `
                <div class="template-card ${alreadyAdded ? 'already-added' : ''}">
                    <div class="template-info">
                        <span class="template-name">${this.escapeHtml(tmpl.name)}</span>
                        <span class="template-version">v${tmpl.version || '1.0'}</span>
                    </div>
                    ${alreadyAdded ? 
                        '<span class="template-added-badge">Added</span>' :
                        `<button class="btn btn-sm btn-primary" 
                                 onclick="workbench.addTemplateToWorkgroup('${this.escapeHtml(tmpl.name)}')">
                            Add to Workgroup
                        </button>`
                    }
                </div>
            `;
        });
        
        html += '</div>';
        container.innerHTML = html;
    };
    
    // ==========================================
    // System Loading from Workbench
    // ==========================================
    
    ESSWorkbench.prototype.openSystemProtocol = async function(system, protocol, variant = '') {
        // Check if system is being used by someone else
        const lockCheck = await this.checkSystemLock(system, protocol);
        
        if (lockCheck.locked && lockCheck.lockedBy !== this.registryState.user) {
            const proceed = confirm(
                `This system is currently being used by ${lockCheck.lockedBy}.\n\n` +
                `Are you sure you want to open it anyway? Changes may conflict.`
            );
            if (!proceed) return;
        }
        
        // Load the system via ess::load_system
        try {
            const cmd = variant ? 
                `ess::load_system ${system} ${protocol} ${variant}` :
                `ess::load_system ${system} ${protocol}`;
            
            await this.execRegistryCmd(cmd);
            
            this.showNotification(`Loaded ${system}/${protocol}`, 'success');
            
            // Close modal
            const modal = document.getElementById('system-browser-modal');
            if (modal) modal.style.display = 'none';
            
            // The snapshot subscription will update the UI
            
        } catch(err) {
            this.showNotification(`Failed to load system: ${err.message}`, 'error');
        }
    };
    
    ESSWorkbench.prototype.checkSystemLock = async function(system, protocol) {
        try {
            const locks = await this.registry.getLocks();
            const key = protocol ? 
                `${this.registryState.workgroup}/${system}/${protocol}` :
                `${this.registryState.workgroup}/${system}`;
            
            const lock = (locks.locks || []).find(l => l.key === key);
            
            if (lock) {
                return { locked: true, lockedBy: lock.lockedBy, expiresAt: lock.expiresAt };
            }
            return { locked: false };
        } catch(err) {
            console.warn('Lock check failed:', err);
            return { locked: false };
        }
    };
    
    ESSWorkbench.prototype.addTemplateToWorkgroup = async function(templateName) {
        try {
            const cmd = `ess::registry::add_template ${templateName}`;
            await this.execRegistryCmd(cmd);
            
            this.showNotification(`Added ${templateName} to workgroup`, 'success');
            
            // Refresh browser
            await this.loadSystemBrowserData();
            
        } catch(err) {
            this.showNotification(`Failed to add template: ${err.message}`, 'error');
        }
    };
    
    // ==========================================
    // Sandbox Operations
    // ==========================================
    
    ESSWorkbench.prototype.createSandbox = async function() {
        const user = this.registryState.user;
        if (!user) {
            this.showNotification('Please select a user first', 'warning');
            return;
        }
        
        const comment = prompt('Comment for new sandbox (optional):', '');
        
        try {
            const cmd = `ess::registry::sandbox create -user {${user}} -comment {${comment || ''}}`;
            await this.execRegistryCmd(cmd);
            
            this.showNotification(`Created sandbox for ${user}`, 'success');
            
            // Switch to the new sandbox
            this.registryState.version = user;
            this.updateVersionSelector(user);
            await this.refreshVersionList();
            
        } catch(err) {
            this.showNotification(`Failed to create sandbox: ${err.message}`, 'error');
        }
    };
    
    ESSWorkbench.prototype.promoteSandbox = async function() {
        const version = this.registryState.version;
        if (version === 'main') {
            this.showNotification('Already on main version', 'info');
            return;
        }
        
        const confirm_msg = `Promote sandbox "${version}" to main?\n\nThis will replace the main version with your changes.`;
        if (!confirm(confirm_msg)) return;
        
        const comment = prompt('Comment for this promotion:', `Promoted from ${version}`);
        
        try {
            const cmd = `ess::registry::sandbox promote -from {${version}} -comment {${comment || ''}}`;
            await this.execRegistryCmd(cmd);
            
            this.showNotification('Sandbox promoted to main!', 'success');
            
            // Switch to main
            this.registryState.version = 'main';
            this.updateVersionSelector('main');
            
        } catch(err) {
            this.showNotification(`Promotion failed: ${err.message}`, 'error');
        }
    };
    
    ESSWorkbench.prototype.syncSandbox = async function() {
        const version = this.registryState.version;
        if (version === 'main') {
            this.showNotification('Already on main version', 'info');
            return;
        }
        
        const confirm_msg = `Sync sandbox "${version}" with latest from main?\n\nThis will overwrite your sandbox with main's current state.`;
        if (!confirm(confirm_msg)) return;
        
        try {
            const cmd = `ess::registry::sandbox sync -to {${version}}`;
            await this.execRegistryCmd(cmd);
            
            this.showNotification('Sandbox synced with main', 'success');
            this.checkSyncStatus();
            
        } catch(err) {
            this.showNotification(`Sync failed: ${err.message}`, 'error');
        }
    };
    
    ESSWorkbench.prototype.deleteSandbox = async function() {
        const version = this.registryState.version;
        if (version === 'main') {
            this.showNotification('Cannot delete main version', 'error');
            return;
        }
        
        const confirm_msg = `Delete sandbox "${version}"?\n\nThis cannot be undone.`;
        if (!confirm(confirm_msg)) return;
        
        try {
            const cmd = `ess::registry::sandbox delete -version {${version}}`;
            await this.execRegistryCmd(cmd);
            
            this.showNotification('Sandbox deleted', 'success');
            
            // Switch to main
            this.registryState.version = 'main';
            this.updateVersionSelector('main');
            await this.refreshVersionList();
            
        } catch(err) {
            this.showNotification(`Delete failed: ${err.message}`, 'error');
        }
    };
    
    // ==========================================
    // Version List Management
    // ==========================================
    
    ESSWorkbench.prototype.refreshVersionList = async function() {
        try {
            const result = await this.execRegistryCmd('ess::registry::versions');
            const versions = result.versions || ['main'];
            
            const select = document.getElementById('version-select');
            if (!select) return;
            
            const currentValue = select.value;
            select.innerHTML = '';
            
            versions.forEach(v => {
                const opt = document.createElement('option');
                opt.value = v;
                opt.textContent = v === 'main' ? 'main (production)' : `sandbox/${v}`;
                if (v === currentValue || v === this.registryState.version) {
                    opt.selected = true;
                }
                select.appendChild(opt);
            });
            
        } catch(err) {
            console.warn('Failed to refresh version list:', err);
        }
    };
    
    // ==========================================
    // UI Helpers
    // ==========================================
    
    ESSWorkbench.prototype.showNotification = function(message, type = 'info') {
        // Create notification element
        const notification = document.createElement('div');
        notification.className = `notification notification-${type}`;
        notification.innerHTML = `
            <span class="notification-message">${this.escapeHtml(message)}</span>
            <button class="notification-close">&times;</button>
        `;
        
        // Style it
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
        
        // Auto-remove after 5 seconds
        setTimeout(() => notification.remove(), 5000);
    };
    
    ESSWorkbench.prototype.updateWorkgroupDisplay = function(workgroup) {
        const el = document.getElementById('workgroup-name');
        if (el) el.textContent = workgroup || 'Not set';
    };
    
    ESSWorkbench.prototype.updateVersionSelector = function(version) {
        const select = document.getElementById('version-select');
        if (select) {
            // Try to select the version, add it if not present
            let found = false;
            for (let opt of select.options) {
                if (opt.value === version) {
                    opt.selected = true;
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                const opt = document.createElement('option');
                opt.value = version;
                opt.textContent = version === 'main' ? 'main (production)' : `sandbox/${version}`;
                opt.selected = true;
                select.appendChild(opt);
            }
        }
        
        // Update sandbox button visibility
        const isMain = version === 'main';
        document.getElementById('sync-sandbox-btn')?.style.setProperty('display', isMain ? 'none' : '');
        document.getElementById('promote-sandbox-btn')?.style.setProperty('display', isMain ? 'none' : '');
        document.getElementById('delete-sandbox-btn')?.style.setProperty('display', isMain ? 'none' : '');
    };
    
    // ==========================================
    // Event Bindings
    // ==========================================
    
    ESSWorkbench.prototype.bindRegistryV2Events = function() {
        // Version selector change
        document.getElementById('version-select')?.addEventListener('change', async (e) => {
            this.registryState.version = e.target.value;
            
            // Update backend
            await this.execRegistryCmd(`ess::registry::configure -version {${e.target.value}}`);
            
            // Update UI
            this.updateVersionSelector(e.target.value);
            this.checkSyncStatus();
        });
        
        // Sandbox buttons
        document.getElementById('create-sandbox-btn')?.addEventListener('click', () => this.createSandbox());
        document.getElementById('sync-sandbox-btn')?.addEventListener('click', () => this.syncSandbox());
        document.getElementById('promote-sandbox-btn')?.addEventListener('click', () => this.promoteSandbox());
        document.getElementById('delete-sandbox-btn')?.addEventListener('click', () => this.deleteSandbox());
        
        // Refresh versions
        document.getElementById('refresh-versions-btn')?.addEventListener('click', () => this.refreshVersionList());
        
        // Add system browser button (if there's a good place for it)
        this.addSystemBrowserButton();
    };
    
    ESSWorkbench.prototype.addSystemBrowserButton = function() {
        // Add to header or dashboard
        const headerRight = document.querySelector('.header-right');
        if (headerRight) {
            const btn = document.createElement('button');
            btn.className = 'btn btn-sm btn-ghost';
            btn.innerHTML = `
                <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                    <path d="M3 9l9-7 9 7v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"></path>
                    <polyline points="9 22 9 12 15 12 15 22"></polyline>
                </svg>
                Browse Systems
            `;
            btn.addEventListener('click', () => this.showSystemBrowser());
            headerRight.insertBefore(btn, headerRight.firstChild);
        }
    };
    
})();
