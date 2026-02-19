/**
 * ESS Overlay Plugin
 *
 * Manages user overlay workflow: per-user working copies of scripts
 * that can be promoted to base or discarded.
 *
 * Backend commands:
 *   ess::set_overlay_user <username>
 *   ess::promote_overlay <type> 1
 *   ess::promote_all_overlays 1
 *   ess::discard_overlay <type> 1
 *   ess::discard_all_overlays 1
 *
 * Snapshot fields:
 *   overlay_user   — username string or "" if inactive
 *   overlay_status — dict like {system base protocol base variants overlay ...}
 */

const OverlayPlugin = {

    // ==========================================
    // Lifecycle Hooks
    // ==========================================

    onInit(wb) {
        wb.overlayState = {
            active: false,
            user: '',
            scriptStatus: {}   // {system: "base"|"overlay", ...}
        };
        wb._userMap = {};

        this._bindOverlayEvents(wb);

        // Restore last user from localStorage
        const lastUser = localStorage.getItem('ess_overlay_user');
        if (lastUser) {
            wb._pendingOverlayUser = lastUser;
        }
    },

    onConnected(wb) {
        // Activate pending overlay user after a short delay
        setTimeout(() => this._activatePendingOverlayUser(wb), 500);
    },

    async onRegistryReady(wb) {
        // Load users for the overlay selector
        if (wb.registry?.workgroup) {
            try {
                const result = await wb.registry.getUsers();
                const users = Array.isArray(result) ? result : (result.users || result || []);
                this._populateOverlayUsers(wb, users);
            } catch (err) {
                console.warn('Could not load users for overlay selector:', err);
                this._loadUsersFromDserv(wb);
            }
        } else {
            this._loadUsersFromDserv(wb);
        }
    },

    onSnapshot(wb, snapshot) {
        wb.overlayState.user = snapshot.overlay_user || '';
        wb.overlayState.active = wb.overlayState.user !== '';

        // Parse overlay_status
        const rawStatus = snapshot.overlay_status;
        if (rawStatus && typeof rawStatus === 'string') {
            wb.overlayState.scriptStatus = TclParser.parseDict(rawStatus);
        } else if (rawStatus && typeof rawStatus === 'object') {
            wb.overlayState.scriptStatus = rawStatus;
        } else {
            wb.overlayState.scriptStatus = {};
        }

        // Sync user dropdown to match backend state
        const select = document.getElementById('user-select');
        if (select && wb.overlayState.user) {
            let found = false;
            for (const opt of select.options) {
                if (opt.value === wb.overlayState.user) { found = true; break; }
            }
            if (!found) {
                const opt = document.createElement('option');
                opt.value = wb.overlayState.user;
                opt.textContent = (wb._userMap && wb._userMap[wb.overlayState.user]) || wb.overlayState.user;
                select.appendChild(opt);
            }
            select.value = wb.overlayState.user;
        } else if (select && !wb.overlayState.user) {
            if (!wb._pendingOverlayUser) {
                select.value = '';
            }
        }

        // Update all visual indicators
        this._updateOverlayIndicator(wb);
        this._updateScriptStatusDots(wb);
        this._updateEditorOverlayControls(wb);
        this._updateDashboardOverlayStatus(wb);
    },

    onScriptSelect(wb, scriptName) {
        this._updateEditorOverlayControls(wb);
    },

    // ==========================================
    // Event Bindings
    // ==========================================

    _bindOverlayEvents(wb) {
        // User selector → activate/deactivate overlay
        const userSelect = document.getElementById('user-select');
        userSelect?.addEventListener('change', (e) => {
            this._setOverlayUser(wb, e.target.value);
        });

        // Promote/discard buttons (delegated)
        document.addEventListener('click', (e) => {
            const btn = e.target.closest('[data-overlay-action]');
            if (!btn) return;

            const action = btn.dataset.overlayAction;
            const scriptType = btn.dataset.scriptType || wb.currentScript;

            switch (action) {
                case 'promote':      this._promoteOverlay(wb, scriptType); break;
                case 'promote-all':  this._promoteAllOverlays(wb); break;
                case 'discard':      this._discardOverlay(wb, scriptType); break;
                case 'discard-all':  this._discardAllOverlays(wb); break;
            }
        });
    },

    // ==========================================
    // Core Action: Set Overlay User
    // ==========================================

    async _setOverlayUser(wb, username) {
        try {
            if (!wb.connection?.ws || wb.connection.ws.readyState !== WebSocket.OPEN) {
                console.warn('Cannot set overlay user: not connected');
                return;
            }

            const cmd = username
                ? `ess::set_overlay_user ${username}`
                : 'ess::set_overlay_user {}';

            await wb.execTclCmd(cmd);
            console.log('Overlay user set successfully:', username || '(none)');

            // Persist on success
            if (username) {
                localStorage.setItem('ess_overlay_user', username);
            } else {
                localStorage.removeItem('ess_overlay_user');
            }

            wb.overlayState.user = username;
            wb.overlayState.active = username !== '';

            this._updateOverlayIndicator(wb);

            // Touch snapshot to get refreshed overlay_status
            if (wb.connection?.ws?.readyState === WebSocket.OPEN) {
                setTimeout(() => {
                    wb.connection.ws.send(JSON.stringify({
                        cmd: 'touch',
                        name: 'ess/snapshot'
                    }));
                }, 100);
            }
        } catch (err) {
            console.error('Failed to set overlay user:', err);
            wb.showNotification(`Failed to set overlay user: ${err.message}`, 'error');
        }
    },

    async _activatePendingOverlayUser(wb) {
        const username = wb._pendingOverlayUser;
        if (!username) return;
        wb._pendingOverlayUser = null;

        console.log(`Restoring overlay user from localStorage: "${username}"`);
        try {
            await this._setOverlayUser(wb, username);
        } catch (err) {
            console.warn('Failed to restore overlay user:', err.message);
            wb.showNotification(`Could not activate overlay for "${username}": ${err.message}. Select user manually.`, 'warning');
            localStorage.removeItem('ess_overlay_user');
            const select = document.getElementById('user-select');
            if (select) select.value = '';
        }
    },

    // ==========================================
    // Overlay Operations
    // ==========================================

    async _promoteOverlay(wb, scriptType) {
        if (!wb.overlayState.active) return;

        const confirmed = confirm(
            `Promote "${scriptType}" overlay to base (main)?\n\n` +
            `This will overwrite the main version with your working copy.`
        );
        if (!confirmed) return;

        try {
            await wb.execTclCmd(`ess::promote_overlay ${scriptType} 1`);
            wb.showNotification(`Promoted ${scriptType} to main`, 'success');
        } catch (err) {
            console.error('Promote failed:', err);
            wb.showNotification(`Promote failed: ${err.message}`, 'error');
        }
    },

    async _promoteAllOverlays(wb) {
        if (!wb.overlayState.active) return;

        const overlaid = Object.entries(wb.overlayState.scriptStatus)
            .filter(([, src]) => src === 'overlay')
            .map(([type]) => type);

        if (overlaid.length === 0) {
            wb.showNotification('No overlay files to promote', 'info');
            return;
        }

        const confirmed = confirm(
            `Promote all overlay scripts to main?\n\n` +
            `Scripts: ${overlaid.join(', ')}\n\n` +
            `This will overwrite the main versions with your working copies.`
        );
        if (!confirmed) return;

        try {
            await wb.execTclCmd('ess::promote_all_overlays 1');
            wb.showNotification(`Promoted ${overlaid.length} script(s) to main`, 'success');
        } catch (err) {
            console.error('Promote all failed:', err);
            wb.showNotification(`Promote failed: ${err.message}`, 'error');
        }
    },

    async _discardOverlay(wb, scriptType) {
        if (!wb.overlayState.active) return;

        const confirmed = confirm(
            `Discard overlay for "${scriptType}"?\n\n` +
            `This will revert to the base (main) version. Your working copy will be lost.`
        );
        if (!confirmed) return;

        try {
            await wb.execTclCmd(`ess::discard_overlay ${scriptType} 1`);
            wb.showNotification(`Discarded ${scriptType} overlay`, 'success');
        } catch (err) {
            console.error('Discard failed:', err);
            wb.showNotification(`Discard failed: ${err.message}`, 'error');
        }
    },

    async _discardAllOverlays(wb) {
        if (!wb.overlayState.active) return;

        const overlaid = Object.entries(wb.overlayState.scriptStatus)
            .filter(([, src]) => src === 'overlay')
            .map(([type]) => type);

        if (overlaid.length === 0) {
            wb.showNotification('No overlay files to discard', 'info');
            return;
        }

        const confirmed = confirm(
            `Discard ALL overlay scripts?\n\n` +
            `Scripts: ${overlaid.join(', ')}\n\n` +
            `This will revert all to base (main). Your working copies will be lost.`
        );
        if (!confirmed) return;

        try {
            await wb.execTclCmd('ess::discard_all_overlays 1');
            wb.showNotification(`Discarded ${overlaid.length} overlay(s)`, 'success');
        } catch (err) {
            console.error('Discard all failed:', err);
            wb.showNotification(`Discard failed: ${err.message}`, 'error');
        }
    },

    // ==========================================
    // User List Population
    // ==========================================

    _populateOverlayUsers(wb, users) {
        const select = document.getElementById('user-select');
        if (!select) return;

        const currentVal = select.value || wb._pendingOverlayUser || '';

        select.innerHTML = '<option value="">No user (read-only)</option>';
        wb._userMap = {};

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

                wb._userMap[username] = displayName;
            });
        }

        if (currentVal) {
            select.value = currentVal;
        }

        wb._pendingOverlayUser = null;
    },

    async _loadUsersFromDserv(wb) {
        if (!wb.dpManager) return;

        try {
            const usersDp = await wb.dpManager.get('ess/registry/users');
            const usersVal = usersDp?.data || usersDp?.value;
            if (usersVal) {
                const users = typeof usersVal === 'string' ? JSON.parse(usersVal) : usersVal;
                this._populateOverlayUsers(wb, users);
            }
        } catch (err) {
            console.warn('Could not load users from dserv:', err);
        }
    },

    // ==========================================
    // Visual Updates
    // ==========================================

    _updateOverlayIndicator(wb) {
        let badge = document.getElementById('overlay-mode-badge');

        if (wb.overlayState.active) {
            if (!badge) {
                badge = document.createElement('span');
                badge.id = 'overlay-mode-badge';
                badge.className = 'overlay-mode-badge';
                const userDiv = document.querySelector('.header-user');
                if (userDiv) userDiv.appendChild(badge);
            }

            const overlayCount = Object.values(wb.overlayState.scriptStatus)
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
    },

    _updateScriptStatusDots(wb) {
        document.querySelectorAll('.script-status-dot[data-script]').forEach(dot => {
            const scriptType = dot.dataset.script;
            const source = wb.overlayState.scriptStatus[scriptType];

            dot.className = 'script-status-dot';
            dot.title = '';

            if (!wb.overlayState.active) return;

            if (source === 'overlay') {
                dot.classList.add('modified');
                dot.title = 'Using overlay (working copy)';
            } else if (source === 'base') {
                dot.classList.add('synced');
                dot.title = 'Using base (main)';
            }
        });
    },

    _updateEditorOverlayControls(wb) {
        this._updateOverlayControlsFor(wb, 'scripts');
        this._updateOverlayControlsFor(wb, 'variants');
    },

    _updateOverlayControlsFor(wb, tab) {
        let containerId, scriptType;

        if (tab === 'scripts') {
            containerId = 'script-overlay-controls';
            scriptType = wb.currentScript;
        } else if (tab === 'variants') {
            containerId = 'variant-overlay-controls';
            scriptType = 'variants';
        } else {
            return;
        }

        let container = document.getElementById(containerId);
        const source = wb.overlayState.scriptStatus[scriptType];
        const isOverlaid = wb.overlayState.active && source === 'overlay';

        if (!isOverlaid) {
            if (container) container.innerHTML = '';
            return;
        }

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
    },

    _updateDashboardOverlayStatus(wb) {
        let overlayDetail = document.getElementById('cfg-overlay-status');

        if (wb.overlayState.active) {
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
                const overlayCount = Object.values(wb.overlayState.scriptStatus)
                    .filter(s => s === 'overlay').length;
                const user = wb.overlayState.user;

                if (overlayCount > 0) {
                    overlayDetail.innerHTML =
                        `<span class="overlay-user-label">${wb.escapeHtml(user)}</span>` +
                        `<span class="overlay-count-badge">${overlayCount} modified</span>`;
                } else {
                    overlayDetail.innerHTML =
                        `<span class="overlay-user-label">${wb.escapeHtml(user)}</span>` +
                        `<span class="overlay-clean-badge">clean</span>`;
                }
            }
        } else {
            const wrapper = overlayDetail?.closest('.config-detail');
            if (wrapper) wrapper.remove();
        }
    }
};

ESSWorkbench.registerPlugin(OverlayPlugin);
