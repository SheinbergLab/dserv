/**
 * ESS Registry Integration Module
 * 
 * This module extends ESSWorkbench with registry capabilities:
 * - User management
 * - Sync status tracking
 * - Pull/Commit operations
 * - Registry tab functionality
 * 
 * Include after ess_workbench.js and registry_client.js
 */

(function() {
    'use strict';


    // Wait for ESSWorkbench to be defined
    const originalInit = ESSWorkbench.prototype.init;
    
    ESSWorkbench.prototype.init = function() {
        // Call original init
        originalInit.call(this);
        
        // Add registry properties
        this.workgroup = null;
        this.currentUser = null;
        this.scriptSyncStatus = {};
        this.localChecksums = {};
        this.registryChecksums = {};
        
        // Note: Registry is initialized by main workbench after connection
        // We'll set up UI features when that happens
        
        // Bind registry events
        this.bindRegistryEvents();
    };
    
    // ==========================================
    // Registry Initialization
    // ==========================================
    
    // This gets called by main ess_workbench.js after registry client is created
    ESSWorkbench.prototype.setupRegistryUI = async function() {
        if (!this.registry) {
            console.warn('setupRegistryUI called but registry not initialized');
            return;
        }
        
        console.log('Setting up registry UI features...');
        
        // Update workgroup display
        const wgEl = document.getElementById('workgroup-name');
        if (wgEl && this.registry.workgroup) {
            wgEl.textContent = this.registry.workgroup;
            this.workgroup = this.registry.workgroup;
        }
        
        // Restore user from localStorage
        this.currentUser = this.registry.getUser();
        
        // Load users for selector
        try {
            const users = await this.registry.getUsers();
            this.populateUserSelect(users);
            this.updateRegistryStatus('connected');
        } catch (err) {
            console.warn('Registry connection failed:', err);
            this.updateRegistryStatus('error', err.message);
        }
    };
    
    ESSWorkbench.prototype.populateUserSelect = function(users) {
        const select = document.getElementById('user-select');
        if (!select) return;
        
        select.innerHTML = '<option value="">Select user...</option>';
        
        users.forEach(user => {
            const opt = document.createElement('option');
            opt.value = user.username;
            opt.textContent = user.fullName || user.username;
            if (user.username === this.currentUser) {
                opt.selected = true;
            }
            select.appendChild(opt);
        });
    };
    
    ESSWorkbench.prototype.updateRegistryStatus = function(status, message) {
        const el = document.getElementById('registry-status');
        if (!el) return;
        
        el.className = 'footer-item registry-status ' + status;
        const text = el.querySelector('span');
        if (text) {
            text.textContent = status === 'connected' ? 
                `Registry: ${this.workgroup}` : 
                `Registry: ${message || 'disconnected'}`;
        }
    };
    
    // ==========================================
    // Registry Event Bindings
    // ==========================================
    
    ESSWorkbench.prototype.bindRegistryEvents = function() {
        // User selector
        const userSelect = document.getElementById('user-select');
        if (userSelect) {
            userSelect.addEventListener('change', (e) => {
                this.currentUser = e.target.value;
                if (this.registry) {
                    this.registry.setUser(this.currentUser);
                }
            });
        }
        
        // Variant tab registry buttons
        document.getElementById('variant-pull-btn')?.addEventListener('click', () => {
            this.pullFromRegistry('variants');
        });
        
        document.getElementById('variant-commit-btn')?.addEventListener('click', () => {
            this.showCommitDialog('variants');
        });
        
        // Script tab registry buttons
        document.getElementById('script-pull-btn')?.addEventListener('click', () => {
            this.pullScriptFromRegistry();
        });
        
        document.getElementById('script-commit-btn')?.addEventListener('click', () => {
            this.showCommitDialog(this.currentScript);
        });
        
        // Script tab save button
        document.getElementById('script-save-btn')?.addEventListener('click', () => {
            this.saveCurrentScript();
        });
        
        // Registry tab buttons
        document.getElementById('add-user-btn')?.addEventListener('click', () => {
            document.getElementById('add-user-modal').style.display = 'flex';
        });
        
        document.getElementById('backup-now-btn')?.addEventListener('click', () => {
            this.createBackup();
        });
        
        // Modal events
        this.bindModalEvents();
        
        // Hook into script selection to update sync status
        this.hookScriptSelection();
    };
    
    // Hook into script item clicks to update sync status display
    ESSWorkbench.prototype.hookScriptSelection = function() {
        const scriptsList = document.getElementById('scripts-list');
        if (!scriptsList) return;
        
        scriptsList.addEventListener('click', (e) => {
            const scriptItem = e.target.closest('.script-item');
            if (scriptItem) {
                const scriptType = scriptItem.dataset.script;
                // Update sync status display for the newly selected script
                setTimeout(() => {
                    this.updateSingleSyncStatus('scripts-sync-status', scriptType);
                }, 50);
            }
        });
    };
    
    ESSWorkbench.prototype.bindModalEvents = function() {
        // Add user modal
        const addUserModal = document.getElementById('add-user-modal');
        if (addUserModal) {
            document.getElementById('add-user-modal-close')?.addEventListener('click', () => {
                addUserModal.style.display = 'none';
            });
            
            document.getElementById('add-user-cancel')?.addEventListener('click', () => {
                addUserModal.style.display = 'none';
            });
            
            document.getElementById('add-user-submit')?.addEventListener('click', () => {
                this.submitAddUser();
            });
        }
        
        // Commit modal
        const commitModal = document.getElementById('commit-modal');
        if (commitModal) {
            document.getElementById('commit-modal-close')?.addEventListener('click', () => {
                commitModal.style.display = 'none';
            });
            
            document.getElementById('commit-cancel')?.addEventListener('click', () => {
                commitModal.style.display = 'none';
            });
            
            document.getElementById('commit-submit')?.addEventListener('click', () => {
                const scriptType = commitModal.dataset.scriptType;
                const comment = document.getElementById('commit-comment')?.value || '';
                this.commitToRegistry(scriptType, comment);
            });
        }
        
        // Close modals on overlay click
        document.querySelectorAll('.modal-overlay').forEach(overlay => {
            overlay.addEventListener('click', (e) => {
                if (e.target === overlay) {
                    overlay.style.display = 'none';
                }
            });
        });
    };
    
    // ==========================================
    // Sync Status Management
    // ==========================================
    
 ESSWorkbench.prototype.checkSyncStatus = async function(scriptType) {
        // Delegate to v2 Tcl-based sync status (single round trip)
        if (this.registrySyncStatus) {
            await this.registrySyncStatus();
        }
    };
    
    ESSWorkbench.prototype.updateSyncStatusUI = function() {
        // Variants tab status
        this.updateSingleSyncStatus('variants-sync-status', 'variants');
        
        // Scripts tab status  
        this.updateSingleSyncStatus('scripts-sync-status', this.currentScript);
        
        // Script sidebar dots
        document.querySelectorAll('.script-status-dot').forEach(dot => {
            const script = dot.dataset.script;
            const status = this.scriptSyncStatus[script] || '';
            dot.className = 'script-status-dot ' + status;
        });

	// Disable commit buttons when synced
	const variantsStatus = this.scriptSyncStatus['variants'] || 'unknown';
	const variantCommitBtn = document.getElementById('variant-commit-btn');
	if (variantCommitBtn) {
	    variantCommitBtn.disabled = (variantsStatus === 'synced');
	}
	
	const currentScriptStatus = this.scriptSyncStatus[this.currentScript] || 'unknown';
	const scriptCommitBtn = document.getElementById('script-commit-btn');
	if (scriptCommitBtn) {
	    scriptCommitBtn.disabled = (currentScriptStatus === 'synced');
	}
	
    };
    
    ESSWorkbench.prototype.updateSingleSyncStatus = function(elementId, scriptType) {
        const el = document.getElementById(elementId);
        if (!el) return;
        
        const status = this.scriptSyncStatus[scriptType] || 'unknown';
        el.className = 'sync-status ' + status;
        
        const text = el.querySelector('.sync-text');
        if (text) {
            const labels = {
                'synced': 'Synced',
                'modified': 'Modified', 
                'outdated': 'Update Available',
                'conflict': 'Conflict',
                'checking': 'Checking...',
                'unknown': '—'
            };
            text.textContent = labels[status] || status;
        }
    };
    
    // ==========================================
    // Pull/Commit Operations
    // ==========================================
    
    ESSWorkbench.prototype.pullFromRegistry = async function(scriptType) {
        if (!confirm('Pull latest from registry? This will overwrite local base files and reload.')) {
            return;
        }
        
        try {
            // Use v2 Tcl-based sync (pulls all scripts for current system)
            await this.registrySync();
            
            // Re-publish snapshot to pick up new file contents
            if (this.connection?.ws?.readyState === WebSocket.OPEN) {
                this.connection.ws.send(JSON.stringify({
                    cmd: 'eval',
                    script: 'ess::publish_snapshot'
                }));
            }
        } catch(err) {
            // registrySync already shows notification
            console.error('Pull failed:', err);
        }
    };
    
    ESSWorkbench.prototype.showCommitDialog = function(scriptType) {
        if (!this.currentUser) {
            alert('Please select a user first');
            return;
        }
        
        const modal = document.getElementById('commit-modal');
        if (!modal) return;
        
        const fileEl = document.getElementById('commit-file');
        if (fileEl) {
            fileEl.textContent = `File: ${scriptType}.tcl`;
        }
        
        modal.dataset.scriptType = scriptType;
        document.getElementById('commit-comment').value = '';
        modal.style.display = 'flex';
    };
    
   ESSWorkbench.prototype.commitToRegistry = async function(scriptType, comment) {
        if (!scriptType) {
            alert('No script type specified');
            return;
        }
        
        try {
            // Use v2 Tcl-based commit (base → registry)
            await this.registryCommit(scriptType, { comment: comment || '' });
            
            // Close commit modal
            const modal = document.getElementById('commit-modal');
            if (modal) modal.style.display = 'none';
            
        } catch(err) {
            // registryCommit already shows notification
            console.error('Commit failed:', err);
        }
    };
    
    // ==========================================
    // Registry Tab
    // ==========================================
    
    // Override switchTab to load registry data
    const originalSwitchTab = ESSWorkbench.prototype.switchTab;
    ESSWorkbench.prototype.switchTab = function(tabName) {
        originalSwitchTab.call(this, tabName);
        
        if (tabName === 'registry') {
            this.loadRegistryTab();
        }

	// Touch snapshot to refresh sync status for scripts or variants tabs
	if ((tabName === 'scripts' || tabName === 'variants') && this.connection?.ws?.readyState === WebSocket.OPEN) {
            this.connection.ws.send(JSON.stringify({
		cmd: 'touch',
		name: 'ess/snapshot'
            }));
	}	
    };
    
    ESSWorkbench.prototype.loadRegistryTab = async function() {
        await Promise.all([
            this.loadWorkgroupSystems(),
            this.loadTemplates(),
            this.loadWorkgroupUsers(),
            this.loadBackupStatus(),
            this.loadLocks()
        ]);
    };
    
    ESSWorkbench.prototype.loadWorkgroupSystems = async function() {
        const container = document.getElementById('workgroup-systems-list');
        if (!container) return;
        
        try {
            const systems = await this.registry.getSystems();
            
            if (!systems || systems.length === 0) {
                container.innerHTML = '<div class="empty-state-small">No systems in this workgroup</div>';
                return;
            }
            
            container.innerHTML = systems.map(sys => `
                <div class="system-card">
                    <div class="system-card-info">
                        <span class="system-card-name">${this.escapeHtml(sys.name)}</span>
                        <span class="system-card-meta">
                            v${sys.version || '1.0'} · ${sys.protocols?.length || 0} protocols
                        </span>
                    </div>
                </div>
            `).join('');
            
        } catch (err) {
            container.innerHTML = `<div class="empty-state-small" style="color: var(--wb-error);">Failed to load</div>`;
        }
    };
    
    ESSWorkbench.prototype.loadTemplates = async function() {
        const container = document.getElementById('templates-list');
        if (!container) return;
        
        try {
            // Get both templates and current workgroup systems
            const [templates, systems] = await Promise.all([
                this.registry.getTemplates(),
                this.registry.getSystems()
            ]);
            
            if (!templates || templates.length === 0) {
                container.innerHTML = '<div class="empty-state-small">No templates available</div>';
                return;
            }
            
            // Get names of systems already in workgroup
            const existingNames = new Set((systems || []).map(s => s.name));
            
            container.innerHTML = templates.map(tmpl => {
                const alreadyAdded = existingNames.has(tmpl.name);
                return `
                    <div class="template-card ${alreadyAdded ? 'already-added' : ''}">
                        <div>
                            <span class="template-card-name">${this.escapeHtml(tmpl.name)}</span>
                            <span class="template-card-version">v${tmpl.version || '1.0'}</span>
                        </div>
                        ${alreadyAdded 
                            ? '<span class="template-added-badge">Added</span>'
                            : `<button class="btn btn-xs btn-primary" onclick="workbench.addTemplateToWorkgroup('${tmpl.name}')">Add</button>`
                        }
                    </div>
                `;
            }).join('');
            
        } catch (err) {
            container.innerHTML = '<div class="empty-state-small">Failed to load templates</div>';
        }
    };
    
    ESSWorkbench.prototype.loadWorkgroupUsers = async function() {
        const container = document.getElementById('users-list');
        if (!container) return;
        
        try {
            const users = await this.registry.getUsers();
            
            if (!users || users.length === 0) {
                container.innerHTML = '<div class="empty-state-small">No users</div>';
                return;
            }
            
            container.innerHTML = users.map(user => `
                <div class="user-item">
                    <div class="user-item-info">
                        <div class="user-avatar">${this.getInitials(user.fullName || user.username)}</div>
                        <div class="user-details">
                            <span class="user-name">${this.escapeHtml(user.fullName || user.username)}</span>
                            <span class="user-role">${user.role || 'editor'}</span>
                        </div>
                    </div>
                    <button class="btn btn-xs btn-ghost" onclick="workbench.removeUser('${user.username}')">
                        ✕
                    </button>
                </div>
            `).join('');
            
        } catch (err) {
            container.innerHTML = '<div class="empty-state-small">Failed to load users</div>';
        }
    };
    
    ESSWorkbench.prototype.loadBackupStatus = async function() {
        const container = document.getElementById('backup-status');
        if (!container) return;
        
        try {
            const resp = await fetch('/api/v1/ess/admin/backups');
            if (!resp.ok) throw new Error('Failed');
            const data = await resp.json();
            
            const backups = data.backups || [];
            
            container.innerHTML = `
                <div class="backup-info">
                    <div class="backup-stat">
                        <span class="backup-stat-label">Backups</span>
                        <span class="backup-stat-value">${backups.length}</span>
                    </div>
                    <div class="backup-stat">
                        <span class="backup-stat-label">Last</span>
                        <span class="backup-stat-value">${backups[0] ? this.formatRelativeTime(backups[0].createdAt) : 'Never'}</span>
                    </div>
                </div>
            `;
        } catch (err) {
            container.innerHTML = '<div class="empty-state-small">Unable to check</div>';
        }
    };
    
    ESSWorkbench.prototype.loadLocks = async function() {
        const container = document.getElementById('locks-list');
        if (!container) return;
        
        try {
            const resp = await this.registry.getLocks();
            const locks = resp.locks || [];
            
            if (locks.length === 0) {
                container.innerHTML = '<div class="empty-state-small">No active locks</div>';
                return;
            }
            
            container.innerHTML = locks.map(lock => `
                <div class="lock-item">
                    <span class="lock-key">${this.escapeHtml(lock.key)}</span>
                    <span class="lock-meta">by ${lock.lockedBy}</span>
                </div>
            `).join('');
        } catch (err) {
            container.innerHTML = '<div class="empty-state-small">Unable to check</div>';
        }
    };
    
    // Registry actions
    ESSWorkbench.prototype.addTemplateToWorkgroup = async function(templateName) {
        if (!this.currentUser) {
            alert('Please select a user first');
            return;
        }
        
        try {
            await this.registry.addToWorkgroup(templateName, 'latest');
            alert(`Added ${templateName} to ${this.workgroup}`);
            this.loadWorkgroupSystems();
            this.loadTemplates();  // Refresh to show "Added" badge
        } catch (err) {
            alert(`Failed: ${err.message}`);
        }
    };
    
    ESSWorkbench.prototype.removeUser = async function(username) {
        if (!confirm(`Remove ${username}?`)) return;
        
        try {
            await this.registry.deleteUser(username);
            this.loadWorkgroupUsers();
            const users = await this.registry.getUsers();
            this.populateUserSelect(users);
        } catch (err) {
            alert(`Failed: ${err.message}`);
        }
    };
    
    ESSWorkbench.prototype.submitAddUser = async function() {
        const username = document.getElementById('new-user-username')?.value.trim();
        const fullName = document.getElementById('new-user-fullname')?.value.trim();
        const email = document.getElementById('new-user-email')?.value.trim();
        const role = document.getElementById('new-user-role')?.value || 'editor';
        
        if (!username) {
            alert('Username required');
            return;
        }
        
        try {
            await this.registry.addUser(username, fullName, email, role);
            document.getElementById('add-user-modal').style.display = 'none';
            this.loadWorkgroupUsers();
            const users = await this.registry.getUsers();
            this.populateUserSelect(users);
            
            // Clear form
            document.getElementById('new-user-username').value = '';
            document.getElementById('new-user-fullname').value = '';
            document.getElementById('new-user-email').value = '';
        } catch (err) {
            alert(`Failed: ${err.message}`);
        }
    };
    
    ESSWorkbench.prototype.createBackup = async function() {
        try {
            const resp = await fetch('/api/v1/ess/admin/backup', { method: 'POST' });
            if (!resp.ok) throw new Error('Failed');
            const data = await resp.json();
            alert(`Backup created: ${data.backup}`);
            this.loadBackupStatus();
        } catch (err) {
            alert(`Failed: ${err.message}`);
        }
    };
    
    // Utility methods
    ESSWorkbench.prototype.getInitials = function(name) {
        if (!name) return '?';
        return name.split(' ')
            .map(p => p[0])
            .join('')
            .toUpperCase()
            .slice(0, 2);
    };
    
    ESSWorkbench.prototype.formatRelativeTime = function(timestamp) {
        if (!timestamp) return 'Never';
        
        const date = new Date(timestamp);
        const now = new Date();
        const diff = now - date;
        
        const minutes = Math.floor(diff / 60000);
        const hours = Math.floor(diff / 3600000);
        const days = Math.floor(diff / 86400000);
        
        if (minutes < 1) return 'Just now';
        if (minutes < 60) return `${minutes}m ago`;
        if (hours < 24) return `${hours}h ago`;
        if (days < 7) return `${days}d ago`;
        
        return date.toLocaleDateString();
    };
    
    // Hook into snapshot handling to trigger sync check
    const originalHandleSnapshot = ESSWorkbench.prototype.handleSnapshot;
    if (originalHandleSnapshot) {
        ESSWorkbench.prototype.handleSnapshot = function(data) {
            originalHandleSnapshot.call(this, data);
            
            // Check sync status after snapshot update
            if (this.registry && this.snapshot) {
                this.checkSyncStatus();
            }
        };
    }
    
    // Hook into saveVariantsScript to update sync status after save
    const originalSaveVariantsScript = ESSWorkbench.prototype.saveVariantsScript;
    if (originalSaveVariantsScript) {
        ESSWorkbench.prototype.saveVariantsScript = async function(andReload = false) {
            // Call original save
            await originalSaveVariantsScript.call(this, andReload);
            
            // Mark as modified (local changes not yet in registry)
            this.scriptSyncStatus['variants'] = 'modified';
            this.updateSyncStatusUI();
        };
    }
    
    // Pull and apply — pullFromRegistry now handles sync + reload via Tcl backend
    ESSWorkbench.prototype.pullAndApply = async function(scriptType) {
        await this.pullFromRegistry(scriptType);
    };
    
    // ==========================================
    // Scripts Tab - Save/Pull/Commit
    // ==========================================
    
    /**
     * Save current script to local filesystem via dserv
     */
    ESSWorkbench.prototype.saveCurrentScript = async function() {
        if (!this.editor?.view) {
            alert('No editor available');
            return;
        }
        
        const scriptType = this.currentScript;
        const content = this.editor.getValue();
        
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
            
            // Send save command to dserv
            if (this.connection?.ws?.readyState === WebSocket.OPEN) {
                const saveCmd = `ess::save_script ${scriptType} {${content}}`;
                this.connection.ws.send(JSON.stringify({
                    cmd: 'eval',
                    script: saveCmd
                }));
                
                console.log(`Saved ${scriptType} script to local filesystem`);
                
                // Update tracking
                this.scripts[scriptType] = content;
                
                // Mark as modified (local save, not yet in registry)
                this.scriptSyncStatus[scriptType] = 'modified';
                this.updateSyncStatusUI();
                
                // Show success
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
    };
    
    /**
     * Pull script from registry — delegates to pullFromRegistry which
     * handles confirmation, sync, and snapshot refresh via Tcl backend
     */
    ESSWorkbench.prototype.pullScriptFromRegistry = async function() {
        await this.pullFromRegistry(this.currentScript);
    };
    
    // Override pull button to use pullAndApply
    const originalBindRegistryEvents = ESSWorkbench.prototype.bindRegistryEvents;
    ESSWorkbench.prototype.bindRegistryEvents = function() {
        originalBindRegistryEvents.call(this);
        
        // Re-bind pull button to use pullAndApply
        const pullBtn = document.getElementById('variant-pull-btn');
        if (pullBtn) {
            // Remove old listener by cloning
            const newPullBtn = pullBtn.cloneNode(true);
            pullBtn.parentNode.replaceChild(newPullBtn, pullBtn);
            
            newPullBtn.addEventListener('click', async () => {
                const confirmed = confirm(
                    'Pull latest from registry?\n\n' +
                    'This will replace your local changes and reload the system.'
                );
                if (confirmed) {
                    await this.pullAndApply('variants');
                }
            });
        }
    };
    
})();
