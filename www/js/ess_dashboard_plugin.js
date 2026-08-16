/**
 * ESS Dashboard Plugin
 *
 * System/protocol navigator for the Dashboard tab: browse the systems
 * on this box, preview a system's protocols, and load one to inspect
 * it. Read-and-load only — creating, cloning, and deleting protocols
 * live in ess_control (Clone modal / Sync modal), not here.
 *
 * Data comes from the ess interp's non-sourcing getters
 * (ess::get_systems / ess::get_protocols) via evalAsync.
 */

const DashboardPlugin = {

    // ==========================================
    // Lifecycle Hooks
    // ==========================================

    onInit(wb) {
        this._systems = [];
        this._protocols = new Map();  // system -> [protocol...]
        this._selectedSystem = null;  // browsing highlight (not necessarily loaded)
    },

    onConnected(wb) {
        this._refreshSystems(wb);
    },

    onSnapshot(wb, snapshot) {
        if (!this._systems.length) this._refreshSystems(wb);
        else this._render(wb);
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
        try {
            const resp = await wb.connection.evalAsync('send ess {ess::get_systems}');
            this._systems = TclParser.parseList(resp || '');
            this._render(wb);
        } catch (err) {
            console.warn('Dashboard: failed to load systems', err);
        }
    },

    async _protocolsFor(wb, system) {
        if (this._protocols.has(system)) return this._protocols.get(system);
        try {
            if (!/^[\w.\-]+$/.test(system)) return [];
            const resp = await wb.connection.evalAsync(`send ess {ess::get_protocols ${system}}`);
            const list = TclParser.parseList(resp || '');
            this._protocols.set(system, list);
            return list;
        } catch (err) {
            console.warn(`Dashboard: failed to load protocols for ${system}`, err);
            return [];
        }
    },

    // ==========================================
    // Rendering
    // ==========================================

    async _render(wb) {
        const container = document.getElementById('dashboard-navigator');
        if (!container) return;

        const activeSystem = wb.snapshot?.system || null;
        const activeProtocol = wb.snapshot?.protocol || null;

        if (!this._selectedSystem && activeSystem) {
            this._selectedSystem = activeSystem;
        }

        const systemsList = container.querySelector('.dash-systems-list');
        if (systemsList) {
            if (this._systems.length === 0) {
                systemsList.innerHTML = '<div class="dash-empty">No systems found</div>';
            } else {
                systemsList.innerHTML = this._systems.map(name => {
                    const classes = ['dash-system-item'];
                    if (name === activeSystem) classes.push('active');
                    if (name === this._selectedSystem) classes.push('selected');
                    return `
                        <div class="${classes.join(' ')}" data-system="${wb.escapeHtml(name)}">
                            <span class="dash-item-name">${wb.escapeHtml(name)}</span>
                            ${name === activeSystem ? '<span class="dash-active-badge">active</span>' : ''}
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

        const protocolsList = container.querySelector('.dash-protocols-list');
        const protocolsHeader = container.querySelector('.dash-protocols-header-text');
        if (protocolsHeader) {
            protocolsHeader.textContent = this._selectedSystem
                ? `Protocols — ${this._selectedSystem}`
                : 'Protocols';
        }
        if (protocolsList) {
            if (!this._selectedSystem) {
                protocolsList.innerHTML = '<div class="dash-empty">Select a system</div>';
            } else {
                const sel = this._selectedSystem;
                const protocols = await this._protocolsFor(wb, sel);
                if (this._selectedSystem !== sel) return;   // selection moved on
                if (!protocols.length) {
                    protocolsList.innerHTML = '<div class="dash-empty">No protocols</div>';
                } else {
                    protocolsList.innerHTML = protocols.map(proto => {
                        const isActive = (this._selectedSystem === activeSystem && proto === activeProtocol);
                        return `
                            <div class="dash-protocol-item${isActive ? ' active' : ''}" data-protocol="${wb.escapeHtml(proto)}">
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
        }
    },

    // ==========================================
    // Actions
    // ==========================================

    async _loadProtocol(wb, system, protocol) {
        if (!/^[\w.\-]+$/.test(system) || !/^[\w.\-]+$/.test(protocol)) return;
        try {
            await wb.execTclCmd(`ess::load_system ${system} ${protocol}`);
            wb.showNotification(`Loaded ${system}/${protocol}`, 'success');
            this._protocols.delete(system);   // reload may add/remove protocols
        } catch (err) {
            wb.showNotification(`Failed to load: ${err.message}`, 'error');
        }
    }
};

ESSWorkbench.registerPlugin(DashboardPlugin);
