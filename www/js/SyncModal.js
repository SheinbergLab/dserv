/**
 * SyncModal.js — Cloud sync dialog for ESS Control
 *
 * Single table of systems (plus shared libs). Expanding a system row shows
 * pull status and an inline push section (commit message, include-libs, Push).
 */

class SyncModal {
    constructor(options = {}) {
        this.connection = options.connection;
        this.dpManager = options.dpManager;
        this.essControl = options.essControl;
        this.execTclCmd = options.execTclCmd;
        this.log = options.log || console.log;

        this.registry = new RegistryClient();
        this.registryState = { url: '', workgroup: '' };
        this.users = [];
        this.pullPreview = null;
        this.pushPreviews = {};   // { [system]: previewObject }
        this.expandedKey = null;  // currently expanded row key, or null
        this.loading = false;

        this._overlay = null;
    }

    open() {
        if (!this.connection?.ws || this.connection.ws.readyState !== WebSocket.OPEN) {
            this.log('Cannot sync: not connected to dserv', 'error');
            return;
        }

        // The modal instance is reused across opens, so previews from the
        // previous session must be cleared.
        this.pullPreview = null;
        this.pushPreviews = {};
        this.expandedKey = null;

        this._buildModal();
        document.body.appendChild(this._overlay);
        this._loadUsers();
        this._loadPullPreview();
    }

    _close() {
        this._overlay?.remove();
        this._overlay = null;
    }

    _escapeHtml(str) {
        if (str == null) return '';
        return String(str)
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;');
    }

    _parseTclResult(result) {
        if (!result) return null;
        if (typeof result === 'object') return result;
        if (typeof result === 'string') {
            try { return JSON.parse(result); } catch (_) {
                return { raw: result };
            }
        }
        return result;
    }

    _getWorkgroup() {
        return this.registryState.workgroup
            || this.essControl?.state?.registryWorkgroup
            || '';
    }


    async _execSyncCmd(cmd) {
        try {
            return await this.execTclCmd(cmd);
        } catch (err) {
            const msg = err.message || '';
            if (!/invalid command name.*sync_(preview|push)/i.test(msg)) throw err;
            this.log('Reloading ess_sync module...', 'info');
            await this.execTclCmd('ess::reload_sync_module');
            return await this.execTclCmd(cmd);
        }
    }

    _getSystemsList() {
        const fromControl = this.essControl?.state?.systems;
        if (Array.isArray(fromControl) && fromControl.length) return fromControl;
        if (typeof fromControl === 'string' && fromControl.trim()) {
            return fromControl.split(/\s+/).filter(Boolean);
        }
        const current = this.essControl?.state?.currentSystem;
        return current ? [current] : [];
    }

    _buildModal() {
        const wg = this._escapeHtml(this._getWorkgroup() || '—');
        this._overlay = document.createElement('div');
        this._overlay.className = 'ess-modal-overlay ess-sync-modal-overlay';
        this._overlay.innerHTML = `
            <div class="ess-modal ess-sync-modal">
                <div class="ess-modal-header">
                    <span class="ess-modal-title">Cloud Sync</span>
                    <button type="button" class="ess-modal-close" id="sync-modal-close">&times;</button>
                </div>
                <div class="ess-modal-body">
                    <div class="ess-modal-section">
                        <label class="ess-modal-label">Workgroup</label>
                        <div class="ess-modal-value" id="sync-modal-workgroup">${wg}</div>
                    </div>
                    <div class="ess-sync-user-row">
                        <label class="ess-modal-label" for="sync-user-select">User</label>
                        <select class="ess-modal-select" id="sync-user-select"></select>
                        <button type="button" class="ess-modal-btn cancel ess-sync-user-btn" id="sync-add-user-btn">Add</button>
                        <button type="button" class="ess-modal-btn cancel ess-sync-user-btn" id="sync-remove-user-btn">Remove</button>
                    </div>
                    <div id="sync-preview-container">
                        <div class="ess-sync-preview" id="sync-pull-preview">
                            <div class="ess-modal-loading">Loading preview...</div>
                        </div>
                    </div>
                </div>
                <div class="ess-modal-footer">
                    <button type="button" class="ess-modal-btn cancel" id="sync-modal-refresh">Refresh</button>
                    <div class="ess-modal-footer-spacer"></div>
                    <button type="button" class="ess-modal-btn cancel" id="sync-modal-cancel">Cancel</button>
                    <button type="button" class="ess-modal-btn primary" id="sync-modal-action">Pull All</button>
                </div>
            </div>
            <div class="ess-modal ess-sync-add-user-modal" id="sync-add-user-modal" hidden>
                <div class="ess-modal-header">
                    <span class="ess-modal-title">Add User</span>
                    <button type="button" class="ess-modal-close" id="sync-add-user-close">&times;</button>
                </div>
                <div class="ess-modal-body">
                    <div class="ess-modal-section">
                        <label class="ess-modal-label">Username</label>
                        <input type="text" class="ess-modal-input" id="sync-new-username" placeholder="e.g., david">
                    </div>
                    <div class="ess-modal-section">
                        <label class="ess-modal-label">Full Name</label>
                        <input type="text" class="ess-modal-input" id="sync-new-fullname" placeholder="Optional">
                    </div>
                    <div class="ess-modal-section">
                        <label class="ess-modal-label">Email</label>
                        <input type="email" class="ess-modal-input" id="sync-new-email" placeholder="Optional">
                    </div>
                    <div class="ess-modal-section">
                        <label class="ess-modal-label">Role</label>
                        <select class="ess-modal-select" id="sync-new-role">
                            <option value="editor">Editor</option>
                            <option value="admin">Admin</option>
                            <option value="viewer">Viewer</option>
                        </select>
                    </div>
                </div>
                <div class="ess-modal-footer">
                    <button type="button" class="ess-modal-btn cancel" id="sync-add-user-cancel">Cancel</button>
                    <button type="button" class="ess-modal-btn primary" id="sync-add-user-submit">Add User</button>
                </div>
            </div>
        `;

        this._overlay.querySelector('#sync-modal-close').addEventListener('click', () => this._close());
        this._overlay.querySelector('#sync-modal-cancel').addEventListener('click', () => this._close());
        this._overlay.addEventListener('click', (e) => {
            if (e.target === this._overlay) this._close();
        });

        // Delegated on the stable container (not #sync-pull-preview, whose
        // innerHTML is replaced on every load/refresh).
        const previewContainer = this._overlay.querySelector('#sync-preview-container');

        previewContainer.addEventListener('click', (e) => {
            const pushBtn = e.target.closest('.ess-sync-push-btn');
            if (pushBtn) {
                e.stopPropagation();
                this._executePush(pushBtn.dataset.key);
                return;
            }
            // Don't toggle the row when interacting with push controls.
            if (e.target.closest('.ess-sync-push-block') ||
                e.target.closest('textarea') ||
                e.target.closest('input') ||
                e.target.closest('label')) {
                return;
            }
            const row = e.target.closest('.ess-sync-row');
            if (row) this._toggleRow(row.dataset.key);
        });

        previewContainer.addEventListener('input', (e) => {
            if (e.target.classList.contains('ess-sync-commit-message')) {
                this._updatePushButtonState(e.target.dataset.key);
            }
        });

        previewContainer.addEventListener('change', (e) => {
            if (e.target.classList.contains('ess-sync-include-libs')) {
                this._renderPushSection(e.target.dataset.key);
            }
        });

        this._overlay.querySelector('#sync-user-select').addEventListener('change', (e) => {
            this.registry.setUser(e.target.value);
            this._updateActionButton();
            if (this.expandedKey && this.expandedKey !== '__libs__') {
                this._updatePushButtonState(this.expandedKey);
            }
        });

        this._overlay.querySelector('#sync-add-user-btn').addEventListener('click', () => {
            this._overlay.querySelector('#sync-add-user-modal').hidden = false;
        });
        this._overlay.querySelector('#sync-add-user-close').addEventListener('click', () => {
            this._overlay.querySelector('#sync-add-user-modal').hidden = true;
        });
        this._overlay.querySelector('#sync-add-user-cancel').addEventListener('click', () => {
            this._overlay.querySelector('#sync-add-user-modal').hidden = true;
        });
        this._overlay.querySelector('#sync-add-user-submit').addEventListener('click', () => this._submitAddUser());

        this._overlay.querySelector('#sync-remove-user-btn').addEventListener('click', () => this._removeUser());

        this._overlay.querySelector('#sync-modal-action').addEventListener('click', () => this._executePullAll());
        this._overlay.querySelector('#sync-modal-refresh').addEventListener('click', () => this._refreshAll());

        this._updateActionButton();
        this._overlay.querySelector('#sync-add-user-modal').hidden = true;
    }

    async _loadUsers() {
        const select = this._overlay.querySelector('#sync-user-select');
        const wg = this._getWorkgroup();
        if (!wg) {
            select.innerHTML = '<option value="">Cloud not configured</option>';
            return;
        }
        this.registry.setWorkgroup(wg);
        try {
            this.users = await this.registry.getUsers();
            const saved = this.registry.getUser();
            select.innerHTML = this.users.map(u => {
                const name = u.fullName || u.username;
                return `<option value="${this._escapeHtml(u.username)}" data-role="${this._escapeHtml(u.role || 'editor')}">${this._escapeHtml(name)} (${this._escapeHtml(u.role || 'editor')})</option>`;
            }).join('');
            if (saved && this.users.some(u => u.username === saved)) {
                select.value = saved;
            } else if (this.users.length) {
                select.value = this.users[0].username;
            }
            this.registry.setUser(select.value);
        } catch (err) {
            select.innerHTML = `<option value="">Failed to load users</option>`;
            this.log(`Failed to load users: ${err.message}`, 'error');
        }
        this._updateActionButton();
        if (this.expandedKey && this.expandedKey !== '__libs__') {
            this._updatePushButtonState(this.expandedKey);
        }
    }

    _selectedUserRole() {
        const select = this._overlay?.querySelector('#sync-user-select');
        if (!select) return '';
        const opt = select.selectedOptions[0];
        return opt?.dataset?.role || '';
    }

    _updateActionButton() {
        const btn = this._overlay?.querySelector('#sync-modal-action');
        if (!btn) return;
        btn.textContent = 'Pull All';
        const hasPull = this._countPullItems() > 0;
        btn.disabled = this.loading || !this._getWorkgroup() || !hasPull;
        btn.title = hasPull ? '' : 'Already up to date';
    }

    _countPullItems() {
        if (!this.pullPreview) return 0;
        let n = (this.pullPreview.libs?.to_pull || []).length;
        const systems = this.pullPreview.systems || {};
        for (const sys of Object.keys(systems)) {
            n += (systems[sys].to_pull || []).length;
        }
        return n;
    }

    // Previews are served from a manifest cached in the ess subprocess, so
    // an explicit refresh is the way to pick up someone else's push.
    async _refreshAll() {
        const btn = this._overlay.querySelector('#sync-modal-refresh');
        if (btn) {
            btn.disabled = true;
            btn.textContent = 'Refreshing...';
        }
        this.pullPreview = null;
        this.pushPreviews = {};
        try {
            await this._loadPullPreview(true);
        } catch (err) {
            this.log(`Refresh failed: ${err.message}`, 'error');
        } finally {
            if (btn) {
                btn.disabled = false;
                btn.textContent = 'Refresh';
            }
        }
    }

    async _loadPullPreview(force = false) {
        const el = this._overlay.querySelector('#sync-pull-preview');
        el.innerHTML = '<div class="ess-modal-loading">Loading preview...</div>';
        this.loading = true;
        this._updateActionButton();
        try {
            const raw = await this._execSyncCmd(
                force ? 'ess::sync_preview -force' : 'ess::sync_preview');
            this.pullPreview = this._parseTclResult(raw);
            el.innerHTML = this._renderPullPreview(this.pullPreview);
            // Re-expand the previously-open row so Refresh/Pull All don't
            // silently collapse what the user was looking at.
            if (this.expandedKey) {
                const key = this.expandedKey;
                this.expandedKey = null;
                this._expandRow(key);
            }
        } catch (err) {
            el.innerHTML = `<div class="ess-modal-error">${this._escapeHtml(err.message)}</div>`;
        } finally {
            this.loading = false;
            this._updateActionButton();
        }
    }

    _plural(n, word) {
        return `${n} ${word}${n === 1 ? '' : 's'}`;
    }

    // Flatten one system's (or shared libs') preview into a display-ready
    // list of changed files plus counts. The 3-way decision says which side
    // moved: pull/conflict/cold arrive in to_pull, keep_local in skipped,
    // and extra[] are local files the cloud has never seen.
    _rowStats(data, opts = {}) {
        const isLibs = !!opts.isLibs;
        const prefix = isLibs ? 'lib/' : '';
        const toPull = data?.to_pull || [];
        const skipped = data?.skipped || [];
        const extra = data?.extra || [];
        const unchanged = data?.unchanged || 0;
        const files = [];
        let conflicts = 0;

        for (const item of toPull) {
            const dec = item.decision || 'pull';
            const isConflict = dec === 'conflict' || dec === 'cold';
            if (isConflict) conflicts++;
            files.push({
                label: prefix + (item.relpath || item.filename || '?'),
                where: isConflict ? 'conflict' : 'cloud',
                badge: dec === 'cold' ? 'no ancestor' : (isConflict ? 'conflict' : 'cloud'),
                title: dec === 'cold'
                    ? 'Differs from the cloud with no common ancestor recorded. Pulling takes the cloud version and saves your copy to .sync_displaced/'
                    : (isConflict
                        ? 'Changed both locally and on the cloud. Pulling takes the cloud version and saves your copy to .sync_displaced/'
                        : 'Changed on the cloud. Pulling updates your local copy.')
            });
        }

        for (const item of skipped) {
            files.push({
                label: prefix + (item.relpath || item.filename || '?'),
                where: 'local',
                badge: 'local',
                title: 'Changed locally. Push to send it to the cloud.'
            });
        }

        for (const relkey of extra) {
            files.push({
                label: prefix + relkey,
                where: 'local',
                badge: 'local new',
                title: 'New local file, not on the cloud yet.'
            });
        }

        files.sort((a, b) => a.label.localeCompare(b.label));

        return {
            total: unchanged + toPull.length + skipped.length + extra.length,
            unchanged,
            cloudCount: toPull.length,
            // A conflict counts on both sides, because it genuinely is both.
            localCount: skipped.length + extra.length + conflicts,
            conflicts,
            files
        };
    }

    _renderChangedFileList(files) {
        const items = files.map(f => {
            const arrow = f.where === 'cloud' ? '↓' : (f.where === 'conflict' ? '⚠' : '↑');
            return `<li class="ess-sync-file-item ess-sync-where-${f.where}" title="${this._escapeHtml(f.title)}">`
                + `${arrow} ${this._escapeHtml(f.label)} `
                + `<span class="ess-sync-badge ess-sync-badge-${f.where}">${this._escapeHtml(f.badge)}</span></li>`;
        }).join('');
        return `<ul class="ess-sync-file-list">${items}</ul>`;
    }

    _renderRowDetail(row) {
        // Never pushed: nothing to compare against, so go straight to push.
        if (!row.inRegistry) {
            return '<div class="ess-sync-summary">Not on the cloud yet — push to create it.</div>'
                + this._renderPushBlock(row.key);
        }

        const s = row.stats;
        const counts = `${this._plural(s.total, 'file')}, `
            + `${this._plural(s.localCount, 'local change')}, `
            + `${this._plural(s.cloudCount, 'cloud change')}`;

        if (!s.files.length) {
            return `<div class="ess-sync-summary">${counts} — nothing to do.</div>`;
        }

        const parts = [
            `<div class="ess-sync-summary">${counts}</div>`,
            this._renderChangedFileList(s.files)
        ];

        if (s.conflicts) {
            parts.push(`<div class="ess-sync-warning">${this._plural(s.conflicts, 'file')} changed in both places. Pull All takes the cloud version and saves your copy to <code>.sync_displaced/</code>.</div>`);
        }

        // Shared libs ride along with whichever system pushes them.
        if (row.isLibs) return parts.join('');

        if (s.cloudCount > 0) {
            // Pushing now would send stale content back to the cloud, and the
            // server rejects conflicted writes anyway. Pull first.
            parts.push(`<div class="ess-sync-extra">Pull first — the cloud has newer changes for ${this._plural(s.cloudCount, 'file')}. Push becomes available once this system is up to date.</div>`);
        } else {
            parts.push(this._renderPushBlock(row.key));
        }

        return parts.join('');
    }

    _renderPushBlock(system) {
        const k = this._escapeHtml(system);
        return `
            <div class="ess-sync-push-block">
                <div class="ess-sync-group-title">Push to cloud</div>
                <label class="ess-sync-checkbox-row">
                    <input type="checkbox" class="ess-sync-include-libs" data-key="${k}" checked>
                    Include changed shared libs
                </label>
                <textarea class="ess-sync-textarea ess-sync-commit-message" data-key="${k}" rows="2"
                    placeholder="Describe your changes..."></textarea>
                <div class="ess-sync-preview ess-sync-push-preview" data-key="${k}">
                    <div class="ess-modal-loading">Loading push status...</div>
                </div>
                <div class="ess-sync-push-actions">
                    <button type="button" class="ess-modal-btn primary ess-sync-push-btn" data-key="${k}" disabled>Push</button>
                </div>
            </div>`;
    }

    _renderPullPreview(preview) {
        if (!preview) return '<div class="ess-modal-empty">No preview data</div>';

        const libs = preview.libs || {};
        const systems = preview.systems || {};
        const allNames = new Set([...Object.keys(systems), ...this._getSystemsList()]);

        const rows = [{
            key: '__libs__',
            label: 'Shared libs',
            isLibs: true,
            inRegistry: true,
            data: libs,
            stats: this._rowStats(libs, { isLibs: true })
        }];

        for (const sysName of Array.from(allNames).sort()) {
            const sys = systems[sysName];
            const inRegistry = !!sys;
            rows.push({
                key: sysName,
                label: sysName,
                isLibs: false,
                inRegistry,
                data: sys || null,
                stats: inRegistry ? this._rowStats(sys, {}) : null
            });
        }

        const tableRows = rows.map(row => {
            const keyAttr = this._escapeHtml(row.key);
            const s = row.stats;
            const hasChanges = !!s && (s.localCount > 0 || s.cloudCount > 0);
            const badge = row.inRegistry
                ? ''
                : ' <span class="ess-sync-new-badge">new</span>';
            const totalCell = s ? s.total : '—';
            const localCell = s ? s.localCount : '—';
            const cloudCell = s ? s.cloudCount : '—';

            return `
                <tr class="ess-sync-row${hasChanges ? '' : ' ess-sync-row-uptodate'}" data-key="${keyAttr}">
                    <td class="ess-sync-col-name">${this._escapeHtml(row.label)}${badge}</td>
                    <td class="ess-sync-col-num">${totalCell}</td>
                    <td class="ess-sync-col-num${s && s.localCount ? ' ess-sync-col-local' : ''}">${localCell}</td>
                    <td class="ess-sync-col-num${s && s.cloudCount ? ' ess-sync-col-registry' : ''}">${cloudCell}</td>
                    <td class="ess-sync-col-caret"><span class="ess-sync-caret" aria-hidden="true">&#9662;</span></td>
                </tr>
                <tr class="ess-sync-detail-row" data-key="${keyAttr}" hidden>
                    <td colspan="5">${this._renderRowDetail(row)}</td>
                </tr>`;
        }).join('');

        const table = `
            <table class="ess-sync-table">
                <thead>
                    <tr>
                        <th>System</th>
                        <th>Total</th>
                        <th>Changed (local)</th>
                        <th>Changed (cloud)</th>
                        <th class="ess-sync-col-caret"></th>
                    </tr>
                </thead>
                <tbody>${tableRows}</tbody>
            </table>`;

        const pullCount = this._countPullItems();
        // Only advertise pushing for rows that actually offer it: gated rows
        // (cloud changes pending) hide their push block.
        const hasPushable = rows.some(r => !r.isLibs && (
            !r.inRegistry || (r.stats.cloudCount === 0 && r.stats.localCount > 0)
        ));
        let summaryText = pullCount === 0
            ? 'Already up to date with the cloud.'
            : `${pullCount} file(s) would be pulled.`;
        if (hasPushable) {
            summaryText += ' Select a system to push.';
        }
        const summary = `<div class="ess-sync-summary">${summaryText}</div>`;

        return summary + table;
    }

    _expandRow(key) {
        const c = this._overlay.querySelector('#sync-preview-container');
        if (this.expandedKey && this.expandedKey !== key) this._collapseRow(this.expandedKey);
        const row = c.querySelector(`.ess-sync-row[data-key="${CSS.escape(key)}"]`);
        const detail = c.querySelector(`.ess-sync-detail-row[data-key="${CSS.escape(key)}"]`);
        if (!row || !detail) { this.expandedKey = null; return; }
        row.classList.add('expanded');
        detail.hidden = false;
        this.expandedKey = key;
        if (key !== '__libs__') {
            if (this.pushPreviews[key]) this._renderPushSection(key);
            else this._loadPushPreviewFor(key);
        }
    }

    _collapseRow(key) {
        const c = this._overlay.querySelector('#sync-preview-container');
        c.querySelector(`.ess-sync-row[data-key="${CSS.escape(key)}"]`)?.classList.remove('expanded');
        const detail = c.querySelector(`.ess-sync-detail-row[data-key="${CSS.escape(key)}"]`);
        if (detail) detail.hidden = true;
        if (this.expandedKey === key) this.expandedKey = null;
    }

    _toggleRow(key) {
        if (this.expandedKey === key) this._collapseRow(key);
        else this._expandRow(key);
    }

    async _loadPushPreviewFor(system, force = false) {
        const el = this._overlay?.querySelector(`.ess-sync-push-preview[data-key="${CSS.escape(system)}"]`);
        if (!el) return;
        el.innerHTML = '<div class="ess-modal-loading">Loading push status...</div>';
        this._updatePushButtonState(system);
        try {
            const raw = await this._execSyncCmd(
                `ess::push_preview {${system}}${force ? ' -fresh' : ''}`);
            this.pushPreviews[system] = this._parseTclResult(raw);
            el.innerHTML = this._renderPushPreview(this.pushPreviews[system], system);
        } catch (err) {
            el.innerHTML = `<div class="ess-modal-error">${this._escapeHtml(err.message)}</div>`;
            delete this.pushPreviews[system];
        } finally {
            this._updatePushButtonState(system);
        }
    }

    _renderPushSection(system) {
        const el = this._overlay?.querySelector(`.ess-sync-push-preview[data-key="${CSS.escape(system)}"]`);
        const preview = this.pushPreviews[system];
        if (el && preview) el.innerHTML = this._renderPushPreview(preview, system);
        this._updatePushButtonState(system);
    }

    _countPushChangesFor(system) {
        const preview = this.pushPreviews[system];
        if (!preview) return 0;
        let n = (preview.changed || []).length;
        const newCount = (preview.new || []).length;
        if (newCount > 0) n += newCount;
        const includeLibs = this._overlay?.querySelector(
            `.ess-sync-include-libs[data-key="${CSS.escape(system)}"]`)?.checked;
        if (includeLibs) n += (preview.libs || []).length;
        return n;
    }

    // The files themselves are already listed above the push block, so this
    // only summarises what the push would carry. The exception is a system the
    // cloud has never seen: nothing is listed above it, so name the files here.
    _renderPushPreview(preview, system) {
        if (!preview) return '<div class="ess-modal-empty">No preview data</div>';
        const parts = [];
        const items = [...(preview.changed || []), ...(preview.new || [])];
        const missing = preview.missing || [];
        const allLibs = preview.libs || [];
        const includeLibs = this._overlay?.querySelector(
            `.ess-sync-include-libs[data-key="${CSS.escape(system)}"]`)?.checked;
        const libCount = includeLibs ? allLibs.length : 0;

        if (!preview.systemExists) {
            parts.push('<div class="ess-sync-warning">System not on the cloud yet — push will create it.</div>');
            if (items.length) {
                parts.push('<ul class="ess-sync-file-list">');
                for (const item of items) {
                    parts.push(`<li class="ess-sync-file-item ess-sync-where-local">↑ ${this._escapeHtml(item.relkey || item.relpath || item.filename || '?')}</li>`);
                }
                parts.push('</ul>');
            }
        }

        if (items.length || libCount) {
            const bits = [];
            if (items.length) bits.push(this._plural(items.length, 'script'));
            if (libCount) bits.push(this._plural(libCount, 'shared lib'));
            parts.push(`<div class="ess-sync-summary">${bits.join(' and ')} will be pushed.</div>`);
        } else if (allLibs.length) {
            parts.push(`<div class="ess-sync-summary">${this._plural(allLibs.length, 'changed shared lib')} excluded — tick "Include changed shared libs" to push them.</div>`);
        } else {
            parts.push('<div class="ess-sync-summary">Nothing to push.</div>');
        }

        if (missing.length) {
            parts.push('<div class="ess-sync-extra">Missing locally: ' + missing.map(m => this._escapeHtml(m)).join(', ') + '</div>');
        }
        return parts.join('');
    }

    _updatePushButtonState(system) {
        const btn = this._overlay?.querySelector(
            `.ess-sync-push-btn[data-key="${CSS.escape(system)}"]`);
        if (!btn) return;
        const role = this._selectedUserRole();
        const changes = this._countPushChangesFor(system);
        const msg = this._overlay.querySelector(
            `.ess-sync-commit-message[data-key="${CSS.escape(system)}"]`)?.value.trim() || '';
        const preview = this.pushPreviews[system];
        const needAdd = preview && !preview.systemExists && (preview.new?.length || 0) > 0;

        btn.disabled = this.loading
            || !this._getWorkgroup()
            || role === 'viewer'
            || changes === 0
            || !msg;
        if (role === 'viewer') {
            btn.title = 'Viewer role cannot push';
        } else if (changes === 0) {
            btn.title = 'No changes to push';
        } else if (!msg) {
            btn.title = 'Commit message required';
        } else if (needAdd) {
            btn.title = 'Will create the system on the cloud if needed';
        } else {
            btn.title = '';
        }
    }

    async _executePush(system) {
        const btn = this._overlay.querySelector(
            `.ess-sync-push-btn[data-key="${CSS.escape(system)}"]`);
        if (!btn || btn.disabled) return;
        btn.disabled = true;
        const origText = btn.textContent;
        btn.textContent = 'Pushing...';
        this.loading = true;
        this._updateActionButton();
        try {
            const user = this._overlay.querySelector('#sync-user-select').value;
            const comment = this._overlay.querySelector(
                `.ess-sync-commit-message[data-key="${CSS.escape(system)}"]`).value.trim();
            const includeLibs = this._overlay.querySelector(
                `.ess-sync-include-libs[data-key="${CSS.escape(system)}"]`).checked ? 1 : 0;
            const preview = this.pushPreviews[system];
            const needAdd = (preview?.new?.length || 0) > 0;
            const safeComment = comment.replace(/[{}]/g, '');
            const safeUser = user.replace(/[{}]/g, '');
            let cmd = `ess::push_system {${system}} -user {${safeUser}} -comment {${safeComment}} -include_libs ${includeLibs}`;
            if (needAdd) cmd += ' -add';
            const result = await this._execSyncCmd(cmd);
            const parsed = this._parseTclResult(result);
            this.log(`Push (${system}): ${parsed?.pushed || 0} updated, ${parsed?.added || 0} added, ${parsed?.lib_pushed || 0} libs`, 'info');
            if (parsed?.errors?.length) this.log(`Push errors: ${parsed.errors.join('; ')}`, 'error');
            await this._loadPushPreviewFor(system, true);
            // Pushing can change this system's pull status (new base checksum).
            await this._loadPullPreview();
        } catch (err) {
            this.log(`Push failed: ${err.message}`, 'error');
        } finally {
            this.loading = false;
            btn.textContent = origText;
            this._updatePushButtonState(system);
            this._updateActionButton();
        }
    }

    async _executePullAll() {
        const btn = this._overlay.querySelector('#sync-modal-action');
        btn.disabled = true;
        const origText = btn.textContent;
        btn.textContent = 'Pulling...';
        this.loading = true;

        try {
            const result = await this._execSyncCmd('ess::sync_base');
            const parsed = this._parseTclResult(result);
            const pulled = parsed?.pulled ?? 0;
            const unchanged = parsed?.unchanged ?? 0;
            const errors = parsed?.errors || [];
            this.log(`Sync complete: ${pulled} pulled, ${unchanged} unchanged`, errors.length ? 'warn' : 'info');
            if (errors.length) this.log(`Sync errors: ${errors.join('; ')}`, 'error');
            this.pushPreviews = {};
            await this._loadPullPreview();
        } catch (err) {
            this.log(`Pull failed: ${err.message}`, 'error');
        } finally {
            this.loading = false;
            btn.textContent = origText;
            this._updateActionButton();
        }
    }

    async _submitAddUser() {
        const username = this._overlay.querySelector('#sync-new-username').value.trim();
        const fullName = this._overlay.querySelector('#sync-new-fullname').value.trim();
        const email = this._overlay.querySelector('#sync-new-email').value.trim();
        const role = this._overlay.querySelector('#sync-new-role').value;
        if (!username) { alert('Username required'); return; }
        try {
            await this.registry.addUser(username, fullName, email, role);
            this._overlay.querySelector('#sync-add-user-modal').hidden = true;
            this._overlay.querySelector('#sync-new-username').value = '';
            this._overlay.querySelector('#sync-new-fullname').value = '';
            this._overlay.querySelector('#sync-new-email').value = '';
            await this._loadUsers();
            this._overlay.querySelector('#sync-user-select').value = username;
            this.registry.setUser(username);
        } catch (err) {
            alert(`Failed: ${err.message}`);
        }
    }

    async _removeUser() {
        const username = this._overlay.querySelector('#sync-user-select').value;
        if (!username) return;
        if (!confirm(`Remove ${username} from workgroup?`)) return;
        try {
            await this.registry.deleteUser(username);
            await this._loadUsers();
        } catch (err) {
            alert(`Failed: ${err.message}`);
        }
    }

    setRegistryState(url, workgroup) {
        if (url) this.registryState.url = url;
        if (workgroup) {
            this.registryState.workgroup = workgroup;
            this.registry.setWorkgroup(workgroup);
        }
        if (url) this.registry.baseUrl = url;
    }
}
