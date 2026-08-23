/**
 * SliderCalModal.js — the analog stick calibration wizard
 *
 * NOT a settings form, and deliberately not folded into the settings gear.
 * A form is read / render / write; this is a SEQUENCE with the operator's
 * hands in it, and the backend is already shaped that way
 * (config/sliderconf.tcl): cal_begin, four cal_marks, cal_apply, with
 * cal_cancel restoring whatever was in force at any point.
 *
 * Three things this page must get right, all of them lessons the backend
 * already learned the hard way (docs/settings_panel_plan.md §2):
 *
 *  - THE LIVE READOUT IS THE POINT. The commonest failure is a stick that
 *    is not streaming at all — an on-change ain group publishes nothing at
 *    rest, so a rest mark has nothing to average. slider/cal/live carries
 *    the raw column vector and a climbing sample count, so that shows as a
 *    dead readout before anyone tries a mark and reads a refusal.
 *
 *  - REFUSALS ARE GUIDANCE, NOT ERRORS. cal_mark and cal_apply say things
 *    like "the stick was still MOVING (sd 12.4/3.1 counts) — hold it
 *    steady, then mark". Rendered as instructions, and a bad mark costs a
 *    RE-MARK, never a restart.
 *
 *  - DERIVING IS NOT CONFIRMING. The 8-direction sweep at the end checks
 *    the transform against the frame the TASK uses (ess/joystick/dir) —
 *    that check is what caught a 90° rotation on officepi that a channel
 *    swap alone would not have fixed.
 *
 * Measured values go to db/calibration.db via cal_apply. This wizard must
 * never write local/slider.tcl: humans DECLARE to files, the system LEARNS
 * to the db, and a panel that "helpfully" edited the file would undo the
 * whole migration.
 */

class SliderCalModal {
    static get STAGES() {
        return [
            { id: 'rest',  label: 'Rest',  prompt: 'Let the stick sit at rest — hands off.' },
            { id: 'up',    label: 'Up',    prompt: 'Push UP to the stop and HOLD it there.' },
            { id: 'right', label: 'Right', prompt: 'Push RIGHT to the stop and HOLD it there.' },
            { id: 'sweep', label: 'Sweep', prompt: 'Sweep the stick to every mechanical stop, all the way round.' }
        ];
    }

    static get DIRS() {
        return ['up', 'up_right', 'right', 'down_right',
                'down', 'down_left', 'left', 'up_left'];
    }

    constructor(options = {}) {
        this.connection = options.connection;
        this.dpManager = options.dpManager;
        this.log = options.log || console.log;
        this._overlay = null;
        this._unsubs = [];
        this._busy = false;
        this._status = { active: 0, stage: '', samples: 0, msg: '' };
        this._live = { n: 0, cols: [], at: 0 };
        this._marked = {};              // stage id -> true
        this._guidance = '';            // the backend's own words
        this._result = null;            // cal_apply's report
        this._profile = null;
        this._confirm = null;           // {i, seen: {dir: reported}}
        this._joystickActive = false;
        this._dir = -1;
    }

    open() {
        if (!this.connection || !this.connection.connected) {
            this.log('Cannot calibrate: not connected to dserv', 'error');
            return;
        }
        this._buildModal();
        document.body.appendChild(this._overlay);
        this._onKeyDown = (e) => { if (e.key === 'Escape') this._close(); };
        document.addEventListener('keydown', this._onKeyDown);
        this._subscribe();
        this._loadProfile();
        this._render();
    }

    /*
     * Cancel on the way out, ALWAYS. A half-finished calibration left active
     * keeps feeding cal_feed and leaves the rig in a state nobody chose;
     * cal_cancel restores what was in force. Closing the window is the most
     * likely way to abandon this, so it has to be the safe one.
     */
    _close() {
        if (this._status.active) {
            this._send('slider::cal_cancel').catch(() => {});
        }
        this._unsubs.forEach(u => u());
        this._unsubs = [];
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
            <div class="ess-modal ess-cal-modal">
                <div class="ess-modal-header">
                    <span class="ess-modal-title">Calibrate stick</span>
                    <button class="ess-modal-close" type="button">×</button>
                </div>
                <div class="ess-modal-body ess-cal-body" id="ess-cal-body"></div>
                <div class="ess-modal-footer">
                    <span class="ess-cal-profile" id="ess-cal-profile"></span>
                    <button class="ess-modal-btn cancel" type="button">Close</button>
                </div>
            </div>
        `;
        overlay.querySelector('.ess-modal-close')
            .addEventListener('click', () => this._close());
        overlay.querySelector('.ess-modal-btn.cancel')
            .addEventListener('click', () => this._close());
        this._overlay = overlay;
    }

    _subscribe() {
        const sub = (name, fn) => {
            if (this.dpManager) this._unsubs.push(this.dpManager.subscribe(name, fn));
        };
        sub('slider/cal/status', (d) => {
            const s = TclParser.parseDict(String(d.data ?? d.value ?? ''));
            this._status = {
                active: parseInt(s.active) > 0,
                stage: s.stage || '',
                samples: parseInt(s.samples) || 0,
                msg: s.msg || ''
            };
            this._render();
        });
        sub('slider/cal/live', (d) => {
            const l = TclParser.parseDict(String(d.data ?? d.value ?? ''));
            this._live = {
                n: parseInt(l.n) || 0,
                cols: TclParser.parseList(l.cols || '').map(Number),
                at: Date.now()
            };
            this._renderLive();
        });
        // The confirmation step reads the frame the TASK uses, not ours.
        sub('ess/joystick_active', (d) => {
            this._joystickActive = parseInt(d.value) > 0;
        });
        sub('ess/joystick/dir', (d) => {
            const dir = parseInt(d.data ?? d.value);
            this._dir = isNaN(dir) ? -1 : dir;
            this._onDir();
        });
    }

    async _send(cmd) {
        return await this.connection.evalAsync(`send slider {${cmd}}`);
    }

    async _loadProfile() {
        try {
            this._profile = (await this._send('set slider::cal_profile')).trim();
        } catch (e) {
            this._profile = null;
        }
        this._renderProfile();
    }

    // ---- actions ---------------------------------------------------------

    async _do(cmd, after) {
        if (this._busy) return;
        this._busy = true;
        this._guidance = '';
        this._render();
        try {
            const reply = await this._send(cmd);
            if (after) after(reply);
        } catch (e) {
            // The backend's refusals name the cause AND the fix. Showing
            // them verbatim is the whole design; a bad mark costs a re-mark.
            this._guidance = e.message;
        } finally {
            this._busy = false;
            this._render();
        }
    }

    _begin() {
        this._marked = {};
        this._result = null;
        this._confirm = null;
        this._do('slider::cal_begin');
    }

    _mark(stage) {
        this._do(`slider::cal_mark ${stage}`, () => { this._marked[stage] = true; });
    }

    _apply() {
        this._do('slider::cal_apply', (reply) => {
            this._result = TclParser.parseDict(reply);
            this._confirm = { i: 0, seen: {} };
            this.log(`slider calibrated: ${reply}`, 'info');
        });
    }

    _cancel() {
        this._do('slider::cal_cancel', () => { this._marked = {}; });
    }

    /*
     * Confirmation: prompt for each of the eight directions in a known order
     * and record what ESS reports. Deriving the transform and checking it
     * are different questions — a 90° rotation satisfies the first and fails
     * this one.
     */
    _onDir() {
        if (!this._confirm || this._dir < 0) return;
        const want = SliderCalModal.DIRS[this._confirm.i];
        const got = SliderCalModal.DIRS[this._dir];
        if (this._confirm.seen[want] === undefined) {
            this._confirm.seen[want] = got;
            if (this._confirm.i < SliderCalModal.DIRS.length - 1) this._confirm.i++;
            this._render();
        }
    }

    // ---- rendering -------------------------------------------------------

    _render() {
        if (!this._overlay) return;
        const body = this._overlay.querySelector('#ess-cal-body');
        body.innerHTML = `
            ${this._liveHtml()}
            ${this._guidance ? `<div class="ess-cal-guidance"></div>` : ''}
            ${this._result ? this._resultHtml() : this._stepsHtml()}
        `;
        if (this._guidance) {
            body.querySelector('.ess-cal-guidance').textContent = this._guidance;
        }
        this._wire(body);
        this._renderProfile();
    }

    _liveHtml() {
        const streaming = this._status.active && this._live.n > 0;
        const cols = this._live.cols.map((v, i) =>
            `<span class="ess-cal-col"><b>c${i}</b> ${Math.round(v)}</span>`).join('');
        return `
            <div class="ess-cal-live ${streaming ? 'ok' : 'idle'}">
                <div class="ess-cal-live-head">
                    <span id="ess-cal-state">${this._status.active
                        ? (streaming ? 'streaming' : 'NO SAMPLES')
                        : 'not calibrating'}</span>
                    <span class="ess-cal-samples" id="ess-cal-samples">${
                        this._status.active ? `${this._live.n} samples` : ''}</span>
                </div>
                <div class="ess-cal-cols" id="ess-cal-cols">${cols}</div>
                ${this._status.active && !streaming ? `
                    <div class="ess-cal-live-warn">Nothing is arriving from the stick.
                        A <code>continuous</code> ain group publishes at rest; an
                        <code>onchange</code> group publishes nothing until something moves,
                        which is why a rest mark would have nothing to average.</div>` : ''}
            </div>
        `;
    }

    /* The one part that updates without a rebuild: it changes at 20 Hz. */
    _renderLive() {
        if (!this._overlay) return;
        const n = this._overlay.querySelector('#ess-cal-samples');
        const cols = this._overlay.querySelector('#ess-cal-cols');
        if (!n || !cols) return;
        n.textContent = `${this._live.n} samples`;
        cols.innerHTML = this._live.cols.map((v, i) =>
            `<span class="ess-cal-col"><b>c${i}</b> ${Math.round(v)}</span>`).join('');
        const box = this._overlay.querySelector('.ess-cal-live');
        if (box) box.className = `ess-cal-live ${this._live.n > 0 ? 'ok' : 'idle'}`;
        // The state WORD too, or the panel says "NO SAMPLES" beside a
        // climbing sample count — the one thing this readout exists to
        // report, contradicting itself.
        const state = this._overlay.querySelector('#ess-cal-state');
        if (state && this._status.active) {
            state.textContent = this._live.n > 0 ? 'streaming' : 'NO SAMPLES';
        }
        const warn = this._overlay.querySelector('.ess-cal-live-warn');
        if (warn && this._live.n > 0) warn.remove();
    }

    _stepsHtml() {
        if (!this._status.active) {
            return `
                <div class="ess-cal-intro">
                    <p>Measures where this stick rests, which way its axes run, and how
                       far it throws — then writes that to the calibration db. Nothing
                       is applied until the last step, and Cancel restores what is in
                       force now.</p>
                    <button class="ess-modal-btn primary" data-act="begin" type="button">Begin</button>
                </div>`;
        }
        // Bind the array ONCE: the getter builds a fresh one per call, so
        // indexOf(st) against a second instance compares object identities
        // that cannot match, and every step reads as "not next".
        const stages = SliderCalModal.STAGES;
        const nextIdx = stages.findIndex(s => !this._marked[s.id]);
        const rows = stages.map((st, i) => {
            const done = !!this._marked[st.id];
            const next = !done && i === nextIdx;
            return `
                <div class="ess-cal-step ${done ? 'done' : next ? 'next' : ''}">
                    <span class="ess-cal-step-mark">${done ? '✓' : next ? '▸' : ''}</span>
                    <div class="ess-cal-step-body">
                        <div class="ess-cal-step-label">${st.label}</div>
                        <div class="ess-cal-step-prompt">${st.prompt}</div>
                    </div>
                    <button class="ess-modal-btn" data-act="mark" data-stage="${st.id}"
                            type="button" ${this._busy ? 'disabled' : ''}>${done ? 'Re-mark' : 'Mark'}</button>
                </div>`;
        }).join('');
        const all = stages.every(s => this._marked[s.id]);
        return `
            <div class="ess-cal-steps">${rows}</div>
            <div class="ess-cal-actions">
                <button class="ess-modal-btn" data-act="cancel" type="button">Cancel</button>
                <div class="ess-modal-footer-spacer"></div>
                <button class="ess-modal-btn primary" data-act="apply" type="button"
                        ${all && !this._busy ? '' : 'disabled'}>Apply</button>
            </div>`;
    }

    _resultHtml() {
        const r = this._result || {};
        const rows = [
            ['x from column', r.chan_x], ['y from column', r.chan_y],
            ['center', `${r.center_x}, ${r.center_y}`],
            ['inverted', `x ${r.invert_x === '1' ? 'yes' : 'no'}, y ${r.invert_y === '1' ? 'yes' : 'no'}`],
            ['throw', `${r.throw_counts} counts`],
            ['rest noise', `sd ${r.rest_noise_sd}`]
        ].map(([k, v]) => `<div class="ess-cal-kv"><span>${k}</span><b>${v ?? '—'}</b></div>`).join('');
        return `
            <div class="ess-cal-result">
                <div class="ess-cal-result-head">Applied and saved</div>
                <div class="ess-cal-kvs">${rows}</div>
                <div class="ess-cal-note">Saved to the calibration db, which from now on
                    WINS over any measured values (center, channels, invert) still written
                    in <code>local/slider.tcl</code> — those lines stay readable and stop
                    being in force. Retiring them is the point; leaving them is only
                    misleading.</div>
            </div>
            ${this._confirmHtml()}`;
    }

    /*
     * Deriving the transform is not the same as checking it against the
     * frame the TASK uses. This reads ess/joystick/dir — the same datapoint
     * the d-pad panel shows — so a mapping that is merely self-consistent
     * still has to agree with what a subject's push will mean.
     */
    _confirmHtml() {
        if (!this._confirm) return '';
        if (!this._joystickActive) {
            return `<div class="ess-cal-confirm idle">
                The 8-direction check needs a system that has called
                <code>joystick_init</code> — load one and reopen this to verify the
                mapping against <code>ess/joystick/dir</code>. The calibration above is
                already saved.</div>`;
        }
        const want = SliderCalModal.DIRS[this._confirm.i];
        const rows = SliderCalModal.DIRS.map(d => {
            const got = this._confirm.seen[d];
            const state = got === undefined ? '' : (got === d ? 'ok' : 'bad');
            return `<div class="ess-cal-dir ${state} ${d === want ? 'want' : ''}">
                <span class="ess-cal-dir-name">${d}</span>
                <span class="ess-cal-dir-got">${got === undefined ? '—' : got}</span>
            </div>`;
        }).join('');
        const wrong = Object.entries(this._confirm.seen).filter(([d, g]) => d !== g);
        const done = Object.keys(this._confirm.seen).length === SliderCalModal.DIRS.length;
        return `
            <div class="ess-cal-confirm">
                <div class="ess-cal-confirm-head">Now push <b>${want}</b> and release</div>
                <div class="ess-cal-dirs">${rows}</div>
                ${done ? (wrong.length
                    ? `<div class="ess-cal-guidance">${wrong.length} direction${
                        wrong.length > 1 ? 's' : ''} came back wrong — the stick is
                        calibrated but not oriented the way the task reads it. Re-run
                        with a cleaner 'up' and 'right', pushing straight along each
                        axis.</div>`
                    : `<div class="ess-cal-confirm-ok">All eight agree.</div>`) : ''}
                <button class="ess-modal-btn" data-act="reconfirm" type="button">Start over</button>
            </div>`;
    }

    _renderProfile() {
        const el = this._overlay && this._overlay.querySelector('#ess-cal-profile');
        if (!el) return;
        if (!this._profile) { el.textContent = ''; return; }
        // A rig with two analog inputs needs a profile per input or one
        // device's mapping is forced onto the other. Naming the target is
        // the smallest honest version of that: it cannot detect a second
        // input, but it can refuse to be silent about where this lands.
        el.textContent = `saves to profile: ${this._profile}`;
        // The profile is a DECLARATION now (`slider cal_profile`, in the
        // settings gear), not a line in local/slider.tcl — that file is
        // what the declarations replaced, and pointing at it here sent
        // people to edit something the rig no longer reads.
        el.title = this._profile === 'default'
            ? 'a rig with more than one analog input must name a profile per '
              + 'input — Settings ▸ slider ▸ cal_profile — or this measurement '
              + 'is forced onto both'
            : 'declared as `slider cal_profile`; the gear lists what each '
              + 'profile holds and when it was measured';
        el.classList.toggle('warn', this._profile === 'default');
    }

    _wire(body) {
        body.querySelectorAll('[data-act]').forEach(btn => {
            btn.addEventListener('click', () => {
                switch (btn.dataset.act) {
                    case 'begin':  this._begin(); break;
                    case 'mark':   this._mark(btn.dataset.stage); break;
                    case 'apply':  this._apply(); break;
                    case 'cancel': this._cancel(); break;
                    case 'reconfirm':
                        this._confirm = { i: 0, seen: {} };
                        this._render();
                        break;
                }
            });
        });
    }
}
