/**
 * SyncModal.js — Cloud sync dialog for ESS Control
 *
 * Single table of systems (plus shared libs). Expanding a row shows pull
 * status with per-file diff/history views and an inline push section
 * (commit message, include-libs, Push).
 *
 * Backed by the `scripts` subprocess (lib/ess_scripts-1.0.tm): previews,
 * pulls, pushes, diffs, history, and the user roster all run there, so
 * registry HTTP never blocks the ess interp or the main interp. Fast
 * commands round-trip via connection.evalAsync ("send scripts {...}");
 * mutating commands (pull/push) are fired with sendNoReply and resolved
 * from their scripts/sync_result / scripts/push_result datapoint — the
 * subprocess publishes exactly one result per invocation, refusals
 * included, so nothing blocks anywhere while a sync runs.
 */

class SyncModal {
    constructor(options = {}) {
        this.connection = options.connection;
        this.dpManager = options.dpManager;
        this.essControl = options.essControl;
        this.log = options.log || console.log;

        this.users = [];
        this.pullPreview = null;
        this.pushPreviews = {};  // { [system]: previewObject }
        this.expandedKey = null; // currently expanded row key, or null
        this.loading = false;

        this._rowFiles = {};     // { [rowKey]: file-entry array } for diff/history actions
        this._overlay = null;
    }

    open() {
        if (!this.connection || !this.connection.connected) {
            this.log('Cannot sync: not connected to dserv', 'error');
            return;
        }

        // The modal instance is reused across opens, so previews from the
        // previous session must be cleared.
        this.pullPreview = null;
        this.pushPreviews = {};
        this.expandedKey = null;
        this._rowFiles = {};

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

    // Strip characters that could break out of a braced Tcl word. The
    // values interpolated into commands are registry-derived names plus
    // the user's commit message; none legitimately contain these.
    _tclSafe(str) {
        return String(str ?? '').replace(/[{}\[\]$\\;]/g, '');
    }

    // ── Transport ───────────────────────────────────────────────────

    // Fast command: evaluate in the scripts subprocess, await the reply.
    // evalAsync uses the requestId protocol, so the server's websocket
    // loop is never blocked and responses can't cross-resolve.
    async _exec(cmd, timeoutMs = 30000) {
        return this.connection.evalAsync(`send scripts {${cmd}}`, timeoutMs);
    }

    async _execJson(cmd, timeoutMs = 30000) {
        const raw = await this._exec(cmd, timeoutMs);
        try {
            return JSON.parse(raw);
        } catch (e) {
            throw new Error(`Bad response from scripts subprocess: ${String(raw).slice(0, 200)}`);
        }
    }

    // Slow mutating command: fire with sendNoReply (returns immediately,
    // blocking nothing) and resolve from the operation's result
    // datapoint. Subscribe before firing so a fast result can't be missed.
    _execAwaitResult(cmd, dpName, matchFn, timeoutMs = 120000) {
        return new Promise((resolve, reject) => {
            let done = false;
            let unsubscribe = null;
            const finish = (fn, arg) => {
                if (done) return;
                done = true;
                clearTimeout(timer);
                if (unsubscribe) unsubscribe();
                fn(arg);
            };
            const timer = setTimeout(
                () => finish(reject, new Error(`Timed out waiting for ${dpName}`)),
                timeoutMs);

            unsubscribe = this.dpManager.subscribe(dpName, (data) => {
                const raw = data.value !== undefined ? data.value : data.data;
                let parsed;
                try { parsed = JSON.parse(raw); } catch (e) { return; }
                if (matchFn && !matchFn(parsed)) return;
                finish(resolve, parsed);
            });

            this.connection.evalAsync(`sendNoReply scripts {${cmd}}`)
                .catch(err => finish(reject, err));
        });
    }

    _getWorkgroup() {
        return this.essControl?.state?.registryWorkgroup || '';
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

    // ── Modal skeleton ──────────────────────────────────────────────

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
            const action = e.target.closest('.ess-sync-action');
            if (action) {
                e.preventDefault();
                e.stopPropagation();
                this._onFileAction(action.dataset.row, parseInt(action.dataset.idx, 10),
                    action.dataset.action);
                return;
            }
            const version = e.target.closest('.ess-sync-hist-row');
            if (version) {
                e.stopPropagation();
                this._onHistoryVersion(version.dataset.row, parseInt(version.dataset.idx, 10),
                    version.dataset.checksum);
                return;
            }
            const pushBtn = e.target.closest('.ess-sync-push-btn');
            if (pushBtn) {
                e.stopPropagation();
                this._executePush(pushBtn.dataset.key);
                return;
            }
            // Don't toggle the row when interacting with push controls or
            // an open file-detail panel.
            if (e.target.closest('.ess-sync-push-block') ||
                e.target.closest('.ess-sync-file-detail') ||
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
            localStorage.setItem('ess_registry_user', e.target.value);
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

    // ── Users (roster lives on the registry; routed via scripts) ────

    async _loadUsers() {
        const select = this._overlay.querySelector('#sync-user-select');
        try {
            const data = await this._execJson('scripts::users');
            this.users = data.users || [];
            const saved = localStorage.getItem('ess_registry_user');
            select.innerHTML = this.users.map(u => {
                const name = u.fullName || u.username;
                return `<option value="${this._escapeHtml(u.username)}" data-role="${this._escapeHtml(u.role || 'editor')}">${this._escapeHtml(name)} (${this._escapeHtml(u.role || 'editor')})</option>`;
            }).join('');
            if (saved && this.users.some(u => u.username === saved)) {
                select.value = saved;
            } else if (this.users.length) {
                select.value = this.users[0].username;
            }
            if (select.value) localStorage.setItem('ess_registry_user', select.value);
        } catch (err) {
            select.innerHTML = `<option value="">Failed to load users</option>`;
            this.log(`Failed to load users: ${err.message}`, 'error');
        }
        this._updateActionButton();
        if (this.expandedKey && this.expandedKey !== '__libs__') {
            this._updatePushButtonState(this.expandedKey);
        }
    }

    _selectedUser() {
        return this._overlay?.querySelector('#sync-user-select')?.value || '';
    }

    _selectedUserRole() {
        const select = this._overlay?.querySelector('#sync-user-select');
        if (!select) return '';
        const opt = select.selectedOptions[0];
        return opt?.dataset?.role || '';
    }

    async _submitAddUser() {
        const username = this._tclSafe(this._overlay.querySelector('#sync-new-username').value.trim());
        const fullName = this._tclSafe(this._overlay.querySelector('#sync-new-fullname').value.trim());
        const email = this._tclSafe(this._overlay.querySelector('#sync-new-email').value.trim());
        const role = this._overlay.querySelector('#sync-new-role').value;
        if (!username) { alert('Username required'); return; }
        try {
            await this._exec(`scripts::user_add {${username}} {${fullName}} {${email}} {${role}}`);
            this._overlay.querySelector('#sync-add-user-modal').hidden = true;
            this._overlay.querySelector('#sync-new-username').value = '';
            this._overlay.querySelector('#sync-new-fullname').value = '';
            this._overlay.querySelector('#sync-new-email').value = '';
            await this._loadUsers();
            this._overlay.querySelector('#sync-user-select').value = username;
            localStorage.setItem('ess_registry_user', username);
        } catch (err) {
            alert(`Failed: ${err.message}`);
        }
    }

    async _removeUser() {
        const username = this._selectedUser();
        if (!username) return;
        if (!confirm(`Remove ${username} from workgroup?`)) return;
        try {
            await this._exec(`scripts::user_remove {${this._tclSafe(username)}}`);
            await this._loadUsers();
        } catch (err) {
            alert(`Failed: ${err.message}`);
        }
    }

    // ── Pull preview ────────────────────────────────────────────────

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

    // Previews are served from a manifest cached in the scripts
    // subprocess (60s TTL), so an explicit refresh is the way to pick up
    // someone else's push immediately.
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
            this.pullPreview = await this._execJson(
                force ? 'scripts::sync_preview -fresh' : 'scripts::sync_preview');
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
    // list of changed files plus counts. The 3-way decision says which
    // side moved: pull/conflict/cold arrive in to_pull, keep_local in
    // skipped, and extra[] are local files the cloud has never seen.
    //
    // The counts keep push and pull separable on purpose: pushCount
    // (edited + conflicts) is what actually needs pushing, newCount is
    // untracked strays. Lumping them into one "local" number buried the
    // real edits under a dev machine's stray files.
    _rowStats(data, opts = {}) {
        const isLibs = !!opts.isLibs;
        const system = opts.system || '';
        const prefix = isLibs ? 'lib/' : '';
        const toPull = data?.to_pull || [];
        const skipped = data?.skipped || [];
        const extra = data?.extra || [];
        const unchanged = data?.unchanged || 0;
        const files = [];
        let conflicts = 0;

        const fileEntry = (item, where, badge, title) => ({
            label: prefix + (item.relpath || item.filename || '?'),
            where, badge, title,
            kind: isLibs ? 'lib' : 'script',
            system,
            protocol: item.protocol ?? '',
            type: item.type ?? '',
            filename: item.filename ?? '',
            diffable: true
        });

        for (const item of toPull) {
            const dec = item.decision || 'pull';
            const isConflict = dec === 'conflict' || dec === 'cold';
            if (isConflict) conflicts++;
            files.push(fileEntry(item,
                isConflict ? 'conflict' : 'cloud',
                dec === 'cold' ? 'no ancestor' : (isConflict ? 'conflict' : 'cloud'),
                dec === 'cold'
                    ? 'Differs from the cloud with no common ancestor recorded. Pulling takes the cloud version and saves your copy to .sync_displaced/'
                    : (isConflict
                        ? 'Changed both locally and on the cloud. Pulling takes the cloud version and saves your copy to .sync_displaced/'
                        : 'Changed on the cloud. Pulling updates your local copy.')));
        }

        for (const item of skipped) {
            files.push(fileEntry(item, 'local', 'edited',
                'Changed locally. Push to send it to the cloud.'));
        }

        for (const relkey of extra) {
            files.push({
                label: prefix + relkey,
                where: 'local',
                badge: 'local new',
                title: 'New local file, not on the cloud yet.',
                kind: isLibs ? 'lib' : 'script',
                system,
                untracked: true,
                diffable: false,
                // local-only files can be stashed to <project>/.trash
                stashable: true,
                stashPath: isLibs ? `lib/${relkey}` : `${system}/${relkey}`
            });
        }

        // Push-needed edits lead, then conflicts, then cloud pulls;
        // untracked strays render last (collapsed by the file list).
        const rank = f => f.untracked ? 3
            : (f.where === 'conflict' ? 1 : (f.where === 'cloud' ? 2 : 0));
        files.sort((a, b) => (rank(a) - rank(b)) || a.label.localeCompare(b.label));

        return {
            total: unchanged + toPull.length + skipped.length + extra.length,
            unchanged,
            cloudCount: toPull.length,
            // A conflict counts on both sides, because it genuinely is both.
            pushCount: skipped.length + conflicts,
            newCount: extra.length,
            conflicts,
            files
        };
    }

    _renderChangedFileList(files, rowKey) {
        const item = (f, idx) => {
            const arrow = f.where === 'cloud' ? '↓' : (f.where === 'conflict' ? '⚠' : '↑');
            const links = [];
            if (f.diffable) {
                links.push(`<a href="#" class="ess-sync-action" data-action="diff" data-row="${this._escapeHtml(rowKey)}" data-idx="${idx}">diff</a>`);
                if (f.kind === 'script') {
                    links.push(`<a href="#" class="ess-sync-action" data-action="history" data-row="${this._escapeHtml(rowKey)}" data-idx="${idx}">history</a>`);
                }
            }
            if (f.stashable) {
                links.push(`<a href="#" class="ess-sync-action ess-sync-action-stash" data-action="stash" data-row="${this._escapeHtml(rowKey)}" data-idx="${idx}" title="Move to .trash (kept ~30 days) — removes it from the tree and the badge">stash</a>`);
            }
            const actions = links.length
                ? `<span class="ess-sync-file-actions">${links.join(' ')}</span>` : '';
            return `<li class="ess-sync-file-item ess-sync-where-${f.where}" title="${this._escapeHtml(f.title)}">`
                + `${arrow} ${this._escapeHtml(f.label)} `
                + `<span class="ess-sync-badge ess-sync-badge-${f.where}">${this._escapeHtml(f.badge)}</span>`
                + `${actions}</li>`
                + `<li class="ess-sync-file-detail" data-row="${this._escapeHtml(rowKey)}" data-idx="${idx}" hidden></li>`;
        };

        // Untracked strays fold away so real push/pull work stays legible;
        // indexes stay global across both lists (diff/stash handlers key
        // into one files array).
        const main = [];
        const untracked = [];
        files.forEach((f, idx) => (f.untracked ? untracked : main).push(item(f, idx)));

        let html = main.length ? `<ul class="ess-sync-file-list">${main.join('')}</ul>` : '';
        if (untracked.length) {
            html += `<details class="ess-sync-untracked">`
                + `<summary>${this._plural(untracked.length, 'untracked local file')}`
                + ` — not on the cloud (stash to clean up)</summary>`
                + `<ul class="ess-sync-file-list">${untracked.join('')}</ul>`
                + `</details>`;
        }
        return html;
    }

    _renderRowDetail(row) {
        this._rowFiles[row.key] = row.stats ? row.stats.files : [];

        // Never pushed: nothing to compare against, so go straight to push.
        if (!row.inRegistry) {
            return '<div class="ess-sync-summary">Not on the cloud yet — push to create it.</div>'
                + this._renderPushBlock(row.key);
        }

        const s = row.stats;
        const parts_ = [`${this._plural(s.total, 'file')}`];
        if (s.pushCount) parts_.push(`<b>${s.pushCount} to push</b>`);
        if (s.cloudCount) parts_.push(`${s.cloudCount} to pull`);
        if (s.newCount) parts_.push(`${s.newCount} untracked`);
        if (parts_.length === 1) parts_.push('in sync');
        const counts = parts_.join(', ');

        if (!s.files.length) {
            return `<div class="ess-sync-summary">${counts} — nothing to do.</div>`;
        }

        const parts = [
            `<div class="ess-sync-summary">${counts}</div>`,
            this._renderChangedFileList(s.files, row.key)
        ];

        if (s.conflicts) {
            parts.push(`<div class="ess-sync-warning">${this._plural(s.conflicts, 'file')} changed in both places. Pull All takes the cloud version and saves your copy to <code>.sync_displaced/</code>.</div>`);
        }

        // Shared libs ride along with whichever system pushes them.
        if (row.isLibs) return parts.join('');

        if (s.cloudCount > 0) {
            // Pushing now would send stale content back to the cloud, and
            // the registry rejects conflicted writes anyway. Pull first.
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
                stats: inRegistry ? this._rowStats(sys, { system: sysName }) : null
            });
        }

        // Systems that need pushing first — that is what the operator came
        // to find — then pull-pending, then untracked-only/new, then clean.
        // Shared libs stay pinned on top. Alphabetical within a group.
        const rowRank = r => {
            if (r.isLibs) return -1;
            if (!r.inRegistry) return 2;
            const s = r.stats;
            if (s.pushCount > 0) return 0;
            if (s.cloudCount > 0) return 1;
            if (s.newCount > 0) return 2;
            return 3;
        };
        rows.sort((a, b) => (rowRank(a) - rowRank(b)) || a.label.localeCompare(b.label));

        const tableRows = rows.map(row => {
            const keyAttr = this._escapeHtml(row.key);
            const s = row.stats;
            // Untracked-only rows render dimmed like clean ones: strays
            // should not light a row up as if it had real work pending.
            const hasChanges = !!s && (s.pushCount > 0 || s.cloudCount > 0);
            const badge = row.inRegistry
                ? ''
                : ' <span class="ess-sync-new-badge">new</span>';
            const totalCell = s ? s.total : '—';
            const pushCell = s ? s.pushCount : '—';
            const cloudCell = s ? s.cloudCount : '—';
            const newCell = s ? (s.newCount || '') : '—';

            return `
                <tr class="ess-sync-row${hasChanges ? '' : ' ess-sync-row-uptodate'}" data-key="${keyAttr}">
                    <td class="ess-sync-col-name">${this._escapeHtml(row.label)}${badge}</td>
                    <td class="ess-sync-col-num">${totalCell}</td>
                    <td class="ess-sync-col-num${s && s.pushCount ? ' ess-sync-col-local' : ''}">${pushCell}</td>
                    <td class="ess-sync-col-num${s && s.cloudCount ? ' ess-sync-col-registry' : ''}">${cloudCell}</td>
                    <td class="ess-sync-col-num ess-sync-col-new">${newCell}</td>
                    <td class="ess-sync-col-caret"><span class="ess-sync-caret" aria-hidden="true">&#9662;</span></td>
                </tr>
                <tr class="ess-sync-detail-row" data-key="${keyAttr}" hidden>
                    <td colspan="6">${this._renderRowDetail(row)}</td>
                </tr>`;
        }).join('');

        const table = `
            <table class="ess-sync-table">
                <thead>
                    <tr>
                        <th>System</th>
                        <th>Total</th>
                        <th>To push</th>
                        <th>To pull</th>
                        <th class="ess-sync-col-new">Untracked</th>
                        <th class="ess-sync-col-caret"></th>
                    </tr>
                </thead>
                <tbody>${tableRows}</tbody>
            </table>`;

        // Lead with what needs pushing — the question the operator opens
        // this modal to answer — then pull state, then stray noise.
        const pushRows = rows.filter(r => r.stats && r.stats.pushCount > 0)
            .map(r => r.label);
        const totalNew = rows.reduce((n, r) => n + (r.stats?.newCount || 0), 0);
        const pullCount = this._countPullItems();

        const bits = [];
        bits.push(pushRows.length
            ? `<b>Unpushed changes in: ${pushRows.map(l => this._escapeHtml(l)).join(', ')}.</b>`
            : 'Nothing waiting to push.');
        bits.push(pullCount === 0
            ? 'Up to date with the cloud.'
            : `${pullCount} file(s) would be pulled.`);
        if (totalNew > 0) {
            bits.push(`${this._plural(totalNew, 'untracked local file')}.`);
        }
        const summary = `<div class="ess-sync-summary">${bits.join(' ')}</div>`;

        return summary + table;
    }

    // ── Row expand/collapse ─────────────────────────────────────────

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

    // ── Per-file diff / history panels ──────────────────────────────

    _fileDetailEl(rowKey, idx) {
        return this._overlay?.querySelector(
            `.ess-sync-file-detail[data-row="${CSS.escape(rowKey)}"][data-idx="${idx}"]`);
    }

    async _onFileAction(rowKey, idx, action) {
        const f = (this._rowFiles[rowKey] || [])[idx];
        const el = this._fileDetailEl(rowKey, idx);
        if (!f || !el) return;

        if (action === 'stash') {
            if (!confirm(`Move ${f.label} to the trash?\n\nIt leaves the tree (and the badge) now and stays recoverable in .trash/ for ~30 days.`)) {
                return;
            }
            el.hidden = false;
            el.innerHTML = '<div class="ess-modal-loading">Stashing...</div>';
            try {
                await this._exec(`scripts::stash {${this._tclSafe(f.stashPath)}}`);
                this.log(`Stashed ${f.stashPath} — recoverable in .trash/ for ~30 days`, 'info');
                delete this.pushPreviews[rowKey];
                await this._loadPullPreview();
            } catch (err) {
                el.innerHTML = `<div class="ess-modal-error">${this._escapeHtml(err.message)}</div>`;
            }
            return;
        }

        // Toggle off if the same view is already open.
        if (!el.hidden && el.dataset.view === action) {
            el.hidden = true;
            el.dataset.view = '';
            return;
        }
        el.hidden = false;
        el.dataset.view = action;
        el.innerHTML = '<div class="ess-modal-loading">Loading...</div>';

        try {
            if (action === 'diff') {
                let parsed;
                if (f.kind === 'lib') {
                    parsed = await this._execJson(
                        `scripts::lib_diff {${this._tclSafe(f.filename)}}`);
                } else {
                    parsed = await this._execJson(
                        `scripts::diff {${this._tclSafe(f.system)}} {${this._tclSafe(f.protocol)}} {${this._tclSafe(f.type)}}`);
                }
                el.innerHTML = this._renderDiff(parsed);
            } else if (action === 'history') {
                const parsed = await this._execJson(
                    `scripts::history {${this._tclSafe(f.system)}} {${this._tclSafe(f.protocol)}} {${this._tclSafe(f.type)}}`);
                el.innerHTML = this._renderHistory(parsed, rowKey, idx);
            }
        } catch (err) {
            el.innerHTML = `<div class="ess-modal-error">${this._escapeHtml(err.message)}</div>`;
        }
    }

    async _onHistoryVersion(rowKey, idx, checksum) {
        const f = (this._rowFiles[rowKey] || [])[idx];
        const el = this._fileDetailEl(rowKey, idx);
        if (!f || !el) return;
        el.innerHTML = '<div class="ess-modal-loading">Loading diff...</div>';
        try {
            const parsed = await this._execJson(
                `scripts::history_diff {${this._tclSafe(f.system)}} {${this._tclSafe(f.protocol)}} {${this._tclSafe(f.type)}} {${this._tclSafe(checksum)}}`);
            el.innerHTML = `<div class="ess-sync-hist-back"><a href="#" class="ess-sync-action" data-action="history" data-row="${this._escapeHtml(rowKey)}" data-idx="${idx}">&larr; back to history</a></div>`
                + this._renderDiff(parsed);
            el.dataset.view = 'history-diff';
        } catch (err) {
            el.innerHTML = `<div class="ess-modal-error">${this._escapeHtml(err.message)}</div>`;
        }
    }

    _renderDiff(parsed) {
        if (!parsed) return '<div class="ess-modal-empty">No diff data</div>';
        if (parsed.registryExists === false) {
            return '<div class="ess-sync-extra">Not on the cloud — nothing to diff against.</div>';
        }
        if (parsed.localExists === false) {
            return '<div class="ess-sync-extra">No local copy yet — pulling will create it.</div>'
                + (parsed.changed ? this._renderDiffText(parsed.diff) : '');
        }
        if (!parsed.changed) {
            return '<div class="ess-sync-extra">Identical — no differences.</div>';
        }
        return this._renderDiffText(parsed.diff);
    }

    _renderDiffText(text) {
        const lines = String(text || '').split('\n').map(line => {
            let cls = 'ctx';
            if (line.startsWith('+++') || line.startsWith('---')) cls = 'file';
            else if (line.startsWith('@@')) cls = 'hunk';
            else if (line.startsWith('+')) cls = 'add';
            else if (line.startsWith('-')) cls = 'del';
            return `<span class="ess-sync-diff-${cls}">${this._escapeHtml(line)}\n</span>`;
        }).join('');
        return `<pre class="ess-sync-diff">${lines}</pre>`;
    }

    _renderHistory(parsed, rowKey, idx) {
        const entries = parsed?.history || [];
        if (!entries.length) {
            return '<div class="ess-sync-extra">No saved versions on the cloud yet.</div>';
        }
        const cur = parsed.current || {};
        const rows = entries.map(h => {
            const when = (h.savedAt || '').replace('T', ' ').replace(/[-+]\d\d:\d\d$/, '');
            return `<tr class="ess-sync-hist-row" data-row="${this._escapeHtml(rowKey)}" data-idx="${idx}" data-checksum="${this._escapeHtml(h.checksum)}" title="Show changes from this version to current">
                <td class="ess-sync-hist-when">${this._escapeHtml(when)}</td>
                <td class="ess-sync-hist-who">${this._escapeHtml(h.savedBy || '')}</td>
                <td class="ess-sync-hist-cs">${this._escapeHtml((h.checksum || '').slice(0, 8))}</td>
                <td class="ess-sync-hist-comment">${this._escapeHtml(h.comment || '')}</td>
            </tr>`;
        }).join('');
        const curLine = cur.checksum
            ? `<div class="ess-sync-extra">Current: ${this._escapeHtml(cur.checksum.slice(0, 8))} by ${this._escapeHtml(cur.updatedBy || '?')} — click a version to see what changed since it.</div>`
            : '';
        return `${curLine}<table class="ess-sync-hist-table"><tbody>${rows}</tbody></table>`;
    }

    // ── Push preview / execute ──────────────────────────────────────

    async _loadPushPreviewFor(system, force = false) {
        const el = this._overlay?.querySelector(`.ess-sync-push-preview[data-key="${CSS.escape(system)}"]`);
        if (!el) return;
        el.innerHTML = '<div class="ess-modal-loading">Loading push status...</div>';
        this._updatePushButtonState(system);
        try {
            this.pushPreviews[system] = await this._execJson(
                `scripts::push_preview {${this._tclSafe(system)}}${force ? ' -fresh' : ''}`);
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

    _pushableNew(preview) {
        return (preview?.new || []).filter(item => item.canonical !== false);
    }

    _countPushChangesFor(system) {
        const preview = this.pushPreviews[system];
        if (!preview) return 0;
        let n = (preview.changed || []).length + this._pushableNew(preview).length;
        const includeLibs = this._overlay?.querySelector(
            `.ess-sync-include-libs[data-key="${CSS.escape(system)}"]`)?.checked;
        if (includeLibs) n += (preview.libs || []).length;
        return n;
    }

    // The files themselves are already listed above the push block, so
    // this only summarises what the push would carry. The exception is a
    // system the cloud has never seen: nothing is listed above it, so
    // name the files here.
    _renderPushPreview(preview, system) {
        if (!preview) return '<div class="ess-modal-empty">No preview data</div>';
        const parts = [];
        const pushableNew = this._pushableNew(preview);
        const nonCanonical = (preview.new || []).filter(item => item.canonical === false);
        const items = [...(preview.changed || []), ...pushableNew];
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
            if (libCount) {
                bits.push(`${this._plural(libCount, 'shared lib')} (${allLibs.map(l => this._escapeHtml(l)).join(', ')})`);
            }
            parts.push(`<div class="ess-sync-summary">${bits.join(' and ')} will be pushed.</div>`);
        } else if (allLibs.length) {
            parts.push(`<div class="ess-sync-summary">${this._plural(allLibs.length, 'changed shared lib')} excluded — tick "Include changed shared libs" to push them.</div>`);
        } else {
            parts.push('<div class="ess-sync-summary">Nothing to push.</div>');
        }

        if (nonCanonical.length) {
            parts.push('<div class="ess-sync-extra">Not pushable (filename doesn\'t match the cloud convention): '
                + nonCanonical.map(item => this._escapeHtml(item.relkey)).join(', ') + '</div>');
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
        const needAdd = preview && !preview.systemExists && this._pushableNew(preview).length > 0;

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
            btn.title = 'Will create the system on the cloud';
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
            const user = this._tclSafe(this._selectedUser());
            const comment = this._tclSafe(this._overlay.querySelector(
                `.ess-sync-commit-message[data-key="${CSS.escape(system)}"]`).value.trim());
            const includeLibs = this._overlay.querySelector(
                `.ess-sync-include-libs[data-key="${CSS.escape(system)}"]`).checked ? 1 : 0;
            const preview = this.pushPreviews[system];
            const needAdd = preview && (!preview.systemExists || this._pushableNew(preview).length > 0);
            const sys = this._tclSafe(system);
            const cmd = `scripts::push {${sys}} -user {${user}} -comment {${comment}}`
                + ` -include_libs ${includeLibs} -add ${needAdd ? 1 : 0}`;

            const result = await this._execAwaitResult(
                cmd, 'scripts/push_result', r => r.op === 'push' && r.system === system);

            this.log(`Push (${system}): ${result.pushed || 0} updated, ${result.added || 0} added, ${result.lib_pushed || 0} libs`,
                (result.errors || []).length ? 'warn' : 'info');
            if ((result.errors || []).length) {
                this.log(`Push errors: ${result.errors.join('; ')}`, 'error');
            }
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
            const result = await this._execAwaitResult(
                'scripts::pull_all', 'scripts/sync_result', r => r.op === 'pull_all');
            const pulled = result.pulled ?? 0;
            const unchanged = result.unchanged ?? 0;
            const errors = result.errors || [];
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
}
