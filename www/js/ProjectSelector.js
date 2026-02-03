/**
 * ProjectSelector.js
 * 
 * Dropdown component for selecting the active project.
 * Placed in the status bar, affects filtering across the app.
 * 
 * In the new architecture, projects are REQUIRED - every config and queue
 * belongs to exactly one project. There is no "All Projects" view.
 * 
 * Subscribes to:
 *   - projects/list: Available projects
 *   - projects/active: Currently active project name
 * 
 * Sends commands via configs subprocess:
 *   - project_activate "name"
 *   - project_create "name"
 */

class ProjectSelector {
    constructor(container, dpManager) {
        this.container = typeof container === 'string' 
            ? document.getElementById(container) 
            : container;
        this.dpManager = dpManager;
        
        this.state = {
            projects: [],
            activeProject: ''
        };
        
        this.render();
        this.setupSubscriptions();
    }
    
    render() {
        this.container.innerHTML = `
            <div class="project-selector">
                <label class="project-selector-label">Project:</label>
                <select class="project-selector-select" id="project-select">
                    <option value="">-- Select Project --</option>
                </select>
                <button class="project-selector-new" id="project-new-btn" title="Create new project">+</button>
            </div>
        `;
        
        this.elements = {
            select: this.container.querySelector('#project-select'),
            newBtn: this.container.querySelector('#project-new-btn')
        };
        
        this.elements.select.addEventListener('change', (e) => {
            this.onProjectChange(e.target.value);
        });
        
        this.elements.newBtn.addEventListener('click', () => {
            this.showCreateProjectDialog();
        });
    }
    
    setupSubscriptions() {
        this.dpManager.subscribe('projects/list', (data) => {
            this.updateProjectList(data.value);
        });
        
        this.dpManager.subscribe('projects/active', (data) => {
            this.updateActiveProject(data.value);
        });
    }
    
    updateProjectList(data) {
        try {
            const projects = typeof data === 'string' ? JSON.parse(data) : data;
            this.state.projects = Array.isArray(projects) ? projects : [];
            this.renderSelect();
            
            // If no project is active but we have projects, activate the first one
            if (!this.state.activeProject && this.state.projects.length > 0) {
                const firstProject = this.state.projects[0].name;
                this.onProjectChange(firstProject);
            }
        } catch (e) {
            console.error('Failed to parse projects list:', e);
            this.state.projects = [];
            this.renderSelect();
        }
    }
    
    updateActiveProject(data) {
        this.state.activeProject = data || '';
        this.elements.select.value = this.state.activeProject;
        
        // Update visual state
        this.container.classList.toggle('no-project', !this.state.activeProject);
    }
    
    renderSelect() {
        const select = this.elements.select;
        const currentValue = this.state.activeProject;
        
        // Build options - no "All Projects", project is required
        let html = '<option value="" disabled>-- Select Project --</option>';
        
        // Sort alphabetically
        const sorted = [...this.state.projects].sort((a, b) => {
            return a.name.localeCompare(b.name);
        });
        
        sorted.forEach(p => {
            const selected = p.name === currentValue ? 'selected' : '';
html += `<option value="${this.escapeAttr(p.name)}" ${selected}>${this.escapeHtml(p.name)}</option>`;
        });
        
        select.innerHTML = html;
        
        // Restore selection
        if (currentValue) {
            select.value = currentValue;
        }
    }
    
    onProjectChange(projectName) {
        if (projectName) {
            this.sendCommand(`project_activate {${projectName}}`);
        }
        // No deactivate - project is required
    }
    
    /**
     * Show dialog to create a new project
     */
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
            
            // Validate name (alphanumeric, underscore, dash)
            if (!/^[\w\-]+$/.test(name)) {
                alert('Project name can only contain letters, numbers, underscores, and dashes');
                nameInput.focus();
                return;
            }
            
            const description = descInput.value.trim();
            
            try {
                createBtn.disabled = true;
                createBtn.textContent = 'Creating...';
                
                // Create the project
                let cmd = `project_create {${name}}`;
                if (description) {
                    cmd += ` -description {${description}}`;
                }
                await this.sendCommandAsync(cmd);
                
                // Activate it
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
    
    /**
     * Send command and wait for response (for create operations)
     */
    sendCommandAsync(cmd) {
        return new Promise((resolve, reject) => {
            try {
                const conn = this.dpManager?.connection;
                if (!conn || !conn.connected || !conn.ws) {
                    reject(new Error('Not connected'));
                    return;
                }
                
                // For now, just send and resolve after a short delay
                // A proper implementation would track the response
                conn.ws.send(`send configs {${cmd}}`);
                setTimeout(resolve, 100);
            } catch (error) {
                reject(error);
            }
        });
    }
    
    // Get current active project (for other components to query)
    getActiveProject() {
        return this.state.activeProject;
    }
    
    // Get project details
    getActiveProjectDetail() {
        if (!this.state.activeProject) return null;
        return this.state.projects.find(p => p.name === this.state.activeProject) || null;
    }
    
    // Check if a project is active (for other components)
    hasActiveProject() {
        return !!this.state.activeProject;
    }
    
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
