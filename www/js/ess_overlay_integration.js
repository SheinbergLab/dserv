/**
 * ESS Workbench - Overlay Integration
 * 
 * Wires up the user selector → overlay activation → script status badges →
 * save routing → promote/discard workflow.
 *
 * Depends on: ess_workbench.js (ESSWorkbench class), ess_registry_integration_v2.js
 *
 * Backend commands used:
 *   ess::set_overlay_user <username>   — activate/deactivate overlay
 *   ess::save_script <type> {content}  — saves to overlay when active
 *   ess::promote_overlay <type>        — copy overlay → base, delete overlay
 *   ess::promote_all_overlays          — promote everything at once
 *   ess::discard_overlay <type>        — delete overlay file, revert to base
 *   ess::discard_all_overlays          — discard everything at once
 *   ess::overlay_summary               — get overlay state summary
 *   ess::reload_system                 — reload after changes
 *
 * Snapshot fields consumed:
 *   overlay_user   — username string or "" if inactive
 *   overlay_status — dict like {system base protocol base variants overlay ...}
 */

(function() {
    'use strict';

    // ========================================================================
    // Patch init to add overlay state
    // ========================================================================

    const _origInit = ESSWorkbench.prototype.init;
    ESSWorkbench.prototype.init = function() {
        // Overlay state
        this.overlayState = {
            active: false,
            user: '',
            scriptStatus: {}   // {system: "base"|"overlay", protocol: "base"|"overlay", ...}
        };

        _origInit.call(this);
        this.initOverlayUI();
    };

    // ========================================================================
    // Overlay UI Initialization
    // ========================================================================

    ESSWorkbench.prototype.initOverlayUI = function() {
        this.bindOverlayEvents();

        // Restore last user from localStorage into dropdown (display only)
        const lastUser = localStorage.getItem('ess_overlay_user');
        if (lastUser) {
            this._pendingOverlayUser = lastUser;
        }

        // When connected, activate the pending overlay user
        if (this.connection) {
            this.connection.on('connected', () => {
                // Small delay to let subscriptions set up first
                setTimeout(() => this.activatePendingOverlayUser(), 500);
            });
        }
    };

    /**
     * Called once after WebSocket connection is established.
     * Sends the stored overlay user to the backend.
     */
    ESSWorkbench.prototype.activatePendingOverlayUser = async function() {
        const username = this._pendingOverlayUser;
        if (!username) return;
        this._pendingOverlayUser = null;

        console.log(`Restoring overlay user from localStorage: "${username}"`);
        try {
            await this.setOverlayUser(username);
        } catch (err) {
            console.warn('Failed to restore overlay user:', err.message);
            // Don't loop — just clear and let user re-select manually
            this.showNotification?.(`Could not activate overlay for "${username}": ${err.message}. Select user manually.`, 'warning');
            localStorage.removeItem('ess_overlay_user');
            const select = document.getElementById('user-select');
            if (select) select.value = '';
        }
    };

    // ========================================================================
    // Event Bindings
    // ========================================================================

    ESSWorkbench.prototype.bindOverlayEvents = function() {
        // User selector change → activate/deactivate overlay
        const userSelect = document.getElementById('user-select');
        userSelect?.addEventListener('change', (e) => {
            this.setOverlayUser(e.target.value);
        });

        // Promote/discard buttons in the editor header (we'll add these dynamically)
        document.addEventListener('click', (e) => {
            const btn = e.target.closest('[data-overlay-action]');
            if (!btn) return;

            const action = btn.dataset.overlayAction;
            const scriptType = btn.dataset.scriptType || this.currentScript;

            switch (action) {
                case 'promote':
                    this.promoteOverlay(scriptType);
                    break;
                case 'promote-all':
                    this.promoteAllOverlays();
                    break;
                case 'discard':
                    this.discardOverlay(scriptType);
                    break;
                case 'discard-all':
                    this.discardAllOverlays();
                    break;
            }
        });
    };

    // ========================================================================
    // Set Overlay User (core action)
    // ========================================================================

    ESSWorkbench.prototype.setOverlayUser = async function(username) {
        try {
            if (!this.connection?.ws || this.connection.ws.readyState !== WebSocket.OPEN) {
                console.warn('Cannot set overlay user: not connected');
                return;
            }

            // Use execRegistryCmd which handles response routing and error detection
            const cmd = username
                ? `ess::set_overlay_user ${username}`
                : 'ess::set_overlay_user {}';
            console.log('Setting overlay user via eval:', cmd);

            await this.execRegistryCmd(cmd);
            console.log('Overlay user set successfully:', username || '(none)');

            // Persist on success
            if (username) {
                localStorage.setItem('ess_overlay_user', username);
            } else {
                localStorage.removeItem('ess_overlay_user');
            }

            // Update local state
            this.overlayState.user = username;
            this.overlayState.active = username !== '';

            // Persist
            if (username) {
                localStorage.setItem('ess_overlay_user', username);
            } else {
                localStorage.removeItem('ess_overlay_user');
            }

            // Update header display
            this.updateOverlayIndicator();

            // Touch snapshot to get refreshed overlay_status
            if (this.connection?.ws?.readyState === WebSocket.OPEN) {
                setTimeout(() => {
                    this.connection.ws.send(JSON.stringify({
                        cmd: 'touch',
                        name: 'ess/snapshot'
                    }));
                }, 100);
            }

            console.log(`Overlay user set to: ${username || '(none)'}`);

        } catch (err) {
            console.error('Failed to set overlay user:', err);
            this.showNotification?.(`Failed to set overlay user: ${err.message}`, 'error');
        }
    };

    // ========================================================================
    // Snapshot Integration — read overlay fields
    // ========================================================================

    // Patch handleSnapshot to extract overlay info
    const _origHandleSnapshot = ESSWorkbench.prototype.handleSnapshot;
    ESSWorkbench.prototype.handleSnapshot = function(data) {
        // Call original
        _origHandleSnapshot.call(this, data);

        // Now extract overlay fields from parsed snapshot
        if (this.snapshot) {
            this.overlayState.user = this.snapshot.overlay_user || '';
            this.overlayState.active = this.overlayState.user !== '';

            // Parse overlay_status — comes as Tcl dict string: "system base protocol base variants overlay"
            // or could be JSON object if dict_to_json handles it
            const rawStatus = this.snapshot.overlay_status;
            if (rawStatus && typeof rawStatus === 'string') {
                this.overlayState.scriptStatus = TclParser.parseDict(rawStatus);
            } else if (rawStatus && typeof rawStatus === 'object') {
                this.overlayState.scriptStatus = rawStatus;
            } else {
                this.overlayState.scriptStatus = {};
            }

            // Sync UI to match backend state (snapshot is the source of truth)
            const select = document.getElementById('user-select');
            if (select && this.overlayState.user) {
                // Ensure the user exists as a dropdown option
                let found = false;
                for (const opt of select.options) {
                    if (opt.value === this.overlayState.user) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    const opt = document.createElement('option');
                    opt.value = this.overlayState.user;
                    opt.textContent = (this._userMap && this._userMap[this.overlayState.user]) || this.overlayState.user;
                    select.appendChild(opt);
                }
                select.value = this.overlayState.user;
            } else if (select && !this.overlayState.user) {
                // Backend has no overlay user — only clear UI if we're not mid-activation
                if (!this._pendingOverlayUser) {
                    select.value = '';
                }
            }

            // Update all visual indicators
            this.updateOverlayIndicator();
            this.updateScriptStatusDots();
            this.updateEditorOverlayControls();
            this.updateDashboardOverlayStatus();
        }
    };

    // ========================================================================
    // Visual Updates
    // ========================================================================

    /**
     * Update the header overlay mode indicator
     */
    ESSWorkbench.prototype.updateOverlayIndicator = function() {
        // Update/create the overlay badge next to the user selector
        let badge = document.getElementById('overlay-mode-badge');

        if (this.overlayState.active) {
            if (!badge) {
                badge = document.createElement('span');
                badge.id = 'overlay-mode-badge';
                badge.className = 'overlay-mode-badge';
                const userDiv = document.querySelector('.header-user');
                if (userDiv) userDiv.appendChild(badge);
            }

            const overlayCount = Object.values(this.overlayState.scriptStatus)
                .filter(s => s === 'overlay').length;

            if (overlayCount > 0) {
                badge.textContent = `OVERLAY (${overlayCount})`;
                badge.className = 'overlay-mode-badge active modified';
            } else {
                badge.textContent = 'OVERLAY';
                badge.className = 'overlay-mode-badge active';
            }
        } else {
            if (badge) badge.remove();
        }
    };

    /**
     * Update the colored dots next to each script name in the sidebar
     */
    ESSWorkbench.prototype.updateScriptStatusDots = function() {
        const dots = document.querySelectorAll('.script-status-dot[data-script]');
        dots.forEach(dot => {
            const scriptType = dot.dataset.script;
            const source = this.overlayState.scriptStatus[scriptType];

            // Reset classes
            dot.className = 'script-status-dot';
            dot.title = '';

            if (!this.overlayState.active) {
                // No overlay active — no dots
                return;
            }

            if (source === 'overlay') {
                dot.classList.add('modified');
                dot.title = 'Using overlay (working copy)';
            } else if (source === 'base') {
                dot.classList.add('synced');
                dot.title = 'Using base (main)';
            }
        });
    };

    /**
     * Add/update promote/discard buttons in the editor header area
     * for the currently selected script
     */
    ESSWorkbench.prototype.updateEditorOverlayControls = function() {
        // --- Scripts tab controls ---
        this._updateOverlayControlsFor('scripts');
        // --- Variants tab controls ---
        this._updateOverlayControlsFor('variants');
    };

    ESSWorkbench.prototype._updateOverlayControlsFor = function(tab) {
        let containerId, scriptType;

        if (tab === 'scripts') {
            containerId = 'script-overlay-controls';
            scriptType = this.currentScript;
        } else if (tab === 'variants') {
            containerId = 'variant-overlay-controls';
            scriptType = 'variants';
        } else {
            return;
        }

        let container = document.getElementById(containerId);
        const source = this.overlayState.scriptStatus[scriptType];
        const isOverlaid = this.overlayState.active && source === 'overlay';

        if (!isOverlaid) {
            // Remove controls if present
            if (container) container.innerHTML = '';
            return;
        }

        // Find or create container in the editor header
        if (!container) {
            container = document.createElement('div');
            container.id = containerId;
            container.className = 'overlay-controls';

            if (tab === 'scripts') {
                const editorActions = document.querySelector('#tab-scripts .editor-actions');
                if (editorActions) editorActions.prepend(container);
            } else if (tab === 'variants') {
                const variantActions = document.querySelector('.variants-actions');
                if (variantActions) {
                    // Insert before the toolbar divider or at start
                    const divider = variantActions.querySelector('.toolbar-divider');
                    if (divider) {
                        variantActions.insertBefore(container, divider);
                    } else {
                        variantActions.prepend(container);
                    }
                }
            }
        }

        container.innerHTML = `
            <span class="overlay-badge-inline">
                <span class="overlay-dot"></span>
                overlay
            </span>
            <button class="btn btn-xs btn-overlay-promote"
                    data-overlay-action="promote" data-script-type="${scriptType}"
                    title="Promote this overlay to base (main)">
                <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                    <polyline points="17 11 12 6 7 11"></polyline>
                    <polyline points="17 18 12 13 7 18"></polyline>
                </svg>
                Promote
            </button>
            <button class="btn btn-xs btn-overlay-discard"
                    data-overlay-action="discard" data-script-type="${scriptType}"
                    title="Discard overlay, revert to base (main)">
                <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                    <line x1="18" y1="6" x2="6" y2="18"></line>
                    <line x1="6" y1="6" x2="18" y2="18"></line>
                </svg>
                Discard
            </button>
        `;
    };

    // ========================================================================
    // Overlay Operations
    // ========================================================================

    ESSWorkbench.prototype.promoteOverlay = async function(scriptType) {
        if (!this.overlayState.active) return;

        const confirmed = confirm(
            `Promote "${scriptType}" overlay to base (main)?\n\n` +
            `This will overwrite the main version with your working copy.`
        );
        if (!confirmed) return;

        try {
            await this.execRegistryCmd(`ess::promote_overlay ${scriptType} 1`);
            this.showNotification?.(`Promoted ${scriptType} to main`, 'success');
        } catch (err) {
            console.error('Promote failed:', err);
            this.showNotification?.(`Promote failed: ${err.message}`, 'error');
        }
    };

    ESSWorkbench.prototype.promoteAllOverlays = async function() {
        if (!this.overlayState.active) return;

        const overlaid = Object.entries(this.overlayState.scriptStatus)
            .filter(([, src]) => src === 'overlay')
            .map(([type]) => type);

        if (overlaid.length === 0) {
            this.showNotification?.('No overlay files to promote', 'info');
            return;
        }

        const confirmed = confirm(
            `Promote all overlay scripts to main?\n\n` +
            `Scripts: ${overlaid.join(', ')}\n\n` +
            `This will overwrite the main versions with your working copies.`
        );
        if (!confirmed) return;

        try {
            await this.execRegistryCmd('ess::promote_all_overlays 1');
            this.showNotification?.(`Promoted ${overlaid.length} script(s) to main`, 'success');
        } catch (err) {
            console.error('Promote all failed:', err);
            this.showNotification?.(`Promote failed: ${err.message}`, 'error');
        }
    };

    ESSWorkbench.prototype.discardOverlay = async function(scriptType) {
        if (!this.overlayState.active) return;

        const confirmed = confirm(
            `Discard overlay for "${scriptType}"?\n\n` +
            `This will revert to the base (main) version. Your working copy will be lost.`
        );
        if (!confirmed) return;

        try {
            await this.execRegistryCmd(`ess::discard_overlay ${scriptType} 1`);
            this.showNotification?.(`Discarded ${scriptType} overlay`, 'success');
        } catch (err) {
            console.error('Discard failed:', err);
            this.showNotification?.(`Discard failed: ${err.message}`, 'error');
        }
    };

    ESSWorkbench.prototype.discardAllOverlays = async function() {
        if (!this.overlayState.active) return;

        const overlaid = Object.entries(this.overlayState.scriptStatus)
            .filter(([, src]) => src === 'overlay')
            .map(([type]) => type);

        if (overlaid.length === 0) {
            this.showNotification?.('No overlay files to discard', 'info');
            return;
        }

        const confirmed = confirm(
            `Discard ALL overlay scripts?\n\n` +
            `Scripts: ${overlaid.join(', ')}\n\n` +
            `This will revert all to base (main). Your working copies will be lost.`
        );
        if (!confirmed) return;

        try {
            await this.execRegistryCmd('ess::discard_all_overlays 1');
            this.showNotification?.(`Discarded ${overlaid.length} overlay(s)`, 'success');
        } catch (err) {
            console.error('Discard all failed:', err);
            this.showNotification?.(`Discard failed: ${err.message}`, 'error');
        }
    };

    // ========================================================================
    // User List Population
    // ========================================================================

    /**
     * Populate the user selector dropdown.
     * Called after registry connection or from snapshot data.
     */
    ESSWorkbench.prototype.populateOverlayUsers = async function(users) {
        const select = document.getElementById('user-select');
        if (!select) return;

        // Remember current value
        const currentVal = select.value || this._pendingOverlayUser || '';

        // Clear and rebuild
        select.innerHTML = '<option value="">No user (read-only)</option>';

        // Store user map for later lookups (username → fullname)
        this._userMap = {};

        if (Array.isArray(users)) {
            users.forEach(u => {
                const opt = document.createElement('option');
                let username, displayName;

                if (typeof u === 'object') {
                    username = u.username || u.name || '';
                    displayName = u.fullName || u.fullname || u.full_name || u.displayName || username;
                } else {
                    username = u;
                    displayName = u;
                }

                opt.value = username;
                opt.textContent = displayName;
                select.appendChild(opt);

                this._userMap[username] = displayName;
            });
        }

        // Restore selection (display only — activation is handled by activatePendingOverlayUser)
        if (currentVal) {
            select.value = currentVal;
        }

        this._pendingOverlayUser = null;
    };

    // ========================================================================
    // Hook into existing initRegistry to load users
    // ========================================================================

    const _origInitRegistry = ESSWorkbench.prototype.initRegistry;
    ESSWorkbench.prototype.initRegistry = async function() {
        await _origInitRegistry.call(this);

        // After registry is initialized, try to load users for the overlay selector
        if (this.registry?.workgroup) {
            try {
                const result = await this.registry.getUsers();
                const users = Array.isArray(result) ? result : (result.users || result || []);
                this.populateOverlayUsers(users);
            } catch (err) {
                console.warn('Could not load users for overlay selector:', err);
                // Fallback: try to get from dserv datapoint
                this.loadUsersFromDserv();
            }
        } else {
            this.loadUsersFromDserv();
        }
    };

    /**
     * Fallback: load users from dserv datapoints if registry isn't available
     */
    ESSWorkbench.prototype.loadUsersFromDserv = async function() {
        if (!this.dpManager) return;

        try {
            const usersDp = await this.dpManager.get('ess/registry/users');
            const usersVal = usersDp?.data || usersDp?.value;
            if (usersVal) {
                const users = typeof usersVal === 'string' ? JSON.parse(usersVal) : usersVal;
                this.populateOverlayUsers(users);
            }
        } catch (err) {
            console.warn('Could not load users from dserv:', err);
        }
    };

    // ========================================================================
    // Patch selectScript to update overlay controls when script selection changes
    // ========================================================================

    const _origSelectScript = ESSWorkbench.prototype.selectScript;
    ESSWorkbench.prototype.selectScript = async function(scriptName) {
        await _origSelectScript.call(this, scriptName);
        this.updateEditorOverlayControls();
    };

    // ========================================================================
    // Dashboard overlay summary
    // ========================================================================

    /**
     * Update the dashboard config hero to show overlay status
     */
    ESSWorkbench.prototype.updateDashboardOverlayStatus = function() {
        // Add overlay indicator to the config-details area if active
        let overlayDetail = document.getElementById('cfg-overlay-status');

        if (this.overlayState.active) {
            if (!overlayDetail) {
                const detailsDiv = document.querySelector('.config-details');
                if (detailsDiv) {
                    const div = document.createElement('div');
                    div.className = 'config-detail';
                    div.innerHTML = `
                        <span class="config-detail-label">Overlay</span>
                        <span class="config-detail-value" id="cfg-overlay-status"></span>
                    `;
                    detailsDiv.appendChild(div);
                    overlayDetail = document.getElementById('cfg-overlay-status');
                }
            }

            if (overlayDetail) {
                const overlayCount = Object.values(this.overlayState.scriptStatus)
                    .filter(s => s === 'overlay').length;
                const user = this.overlayState.user;

                if (overlayCount > 0) {
                    overlayDetail.innerHTML =
                        `<span class="overlay-user-label">${this.escapeHtml(user)}</span>` +
                        `<span class="overlay-count-badge">${overlayCount} modified</span>`;
                } else {
                    overlayDetail.innerHTML =
                        `<span class="overlay-user-label">${this.escapeHtml(user)}</span>` +
                        `<span class="overlay-clean-badge">clean</span>`;
                }
            }
        } else {
            // Remove overlay detail if present
            const wrapper = overlayDetail?.closest('.config-detail');
            if (wrapper) wrapper.remove();
        }
    };

    // Patch updateConfigDisplay to also update overlay info on dashboard
    const _origUpdateConfigDisplay = ESSWorkbench.prototype.updateConfigDisplay;
    ESSWorkbench.prototype.updateConfigDisplay = function() {
        _origUpdateConfigDisplay.call(this);
        this.updateDashboardOverlayStatus();
    };

})();
