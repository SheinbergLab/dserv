/**
 * ESS Editor-User Plugin
 *
 * Manages the editor identity: who is making edits from this browser.
 * The username tags saves and registry commits for attribution, and
 * the editors stay read-only until one is chosen.
 *
 * (This plugin previously managed the per-user overlay layer —
 * promote/discard of per-user working copies.  The overlay was retired
 * 2026-08; edits now go straight to the synced tree, and push/pull to
 * the cloud is handled by the Sync modal in ess_control.)
 *
 * Backend commands:
 *   ess::set_editor_user <username>
 *
 * Snapshot fields:
 *   editor_user — username string or "" if none set
 */

const EditorUserPlugin = {

    // ==========================================
    // Lifecycle Hooks
    // ==========================================

    onInit(wb) {
        wb.editorUser = '';
        wb._userMap = {};

        const userSelect = document.getElementById('user-select');
        userSelect?.addEventListener('change', (e) => {
            this._setEditorUser(wb, e.target.value);
        });

        // Restore last user from localStorage (legacy key carried the
        // same value when this plugin managed the overlay)
        const lastUser = localStorage.getItem('ess_editor_user')
            || localStorage.getItem('ess_overlay_user');
        if (lastUser) {
            wb._pendingEditorUser = lastUser;
        }
    },

    onConnected(wb) {
        // Activate pending user after a short delay
        setTimeout(() => this._activatePendingEditorUser(wb), 500);
        this._loadUsers(wb);

        // Footer workgroup indicator (was fed by the retired registry plugin)
        wb.dpManager?.subscribe('ess/registry/workgroup', (dp) => {
            const v = (dp && typeof dp === 'object') ? (dp.data ?? dp.value ?? '') : dp;
            const el = document.querySelector('#registry-status span:last-child');
            if (el) el.textContent = v ? `Registry: ${v}` : 'Registry: —';
        });
        try {
            wb.connection.ws.send(JSON.stringify({ cmd: 'touch', name: 'ess/registry/workgroup' }));
        } catch (e) { /* fine */ }
    },

    // Workgroup roster from the scripts subprocess (same source the
    // Sync modal uses).
    async _loadUsers(wb) {
        try {
            const r = await wb.connection.evalAsync('send scripts {scripts::users}');
            const users = JSON.parse(r)?.users || [];
            this._populateUsers(wb, users);
        } catch (err) {
            console.warn('Could not load users for editor selector:', err);
        }
    },

    onSnapshot(wb, snapshot) {
        wb.editorUser = snapshot.editor_user || '';

        // Sync user dropdown to match backend state
        const select = document.getElementById('user-select');
        if (select && wb.editorUser) {
            let found = false;
            for (const opt of select.options) {
                if (opt.value === wb.editorUser) { found = true; break; }
            }
            if (!found) {
                const opt = document.createElement('option');
                opt.value = wb.editorUser;
                opt.textContent = (wb._userMap && wb._userMap[wb.editorUser]) || wb.editorUser;
                select.appendChild(opt);
            }
            select.value = wb.editorUser;
        } else if (select && !wb.editorUser) {
            if (!wb._pendingEditorUser) {
                select.value = '';
            }
        }

    },

    // ==========================================
    // Core Action: Set Editor User
    // ==========================================

    async _setEditorUser(wb, username) {
        try {
            if (!wb.connection?.ws || wb.connection.ws.readyState !== WebSocket.OPEN) {
                console.warn('Cannot set editor user: not connected');
                return;
            }

            const cmd = username
                ? `ess::set_editor_user ${username}`
                : 'ess::set_editor_user {}';

            await wb.execTclCmd(cmd);
            console.log('Editor user set successfully:', username || '(none)');

            // Persist on success
            if (username) {
                localStorage.setItem('ess_editor_user', username);
            } else {
                localStorage.removeItem('ess_editor_user');
            }
            localStorage.removeItem('ess_overlay_user');

            wb.editorUser = username;
        } catch (err) {
            console.error('Failed to set editor user:', err);
            wb.showNotification(`Failed to set editor user: ${err.message}`, 'error');
        }
    },

    async _activatePendingEditorUser(wb) {
        const username = wb._pendingEditorUser;
        if (!username) return;
        wb._pendingEditorUser = null;

        console.log(`Restoring editor user from localStorage: "${username}"`);
        try {
            await this._setEditorUser(wb, username);
        } catch (err) {
            console.warn('Failed to restore editor user:', err.message);
            wb.showNotification(`Could not set editor user "${username}": ${err.message}. Select user manually.`, 'warning');
            localStorage.removeItem('ess_editor_user');
            const select = document.getElementById('user-select');
            if (select) select.value = '';
        }
    },

    // ==========================================
    // User List Population
    // ==========================================

    _populateUsers(wb, users) {
        const select = document.getElementById('user-select');
        if (!select) return;

        const currentVal = select.value || wb._pendingEditorUser || '';

        select.innerHTML = '<option value="">No user set</option>';
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

        wb._pendingEditorUser = null;
    },
};

ESSWorkbench.registerPlugin(EditorUserPlugin);
