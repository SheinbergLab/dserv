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
        this._sel = 'all';             // selected subsystem, or 'all'
        this._q = '';                  // filter text
        this._errors = [];             // rejected local/rig.tcl lines, per interp
        this._declaredOnly = false;    // show only what this rig has decided
        this._want = null;             // subsystem to open at, once loaded
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
            // Breadcrumbs are per interp: each one reads the whole rig file
            // but judges only its own declarations, so the bad joystick line
            // is a thing only `ess` can report.
            'set _errs {}',
            'foreach _k [lsort [dservKeys settings/parse_errors/*]] {',
            '    set _who [lindex [split $_k /] 2]',
            '    catch { foreach _e [dservGet $_k] { lappend _errs [list $_who $_e] } }',
            '}',
            'list $_out $_errs'
        ].join('\n');
    }

    /*
     * `section` opens straight at one subsystem, so another panel can hand
     * off to this one ("everything else about the juicer is here") instead
     * of duplicating a control. Applied after the load, and only if that
     * subsystem actually declares something — a rig that never started the
     * juicer would otherwise open on an empty pane.
     */
    open(section = null) {
        if (!this.connection || !this.connection.connected) {
            this.log('Cannot open settings: not connected to dserv', 'error');
            return;
        }
        this._want = section;
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
                <div class="ess-modal-body ess-settings-body" id="ess-settings-body">
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
            const before = this._visibleSig();
            knob[field] = value;
            // Values flow into the standing DOM; the DOM is REBUILT only
            // when the set of visible rows changes — clearing a knob under
            // "declared only" makes its row go away, and nothing else does.
            // (extio-config's structure/values split, for the same reason:
            // a rebuild loses half-typed text.)
            if (this._visibleSig() !== before) {
                this._renderNav();
                this._renderPane();
            } else {
                if (field === 'source') this._renderNav();   // the n/N counts
                this._refreshRow(knob);
            }
        });
    }

    async _load() {
        if (this._loading) return;
        this._loading = true;
        const gen = this._gen;
        try {
            const reply = await this.connection.evalAsync(SettingsModal.SCAN_SCRIPT);
            if (gen !== this._gen || !this._overlay) return;
            const [knobList, errList] = TclParser.parseList(reply);
            this._errors = TclParser.parseList(errList || '')
                .map(e => TclParser.parseList(e))
                .map(([who, msg]) => ({ who, msg }));
            this._knobs = TclParser.parseList(knobList).map(entry => {
                const f = TclParser.parseList(entry);
                const schema = TclParser.parseDict(f[4] || '');
                return {
                    sub: f[0], key: f[1],
                    value: f[2] ?? '', source: f[3] ?? '',
                    schema,
                    interp: schema.interp ?? null,
                    values: TclParser.parseList(schema.values || ''),
                    candidates: schema.candidates || ''
                };
            });
            if (this._want) {
                if (this._knobs.some(k => k.sub === this._want)) {
                    this._sel = this._want;
                } else {
                    // Land on All, not on whatever was selected last time:
                    // a jump that quietly opens a DIFFERENT subsystem reads
                    // as the wrong thing having been clicked. (A rig whose
                    // em/slider predates these declarations gets exactly
                    // this case.)
                    this._sel = 'all';
                    this.log(`settings: nothing declared for '${this._want}'`
                             + ' — showing everything', 'warn');
                }
                this._want = null;
            }
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

    /*
     * Subsystem list on the left, one subsystem's knobs on the right. Seven
     * groups of one to four knobs is a long scroll for a question that is
     * always about ONE subsystem ("what is the juicer set to?").
     *
     * The filter is not a nicety: it is the answer to the other question a
     * settings panel gets, "where is the thing called <x>", which no
     * sidebar can answer because the asker does not know which subsystem
     * owns it. It searches key, value and doc across every section at once.
     */
    _render() {
        const body = this._overlay.querySelector('#ess-settings-body');
        if (!this._knobs.length) {
            body.innerHTML = `<div class="ess-settings-loading">no settings declared on this rig</div>`;
            return;
        }
        if (!body.querySelector('.ess-settings-layout')) {
            body.innerHTML = `
                <div class="ess-settings-alert" id="ess-settings-alert" hidden></div>
                <div class="ess-settings-layout">
                    <div class="ess-settings-nav">
                        <input type="text" class="ess-settings-filter" id="ess-settings-filter"
                               placeholder="filter…" autocomplete="off">
                        <label class="ess-settings-only"
                               title="only knobs this rig has decided — declared in local/rig.tcl or overridden live">
                            <input type="checkbox" id="ess-settings-only">
                            <span>declared only</span>
                        </label>
                        <div class="ess-settings-nav-list" id="ess-settings-nav-list"></div>
                    </div>
                    <div class="ess-settings-pane" id="ess-settings-pane"></div>
                </div>
            `;
            const only = body.querySelector('#ess-settings-only');
            only.checked = this._declaredOnly;
            only.addEventListener('change', () => {
                this._declaredOnly = only.checked;
                this._widenIfEmpty();
                this._renderNav();
                this._renderPane();
            });
            const filter = body.querySelector('#ess-settings-filter');
            filter.addEventListener('input', () => {
                this._q = filter.value.trim().toLowerCase();
                this._widenIfEmpty();
                this._renderNav();
                this._renderPane();
            });
        }
        this._renderAlert();
        this._renderNav();
        this._renderPane();

        // One honest note rather than a broken control per row.
        const stale = this._knobs.filter(k => k.interp === null).length;
        const note = this._overlay.querySelector('#ess-settings-note');
        note.textContent = stale
            ? `${stale} knob${stale > 1 ? 's' : ''} read-only — this rig's lib/settings-1.0.tm predates the interp stamp`
            : '';
    }

    /*
     * A line in local/rig.tcl that did not survive validation costs a
     * breadcrumb and a fallback to the default — deliberately, so one bad
     * line cannot abort a boot. But nothing ever SHOWED those breadcrumbs,
     * which made "I set it in the file and the rig ignores me" unanswerable
     * without an essctrl session. This is where they surface.
     */
    _renderAlert() {
        const el = this._overlay.querySelector('#ess-settings-alert');
        if (!el) return;
        const errs = this._errors || [];
        el.hidden = !errs.length;
        if (!errs.length) return;
        el.innerHTML = `
            <div class="ess-settings-alert-head">${errs.length} line${
                errs.length > 1 ? 's' : ''} in local/rig.tcl rejected — the
                default is in force for ${errs.length > 1 ? 'these' : 'this'}</div>
            ${errs.map(e => `<div class="ess-settings-alert-row">
                <span class="ess-settings-alert-who">${this._esc(e.who)}</span>
                <span>${this._esc(e.msg)}</span>
            </div>`).join('')}
        `;
    }

    _subs() { return [...new Set(this._knobs.map(k => k.sub))]; }

    /* A knob this rig has actually DECIDED — the reason to look at all. */
    _declared(k) { return k.source === 'file' || k.source === 'runtime'; }

    _matches(k) {
        if (this._declaredOnly && !this._declared(k)) return false;
        if (!this._q) return true;
        return `${k.sub} ${k.key} ${k.value} ${k.schema.doc || ''}`
            .toLowerCase().includes(this._q);
    }

    /*
     * Narrowing to nothing HERE while something matches elsewhere is the
     * normal case for both controls — someone filters because they do not
     * know which subsystem owns the thing, and "declared only" is asked at
     * rig scale, not section scale. Widen rather than show an empty pane
     * beside a sidebar that says the matches exist.
     */
    _widenIfEmpty() {
        if (this._sel === 'all') return;
        if (this._knobs.some(k => k.sub === this._sel && this._matches(k))) return;
        if (this._knobs.some(k => this._matches(k))) this._sel = 'all';
    }

    _renderNav() {
        const nav = this._overlay.querySelector('#ess-settings-nav-list');
        const hits = this._knobs.filter(k => this._matches(k));
        const item = (id, label, list) => {
            const n = list.length;
            const dec = list.filter(k => this._declared(k)).length;
            return `<div class="ess-settings-nav-item${this._sel === id ? ' active' : ''}${n ? '' : ' empty'}"
                         data-sub="${this._escAttr(id)}">
                <span class="ess-settings-nav-name">${this._esc(label)}</span>
                <span class="ess-settings-nav-count"
                      title="${dec} of ${n} declared by this rig">${dec ? `${dec}/${n}` : n}</span>
            </div>`;
        };
        nav.innerHTML = item('all', 'All', hits) + this._subs()
            .map(s => item(s, s, hits.filter(k => k.sub === s))).join('');
        nav.querySelectorAll('.ess-settings-nav-item').forEach(el => {
            el.addEventListener('click', () => {
                this._sel = el.dataset.sub;
                this._renderNav();
                this._renderPane();
            });
        });
    }

    _shown() {
        return this._knobs.filter(k =>
            this._matches(k) && (this._sel === 'all' || k.sub === this._sel));
    }

    _visibleSig() {
        return this._shown().map(k => `${k.sub}/${k.key}`).join(',');
    }

    /*
     * Per-section actions: a knob is a value, but some subsystems also have
     * a PROCEDURE, and the section someone opens to set `joystick transport`
     * is where they will look for "calibrate the stick". A table rather than
     * embedded logic, so the panel stays schema-driven and the next wizard
     * is one line. Declared/measured stay separate — the action opens its
     * own window, it does not write a setting.
     */
    static get SECTION_ACTIONS() {
        return {
            joystick: [{
                label: '⟲ Calibrate stick…',
                title: 'measure this stick — rest, axes, throw — into the calibration db',
                available: () => typeof openSliderCalModal === 'function',
                run: () => openSliderCalModal()
            }, {
                label: '✛ Check directions…',
                title: 'press each direction and confirm ESS names it the same one',
                run: (self) => self._showDirCheck()
            }]
        };
    }

    _actionsHtml(sub) {
        const acts = (SettingsModal.SECTION_ACTIONS[sub] || [])
            .filter(a => !a.available || a.available());
        if (!acts.length) return '';
        return `<div class="ess-settings-actions">${acts.map((a, i) =>
            `<button class="ess-modal-btn ess-modal-link" data-action="${sub}:${i}"
                     type="button" title="${this._escAttr(a.title || '')}">${this._esc(a.label)}</button>`
        ).join('')}</div>`;
    }

    _renderPane() {
        const pane = this._overlay.querySelector('#ess-settings-pane');
        const shown = this._shown();
        if (!shown.length) {
            pane.innerHTML = `<div class="ess-settings-loading">${
                this._q ? 'nothing matches that' : 'nothing declared here'}</div>`;
            return;
        }
        // Group headings stay when the pane spans subsystems (All, or a
        // filter that crossed them); a single section does not need one.
        const subs = [...new Set(shown.map(k => k.sub))];
        pane.innerHTML = subs.map(sub => `
            <div class="ess-settings-group">
                ${subs.length > 1 ? `<div class="ess-settings-group-title">${this._esc(sub)}</div>` : ''}
                ${this._actionsHtml(sub)}
                ${shown.filter(k => k.sub === sub).map(k => this._rowHtml(k)).join('')}
            </div>
        `).join('');
        pane.scrollTop = 0;
        shown.forEach(k => this._wireRow(k));
        pane.querySelectorAll('[data-action]').forEach(btn => {
            const [sub, i] = btn.dataset.action.split(':');
            const act = (SettingsModal.SECTION_ACTIONS[sub] || [])
                .filter(a => !a.available || a.available())[Number(i)];
            if (act) btn.addEventListener('click', () => act.run(this));
        });
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
                        ${k.source === 'runtime' && k.interp !== null ? `<button class="ess-settings-pin"
                              type="button" title="make this the rig's answer — writes it to local/rig.tcl">⤓</button>` : ''}
                        ${this._clearable(k) ? `<button class="ess-settings-clear" type="button"
                              title="${this._escAttr(this._clearTitle(k))}">↺</button>` : ''}
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
        // A route is a live fact, not a schema one: which boxes are present,
        // which groups they announce, which members carry labels. Offer them
        // rather than asking for the string — but keep the text field
        // authoritative, so nothing the schema allows becomes unsayable.
        const pick = k.candidates
            ? `<button class="ess-mini-btn ess-settings-picker" type="button"${dis}
                       title="choose from what this rig's boxes are announcing now">Pick…</button>`
            : '';
        // Capture is for the kinds you can DO something to: press a button,
        // move a stick. A group label or an output pin is not one of those.
        const CAPTURE = {
            button: { label: 'Press…',  title: 'press the button on the box and it names itself' },
            analog: { label: 'Wiggle…', title: 'move the input on the box and it names itself' }
        };
        const cap = CAPTURE[k.candidates];
        const learn = cap
            ? `<button class="ess-mini-btn ess-settings-learn" type="button"${dis}
                       title="${this._escAttr(cap.title)}">${this._esc(cap.label)}</button>`
            : '';

        const numeric = (type === 'int' || type === 'double');
        // step MUST be explicit for a double. A number input with no step
        // defaults to step=1, so 0.5 -- juicer hand_ml, joystick
        // threshold_frac -- fails the browser's own validation and shows
        // "the two nearest valid values are 0 and 1" over a value the rig
        // declared and is happily running on.
        const step = type === 'int' ? 'step="1"' : (numeric ? 'step="any"' : '');
        return `<div class="ess-settings-entry">
            <input type="${numeric ? 'number' : 'text'}" class="ess-modal-input ess-settings-input"
                   ${step}
                   value="${this._escAttr(k.value)}"${dis}>
            ${learn}
            ${pick}
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

    /*
     * Only a knob whose value came from somewhere can be sent back: a
     * `default` has nothing to remove, and a knob with no route cannot be
     * written at all.
     */
    _clearable(k) { return k.interp !== null && this._declared(k); }

    _clearTitle(k) {
        return k.source === 'runtime'
            ? 'drop the live override (back to what the rig declares)'
            : "remove this rig's declaration from local/rig.tcl (back to the default)";
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
        const clearBtn = row.querySelector('.ess-settings-clear');
        if (clearBtn) clearBtn.addEventListener('click', () => this._clear(k));
        const pinBtn = row.querySelector('.ess-settings-pin');
        if (pinBtn) pinBtn.addEventListener('click', () => this._pin(k));
        const pickBtn = row.querySelector('.ess-settings-picker');
        if (pickBtn) pickBtn.addEventListener('click', () => this._pick(k));
        const learnBtn = row.querySelector('.ess-settings-learn');
        if (learnBtn) learnBtn.addEventListener('click', () => this._learn(k, learnBtn));
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
            // Half-typed text is protected by a DIRTY FLAG, never by
            // document.activeElement: Chrome focuses every control it is
            // clicked on and Safari/macOS focuses almost none, so a focus
            // gate is invisible here and freezes the panel on Windows
            // (www/CLAUDE.md — extio-config paid for this one).
            input.addEventListener('input', () => { k._dirty = true; });
        }
    }

    async _put(k, value) {
        if (this._busy) return;
        const row = this._overlay.querySelector('#' + this._rowId(k));
        if (!row) return;
        const errEl = row.querySelector('.ess-settings-err');

        // Tcl quoting stops at balanced braces, and this value is about to
        // travel inside two of them. Refusing here beats a mangled write.
        if (/[{}\\]/.test(value) || /[\r\n]/.test(value)) {
            this._showErr(errEl, 'braces, backslashes and newlines cannot be sent from this panel — use essctrl');
            return;
        }

        await this._run(k,
            `settings::put ${k.sub} ${k.key} ${TclParser.toTcl(value)} -persist`,
            `setting ${k.sub} ${k.key} → ${value === '' ? '(empty)' : value}`);
    }

    /*
     * Take the value back. `clear` removes the layer /source is reporting —
     * the live override, else this rig's declaration — so what the badge
     * says is exactly what the button undoes, and a knob carrying both takes
     * two clicks with the badge changing in between.
     */
    async _clear(k) {
        await this._run(k, `settings::clear ${k.sub} ${k.key}`,
            `cleared ${k.sub} ${k.key} (${k.source})`);
    }

    /*
     * Commit a live override. A panel that flips a knob writes a RUNTIME
     * value — the eye source between virtual and real, twenty times an
     * afternoon — and this is where one of those flips becomes the rig's
     * answer. It needs its own button because re-choosing the value already
     * shown fires no `change` event: the control cannot express "yes, that
     * one, permanently".
     */
    async _pin(k) {
        await this._run(k,
            `settings::put ${k.sub} ${k.key} ${TclParser.toTcl(k.value)} -persist`,
            `pinned ${k.sub} ${k.key} = ${k.value === '' ? '(empty)' : k.value}`);
    }

    /*
     * Offer what the rig's boxes are announcing right now.
     *
     * ESS enumerates, not this page: which form survives a box swap, what
     * counts as a direction label, which pins are outputs, and whether a
     * route RESOLVES are all things ess_transports already knows, and a
     * second copy here would drift from the resolvers that actually bind.
     *
     * The status column is the point. extio.html's cards already reassure
     * that a box is there; this says the ESS side reaches it — the two
     * questions look the same and are not.
     */
    async _pick(k) {
        const row = this._overlay.querySelector('#' + this._rowId(k));
        const errEl = row.querySelector('.ess-settings-err');
        this._showErr(errEl, '');
        let cands;
        try {
            const reply = await this.connection.evalAsync(
                `send ess {::ess::candidates ${k.candidates}}`);
            cands = TclParser.parseList(reply).map(c => TclParser.parseDict(c));
        } catch (e) {
            this._showErr(errEl, e.message);
            return;
        }
        if (!cands.length) {
            this._showErr(errEl,
                'no boxes are announcing anything to route to — check the extio page');
            return;
        }
        this._showPicker(k, cands);
    }

    _showPicker(k, cands) {
        const sheet = document.createElement('div');
        sheet.className = 'ess-modal-overlay ess-pick-overlay';
        sheet.innerHTML = `
            <div class="ess-modal ess-pick-modal">
                <div class="ess-modal-header">
                    <span class="ess-modal-title">${this._esc(k.sub)} ${this._esc(k.key)}</span>
                    <button class="ess-modal-close" type="button">×</button>
                </div>
                <div class="ess-modal-body ess-pick-body">
                    ${cands.map((c, i) => `
                        <div class="ess-pick-row${c.route === k.value ? ' current' : ''}${
                            c.selectable === '0' ? ' unselectable' : ''}" data-i="${i}">
                            <div class="ess-pick-main">
                                <span class="ess-pick-label">${this._esc(c.label || c.route)}</span>
                                <span class="ess-pick-route">${this._esc(c.route)}</span>
                            </div>
                            <div class="ess-pick-meta">
                                <span class="ess-pick-detail">${this._esc(c.detail || '')}</span>
                                <span class="ess-pick-status ${this._esc(c.status || '')}"
                                      title="${this._escAttr(this._pickStatusTitle(c))}">${this._esc(c.status || '')}</span>
                                ${c.durable === '0' ? `<span class="ess-pick-warn"
                                      title="a labelled group member survives a box swap; a pin number does not">by pin</span>` : ''}
                                ${c.partial === '1' ? `<span class="ess-pick-warn"
                                      title="only these directions canonicalise from the group's member labels — it will bind, and report nothing for the rest">${this._esc(c.have)} of 4</span>` : ''}
                            </div>
                        </div>`).join('')}
                </div>
                <div class="ess-modal-footer">
                    <span class="ess-settings-note">${k.candidates === 'system' ? 'from ESS_SYSTEM_PATH — reopen to rescan' : 'from the boxes announcing now — reopen to rescan'}</span>
                    <button class="ess-modal-btn cancel" type="button">Cancel</button>
                </div>
            </div>`;
        const close = () => sheet.remove();
        sheet.querySelector('.ess-modal-close').addEventListener('click', close);
        sheet.querySelector('.ess-modal-btn.cancel').addEventListener('click', close);
        sheet.addEventListener('click', (e) => { if (e.target === sheet) close(); });
        sheet.querySelectorAll('.ess-pick-row').forEach(el => {
            // A candidate the enumerator marked unselectable is shown and
            // not offered: choosing it could only fail, and hiding it would
            // leave someone hunting for a system that IS there and is not
            // loadable.
            if (el.classList.contains('unselectable')) return;
            el.addEventListener('click', () => {
                const c = cands[Number(el.dataset.i)];
                close();
                this._put(k, c.route);
            });
        });
        document.body.appendChild(sheet);
    }

    /*
     * Learn a route by pressing the thing. ESS arms, watches every box's DI
     * and group events, and publishes what fired — as the DURABLE route when
     * the pin is a labelled group member, which is the whole reason to do
     * this rather than read a number off the lid.
     *
     * The page only subscribes and waits: which datapoint counts as a press,
     * and what a press means, are the box-and-binding knowledge that belongs
     * on the ESS side.
     */
    async _learn(k, btn) {
        const row = this._overlay.querySelector('#' + this._rowId(k));
        const errEl = row.querySelector('.ess-settings-err');
        this._showErr(errEl, '');
        const was = btn.textContent;
        btn.textContent = k.candidates === 'analog' ? 'move it…' : 'press it…';
        btn.classList.add('waiting');

        let unsub = null;
        const done = () => {
            if (unsub) { unsub(); unsub = null; }
            btn.textContent = was;
            btn.classList.remove('waiting');
        };
        unsub = this.dpManager.subscribe('ess/inputs/capture', (d) => {
            const c = TclParser.parseDict(String(d.data ?? d.value ?? ''));
            if (!c.state || c.state === 'armed') return;
            done();
            if (c.state === 'captured') {
                this.log(`captured ${c.label} → ${c.route}`, 'info');
                this._put(k, c.route);
            } else if (c.state === 'timeout') {
                this._showErr(errEl, c.detail || 'nothing was pressed');
            }
        });
        try {
            await this.connection.evalAsync(
                `send ess {::ess::input_capture_arm ${k.candidates} 30000}`);
        } catch (e) {
            done();
            this._showErr(errEl, e.message);
        }
    }

    /*
     * Press each direction; ESS says which one it thinks it was.
     *
     * The extio card already proves the PIN moved. What it cannot show is
     * everything between that pin and `up`: the group manifest, the label
     * canon, the bit→member index, the resolver. This reads
     * `ess/joystick/dir` — the same datapoint the d-pad panel and a task
     * read — so what it confirms is the contract, not the wiring.
     *
     * Transport-agnostic on purpose: switches and a quantised analog stick
     * both land here, so the same check covers both. A direction that never
     * arrives stays "—", which is the finding on a group that canonicalises
     * only two of the four.
     */
    _showDirCheck() {
        const DIRS = ['up', 'down', 'left', 'right'];
        const seen = {};
        let i = 0;

        const sheet = document.createElement('div');
        sheet.className = 'ess-modal-overlay ess-pick-overlay';
        sheet.innerHTML = `
            <div class="ess-modal ess-pick-modal">
                <div class="ess-modal-header">
                    <span class="ess-modal-title">Check directions</span>
                    <button class="ess-modal-close" type="button">×</button>
                </div>
                <div class="ess-modal-body ess-pick-body">
                    <div class="ess-cal-confirm-head" id="ess-dir-prompt"></div>
                    <div class="ess-cal-dirs" id="ess-dir-grid"></div>
                    <div class="ess-settings-note" id="ess-dir-note"></div>
                </div>
                <div class="ess-modal-footer">
                    <span class="ess-settings-note">reads ess/joystick/dir — nothing is written</span>
                    <button class="ess-modal-btn" id="ess-dir-again" type="button">Start over</button>
                    <button class="ess-modal-btn cancel" type="button">Close</button>
                </div>
            </div>`;

        const grid = () => {
            sheet.querySelector('#ess-dir-grid').innerHTML = DIRS.map(d => {
                const got = seen[d];
                const state = got === undefined ? '' : (got === d ? 'ok' : 'bad');
                return `<div class="ess-cal-dir ${state} ${d === DIRS[i] ? 'want' : ''}">
                    <span class="ess-cal-dir-name">${d}</span>
                    <span class="ess-cal-dir-got">${got === undefined ? '—' : this._esc(got)}</span>
                </div>`;
            }).join('');
            const done = Object.keys(seen).length === DIRS.length;
            const wrong = Object.entries(seen).filter(([d, g]) => d !== g);
            sheet.querySelector('#ess-dir-prompt').innerHTML = done
                ? (wrong.length ? 'Mismatch' : 'All four agree')
                : `Press <b>${DIRS[i]}</b>`;
            sheet.querySelector('#ess-dir-note').textContent = !done ? ''
                : (wrong.length
                    ? `${wrong.length} direction${wrong.length > 1 ? 's' : ''} came back as something else — `
                      + 'the group binds but its member labels do not mean what the task will read. '
                      + 'Relabel on the extio card, or pick a different group.'
                    : 'the labels, the resolver and the task frame agree.');
        };

        const unsub = this.dpManager.subscribe('ess/joystick/dir', (d) => {
            const v = parseInt(d.data ?? d.value);
            if (isNaN(v) || v < 0) return;                  // centre/release
            const got = SliderCalModal.DIRS[v] || String(v);
            const want = DIRS[i];
            if (seen[want] !== undefined) return;
            seen[want] = got;
            if (i < DIRS.length - 1) i++;
            grid();
        });

        const close = () => { unsub(); sheet.remove(); };
        sheet.querySelector('.ess-modal-close').addEventListener('click', close);
        sheet.querySelector('.ess-modal-btn.cancel').addEventListener('click', close);
        sheet.addEventListener('click', (e) => { if (e.target === sheet) close(); });
        sheet.querySelector('#ess-dir-again').addEventListener('click', () => {
            for (const k of Object.keys(seen)) delete seen[k];
            i = 0;
            grid();
        });
        document.body.appendChild(sheet);
        grid();
    }

    _pickStatusTitle(c) {
        switch (c.status) {
            case 'ok':         return 'ESS resolves this route right now';
            case 'unresolved': return c.detail || 'nothing answers this yet';
            case 'unbound':    return 'nothing is bound';
            default:           return '';
        }
    }

    async _run(k, command, logMsg) {
        if (this._busy) return;
        const row = this._overlay.querySelector('#' + this._rowId(k));
        if (!row) return;
        const errEl = row.querySelector('.ess-settings-err');

        this._busy = `${k.sub}/${k.key}`;
        row.classList.add('busy');
        this._showErr(errEl, '');
        try {
            // `send` refuses main by name ("cannot send directly to dserv"),
            // so a knob main declared is evaluated where we already are.
            const script = (k.interp === 'dserv')
                ? command : `send ${k.interp} {${command}}`;
            await this.connection.evalAsync(script);
            this.log(logMsg, 'info');
            k._dirty = false;
            // No re-read: put/clear re-publish value and /source, and the
            // subscription above brings them back.
        } catch (e) {
            this._showErr(errEl, e.message);
            this._refreshRow(k, true);  // put the control back to the truth
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

    /*
     * Update one row in place. `force` is a put's own outcome landing — it
     * overrides the dirty flag, because the value the rig actually took is
     * more true than what is still sitting in the box.
     */
    _refreshRow(k, force = false) {
        const row = this._overlay && this._overlay.querySelector('#' + this._rowId(k));
        if (!row) return;                       // not the visible section
        if (force) k._dirty = false;
        const src = row.querySelector('.ess-settings-src');
        if (src) {
            src.className = `ess-settings-src ${k.source}`;
            src.textContent = k.source || '?';
            src.title = this._sourceTitle(k);
        }
        // ⤓ and ↺ follow /source: clearing a declaration leaves nothing to
        // clear, and a live override arriving from anywhere -- a panel flip,
        // another browser -- makes both appear.
        const tags = row.querySelector('.ess-settings-tags');
        let pinBtn = row.querySelector('.ess-settings-pin');
        if (k.source === 'runtime' && k.interp !== null && !pinBtn && tags) {
            pinBtn = document.createElement('button');
            pinBtn.type = 'button';
            pinBtn.className = 'ess-settings-pin';
            pinBtn.textContent = '⤓';
            pinBtn.title = "make this the rig's answer — writes it to local/rig.tcl";
            pinBtn.addEventListener('click', () => this._pin(k));
            tags.insertBefore(pinBtn, tags.firstChild);
        } else if (k.source !== 'runtime' && pinBtn) {
            pinBtn.remove();
        }
        let clearBtn = row.querySelector('.ess-settings-clear');
        if (this._clearable(k) && !clearBtn && tags) {
            clearBtn = document.createElement('button');
            clearBtn.type = 'button';
            clearBtn.className = 'ess-settings-clear';
            clearBtn.textContent = '↺';
            clearBtn.addEventListener('click', () => this._clear(k));
            tags.insertBefore(clearBtn, tags.firstChild);
        } else if (!this._clearable(k) && clearBtn) {
            clearBtn.remove();
            clearBtn = null;
        }
        if (clearBtn) clearBtn.title = this._clearTitle(k);
        const input = row.querySelector('.ess-settings-input');
        if (!input || k._dirty) return;         // do not stomp half-typed text
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
