/**
 * ProjectSelector.js
 * 
 * Dropdown component for selecting the active project.
 * Placed in the status bar, affects filtering across the app.
 * 
 * In the new architecture, projects are REQUIRED - every config and queue
 * belongs to exactly one project. There is no "All Projects" view.
 * 
 * Features:
 *   - Project selection dropdown
 *   - Create new project
 *   - Registry sync status indicator
 *   - Push/Pull to/from registry
 * 
 * Subscribes to:
 *   - projects/list: Available projects
 *   - projects/active: Currently active project name
 *   - ess/registry/url: Registry URL (configured on backend)
 *   - ess/registry/workgroup: Registry workgroup (configured on backend)
 *   - ess/registry/sync_status: Sync state ("synced", "modified", "never_synced")
 * 
 * Sends commands via configs subprocess:
 *   - project_activate "name"
 *   - project_create "name"
 *   - registry_push "name"
 *   - registry_pull "name"
 */

class ProjectSelector {
    constructor(container, dpManager) {
        this.container = typeof container === 'string' 
            ? document.getElementById(container) 
            : container;
        this.dpManager = dpManager;
        
        this.state = {
            projects: [],
            activeProject: '',
            syncStatus: null,      // {status, message, lastSync}
            registryUrl: '',       // from ess/registry/url datapoint
            registryWorkgroup: ''  // from ess/registry/workgroup datapoint
        };
        
        this.render();
        this.bindEvents();
        this.setupSubscriptions();
    }
    
    render() {
        this.container.innerHTML = `
            <div class="project-selector">
                <label class="project-selector-label">Project:</label>
                <div class="project-selector-dropdown-wrapper">
                    <button class="project-selector-button" id="project-selector-btn">
                        <span class="project-selector-name" id="project-selector-name">-- Select Project --</span>
                        <span class="project-sync-indicator" id="project-sync-indicator" title="Sync status"></span>
                        <span class="project-selector-arrow">▾</span>
                    </button>
                    <div class="project-selector-dropdown" id="project-selector-dropdown">
                        <div class="project-dropdown-section">
                            <div class="project-dropdown-label">Projects</div>
                            <div class="project-dropdown-list" id="project-dropdown-list">
                                <!-- Project items rendered here -->
                            </div>
                        </div>
                        <div class="project-dropdown-divider"></div>
                        <button class="project-dropdown-action" id="project-new-btn">
                            <span class="project-action-icon">+</span>
                            New Project
                        </button>
                        <div class="project-dropdown-divider"></div>
                        <div class="project-dropdown-section">
                            <div class="project-dropdown-label">Registry</div>
                            <button class="project-dropdown-action" id="project-registry-push" disabled>
                                <span class="project-action-icon">↑</span>
                                Push to Registry
                            </button>
                            <button class="project-dropdown-action" id="project-registry-pull">
                                <span class="project-action-icon">↓</span>
                                Pull from Registry
                            </button>
                        </div>
                        <div class="project-sync-footer" id="project-sync-footer">
                            <span class="project-sync-label">Last sync:</span>
                            <span class="project-sync-time" id="project-sync-time">Never</span>
                        </div>
                    </div>
                </div>
            </div>
        `;
        
        this.elements = {
            button: this.container.querySelector('#project-selector-btn'),
            name: this.container.querySelector('#project-selector-name'),
            syncIndicator: this.container.querySelector('#project-sync-indicator'),
            dropdown: this.container.querySelector('#project-selector-dropdown'),
            list: this.container.querySelector('#project-dropdown-list'),
            newBtn: this.container.querySelector('#project-new-btn'),
            pushBtn: this.container.querySelector('#project-registry-push'),
            pullBtn: this.container.querySelector('#project-registry-pull'),
            syncFooter: this.container.querySelector('#project-sync-footer'),
            syncTime: this.container.querySelector('#project-sync-time')
        };
    }
    
    bindEvents() {
        // Toggle dropdown
        this.elements.button.addEventListener('click', (e) => {
            e.stopPropagation();
            this.toggleDropdown();
        });
        
        // Close dropdown when clicking outside
        document.addEventListener('click', (e) => {
            if (!this.container.contains(e.target)) {
                this.closeDropdown();
            }
        });
        
        // New project
        this.elements.newBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            this.closeDropdown();
            this.showCreateProjectDialog();
        });
        
        // Registry push
        this.elements.pushBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            this.closeDropdown();
            this.showRegistryPushDialog();
        });
        
        // Registry pull
        this.elements.pullBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            this.closeDropdown();
            this.showRegistryPullDialog();
        });
    }
    
    setupSubscriptions() {
        this.dpManager.subscribe('projects/list', (data) => {
            this.updateProjectList(data.value);
        });
        
        this.dpManager.subscribe('projects/active', (data) => {
            this.updateActiveProject(data.value);
        });
        
        // Registry configuration from backend
        this.dpManager.subscribe('ess/registry/url', (data) => {
            this.state.registryUrl = data.value || '';
            this.updateRegistryState();
        });
        
        this.dpManager.subscribe('ess/registry/workgroup', (data) => {
            this.state.registryWorkgroup = data.value || '';
            this.updateRegistryState();
        });
        
        // Sync status from backend (set to "modified" on local changes, "synced" on push/pull)
        this.dpManager.subscribe('ess/registry/sync_status', (data) => {
            this.updateSyncStatusFromDatapoint(data.value);
        });
    }
    
    // =========================================================================
    // Dropdown Management
    // =========================================================================
    
    toggleDropdown() {
        const isOpen = this.elements.dropdown.classList.toggle('open');
        if (isOpen) {
            this.updateDropdownState();
        }
    }
    
    closeDropdown() {
        this.elements.dropdown.classList.remove('open');
    }
    
    updateDropdownState() {
        const hasProject = !!this.state.activeProject;
        const hasRegistry = !!this.state.registryUrl;
        
        // Enable/disable push based on active project AND registry configured
        this.elements.pushBtn.disabled = !hasProject || !hasRegistry;
        
        // Pull only needs registry configured
        this.elements.pullBtn.disabled = !hasRegistry;
        
        // Update button titles for clarity
        if (!hasRegistry) {
            this.elements.pushBtn.title = 'Registry not configured';
            this.elements.pullBtn.title = 'Registry not configured';
        } else if (!hasProject) {
            this.elements.pushBtn.title = 'Select a project first';
            this.elements.pullBtn.title = 'Pull project from registry';
        } else {
            this.elements.pushBtn.title = 'Push project to registry';
            this.elements.pullBtn.title = 'Pull project from registry';
        }
    }
    
    /**
     * Called when registry config changes
     */
    updateRegistryState() {
        this.updateDropdownState();
    }
    
    // =========================================================================
    // Project List Management
    // =========================================================================
    
    updateProjectList(data) {
        try {
            const projects = typeof data === 'string' ? JSON.parse(data) : data;
            this.state.projects = Array.isArray(projects) ? projects : [];
            this.renderProjectList();
            
            // If no project is active but we have projects, activate the first one
            if (!this.state.activeProject && this.state.projects.length > 0) {
                const firstProject = this.state.projects[0].name;
                this.selectProject(firstProject);
            }
        } catch (e) {
            console.error('Failed to parse projects list:', e);
            this.state.projects = [];
            this.renderProjectList();
        }
    }
    
    updateActiveProject(data) {
        this.state.activeProject = data || '';
        
        // Update button text
        if (this.state.activeProject) {
            this.elements.name.textContent = this.state.activeProject;
            this.elements.name.classList.remove('placeholder');
        } else {
            this.elements.name.textContent = '-- Select Project --';
            this.elements.name.classList.add('placeholder');
        }
        
        // Update visual state
        this.container.querySelector('.project-selector').classList.toggle('no-project', !this.state.activeProject);
        
        // Re-render list to update selection
        this.renderProjectList();
    }
    
    renderProjectList() {
        const list = this.elements.list;
        const activeProject = this.state.activeProject;
        
        if (this.state.projects.length === 0) {
            list.innerHTML = '<div class="project-dropdown-empty">No projects yet</div>';
            return;
        }
        
        // Sort alphabetically
        const sorted = [...this.state.projects].sort((a, b) => 
            a.name.localeCompare(b.name)
        );
        
        list.innerHTML = sorted.map(p => `
            <button class="project-dropdown-item ${p.name === activeProject ? 'active' : ''}"
                    data-project="${this.escapeAttr(p.name)}">
                <span class="project-item-name">${this.escapeHtml(p.name)}</span>
                <span class="project-item-counts">
                    ${p.config_count || 0} configs, ${p.queue_count || 0} queues
                </span>
            </button>
        `).join('');
        
        // Bind click events
        list.querySelectorAll('.project-dropdown-item').forEach(item => {
            item.addEventListener('click', (e) => {
                e.stopPropagation();
                const projectName = item.dataset.project;
                this.selectProject(projectName);
                this.closeDropdown();
            });
        });
    }
    
    selectProject(projectName) {
        if (projectName) {
            this.sendCommand(`project_activate {${projectName}}`);
        }
    }
    
    // =========================================================================
    // Sync Status (driven by ess/registry/sync_status datapoint)
    // =========================================================================
    
    /**
     * Update sync status from datapoint value
     * Values: "synced", "modified", "never_synced", "error"
     */
    updateSyncStatusFromDatapoint(status) {
        this.state.syncStatus = status || '';
        this.updateSyncStatusDisplay(status);
    }
    
    updateSyncStatusDisplay(status) {
        const indicator = this.elements.syncIndicator;
        const timeEl = this.elements.syncTime;
        
        if (!status) {
            indicator.className = 'project-sync-indicator';
            indicator.title = 'Sync status unknown';
            timeEl.textContent = 'Unknown';
            return;
        }
        
        // Update indicator color
        indicator.className = 'project-sync-indicator sync-' + status;
        
        // Update tooltip and footer text
        switch (status) {
            case 'synced':
                indicator.title = 'In sync with registry';
                timeEl.textContent = 'Synced';
                break;
            case 'modified':
                indicator.title = 'Local changes not pushed';
                timeEl.textContent = 'Modified locally';
                break;
            case 'never_synced':
                indicator.title = 'Never synced to registry';
                timeEl.textContent = 'Never synced';
                break;
            case 'error':
                indicator.title = 'Sync error';
                timeEl.textContent = 'Error';
                break;
            default:
                indicator.title = status;
                timeEl.textContent = status;
        }
    }
    
    // =========================================================================
    // Create Project Dialog
    // =========================================================================
    
    showCreateProjectDialog() {
        const modal = document.createElement('div');
        modal.className = 'project-modal-overlay';
        modal.innerHTML = `
            <div class="project-modal">
                <div class="project-modal-header">
                    <span class="project-modal-title">New Project</span>
                    <button class="project-modal-close">&times;</button>
                </div>
                <div class="project-modal-body">
                    <div class="project-modal-field">
                        <label>Name</label>
                        <input type="text" id="project-new-name" placeholder="Project name" autofocus>
                    </div>
                    <div class="project-modal-field">
                        <label>Description</label>
                        <input type="text" id="project-new-description" placeholder="Optional description">
                    </div>
                </div>
                <div class="project-modal-footer">
                    <button class="project-modal-btn cancel">Cancel</button>
                    <button class="project-modal-btn primary">Create</button>
                </div>
            </div>
        `;
        
        document.body.appendChild(modal);
        
        const nameInput = modal.querySelector('#project-new-name');
        const descInput = modal.querySelector('#project-new-description');
        const createBtn = modal.querySelector('.project-modal-btn.primary');
        const cancelBtn = modal.querySelector('.project-modal-btn.cancel');
        const closeBtn = modal.querySelector('.project-modal-close');
        
        const closeModal = () => modal.remove();
        
        const doCreate = async () => {
            const name = nameInput.value.trim();
            if (!name) {
                nameInput.focus();
                return;
            }
            
            if (!/^[\w\-]+$/.test(name)) {
                alert('Project name can only contain letters, numbers, underscores, and dashes');
                nameInput.focus();
                return;
            }
            
            const description = descInput.value.trim();
            
            try {
                createBtn.disabled = true;
                createBtn.textContent = 'Creating...';
                
                let cmd = `project_create {${name}}`;
                if (description) {
                    cmd += ` -description {${description}}`;
                }
                await this.sendCommandAsync(cmd);
                await this.sendCommandAsync(`project_activate {${name}}`);
                
                closeModal();
            } catch (e) {
                console.error('Failed to create project:', e);
                alert('Failed to create project: ' + e.message);
                createBtn.disabled = false;
                createBtn.textContent = 'Create';
            }
        };
        
        createBtn.addEventListener('click', doCreate);
        nameInput.addEventListener('keypress', (e) => {
            if (e.key === 'Enter') doCreate();
        });
        cancelBtn.addEventListener('click', closeModal);
        closeBtn.addEventListener('click', closeModal);
        modal.addEventListener('click', (e) => {
            if (e.target === modal) closeModal();
        });
        
        nameInput.focus();
    }
    
    // =========================================================================
    // Registry Push Dialog
    // =========================================================================
    
    async showRegistryPushDialog() {
        const projectName = this.state.activeProject;
        if (!projectName) {
            alert('No project selected. Please select a project first.');
            return;
        }
        
        if (!this.state.registryUrl) {
            alert('Registry is not configured. Please configure the registry on the backend.');
            return;
        }
        
        // Get project details
        const project = this.state.projects.find(p => p.name === projectName);
        
        const modal = document.createElement('div');
        modal.className = 'project-modal-overlay';
        modal.innerHTML = `
            <div class="project-modal project-modal-wide">
                <div class="project-modal-header">
                    <span class="project-modal-title">Push to Registry</span>
                    <button class="project-modal-close">&times;</button>
                </div>
                <div class="project-modal-body">
                    <div class="project-modal-info-row">
                        <span class="project-modal-info-label">Project:</span>
                        <span class="project-modal-info-value">${this.escapeHtml(projectName)}</span>
                    </div>
                    <div class="project-modal-info-row">
                        <span class="project-modal-info-label">Registry:</span>
                        <span class="project-modal-info-value">${this.escapeHtml(this.state.registryUrl)}</span>
                    </div>
                    <div class="project-modal-info-row">
                        <span class="project-modal-info-label">Workgroup:</span>
                        <span class="project-modal-info-value">${this.escapeHtml(this.state.registryWorkgroup)}</span>
                    </div>
                    <div class="project-modal-info-box">
                        <strong>This will upload:</strong>
                        <ul>
                            <li>Project definition</li>
                            <li>${project?.config_count || 'All'} configs</li>
                            <li>${project?.queue_count || 'All'} queues</li>
                        </ul>
                        <p>Existing data on the registry will be replaced.</p>
                    </div>
                </div>
                <div class="project-modal-footer">
                    <button class="project-modal-btn cancel">Cancel</button>
                    <button class="project-modal-btn primary">Push to Registry</button>
                </div>
            </div>
        `;
        
        document.body.appendChild(modal);
        
        const pushBtn = modal.querySelector('.project-modal-btn.primary');
        const cancelBtn = modal.querySelector('.project-modal-btn.cancel');
        const closeBtn = modal.querySelector('.project-modal-close');
        
        const closeModal = () => modal.remove();
        
        pushBtn.addEventListener('click', async () => {
            pushBtn.disabled = true;
            pushBtn.textContent = 'Pushing...';
            
            try {
                const result = await this.sendCommandAsync(`registry_push {${projectName}}`);
                const parsed = this.parseResult(result);
                
                if (parsed?.success === false || parsed?.success === 0) {
                    throw new Error(parsed?.errors?.join(', ') || 'Push failed');
                }
                
                closeModal();
                // Sync status will update via ess/registry/sync_status datapoint from backend
            } catch (e) {
                alert('Push failed: ' + e.message);
                pushBtn.disabled = false;
                pushBtn.textContent = 'Push to Registry';
            }
        });
        
        cancelBtn.addEventListener('click', closeModal);
        closeBtn.addEventListener('click', closeModal);
        modal.addEventListener('click', (e) => {
            if (e.target === modal) closeModal();
        });
    }
    
    // =========================================================================
    // Registry Pull Dialog
    // =========================================================================
    
    async showRegistryPullDialog() {
        if (!this.state.registryUrl) {
            alert('Registry is not configured. Please configure the registry on the backend.');
            return;
        }
        
        const modal = document.createElement('div');
        modal.className = 'project-modal-overlay';
        modal.innerHTML = `
            <div class="project-modal project-modal-wide">
                <div class="project-modal-header">
                    <span class="project-modal-title">Pull from Registry</span>
                    <button class="project-modal-close">&times;</button>
                </div>
                <div class="project-modal-body">
                    <div class="project-modal-info-row">
                        <span class="project-modal-info-label">Registry:</span>
                        <span class="project-modal-info-value">${this.escapeHtml(this.state.registryUrl)}</span>
                    </div>
                    <div class="project-modal-info-row">
                        <span class="project-modal-info-label">Workgroup:</span>
                        <span class="project-modal-info-value">${this.escapeHtml(this.state.registryWorkgroup)}</span>
                    </div>
                    <div class="project-modal-field">
                        <label>Select project to pull:</label>
                        <select id="registry-pull-project" class="project-modal-select">
                            <option value="">Loading projects...</option>
                        </select>
                    </div>
                    <div class="project-modal-details" id="registry-pull-details" style="display: none;">
                        <div id="registry-pull-description"></div>
                        <div id="registry-pull-stats"></div>
                    </div>
                    <div class="project-modal-field">
                        <label class="project-modal-checkbox">
                            <input type="checkbox" id="registry-pull-overwrite">
                            Overwrite if project exists locally
                        </label>
                    </div>
                </div>
                <div class="project-modal-footer">
                    <button class="project-modal-btn cancel">Cancel</button>
                    <button class="project-modal-btn primary" disabled>Pull from Registry</button>
                </div>
            </div>
        `;
        
        document.body.appendChild(modal);
        
        const projectSelect = modal.querySelector('#registry-pull-project');
        const detailsDiv = modal.querySelector('#registry-pull-details');
        const descDiv = modal.querySelector('#registry-pull-description');
        const statsDiv = modal.querySelector('#registry-pull-stats');
        const overwriteCheck = modal.querySelector('#registry-pull-overwrite');
        const pullBtn = modal.querySelector('.project-modal-btn.primary');
        const cancelBtn = modal.querySelector('.project-modal-btn.cancel');
        const closeBtn = modal.querySelector('.project-modal-close');
        
        const closeModal = () => modal.remove();
        
        // Load projects from registry
        try {
            const result = await this.sendCommandAsync('registry_list_projects_json');
            const projects = this.parseResult(result);
            
            if (!Array.isArray(projects) || projects.length === 0) {
                projectSelect.innerHTML = '<option value="">No projects found on registry</option>';
            } else {
                projectSelect.innerHTML = '<option value="">-- Select Project --</option>' +
                    projects.map(p => 
                        `<option value="${this.escapeAttr(p.name)}" 
                                 data-description="${this.escapeAttr(p.description || '')}"
                                 data-configs="${p.configCount || 0}"
                                 data-queues="${p.queueCount || 0}">
                            ${this.escapeHtml(p.name)}
                        </option>`
                    ).join('');
            }
        } catch (e) {
            projectSelect.innerHTML = `<option value="">Error: ${this.escapeHtml(e.message)}</option>`;
        }
        
        projectSelect.addEventListener('change', () => {
            const opt = projectSelect.selectedOptions[0];
            if (opt?.value) {
                pullBtn.disabled = false;
                detailsDiv.style.display = 'block';
                descDiv.textContent = opt.dataset.description || '(No description)';
                statsDiv.innerHTML = `<strong>${opt.dataset.configs}</strong> configs, <strong>${opt.dataset.queues}</strong> queues`;
            } else {
                pullBtn.disabled = true;
                detailsDiv.style.display = 'none';
            }
        });
        
        pullBtn.addEventListener('click', async () => {
            const projectName = projectSelect.value;
            if (!projectName) return;
            
            pullBtn.disabled = true;
            pullBtn.textContent = 'Pulling...';
            
            try {
                const overwrite = overwriteCheck.checked;
                const cmd = overwrite 
                    ? `registry_pull {${projectName}} -overwrite 1`
                    : `registry_pull {${projectName}}`;
                    
                const result = await this.sendCommandAsync(cmd);
                const parsed = this.parseResult(result);
                
                if (parsed?.success === false || parsed?.success === 0) {
                    throw new Error(parsed?.errors?.join(', ') || 'Pull failed');
                }
                
                closeModal();
                
                // Activate the pulled project
                await this.sendCommandAsync(`project_activate {${projectName}}`);
                
            } catch (e) {
                alert('Pull failed: ' + e.message);
                pullBtn.disabled = false;
                pullBtn.textContent = 'Pull from Registry';
            }
        });
        
        cancelBtn.addEventListener('click', closeModal);
        closeBtn.addEventListener('click', closeModal);
        modal.addEventListener('click', (e) => {
            if (e.target === modal) closeModal();
        });
    }
    
    // =========================================================================
    // Command Helpers
    // =========================================================================
    
    sendCommand(cmd) {
        try {
            const conn = this.dpManager?.connection;
            if (conn && conn.connected && conn.ws) {
                conn.ws.send(`send configs {${cmd}}`);
            }
        } catch (error) {
            console.error('Failed to send project command:', error);
        }
    }
    
    sendCommandAsync(cmd) {
        return new Promise((resolve, reject) => {
            try {
                const conn = this.dpManager?.connection;
                if (!conn || !conn.connected) {
                    reject(new Error('Not connected'));
                    return;
                }
                
                // Use sendRaw if available for proper response handling
                if (conn.sendRaw) {
                    conn.sendRaw(`send configs {${cmd}}`)
                        .then(resolve)
                        .catch(reject);
                } else {
                    // Fallback: send and resolve after delay
                    conn.ws.send(`send configs {${cmd}}`);
                    setTimeout(resolve, 100);
                }
            } catch (error) {
                reject(error);
            }
        });
    }
    
    /**
     * Parse command result - handles JSON, Tcl dict, or raw string
     */
    parseResult(result) {
        if (!result) return null;
        if (typeof result === 'object') return result;
        
        // Try JSON
        try {
            return JSON.parse(result);
        } catch {
            // Try Tcl dict format: key1 value1 key2 value2
            const obj = {};
            const parts = result.trim().split(/\s+/);
            for (let i = 0; i < parts.length - 1; i += 2) {
                obj[parts[i]] = parts[i + 1];
            }
            return Object.keys(obj).length > 0 ? obj : result;
        }
    }
    
    // =========================================================================
    // Public API
    // =========================================================================
    
    getActiveProject() {
        return this.state.activeProject;
    }
    
    getActiveProjectDetail() {
        if (!this.state.activeProject) return null;
        return this.state.projects.find(p => p.name === this.state.activeProject) || null;
    }
    
    hasActiveProject() {
        return !!this.state.activeProject;
    }
    
    // =========================================================================
    // Utilities
    // =========================================================================
    
    escapeHtml(str) {
        if (!str) return '';
        return String(str)
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;');
    }
    
    escapeAttr(str) {
        if (!str) return '';
        return String(str)
            .replace(/&/g, '&amp;')
            .replace(/"/g, '&quot;');
    }
}

// Export
if (typeof window !== 'undefined') {
    window.ProjectSelector = ProjectSelector;
}
