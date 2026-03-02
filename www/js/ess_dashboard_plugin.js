/**
 * ESS Dashboard Plugin
 *
 * Adds a system/protocol navigator to the Dashboard tab, allowing users
 * to browse all systems in the workgroup, preview their protocols, switch
 * the active system/protocol, and scaffold new protocols.
 *
 * Data comes from registry.getSystems() which returns each system with
 * its protocols[] already populated — no extra API calls needed to browse.
 */

const DashboardPlugin = {

    // ==========================================
    // Lifecycle Hooks
    // ==========================================

    onInit(wb) {
        this._systemsData = [];
        this._selectedSystem = null;  // browsing highlight (not necessarily loaded)
        this._createScaffoldModal();
    },

    async onRegistryReady(wb) {
        await this._refreshSystems(wb);
    },

    onSnapshot(wb, snapshot) {
        this._render(wb);
    },

    onTabSwitch(wb, tabName) {
        if (tabName === 'dashboard') {
            this._refreshSystems(wb);
        }
    },

    // ==========================================
    // Data
    // ==========================================

    async _refreshSystems(wb) {
        if (!wb.registry) return;
        try {
            const result = await wb.registry.getSystems();
            this._systemsData = result.systems || result || [];
            this._render(wb);
        } catch (err) {
            console.warn('Dashboard: failed to load systems', err);
        }
    },

    // ==========================================
    // Rendering
    // ==========================================

    _render(wb) {
        const container = document.getElementById('dashboard-navigator');
        if (!container) return;

        const activeSystem = wb.snapshot?.system || null;
        const activeProtocol = wb.snapshot?.protocol || null;
        const systems = this._systemsData;

        // If nothing selected for browsing, default to active system
        if (!this._selectedSystem && activeSystem) {
            this._selectedSystem = activeSystem;
        }

        // Render systems column
        const systemsList = container.querySelector('.dash-systems-list');
        if (systemsList) {
            if (systems.length === 0) {
                systemsList.innerHTML = '<div class="dash-empty">No systems found</div>';
            } else {
                systemsList.innerHTML = systems.map(sys => {
                    const isActive = sys.name === activeSystem;
                    const isSelected = sys.name === this._selectedSystem;
                    const classes = ['dash-system-item'];
                    if (isActive) classes.push('active');
                    if (isSelected) classes.push('selected');
                    return `
                        <div class="${classes.join(' ')}" data-system="${wb.escapeHtml(sys.name)}">
                            <span class="dash-item-name">${wb.escapeHtml(sys.name)}</span>
                            <span class="dash-item-meta">${sys.protocols ? sys.protocols.length : 0}p / ${sys.scripts || 0}s</span>
                            ${isActive ? '<span class="dash-active-badge">active</span>' : ''}
                        </div>`;
                }).join('');

                systemsList.querySelectorAll('.dash-system-item').forEach(el => {
                    el.addEventListener('click', () => {
                        this._selectedSystem = el.dataset.system;
                        this._render(wb);
                    });
                });
            }
        }

        // Render protocols column
        const protocolsList = container.querySelector('.dash-protocols-list');
        const protocolsHeader = container.querySelector('.dash-protocols-header-text');
        if (protocolsList) {
            const selSys = systems.find(s => s.name === this._selectedSystem);
            if (protocolsHeader) {
                protocolsHeader.textContent = this._selectedSystem
                    ? `Protocols — ${this._selectedSystem}`
                    : 'Protocols';
            }

            if (!selSys || !selSys.protocols || selSys.protocols.length === 0) {
                protocolsList.innerHTML = this._selectedSystem
                    ? '<div class="dash-empty">No protocols</div>'
                    : '<div class="dash-empty">Select a system</div>';
            } else {
                protocolsList.innerHTML = selSys.protocols.map(proto => {
                    const isActive = (selSys.name === activeSystem && proto === activeProtocol);
                    const classes = ['dash-protocol-item'];
                    if (isActive) classes.push('active');
                    return `
                        <div class="${classes.join(' ')}" data-protocol="${wb.escapeHtml(proto)}">
                            <span class="dash-item-name">${wb.escapeHtml(proto)}</span>
                            ${isActive ? '<span class="dash-active-badge">active</span>' : ''}
                        </div>`;
                }).join('');

                protocolsList.querySelectorAll('.dash-protocol-item').forEach(el => {
                    el.addEventListener('click', () => {
                        this._loadProtocol(wb, this._selectedSystem, el.dataset.protocol);
                    });
                });
            }
        }

        // Show/hide the "New Protocol" button based on selected system
        const newProtoBtn = container.querySelector('.dash-new-protocol-btn');
        if (newProtoBtn) {
            newProtoBtn.style.display = this._selectedSystem ? '' : 'none';
        }
    },

    // ==========================================
    // Actions
    // ==========================================

    async _loadProtocol(wb, system, protocol) {
        try {
            await wb.execTclCmd(`ess::load_system ${system} ${protocol}`);
            wb.showNotification(`Loaded ${system}/${protocol}`, 'success');
        } catch (err) {
            wb.showNotification(`Failed to load: ${err.message}`, 'error');
        }
    },

    _openScaffoldModal(wb) {
        const modal = document.getElementById('scaffold-protocol-modal');
        if (!modal) return;

        const systemField = modal.querySelector('#scaffold-system-name');
        const nameInput = modal.querySelector('#scaffold-protocol-name');
        const cloneSelect = modal.querySelector('#scaffold-clone-from');
        const errorEl = modal.querySelector('#scaffold-error');
        const createBtn = modal.querySelector('#scaffold-create-btn');

        // Pre-fill system
        systemField.textContent = this._selectedSystem || '—';
        nameInput.value = '';
        errorEl.textContent = '';
        errorEl.style.display = 'none';
        createBtn.disabled = false;

        // Populate clone-from dropdown
        const selSys = this._systemsData.find(s => s.name === this._selectedSystem);
        const protocols = selSys?.protocols || [];
        cloneSelect.innerHTML = '<option value="">(empty skeleton)</option>';
        protocols.forEach(p => {
            cloneSelect.innerHTML += `<option value="${wb.escapeHtml(p)}">${wb.escapeHtml(p)}</option>`;
        });

        modal.style.display = 'flex';
        nameInput.focus();
    },

    async _doScaffold(wb) {
        const modal = document.getElementById('scaffold-protocol-modal');
        const nameInput = modal.querySelector('#scaffold-protocol-name');
        const cloneSelect = modal.querySelector('#scaffold-clone-from');
        const errorEl = modal.querySelector('#scaffold-error');
        const createBtn = modal.querySelector('#scaffold-create-btn');

        const protocolName = nameInput.value.trim();
        const fromProtocol = cloneSelect.value || null;
        const system = this._selectedSystem;

        // Client-side validation (mirrors backend isValidName)
        if (!protocolName) {
            this._showScaffoldError(errorEl, 'Protocol name is required');
            return;
        }
        if (!/^[a-zA-Z_][a-zA-Z0-9_-]*$/.test(protocolName) || protocolName.length > 128) {
            this._showScaffoldError(errorEl, 'Use only letters, numbers, underscores, hyphens. Must start with a letter or underscore.');
            return;
        }

        createBtn.disabled = true;
        errorEl.style.display = 'none';

        try {
            await wb.registry.scaffoldProtocol(system, protocolName, fromProtocol);
            modal.style.display = 'none';
            wb.showNotification(`Created protocol ${protocolName}`, 'success');

            // Refresh systems data, then load the new protocol
            await this._refreshSystems(wb);
            await wb.execTclCmd(`ess::load_system ${system} ${protocolName}`);
            wb.showNotification(`Loaded ${system}/${protocolName}`, 'success');
        } catch (err) {
            createBtn.disabled = false;
            const msg = err.message || String(err);
            if (msg.includes('already exists')) {
                this._showScaffoldError(errorEl, `Protocol "${protocolName}" already exists`);
            } else {
                this._showScaffoldError(errorEl, msg);
            }
        }
    },

    _showScaffoldError(el, msg) {
        el.textContent = msg;
        el.style.display = 'block';
    },

    // ==========================================
    // Modal Creation
    // ==========================================

    _createScaffoldModal() {
        if (document.getElementById('scaffold-protocol-modal')) return;

        const modal = document.createElement('div');
        modal.id = 'scaffold-protocol-modal';
        modal.className = 'modal-overlay';
        modal.style.display = 'none';
        modal.innerHTML = `
            <div class="modal" style="width: 450px;">
                <div class="modal-header">
                    <h3>New Protocol</h3>
                    <button class="modal-close" onclick="this.closest('.modal-overlay').style.display='none'">&times;</button>
                </div>
                <div class="modal-body">
                    <div class="form-group">
                        <label class="form-label">System</label>
                        <div class="form-static" id="scaffold-system-name">—</div>
                    </div>
                    <div class="form-group">
                        <label class="form-label" for="scaffold-protocol-name">Protocol Name</label>
                        <input type="text" class="form-input" id="scaffold-protocol-name"
                               placeholder="e.g. colormatch_v2" autocomplete="off">
                    </div>
                    <div class="form-group">
                        <label class="form-label" for="scaffold-clone-from">Clone From</label>
                        <select class="form-input" id="scaffold-clone-from">
                            <option value="">(empty skeleton)</option>
                        </select>
                    </div>
                    <div class="scaffold-error" id="scaffold-error" style="display:none;"></div>
                </div>
                <div class="modal-footer">
                    <button class="btn btn-secondary" onclick="this.closest('.modal-overlay').style.display='none'">Cancel</button>
                    <button class="btn btn-primary" id="scaffold-create-btn">Create</button>
                </div>
            </div>
        `;

        document.body.appendChild(modal);

        // Bind create button
        modal.querySelector('#scaffold-create-btn').addEventListener('click', () => {
            const wb = window.workbench;
            if (wb) DashboardPlugin._doScaffold(wb);
        });

        // Enter key in name input
        modal.querySelector('#scaffold-protocol-name').addEventListener('keydown', (e) => {
            if (e.key === 'Enter') {
                const wb = window.workbench;
                if (wb) DashboardPlugin._doScaffold(wb);
            }
        });
    }
};

// Expose action handlers for onclick in HTML
document.addEventListener('DOMContentLoaded', () => {
    setTimeout(() => {
        const wb = window.workbench;
        if (wb) {
            wb._dashboardNewProtocol = function() {
                DashboardPlugin._openScaffoldModal(this);
            };
        }
    }, 0);
});

ESSWorkbench.registerPlugin(DashboardPlugin);
