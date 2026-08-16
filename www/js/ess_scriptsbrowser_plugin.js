/**
 * ESS Scripts Browser Plugin
 *
 * Read-only browsing surface for the loaded system's scripts (and the
 * project's shared libs): syntax-highlighted source, the cloud history
 * of each script, and diffs — version-to-version and local-vs-cloud.
 * Nothing here edits anything; editing happens in files (usually with
 * an LLM in the loop) and pushes through the Sync modal.
 *
 * Data sources:
 *   snapshot.script_*          — current content of the 8 script slots
 *   scripts::history           — cloud version metadata per script
 *   scripts::history_diff      — unified diff, version -> current (or v2)
 *   scripts::diff              — unified diff, cloud -> local file
 *   scripts::lib_diff          — same for a shared lib
 *   ess::list_libs / read_lib  — shared lib listing and content
 *   scripts/dirty              — files with unpushed local changes
 */

const ScriptsBrowserPlugin = {

    // Order matches the tree layout: system-level, then protocol-level.
    TYPES: [
        { key: 'system',        label: 'System',            snap: 'script_system',        proto: false, apiType: 'system' },
        { key: 'sys_extract',   label: 'Extract (system)',  snap: 'script_sys_extract',   proto: false, apiType: 'extract' },
        { key: 'sys_analyze',   label: 'Analyze',           snap: 'script_sys_analyze',   proto: false, apiType: 'analyze' },
        { key: 'protocol',      label: 'Protocol',          snap: 'script_protocol',      proto: true,  apiType: 'protocol' },
        { key: 'loaders',       label: 'Loaders',           snap: 'script_loaders',       proto: true,  apiType: 'loaders' },
        { key: 'variants',      label: 'Variants',          snap: 'script_variants',      proto: true,  apiType: 'variants' },
        { key: 'stim',          label: 'Stim',              snap: 'script_stim',          proto: true,  apiType: 'stim' },
        { key: 'proto_extract', label: 'Extract (protocol)', snap: 'script_proto_extract', proto: true, apiType: 'extract' }
    ],

    onInit(wb) {
        this._wb = wb;
        this._snapshot = null;
        this._sysKey = '';
        this._sel = { kind: 'script', key: 'system' };   // or {kind:'lib', filename}
        this._view = 'source';
        this._editor = null;
        this._histCache = new Map();     // syskey:type -> parsed history
        this._libList = [];
        this._libContent = new Map();    // filename -> content
        this._dirtyFiles = [];

        this._els = {
            files: document.getElementById('sb-files'),
            filename: document.getElementById('sb-filename'),
            dirtyNote: document.getElementById('sb-dirty-note'),
            editor: document.getElementById('sb-editor'),
            diffpane: document.getElementById('sb-diffpane'),
            placeholder: document.getElementById('sb-placeholder'),
            history: document.getElementById('sb-history'),
            historyPane: document.getElementById('sb-history-pane')
        };

        // File rail selection
        this._els.files?.addEventListener('click', (e) => {
            const row = e.target.closest('[data-sel]');
            if (!row || row.classList.contains('sb-file-missing')) return;
            this._select(JSON.parse(row.dataset.sel));
        });

        // Source / diff view switch
        document.querySelectorAll('.sb-view-btn').forEach(btn => {
            btn.addEventListener('click', () => this._setView(btn.dataset.view));
        });

        // History row -> diff of that version against current
        this._els.history?.addEventListener('click', (e) => {
            const row = e.target.closest('[data-checksum]');
            if (row) this._showHistoryDiff(row.dataset.checksum);
            const back = e.target.closest('[data-sb-back]');
            if (back) this._setView('source');
        });

        // Lazily create the editor when the tab first shows
        document.querySelector('.tab-btn[data-tab="scripts"]')
            ?.addEventListener('click', () => this._ensureEditor());

        this._initHistoryResize();
    },

    onConnected(wb) {
        wb.dpManager.subscribe('scripts/dirty', (dp) => {
            const v = (dp && typeof dp === 'object') ? (dp.data ?? dp.value ?? '') : dp;
            try {
                const d = typeof v === 'string' ? JSON.parse(v) : v;
                this._dirtyFiles = d?.files || [];
            } catch (e) { this._dirtyFiles = []; }
            this._renderFiles();
            this._updateDirtyNote();
        });
        try {
            wb.connection.ws.send(JSON.stringify({ cmd: 'touch', name: 'scripts/dirty' }));
        } catch (e) { /* fine, badge priming covers it */ }
    },

    onSnapshot(wb, snapshot) {
        this._snapshot = snapshot;
        const key = `${snapshot.system || ''}/${snapshot.protocol || ''}`;
        const changed = key !== this._sysKey;
        this._sysKey = key;
        if (changed) {
            this._histCache.clear();
            this._libContent.clear();
            this._sel = { kind: 'script', key: 'system' };
            this._view = 'source';
            this._loadLibList();
        }
        this._renderFiles();
        this._renderSelection();
    },

    // ── Metadata for the selected entry ─────────────────────────────

    _typeMeta(t) {
        const s = this._snapshot || {};
        const sys = s.system || '';
        const proto = s.protocol || '';
        let filename;
        switch (t.key) {
            case 'system':        filename = `${sys}.tcl`; break;
            case 'sys_extract':   filename = `${sys}_extract.tcl`; break;
            case 'sys_analyze':   filename = `${sys}_analyze.tcl`; break;
            case 'protocol':      filename = `${proto}.tcl`; break;
            case 'proto_extract': filename = `${proto}_extract.tcl`; break;
            default:              filename = `${proto}_${t.key}.tcl`;
        }
        const content = s[t.snap] || '';
        return {
            ...t, filename,
            apiProto: t.proto ? proto : '',
            exists: content.trim() !== '',
            content,
            dirty: this._isDirty(filename)
        };
    },

    _isDirty(filename) {
        const sys = this._snapshot?.system || '';
        return this._dirtyFiles.some(f => {
            const path = f.split(' ')[0];   // strip "(new)"/"(deleted)" notes
            return path.startsWith(`${sys}/`) && path.endsWith(`/${filename}`)
                || path === `${sys}/${filename}`;
        });
    },

    _selMeta() {
        if (this._sel.kind === 'lib') {
            const f = this._sel.filename;
            return {
                kind: 'lib', label: f, filename: f,
                exists: true,
                content: this._libContent.get(f),
                dirty: this._dirtyFiles.some(d => d.split(' ')[0] === `lib/${f}`)
            };
        }
        const t = this.TYPES.find(x => x.key === this._sel.key) || this.TYPES[0];
        return { kind: 'script', ...this._typeMeta(t) };
    },

    // ── File rail ───────────────────────────────────────────────────

    _renderFiles() {
        const el = this._els.files;
        if (!el || !this._snapshot) return;
        const esc = (s) => this._wb.escapeHtml(s);

        const row = (sel, label, filename, exists, dirty, active) =>
            `<div class="sb-file${active ? ' active' : ''}${exists ? '' : ' sb-file-missing'}"
                  data-sel='${JSON.stringify(sel)}'
                  title="${esc(exists ? filename : filename + ' — not present')}">
                <span class="sb-file-label">${esc(label)}</span>
                <span class="sb-file-name">${esc(filename)}</span>
                ${dirty ? '<span class="sb-file-dot" title="unpushed local changes"></span>' : ''}
            </div>`;

        const sysLevel = [], protoLevel = [];
        for (const t of this.TYPES) {
            const m = this._typeMeta(t);
            const active = this._sel.kind === 'script' && this._sel.key === t.key;
            (t.proto ? protoLevel : sysLevel).push(
                row({ kind: 'script', key: t.key }, m.label, m.filename, m.exists, m.dirty, active));
        }

        const libRows = this._libList.map(l => {
            const active = this._sel.kind === 'lib' && this._sel.filename === l.filename;
            const dirty = this._dirtyFiles.some(d => d.split(' ')[0] === `lib/${l.filename}`);
            return row({ kind: 'lib', filename: l.filename }, l.name, l.filename, true, dirty, active);
        });

        el.innerHTML =
            `<div class="sb-file-group">${esc(this._snapshot.system || '—')}</div>` + sysLevel.join('')
            + `<div class="sb-file-group">${esc(this._snapshot.protocol || '—')}</div>` + protoLevel.join('')
            + (libRows.length
                ? `<div class="sb-file-group">shared libs</div>` + libRows.join('')
                : '');
    },

    async _loadLibList() {
        try {
            const resp = await this._wb.connection.evalAsync('send ess {ess::list_libs}');
            this._libList = JSON.parse(resp) || [];
        } catch (e) {
            this._libList = [];
        }
        this._renderFiles();
    },

    // ── Selection / views ───────────────────────────────────────────

    async _select(sel) {
        this._sel = sel;
        this._view = 'source';
        this._renderFiles();
        await this._renderSelection();
    },

    async _renderSelection() {
        const m = this._selMeta();
        if (this._els.filename) this._els.filename.textContent = m.filename || '—';
        this._updateDirtyNote();
        this._setViewButtons();

        if (this._view === 'diff') return this._renderDiffView();

        // Source view
        this._showPane('editor');
        if (m.kind === 'lib' && m.content === undefined) {
            this._showPane('placeholder', 'loading…');
            try {
                const f = m.filename;
                if (!/^[\w.\-]+$/.test(f)) throw new Error('bad filename');
                const content = await this._wb.connection.evalAsync(`send ess {ess::read_lib {${f}}}`);
                this._libContent.set(f, content);
                if (this._selMeta().filename !== f) return;   // selection moved on
            } catch (err) {
                return this._showPane('placeholder', `could not read lib: ${err.message}`);
            }
            return this._renderSelection();
        }
        if (!m.exists && m.kind === 'script') {
            return this._showPane('placeholder',
                `${m.filename} is not present in this system.`);
        }
        const ed = await this._ensureEditor();
        ed?.setValue(m.content || '');
        this._loadHistory();
    },

    _setView(view) {
        this._view = view;
        this._setViewButtons();
        if (view === 'source') {
            this._renderSelection();
        } else {
            this._renderDiffView();
        }
    },

    _setViewButtons() {
        document.querySelectorAll('.sb-view-btn').forEach(b =>
            b.classList.toggle('active', b.dataset.view === this._view));
    },

    _updateDirtyNote() {
        const m = this._selMeta();
        if (this._els.dirtyNote) this._els.dirtyNote.hidden = !m.dirty;
    },

    _showPane(which, text) {
        const { editor, diffpane, placeholder } = this._els;
        if (editor) editor.hidden = which !== 'editor';
        if (diffpane) diffpane.hidden = which !== 'diff';
        if (placeholder) {
            placeholder.hidden = which !== 'placeholder';
            if (text !== undefined) placeholder.textContent = text;
        }
    },

    // All subprocess round-trips go through evalAsync: the legacy text
    // protocol matches replies by arrival order, so two in-flight sends
    // (a history fetch racing a diff) cross-resolve or come back empty.
    // evalAsync correlates by requestId. One retry after a short pause
    // covers the scripts subprocess's cold first https_get.
    async _sendJsonRetry(cmd) {
        const run = async () => {
            const r = await this._wb.connection.evalAsync(`send scripts {${cmd}}`);
            try {
                return JSON.parse(r);
            } catch (e) {
                throw new Error(`unparseable reply: ${String(r).slice(0, 120)}`);
            }
        };
        try {
            return await run();
        } catch (e) {
            await new Promise(r => setTimeout(r, 800));
            return run();
        }
    },

    // ── Local-vs-cloud diff ─────────────────────────────────────────

    async _renderDiffView() {
        const m = this._selMeta();
        this._showPane('diff');
        this._els.diffpane.innerHTML = '<div class="sb-loading">diffing against the cloud…</div>';
        try {
            let parsed;
            if (m.kind === 'lib') {
                parsed = await this._sendJsonRetry(
                    `scripts::lib_diff {${m.filename}}`);
            } else {
                parsed = await this._sendJsonRetry(
                    `scripts::diff {${this._snapshot.system}} {${m.apiProto}} {${m.apiType}}`);
            }
            this._els.diffpane.innerHTML = this._diffHtml(parsed, 'cloud → local');
        } catch (err) {
            this._els.diffpane.innerHTML =
                `<div class="sb-loading">diff unavailable: ${this._wb.escapeHtml(err.message)}</div>`;
        }
    },

    _diffHtml(parsed, captionText) {
        if (parsed?.registryExists === false) {
            return '<div class="sb-loading">Not on the cloud — nothing to diff against.</div>';
        }
        if (parsed && parsed.changed === false) {
            return '<div class="sb-loading">Identical — local file matches the cloud.</div>';
        }
        const esc = (s) => this._wb.escapeHtml(s);
        const lines = String(parsed?.diff || '').split('\n').map(line => {
            let cls = 'ctx';
            if (line.startsWith('+++') || line.startsWith('---')) cls = 'file';
            else if (line.startsWith('@@')) cls = 'hunk';
            else if (line.startsWith('+')) cls = 'add';
            else if (line.startsWith('-')) cls = 'del';
            return `<span class="sb-diff-${cls}">${esc(line)}\n</span>`;
        }).join('');
        const caption = captionText
            ? `<div class="sb-diff-caption">${esc(captionText)}</div>` : '';
        return `${caption}<pre class="sb-diff">${lines}</pre>`;
    },

    // ── History ─────────────────────────────────────────────────────

    async _loadHistory() {
        const el = this._els.history;
        if (!el) return;
        const m = this._selMeta();
        if (m.kind === 'lib') {
            el.innerHTML = '<div class="sb-loading">Version history is kept for system scripts; libs show the latest cloud copy (see Local vs cloud).</div>';
            return;
        }
        const cacheKey = `${this._sysKey}:${m.key}`;
        if (!this._histCache.has(cacheKey)) {
            el.innerHTML = '<div class="sb-loading">loading…</div>';
            try {
                const parsed = await this._sendJsonRetry(
                    `scripts::history {${this._snapshot.system}} {${m.apiProto}} {${m.apiType}} 30`);
                this._histCache.set(cacheKey, parsed);
            } catch (err) {
                el.innerHTML = `<div class="sb-loading">history unavailable: ${this._wb.escapeHtml(err.message)}</div>`;
                return;
            }
        }
        if (this._selMeta().key !== m.key) return;   // selection moved on
        this._renderHistory(this._histCache.get(cacheKey));
    },

    _renderHistory(parsed) {
        const el = this._els.history;
        const esc = (s) => this._wb.escapeHtml(s);
        const entries = parsed?.history || [];
        const cur = parsed?.current || {};
        const curLine = cur.checksum
            ? `<div class="sb-hist-current">current <code>${esc(cur.checksum.slice(0, 8))}</code>
                 by ${esc(cur.updatedBy || '?')}</div>`
            : '';
        if (!entries.length) {
            el.innerHTML = curLine
                + '<div class="sb-loading">No saved versions on the cloud yet.</div>';
            return;
        }
        const rows = entries.map(h => {
            const when = (h.savedAt || '').replace('T', ' ').replace(/[-+]\d\d:\d\d$/, '').slice(0, 16);
            return `<div class="sb-hist-row" data-checksum="${esc(h.checksum)}"
                         title="Show what changed from this version to current">
                <div class="sb-hist-top">
                    <span class="sb-hist-when">${esc(when)}</span>
                    <span class="sb-hist-who">${esc(h.savedBy || '')}</span>
                    <code class="sb-hist-cs">${esc((h.checksum || '').slice(0, 8))}</code>
                </div>
                ${h.comment ? `<div class="sb-hist-comment">${esc(h.comment)}</div>` : ''}
            </div>`;
        }).join('');
        el.innerHTML = curLine + rows;
    },

    async _showHistoryDiff(checksum) {
        const m = this._selMeta();
        if (m.kind !== 'script' || !/^[0-9a-f]+$/.test(checksum)) return;
        this._view = 'histdiff';
        this._setViewButtons();
        this._showPane('diff');
        this._els.history?.querySelectorAll('.sb-hist-row').forEach(r =>
            r.classList.toggle('active', r.dataset.checksum === checksum));
        this._els.diffpane.innerHTML = '<div class="sb-loading">loading version diff…</div>';
        try {
            const parsed = await this._sendJsonRetry(
                `scripts::history_diff {${this._snapshot.system}} {${m.apiProto}} {${m.apiType}} {${checksum}}`);
            this._els.diffpane.innerHTML =
                `<div class="sb-diff-caption"><a data-sb-back class="sb-back">&larr; back to source</a>
                    &nbsp; ${checksum.slice(0, 8)} → current</div>`
                + this._diffHtml(parsed);
        } catch (err) {
            this._els.diffpane.innerHTML =
                `<div class="sb-loading">diff unavailable: ${this._wb.escapeHtml(err.message)}</div>`;
        }
    },

    // ── Editor + panel resize ───────────────────────────────────────

    async _ensureEditor() {
        if (this._editor?.view) return this._editor;
        if (!this._editor && this._els.editor) {
            this._editor = new TclEditor('sb-editor', { lineNumbers: true });
        }
        for (let i = 0; i < 100 && !this._editor?.view; i++) {
            await new Promise(r => setTimeout(r, 50));
        }
        if (this._editor?.view) this._editor.setReadOnly(true);
        return this._editor?.view ? this._editor : null;
    },

    _initHistoryResize() {
        const divider = document.getElementById('sb-divider');
        const pane = this._els.historyPane;
        if (!divider || !pane) return;
        const saved = parseInt(localStorage.getItem('ess_sb_hist_w'), 10);
        if (saved >= 200 && saved <= 560) pane.style.width = `${saved}px`;
        divider.addEventListener('pointerdown', (e) => {
            e.preventDefault();
            divider.setPointerCapture(e.pointerId);
            divider.classList.add('dragging');
            const onMove = (ev) => {
                const w = Math.min(560, Math.max(200,
                    Math.round(pane.getBoundingClientRect().right - ev.clientX)));
                pane.style.width = `${w}px`;
            };
            const onUp = () => {
                divider.classList.remove('dragging');
                divider.removeEventListener('pointermove', onMove);
                divider.removeEventListener('pointerup', onUp);
                localStorage.setItem('ess_sb_hist_w',
                    String(Math.round(pane.getBoundingClientRect().width)));
            };
            divider.addEventListener('pointermove', onMove);
            divider.addEventListener('pointerup', onUp);
        });
    }
};

ESSWorkbench.registerPlugin(ScriptsBrowserPlugin);
