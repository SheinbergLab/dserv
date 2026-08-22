/**
 * SettingsModal.js — the rig settings gear
 *
 * Every knob a rig DECLARES, rendered from its own schema. Nothing here
 * knows what a joystick transport or a juice destination is; the controls,
 * the allowed values, the help text and the errors all come from the
 * declaration (docs/settings_panel_plan.md §1).
 *
 * WHERE THE KNOBS COME FROM: the datapoint tree, never an interp. A page
 * sees settings/<sub>/<key> from every interp at once, but each interp knows
 * only its OWN declarations — asking one for the list would silently return
 * a fraction of the rig.
 *
 * WHERE A WRITE GOES: settings::put must run in the interp that declared the
 * knob, which the schema now carries as `interp` (dserv stamps ::dserv_interp
 * into every interp; settings::declare copies it). Three cases, and the
 * difference matters:
 *
 *   ess, juicer, extio…   send <interp> {settings::put …}
 *   dserv                 the MAIN interp — `send` refuses it by name, so
 *                         the put is evaluated directly
 *   missing               an older lib/settings-1.0.tm on this rig (the
 *                         binary sets the name, the module stamps it, and a
 *                         half-installed tree has the field absent from
 *                         EVERY schema). Read-only, and say why.
 *
 * ERRORS ARE THE FEATURE. settings::put validates first and throws messages
 * written to teach — allowed values, the shape of a route, what to do
 * instead. They are shown verbatim rather than replaced with "invalid".
 */

class SettingsModal {
    constructor(options = {}) {
        this.connection = options.connection;
        this.dpManager = options.dpManager;
        this.log = options.log || console.log;
        this._overlay = null;
        this._knobs = [];              // [{sub, key, value, source, schema, interp}]
        this._loading = false;
        this._busy = null;             // "sub/key" of an in-flight put
        this._gen = 0;                 // stale-async guard
        this._unsub = null;
    }

    /*
     * One round trip for the whole tree. Walking it in Tcl and returning a
     * list beats a subscription per knob: the page has no idea what exists
     * until it looks, and `dservKeys` is the only thing that does.
     */
    static get SCAN_SCRIPT() {
        return [
            'set _out {}',
            'foreach _k [lsort [dservKeys settings/*/schema]] {',
            '    set _base [string range $_k 0 end-7]',
            '    set _p [split $_base /]',
            '    if { [llength $_p] != 3 } continue',
            '    set _v ""; catch { set _v [dservGet $_base] }',
            '    set _s ""; catch { set _s [dservGet $_base/source] }',
            '    lappend _out [list [lindex $_p 1] [lindex $_p 2] $_v $_s [dservGet $_k]]',
            '}',
            'set _out'
        ].join('\n');
    }

    open() {
        if (!this.connection || !this.connection.connected) {
            this.log('Cannot open settings: not connected to dserv', 'error');
            return;
        }
        this._buildModal();
        document.body.appendChild(this._overlay);
        this._onKeyDown = (e) => {
            if (e.key === 'Escape') {
                e.preventDefault();
                this._close();
            }
        };
        document.addEventListener('keydown', this._onKeyDown);
        this._subscribe();
        this._load();
    }

    _close() {
        this._gen += 1;
        if (this._unsub) {
            this._unsub();
            this._unsub = null;
        }
        if (this._onKeyDown) {
            document.removeEventListener('keydown', this._onKeyDown);
            this._onKeyDown = null;
        }
        if (this._overlay && this._overlay.parentNode) {
            this._overlay.parentNode.removeChild(this._overlay);
        }
        this._overlay = null;
    }

    _buildModal() {
        const overlay = document.createElement('div');
        overlay.className = 'ess-modal-overlay';
        overlay.innerHTML = `
            <div class="ess-modal ess-settings-modal">
                <div class="ess-modal-header">
                    <span class="ess-modal-title">Rig Settings</span>
                    <button class="ess-modal-close" type="button">×</button>
                </div>
                <div class="ess-modal-body" id="ess-settings-body">
                    <div class="ess-settings-loading">loading…</div>
                </div>
                <div class="ess-modal-footer">
                    <span class="ess-settings-note" id="ess-settings-note"></span>
                    <button class="ess-modal-btn" id="ess-settings-reload" type="button">Reload</button>
                    <button class="ess-modal-btn cancel" type="button">Close</button>
                </div>
            </div>
        `;
        overlay.addEventListener('click', (e) => {
            if (e.target === overlay) this._close();
        });
        overlay.querySelector('.ess-modal-close')
            .addEventListener('click', () => this._close());
        overlay.querySelector('.ess-modal-btn.cancel')
            .addEventListener('click', () => this._close());
        overlay.querySelector('#ess-settings-reload')
            .addEventListener('click', () => this._load());
        this._overlay = overlay;
    }

    /*
     * A put's effect arrives the same way every other value does — -persist
     * fires the -apply chain and the effective value and /source re-publish
     * themselves. So the modal never re-reads after a write; it watches.
     */
    _subscribe() {
        if (!this.dpManager) return;
        this._unsub = this.dpManager.subscribe('settings/*', (data) => {
            const name = data && data.name;
            if (!name || !this._overlay) return;
            const value = String(data.data ?? data.value ?? '');
            const parts = name.split('/');
            let field = 'value';
            if (parts.length === 4 && parts[3] === 'source') field = 'source';
            else if (parts.length !== 3) return;         // /schema, or deeper
            const knob = this._knobs.find(
                k => k.sub === parts[1] && k.key === parts[2]);
            if (!knob) return;
            knob[field] = value;
            this._refreshRow(knob);
        });
    }

    async _load() {
        if (this._loading) return;
        this._loading = true;
        const gen = this._gen;
        try {
            const reply = await this.connection.evalAsync(SettingsModal.SCAN_SCRIPT);
            if (gen !== this._gen || !this._overlay) return;
            this._knobs = TclParser.parseList(reply).map(entry => {
                const f = TclParser.parseList(entry);
                const schema = TclParser.parseDict(f[4] || '');
                return {
                    sub: f[0], key: f[1],
                    value: f[2] ?? '', source: f[3] ?? '',
                    schema,
                    interp: schema.interp ?? null,
                    values: TclParser.parseList(schema.values || '')
                };
            });
            this._render();
        } catch (e) {
            if (gen !== this._gen || !this._overlay) return;
            const body = this._overlay.querySelector('#ess-settings-body');
            body.innerHTML = `<div class="ess-settings-error"></div>`;
            body.querySelector('.ess-settings-error').textContent = e.message;
        } finally {
            this._loading = false;
        }
    }

    _render() {
        const body = this._overlay.querySelector('#ess-settings-body');
        if (!this._knobs.length) {
            body.innerHTML = `<div class="ess-settings-loading">no settings declared on this rig</div>`;
            return;
        }

        const subs = [...new Set(this._knobs.map(k => k.sub))];
        body.innerHTML = subs.map(sub => `
            <div class="ess-settings-group">
                <div class="ess-settings-group-title">${this._esc(sub)}</div>
                ${this._knobs.filter(k => k.sub === sub)
                    .map(k => this._rowHtml(k)).join('')}
            </div>
        `).join('');

        this._knobs.forEach(k => this._wireRow(k));

        // One honest note rather than a broken control per row.
        const stale = this._knobs.filter(k => k.interp === null).length;
        const note = this._overlay.querySelector('#ess-settings-note');
        note.textContent = stale
            ? `${stale} knob${stale > 1 ? 's' : ''} read-only — this rig's lib/settings-1.0.tm predates the interp stamp`
            : '';
    }

    _rowId(k) { return `ess-set-${k.sub}-${k.key}`.replace(/[^\w-]/g, '_'); }

    _rowHtml(k) {
        const doc = (k.schema.doc || '').trim();
        const ro = k.interp === null;
        return `
            <div class="ess-settings-row${ro ? ' readonly' : ''}" id="${this._rowId(k)}">
                <div class="ess-settings-row-head">
                    <span class="ess-settings-key">${this._esc(k.key)}</span>
                    <span class="ess-settings-tags">
                        <span class="ess-settings-src ${this._esc(k.source)}"
                              title="${this._esc(this._sourceTitle(k))}">${this._esc(k.source || '?')}</span>
                        <span class="ess-settings-interp"
                              title="the interp that declared this knob, and where a write is sent">${this._esc(k.interp ?? 'unknown')}</span>
                    </span>
                </div>
                <div class="ess-settings-ctl">${this._controlHtml(k)}</div>
                ${doc ? `<div class="ess-settings-doc" title="${this._escAttr(doc)}">${this._esc(doc)}</div>` : ''}
                ${this._hintsHtml(k)}
                <div class="ess-settings-err" hidden></div>
            </div>
        `;
    }

    /*
     * The control is chosen in THIS order, not by `type` alone: `type` is
     * empty on several knobs because the constraint lives in `values`.
     */
    _controlHtml(k) {
        const dis = k.interp === null ? ' disabled' : '';
        const type = (k.schema.type || '').trim();

        if (k.values.length && !this._isHintList(k.values)) {
            // A hand-declared value with no option of its own still has to
            // show as selected rather than silently reading as the first
            // entry in the list.
            const opts = k.values.includes(k.value) ? k.values : [...k.values, k.value];
            return `<select class="ess-modal-select ess-settings-input"${dis}>
                ${opts.map(v => `<option value="${this._escAttr(v)}"${v === k.value ? ' selected' : ''}>${this._esc(v === '' ? '(empty)' : v)}</option>`).join('')}
            </select>`;
        }
        if (type === 'bool') {
            const on = k.value === '1' || k.value === 1;
            return `<label class="ess-settings-check">
                <input type="checkbox" class="ess-settings-input"${on ? ' checked' : ''}${dis}>
                <span>${on ? 'on' : 'off'}</span>
            </label>`;
        }
        const numeric = (type === 'int' || type === 'double');
        return `<div class="ess-settings-entry">
            <input type="${numeric ? 'number' : 'text'}" class="ess-modal-input ess-settings-input"
                   ${numeric && type === 'int' ? 'step="1"' : ''}
                   value="${this._escAttr(k.value)}"${dis}>
            <button class="ess-mini-btn ess-settings-set" type="button"${dis}>Set</button>
        </div>`;
    }

    /*
     * `values` is not always a list of choices. `juicer destination` declares
     * extio:<box>/<pin> — documentation for a SHAPE, not an option anyone
     * could pick. One entry in angle brackets makes the whole list hints.
     */
    _isHintList(values) {
        return values.some(v => /<.*>/.test(v));
    }

    _hintsHtml(k) {
        if (!k.values.length || !this._isHintList(k.values)) return '';
        return `<div class="ess-settings-hints">accepts: ${
            k.values.map(v => `<code>${this._esc(v)}</code>`).join(' · ')}</div>`;
    }

    _sourceTitle(k) {
        switch (k.source) {
            case 'default': return 'nobody has declared this — the value is the declaration default';
            case 'file':    return 'declared by this rig in local/rig.tcl';
            case 'runtime':  return 'a live override — lost on restart unless persisted';
            default:        return '';
        }
    }

    _wireRow(k) {
        const row = this._overlay.querySelector('#' + this._rowId(k));
        if (!row) return;
        const input = row.querySelector('.ess-settings-input');
        const setBtn = row.querySelector('.ess-settings-set');
        if (!input || k.interp === null) return;

        if (input.tagName === 'SELECT') {
            input.addEventListener('change', () => this._put(k, input.value));
        } else if (input.type === 'checkbox') {
            input.addEventListener('change', () => this._put(k, input.checked ? '1' : '0'));
        } else {
            const commit = () => this._put(k, input.value.trim());
            setBtn.addEventListener('click', commit);
            input.addEventListener('keydown', (e) => {
                if (e.key === 'Enter') { e.preventDefault(); commit(); }
            });
        }
    }

    async _put(k, value) {
        const id = `${k.sub}/${k.key}`;
        if (this._busy) return;
        const row = this._overlay.querySelector('#' + this._rowId(k));
        const errEl = row.querySelector('.ess-settings-err');

        // Tcl quoting stops at balanced braces, and this value is about to
        // travel inside two of them. Refusing here beats a mangled write.
        if (/[{}\\]/.test(value) || /[\r\n]/.test(value)) {
            this._showErr(errEl, 'braces, backslashes and newlines cannot be sent from this panel — use essctrl');
            return;
        }

        this._busy = id;
        row.classList.add('busy');
        this._showErr(errEl, '');
        try {
            const put = `settings::put ${k.sub} ${k.key} ${TclParser.toTcl(value)} -persist`;
            // `send` refuses main by name ("cannot send directly to dserv"),
            // so a knob main declared is evaluated where we already are.
            const script = (k.interp === 'dserv') ? put : `send ${k.interp} {${put}}`;
            await this.connection.evalAsync(script);
            this.log(`setting ${k.sub} ${k.key} → ${value === '' ? '(empty)' : value}`, 'info');
            // No re-read: -persist re-publishes value and /source, and the
            // subscription above brings them back.
        } catch (e) {
            this._showErr(errEl, e.message);
            this._refreshRow(k);        // put the control back to the truth
        } finally {
            this._busy = null;
            row.classList.remove('busy');
        }
    }

    _showErr(el, msg) {
        if (!el) return;
        el.textContent = msg || '';
        el.hidden = !msg;
    }

    /* Update one row in place, leaving whatever the operator is typing in. */
    _refreshRow(k) {
        const row = this._overlay && this._overlay.querySelector('#' + this._rowId(k));
        if (!row) return;
        const src = row.querySelector('.ess-settings-src');
        if (src) {
            src.className = `ess-settings-src ${k.source}`;
            src.textContent = k.source || '?';
            src.title = this._sourceTitle(k);
        }
        const input = row.querySelector('.ess-settings-input');
        if (!input || document.activeElement === input) return;
        if (input.tagName === 'SELECT') {
            if (![...input.options].some(o => o.value === k.value)) {
                input.add(new Option(k.value === '' ? '(empty)' : k.value, k.value));
            }
            input.value = k.value;
        } else if (input.type === 'checkbox') {
            input.checked = (k.value === '1');
            const label = input.parentNode.querySelector('span');
            if (label) label.textContent = input.checked ? 'on' : 'off';
        } else {
            input.value = k.value;
        }
    }

    _esc(s) {
        return String(s ?? '').replace(/[&<>"']/g, c => ({
            '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
        }[c]));
    }

    _escAttr(s) { return this._esc(s); }
}
