/**
 * ESS Registry Integration v2
 * 
 * Enhanced integration with:
 * - Direct Tcl command execution for all registry operations
 * - Commit via ess::commit_script (base → registry)
 * - Sync status via ess::sync_status
 * - System browser for exploring available systems
 * - Single path: all registry ops go through dserv/ess Tcl backend
 */

(function() {
    'use strict';
    
    // Extend ESSWorkbench with enhanced registry features
    const originalInit = ESSWorkbench.prototype.init;
    
    ESSWorkbench.prototype.init = function() {
        originalInit.call(this);
        
        // Registry state
        this.registryState = {
            connected: false,
            workgroup: null,
            user: null,
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
    // Commit: base → registry (via ess::commit_script)
    // ==========================================
    
    /**
     * Commit a single script type from local base to registry.
     * This is the ONLY path to push changes to the registry.
     * Requires: overlay promoted first (script is in base).
     */
    ESSWorkbench.prototype.registryCommit = async function(type, options = {}) {
        const comment = options.comment || '';
        
        // Escape braces in comment for Tcl
        const safeComment = comment.replace(/[{}]/g, '');
        const cmd = `ess::commit_script ${type} {${safeComment}}`;
        
        try {
            const result = await this.execRegistryCmd(cmd);
            this.showNotification(`Committed ${type} to registry`, 'success');
            
            // Update sync status
            this.scriptSyncStatus[type] = 'synced';
            this.updateSyncStatusUI();
            
            return result;
        } catch(err) {
            if (err.message.includes('conflict')) {
                this.showNotification('Conflict: registry has a newer version. Sync first.', 'error');
                this.scriptSyncStatus[type] = 'conflict';
                this.updateSyncStatusUI();
            } else if (err.message.includes('viewer')) {
                this.showNotification('Permission denied: viewers cannot commit to registry.', 'error');
            } else {
                this.showNotification(`Commit failed: ${err.message}`, 'error');
            }
            throw err;
        }
    };
    
    /**
     * Commit all scripts for current system to registry.
     */
    ESSWorkbench.prototype.registryCommitAll = async function(options = {}) {
        const comment = options.comment || '';
        const safeComment = comment.replace(/[{}]/g, '');
        const cmd = `ess::commit_system {${safeComment}}`;
        
        try {
            const result = await this.execRegistryCmd(cmd);
            const committed = result.committed || [];
            const errors = result.errors || [];
            
            if (errors.length > 0) {
                this.showNotification(
                    `Committed ${committed.length}, ${errors.length} error(s)`, 'warning'
                );
            } else {
                this.showNotification(
                    `Committed ${committed.length} script(s) to registry`, 'success'
                );
            }
            
            // Refresh sync status
            await this.registrySyncStatus();
            
            return result;
        } catch(err) {
            this.showNotification(`Commit failed: ${err.message}`, 'error');
            throw err;
        }
    };
    
    // ==========================================
    // Sync Status (via ess::sync_status)
    // ==========================================
    
    /**
     * Check sync status of all scripts against registry.
     * Updates scriptSyncStatus dict and UI indicators.
     */
    ESSWorkbench.prototype.registrySyncStatus = async function() {
        try {
            const result = await this.execRegistryCmd('ess::sync_status');
            
            // result is a Tcl dict like: system synced protocol modified ...
            // Parse it into our JS status map
            if (typeof result === 'string') {
                // Tcl dict comes as space-separated key value pairs
                const parts = result.split(/\s+/);
                for (let i = 0; i < parts.length - 1; i += 2) {
                    this.scriptSyncStatus[parts[i]] = parts[i + 1];
                }
            } else if (typeof result === 'object') {
                Object.assign(this.scriptSyncStatus, result);
            }
            
            this.updateSyncStatusUI();
            
        } catch(err) {
            console.warn('Sync status check failed:', err);
        }
    };
    
    // ==========================================
    // Sync: registry → local base (via ess::sync_system)
    // ==========================================
    
    /**
     * Pull latest from registry for the current system.
     * Writes updated files to local base (system_path).
     */
    ESSWorkbench.prototype.registrySync = async function() {
        try {
            const result = await this.execRegistryCmd('ess::sync_system $::ess::current(system)');
            const pulled = result.pulled || 0;
            const unchanged = result.unchanged || 0;
            
            if (pulled > 0) {
                this.showNotification(`Synced: ${pulled} updated, ${unchanged} unchanged`, 'success');
            } else {
                this.showNotification('Already up to date', 'info');
            }
            
            // Refresh sync status
            await this.registrySyncStatus();
            
            return result;
        } catch(err) {
            this.showNotification(`Sync failed: ${err.message}`, 'error');
            throw err;
        }
    };
    
    // ==========================================
    // System Browser
    // ==========================================
    
    ESSWorkbench.prototype.showSystemBrowser = async function() {
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
        
        // Group systems by name
        const grouped = {};
        systems.forEach(sys => {
            if (!grouped[sys.name]) {
                grouped[sys.name] = [];
            }
            grouped[sys.name].push(sys);
        });
        
        let html = '<div class="system-list">';
        
        for (const [name, versions] of Object.entries(grouped)) {
            const latest = versions[0];
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
    // System Loading from Browser
    // ==========================================
    
    ESSWorkbench.prototype.openSystemProtocol = async function(system, protocol, variant = '') {
        try {
            const cmd = variant ? 
                `ess::load_system ${system} ${protocol} ${variant}` :
                `ess::load_system ${system} ${protocol}`;
            
            await this.execRegistryCmd(cmd);
            
            this.showNotification(`Loaded ${system}/${protocol}`, 'success');
            
            // Close modal
            const modal = document.getElementById('system-browser-modal');
            if (modal) modal.style.display = 'none';
            
        } catch(err) {
            this.showNotification(`Failed to load system: ${err.message}`, 'error');
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
    // UI Helpers
    // ==========================================
    
    ESSWorkbench.prototype.showNotification = function(message, type = 'info') {
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
        
        // Auto-remove after 5 seconds
        setTimeout(() => notification.remove(), 5000);
    };
    
    ESSWorkbench.prototype.updateWorkgroupDisplay = function(workgroup) {
        const el = document.getElementById('workgroup-name');
        if (el) el.textContent = workgroup || 'Not set';
    };
    
    // ==========================================
    // Event Bindings
    // ==========================================
    
    ESSWorkbench.prototype.bindRegistryV2Events = function() {
        // System browser button (if present)
        // this.addSystemBrowserButton();
    };
    
    ESSWorkbench.prototype.addSystemBrowserButton = function() {
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
