/**
 * ESS Registry Plugin
 * 
 * Consolidates all registry functionality (formerly split across
 * ess_registry_integration.js and ess_registry_integration_v2.js).
 *
 * Handles: user management, sync status, pull/commit, registry tab,
 * system browser, registry datapoint subscriptions.
 */

const RegistryPlugin = {

    // ==========================================
    // Lifecycle Hooks
    // ==========================================

    onInit(wb) {
        wb.currentUser = null;
        wb.workgroup = null;
        wb.scriptSyncStatus = {};
        wb.registryState = {
            connected: false,
            workgroup: null,
            user: null,
            systems: [],
            currentSystemLocked: false
        };

        this._bindRegistryEvents(wb);
        this._bindModalEvents(wb);
    },

    onConnected(wb) {
        this._setupRegistryDatapoints(wb);
    },

    async onRegistryReady(wb) {
        if (!wb.registry) return;

        const wgEl = document.getElementById('workgroup-name');
        if (wgEl && wb.registry.workgroup) {
            wgEl.textContent = wb.registry.workgroup;
            wb.workgroup = wb.registry.workgroup;
        }

        wb.currentUser = wb.registry.getUser();

        try {
            const users = await wb.registry.getUsers();
            this._populateUserSelect(wb, users);
            this._updateRegistryStatus(wb, 'connected');
        } catch (err) {
            console.warn('Registry connection failed:', err);
            this._updateRegistryStatus(wb, 'error', err.message);
        }
    },

    onSnapshot(wb, snapshot) {
        if (wb.registry && snapshot) {
            this._checkSyncStatus(wb);
        }
    },

    onTabSwitch(wb, tabName) {
        if (tabName === 'registry') {
            this._loadRegistryTab(wb);
        }

        if ((tabName === 'scripts' || tabName === 'variants') && wb.connection?.ws?.readyState === WebSocket.OPEN) {
            wb.connection.ws.send(JSON.stringify({
                cmd: 'touch',
                name: 'ess/snapshot'
            }));
        }
    },

    onScriptSelect(wb, scriptName) {
        setTimeout(() => {
            this._updateSingleSyncStatus(wb, 'scripts-sync-status', scriptName);
        }, 50);
    },

    onVariantsSaved(wb, content) {
        wb.scriptSyncStatus['variants'] = 'modified';
        this._updateSyncStatusUI(wb);
    },

    // Override pull — handled here
    async onPullScript(wb, scriptType) {
        if (!confirm('Pull latest from registry? This will overwrite local base files and reload.')) {
            return false;
        }

        try {
            await this._registrySync(wb);

            if (wb.connection?.ws?.readyState === WebSocket.OPEN) {
                wb.connection.ws.send(JSON.stringify({
                    cmd: 'eval',
                    script: 'ess::publish_snapshot'
                }));
            }
        } catch (err) {
            console.error('Pull failed:', err);
        }
        return false;
    },

    // Override commit dialog — require user
    onShowCommitDialog(wb, scriptType) {
        if (!wb.currentUser) {
            alert('Please select a user first');
            return false;
        }
        return true; // let default handler show the modal
    },

    // Override commit — handled here via Tcl
    async onCommitScript(wb, scriptType, comment) {
        if (!scriptType) {
            alert('No script type specified');
            return false;
        }

        try {
            await this._registryCommit(wb, scriptType, { comment: comment || '' });
            const modal = document.getElementById('commit-modal');
            if (modal) modal.style.display = 'none';
        } catch (err) {
            console.error('Commit failed:', err);
        }
        return false;
    },

    // ==========================================
    // Event Bindings
    // ==========================================

    _bindRegistryEvents(wb) {
        const userSelect = document.getElementById('user-select');
        if (userSelect) {
            userSelect.addEventListener('change', (e) => {
                wb.currentUser = e.target.value;
                if (wb.registry) {
                    wb.registry.setUser(wb.currentUser);
                }
            });
        }

        document.getElementById('variant-pull-btn')?.addEventListener('click', async () => {
            const confirmed = confirm(
                'Pull latest from registry?\n\n' +
                'This will replace your local changes and reload the system.'
            );
            if (confirmed) {
                await wb.pullScriptFromRegistry();
            }
        });

        document.getElementById('variant-commit-btn')?.addEventListener('click', () => {
            wb.showCommitDialog('variants');
        });

        document.getElementById('script-pull-btn')?.addEventListener('click', () => {
            wb.pullScriptFromRegistry();
        });

        document.getElementById('script-commit-btn')?.addEventListener('click', () => {
            wb.showCommitDialog(wb.currentScript);
        });

        document.getElementById('script-save-btn')?.addEventListener('click', () => {
            wb.saveCurrentScript();
        });

        document.getElementById('add-user-btn')?.addEventListener('click', () => {
            document.getElementById('add-user-modal').style.display = 'flex';
        });

        document.getElementById('backup-now-btn')?.addEventListener('click', () => {
            this._createBackup(wb);
        });
    },

    _bindModalEvents(wb) {
        const addUserModal = document.getElementById('add-user-modal');
        if (addUserModal) {
            document.getElementById('add-user-modal-close')?.addEventListener('click', () => {
                addUserModal.style.display = 'none';
            });
            document.getElementById('add-user-cancel')?.addEventListener('click', () => {
                addUserModal.style.display = 'none';
            });
            document.getElementById('add-user-submit')?.addEventListener('click', () => {
                this._submitAddUser(wb);
            });
        }

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
                wb.commitToRegistry(scriptType, comment);
            });
        }

        document.querySelectorAll('.modal-overlay').forEach(overlay => {
            overlay.addEventListener('click', (e) => {
                if (e.target === overlay) overlay.style.display = 'none';
            });
        });
    },

    // ==========================================
    // Registry Datapoints
    // ==========================================

    _setupRegistryDatapoints(wb) {
        if (!wb.dpManager) return;

        ['ess/registry/url', 'ess/registry/workgroup', 'ess/registry/user',
         'ess/registry/last_pull', 'ess/registry/last_push', 'ess/registry/sync_status'
        ].forEach(dp => {
            wb.dpManager.subscribe(dp, (data) => {
                this._handleRegistryDatapoint(wb, dp, data);
            });
        });
    },

    _handleRegistryDatapoint(wb, name, data) {
        const value = data.data !== undefined ? data.data : data.value;

        switch (name) {
            case 'ess/registry/url':
                if (value && wb.registry) wb.registry.baseUrl = value;
                break;
            case 'ess/registry/workgroup':
                wb.registryState.workgroup = value;
                if (wb.registry) wb.registry.setWorkgroup(value);
                this._updateWorkgroupDisplay(wb, value);
                break;
            case 'ess/registry/user':
                wb.registryState.user = value;
                if (wb.registry) wb.registry.setUser(value);
                break;
            case 'ess/registry/sync_status':
                if (value) {
                    try {
                        const status = typeof value === 'string' ? JSON.parse(value) : value;
                        if (typeof status === 'object') {
                            Object.assign(wb.scriptSyncStatus, status);
                            this._updateSyncStatusUI(wb);
                        }
                    } catch (e) {
                        console.warn('Could not parse sync status:', e);
                    }
                }
                break;
        }
    },

    // ==========================================
    // Sync Status
    // ==========================================

    async _checkSyncStatus(wb) {
        try {
            const result = await wb.execTclCmd('ess::sync_status');

            if (typeof result === 'string') {
                const parts = result.split(/\s+/);
                for (let i = 0; i < parts.length - 1; i += 2) {
                    wb.scriptSyncStatus[parts[i]] = parts[i + 1];
                }
            } else if (typeof result === 'object') {
                Object.assign(wb.scriptSyncStatus, result);
            }

            this._updateSyncStatusUI(wb);
        } catch (err) {
            console.warn('Sync status check failed:', err);
        }
    },

    _updateSyncStatusUI(wb) {
        this._updateSingleSyncStatus(wb, 'variants-sync-status', 'variants');
        this._updateSingleSyncStatus(wb, 'scripts-sync-status', wb.currentScript);

        document.querySelectorAll('.script-status-dot[data-script]').forEach(dot => {
            const script = dot.dataset.script;
            const status = wb.scriptSyncStatus[script] || '';
            dot.className = 'script-status-dot ' + status;
        });

        const variantsStatus = wb.scriptSyncStatus['variants'] || 'unknown';
        const variantCommitBtn = document.getElementById('variant-commit-btn');
        if (variantCommitBtn) variantCommitBtn.disabled = (variantsStatus === 'synced');

        const currentScriptStatus = wb.scriptSyncStatus[wb.currentScript] || 'unknown';
        const scriptCommitBtn = document.getElementById('script-commit-btn');
        if (scriptCommitBtn) scriptCommitBtn.disabled = (currentScriptStatus === 'synced');
    },

    _updateSingleSyncStatus(wb, elementId, scriptType) {
        const el = document.getElementById(elementId);
        if (!el) return;

        const status = wb.scriptSyncStatus[scriptType] || 'unknown';
        el.className = 'sync-status ' + status;

        const text = el.querySelector('.sync-text');
        if (text) {
            const labels = {
                'synced': 'Synced', 'modified': 'Modified',
                'outdated': 'Update Available', 'conflict': 'Conflict',
                'checking': 'Checking...', 'unknown': '—'
            };
            text.textContent = labels[status] || status;
        }
    },

    // ==========================================
    // Registry Operations
    // ==========================================

    async _registrySync(wb) {
        try {
            const result = await wb.execTclCmd('ess::sync_system $::ess::current(system)');
            const pulled = result.pulled || 0;
            const unchanged = result.unchanged || 0;

            if (pulled > 0) {
                wb.showNotification(`Synced: ${pulled} updated, ${unchanged} unchanged`, 'success');
            } else {
                wb.showNotification('Already up to date', 'info');
            }

            await this._checkSyncStatus(wb);
            return result;
        } catch (err) {
            wb.showNotification(`Sync failed: ${err.message}`, 'error');
            throw err;
        }
    },

    async _registryCommit(wb, type, options = {}) {
        const comment = options.comment || '';
        const safeComment = comment.replace(/[{}]/g, '');
        const cmd = `ess::commit_script ${type} {${safeComment}}`;

        try {
            const result = await wb.execTclCmd(cmd);
            wb.showNotification(`Committed ${type} to registry`, 'success');
            wb.scriptSyncStatus[type] = 'synced';
            this._updateSyncStatusUI(wb);
            return result;
        } catch (err) {
            if (err.message.includes('conflict')) {
                wb.showNotification('Conflict: registry has a newer version. Sync first.', 'error');
                wb.scriptSyncStatus[type] = 'conflict';
                this._updateSyncStatusUI(wb);
            } else if (err.message.includes('viewer')) {
                wb.showNotification('Permission denied: viewers cannot commit to registry.', 'error');
            } else {
                wb.showNotification(`Commit failed: ${err.message}`, 'error');
            }
            throw err;
        }
    },

    async _registryCommitAll(wb, options = {}) {
        const comment = options.comment || '';
        const safeComment = comment.replace(/[{}]/g, '');
        const cmd = `ess::commit_system {${safeComment}}`;

        try {
            const result = await wb.execTclCmd(cmd);
            const committed = result.committed || [];
            const errors = result.errors || [];

            if (errors.length > 0) {
                wb.showNotification(`Committed ${committed.length}, ${errors.length} error(s)`, 'warning');
            } else {
                wb.showNotification(`Committed ${committed.length} script(s) to registry`, 'success');
            }

            await this._checkSyncStatus(wb);
            return result;
        } catch (err) {
            wb.showNotification(`Commit failed: ${err.message}`, 'error');
            throw err;
        }
    },

    // ==========================================
    // UI Helpers
    // ==========================================

    _populateUserSelect(wb, users) {
        const select = document.getElementById('user-select');
        if (!select) return;

        select.innerHTML = '<option value="">Select user...</option>';

        users.forEach(user => {
            const opt = document.createElement('option');
            opt.value = user.username;
            opt.textContent = user.fullName || user.username;
            if (user.username === wb.currentUser) opt.selected = true;
            select.appendChild(opt);
        });
    },

    _updateRegistryStatus(wb, status, message) {
        const el = document.getElementById('registry-status');
        if (!el) return;

        el.className = 'footer-item registry-status ' + status;
        const text = el.querySelector('span');
        if (text) {
            text.textContent = status === 'connected' ?
                `Registry: ${wb.workgroup}` :
                `Registry: ${message || 'disconnected'}`;
        }
    },

    _updateWorkgroupDisplay(wb, workgroup) {
        const el = document.getElementById('workgroup-name');
        if (el) el.textContent = workgroup || 'Not set';
    },

    // ==========================================
    // Registry Tab
    // ==========================================

    async _loadRegistryTab(wb) {
        await Promise.all([
            this._loadWorkgroupSystems(wb),
            this._loadTemplates(wb),
            this._loadWorkgroupUsers(wb),
            this._loadBackupStatus(wb),
            this._loadLocks(wb)
        ]);
    },

    async _loadWorkgroupSystems(wb) {
        const container = document.getElementById('workgroup-systems-list');
        if (!container || !wb.registry) return;

        try {
            const systems = await wb.registry.getSystems();

            if (!systems || systems.length === 0) {
                container.innerHTML = '<div class="empty-state-small">No systems in this workgroup</div>';
                return;
            }

            container.innerHTML = systems.map(sys => `
                <div class="system-card">
                    <div class="system-card-info">
                        <span class="system-card-name">${wb.escapeHtml(sys.name)}</span>
                        <span class="system-card-meta">
                            v${sys.version || '1.0'} · ${sys.protocols?.length || 0} protocols
                        </span>
                    </div>
                </div>
            `).join('');
        } catch (err) {
            container.innerHTML = '<div class="empty-state-small" style="color: var(--wb-error);">Failed to load</div>';
        }
    },

    async _loadTemplates(wb) {
        const container = document.getElementById('templates-list');
        if (!container || !wb.registry) return;

        try {
            const [templates, systems] = await Promise.all([
                wb.registry.getTemplates(),
                wb.registry.getSystems()
            ]);

            if (!templates || templates.length === 0) {
                container.innerHTML = '<div class="empty-state-small">No templates available</div>';
                return;
            }

            const existingNames = new Set((systems || []).map(s => s.name));

            container.innerHTML = templates.map(tmpl => {
                const alreadyAdded = existingNames.has(tmpl.name);
                return `
                    <div class="template-card ${alreadyAdded ? 'already-added' : ''}">
                        <div>
                            <span class="template-card-name">${wb.escapeHtml(tmpl.name)}</span>
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
    },

    async _loadWorkgroupUsers(wb) {
        const container = document.getElementById('users-list');
        if (!container || !wb.registry) return;

        try {
            const users = await wb.registry.getUsers();

            if (!users || users.length === 0) {
                container.innerHTML = '<div class="empty-state-small">No users</div>';
                return;
            }

            container.innerHTML = users.map(user => `
                <div class="user-item">
                    <div class="user-item-info">
                        <div class="user-avatar">${this._getInitials(user.fullName || user.username)}</div>
                        <div class="user-details">
                            <span class="user-name">${wb.escapeHtml(user.fullName || user.username)}</span>
                            <span class="user-role">${user.role || 'editor'}</span>
                        </div>
                    </div>
                    <button class="btn btn-xs btn-ghost" onclick="workbench._registryRemoveUser('${user.username}')">
                        ✕
                    </button>
                </div>
            `).join('');
        } catch (err) {
            container.innerHTML = '<div class="empty-state-small">Failed to load users</div>';
        }
    },

    async _loadBackupStatus(wb) {
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
                        <span class="backup-stat-value">${backups[0] ? this._formatRelativeTime(backups[0].createdAt) : 'Never'}</span>
                    </div>
                </div>
            `;
        } catch (err) {
            container.innerHTML = '<div class="empty-state-small">Unable to check</div>';
        }
    },

    async _loadLocks(wb) {
        const container = document.getElementById('locks-list');
        if (!container || !wb.registry) return;

        try {
            const resp = await wb.registry.getLocks();
            const locks = resp.locks || [];

            if (locks.length === 0) {
                container.innerHTML = '<div class="empty-state-small">No active locks</div>';
                return;
            }

            container.innerHTML = locks.map(lock => `
                <div class="lock-item">
                    <span class="lock-key">${wb.escapeHtml(lock.key)}</span>
                    <span class="lock-meta">by ${lock.lockedBy}</span>
                </div>
            `).join('');
        } catch (err) {
            container.innerHTML = '<div class="empty-state-small">Unable to check</div>';
        }
    },

    // ==========================================
    // User / Template / Backup Actions
    // ==========================================

    async _submitAddUser(wb) {
        const username = document.getElementById('new-user-username')?.value.trim();
        const fullName = document.getElementById('new-user-fullname')?.value.trim();
        const email = document.getElementById('new-user-email')?.value.trim();
        const role = document.getElementById('new-user-role')?.value || 'editor';

        if (!username) { alert('Username required'); return; }

        try {
            await wb.registry.addUser(username, fullName, email, role);
            document.getElementById('add-user-modal').style.display = 'none';
            this._loadWorkgroupUsers(wb);
            const users = await wb.registry.getUsers();
            this._populateUserSelect(wb, users);

            document.getElementById('new-user-username').value = '';
            document.getElementById('new-user-fullname').value = '';
            document.getElementById('new-user-email').value = '';
        } catch (err) {
            alert(`Failed: ${err.message}`);
        }
    },

    async _createBackup(wb) {
        try {
            const resp = await fetch('/api/v1/ess/admin/backup', { method: 'POST' });
            if (!resp.ok) throw new Error('Failed');
            const data = await resp.json();
            alert(`Backup created: ${data.backup}`);
            this._loadBackupStatus(wb);
        } catch (err) {
            alert(`Failed: ${err.message}`);
        }
    },

    // ==========================================
    // System Browser
    // ==========================================

    async _showSystemBrowser(wb) {
        let modal = document.getElementById('system-browser-modal');

        if (!modal) {
            modal = this._createSystemBrowserModal(wb);
            document.body.appendChild(modal);
        }

        modal.style.display = 'flex';
        await this._loadSystemBrowserData(wb);
    },

    _createSystemBrowserModal(wb) {
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

        modal.querySelectorAll('.tab-btn').forEach(btn => {
            btn.addEventListener('click', () => {
                modal.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
                btn.classList.add('active');
                modal.querySelectorAll('.browser-content').forEach(c => c.style.display = 'none');
                document.getElementById(`browser-${btn.dataset.browserTab}`).style.display = 'block';
            });
        });

        return modal;
    },

    async _loadSystemBrowserData(wb) {
        try {
            const systemsResult = await wb.execTclCmd('ess::registry::list_systems');
            this._renderWorkgroupSystems(wb, systemsResult);
        } catch (err) {
            document.getElementById('browser-workgroup').innerHTML =
                `<div class="empty-state-small">Failed to load: ${err.message}</div>`;
        }

        try {
            const templatesResult = await wb.execTclCmd('ess::registry::list_templates');
            this._renderTemplates(wb, templatesResult);
        } catch (err) {
            document.getElementById('browser-templates').innerHTML =
                `<div class="empty-state-small">Failed to load: ${err.message}</div>`;
        }
    },

    _renderWorkgroupSystems(wb, data) {
        const container = document.getElementById('browser-workgroup');
        const systems = data.systems || [];

        if (systems.length === 0) {
            container.innerHTML = '<div class="empty-state-small">No systems in workgroup</div>';
            return;
        }

        const grouped = {};
        systems.forEach(sys => {
            if (!grouped[sys.name]) grouped[sys.name] = [];
            grouped[sys.name].push(sys);
        });

        let html = '<div class="system-list">';
        for (const [name, versions] of Object.entries(grouped)) {
            const latest = versions[0];
            const protocols = latest.protocols || [];

            html += `
                <div class="system-browser-item" data-system="${wb.escapeHtml(name)}">
                    <div class="system-item-header">
                        <span class="system-name">${wb.escapeHtml(name)}</span>
                        <span class="system-meta">${protocols.length} protocol(s)</span>
                    </div>
                    <div class="system-protocols">
                        ${protocols.map(p => `
                            <button class="protocol-chip"
                                    onclick="workbench._registryOpenSystemProtocol('${wb.escapeHtml(name)}', '${wb.escapeHtml(p)}')">
                                ${wb.escapeHtml(p)}
                            </button>
                        `).join('')}
                    </div>
                    ${latest.description ? `<div class="system-description">${wb.escapeHtml(latest.description)}</div>` : ''}
                </div>
            `;
        }
        html += '</div>';
        container.innerHTML = html;
    },

    _renderTemplates(wb, data) {
        const container = document.getElementById('browser-templates');
        const templates = data.systems || [];

        if (templates.length === 0) {
            container.innerHTML = '<div class="empty-state-small">No templates available</div>';
            return;
        }

        let html = '<div class="template-list">';
        templates.forEach(tmpl => {
            const alreadyAdded = wb.registryState.systems.some(s => s.name === tmpl.name);
            html += `
                <div class="template-card ${alreadyAdded ? 'already-added' : ''}">
                    <div class="template-info">
                        <span class="template-name">${wb.escapeHtml(tmpl.name)}</span>
                        <span class="template-version">v${tmpl.version || '1.0'}</span>
                    </div>
                    ${alreadyAdded ?
                        '<span class="template-added-badge">Added</span>' :
                        `<button class="btn btn-sm btn-primary"
                                 onclick="workbench.addTemplateToWorkgroup('${wb.escapeHtml(tmpl.name)}')">
                            Add to Workgroup
                        </button>`
                    }
                </div>
            `;
        });
        html += '</div>';
        container.innerHTML = html;
    },

    // ==========================================
    // Utility
    // ==========================================

    _getInitials(name) {
        if (!name) return '?';
        return name.split(' ').map(p => p[0]).join('').toUpperCase().slice(0, 2);
    },

    _formatRelativeTime(timestamp) {
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
    }
};

// Expose action methods on workbench instance for onclick handlers in HTML
document.addEventListener('DOMContentLoaded', () => {
    setTimeout(() => {
        const wb = window.workbench;
        if (wb) {
            wb.addTemplateToWorkgroup = async function(templateName) {
                try {
                    const cmd = `ess::registry::add_template ${templateName}`;
                    await this.execTclCmd(cmd);
                    this.showNotification(`Added ${templateName} to workgroup`, 'success');
                } catch (err) {
                    this.showNotification(`Failed to add template: ${err.message}`, 'error');
                }
            };
            wb._registryRemoveUser = async function(username) {
                if (!confirm(`Remove ${username}?`)) return;
                try {
                    await this.registry.deleteUser(username);
                    RegistryPlugin._loadWorkgroupUsers(this);
                    const users = await this.registry.getUsers();
                    RegistryPlugin._populateUserSelect(this, users);
                } catch (err) {
                    alert(`Failed: ${err.message}`);
                }
            };
            wb._registryOpenSystemProtocol = async function(system, protocol) {
                try {
                    await this.execTclCmd(`ess::load_system ${system} ${protocol}`);
                    this.showNotification(`Loaded ${system}/${protocol}`, 'success');
                    const modal = document.getElementById('system-browser-modal');
                    if (modal) modal.style.display = 'none';
                } catch (err) {
                    this.showNotification(`Failed to load system: ${err.message}`, 'error');
                }
            };
        }
    }, 0);
});

ESSWorkbench.registerPlugin(RegistryPlugin);
