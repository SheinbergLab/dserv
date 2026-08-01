/**
 * VariantOptionsModal.js — Edit one loader parameter's option list
 *
 * Opened from the ✎ next to a parameter dropdown in the Variant Options
 * panel. Shows the current options (label + value, first = default) and
 * lets them be added, removed, reordered, and tested, then saved back
 * to the <protocol>_variants.tcl file and reloaded.
 *
 * Backend (ess interp):
 *   ess::variant_options_json            — raw + parsed option lists
 *   ess::set_variant_options v arg pairs — lint + surgical file edit
 *   ess::resource_variants               — re-source the edited file
 *   ess::test_loader proc args           — dry-run the loader (replaces
 *                                          the live stimdg; reload_variant
 *                                          restores it)
 *   ess::reload_variant                  — via the guarded essctrl path
 *
 * The edited file becomes a local change in the Sync Tasks modal, with
 * a diff, and pushes to the registry from there.
 */

class VariantOptionsModal {
    constructor(options = {}) {
        this.connection = options.connection;
        this.dpManager = options.dpManager;
        this.essControl = options.essControl;
        this.log = options.log || console.log;

        this.argName = null;
        this.variant = null;
        this.rows = [];          // [{label, value}] — working copy
        this.testDirty = false;  // a test replaced the live stimdg
        this._overlay = null;
    }

    async open(argName) {
        if (!this.connection || !this.connection.connected) {
            this.log('Cannot edit options: not connected to dserv', 'error');
            return;
        }
        const vi = this.essControl?.state?.variantInfo;
        if (!vi || !vi.loader_arg_names?.includes(argName)) {
            this.log(`No loader info for '${argName}'`, 'error');
            return;
        }

        this.argName = argName;
        this.variant = this.essControl?.state?.currentVariant || '';
        this.testDirty = false;

        this._buildModal();
        document.body.appendChild(this._overlay);

        try {
            const raw = await this.connection.evalAsync(
                'send ess {ess::variant_options_json}');
            const data = JSON.parse(raw);
            this.variant = data.variant || this.variant;
            const info = data.args?.[argName];
            if (!info) throw new Error(`'${argName}' not found in variants file`);
            this.rows = (info.options || []).map(o => ({
                label: o.bare ? '' : o.label,
                value: o.value
            }));
            this._setTitle();
            this._renderRows();
        } catch (err) {
            this._showError(`Failed to load options: ${err.message}`);
        }
    }

    _close() {
        // A test replaced the live stimdg — quietly restore it.
        if (this.testDirty) {
            this.connection.evalAsync('ess::reload_variant').catch(() => {});
            this.testDirty = false;
        }
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

    // Wrap a string as one braced Tcl word. Values may contain braces
    // (nested lists) as long as they balance; brackets are rejected by
    // the backend lint, so only balance is checked here.
    _tclBrace(s) {
        const str = String(s ?? '');
        let depth = 0;
        for (let i = 0; i < str.length; i++) {
            const c = str[i];
            if (c === '\\') { i++; continue; }
            if (c === '{') depth++;
            else if (c === '}') {
                depth--;
                if (depth < 0) throw new Error(`unbalanced '}' in "${str}"`);
            }
        }
        if (depth !== 0) throw new Error(`unbalanced '{' in "${str}"`);
        return `{${str}}`;
    }

    _buildModal() {
        this._overlay = document.createElement('div');
        this._overlay.className = 'ess-modal-overlay ess-varopt-modal-overlay';
        this._overlay.innerHTML = `
            <div class="ess-modal ess-varopt-modal">
                <div class="ess-modal-header">
                    <span class="ess-modal-title" id="varopt-title">Options</span>
                    <button type="button" class="ess-modal-close" id="varopt-close">&times;</button>
                </div>
                <div class="ess-modal-body">
                    <div class="ess-varopt-hint">First option is the default. Leave the label
                        empty for plain values; use a label when the dropdown should show a
                        name for a longer value.</div>
                    <div id="varopt-rows"></div>
                    <button type="button" class="ess-modal-btn cancel ess-varopt-add" id="varopt-add">+ Add option</button>
                    <div class="ess-varopt-status" id="varopt-status"></div>
                </div>
                <div class="ess-modal-footer">
                    <div class="ess-modal-footer-spacer"></div>
                    <button type="button" class="ess-modal-btn cancel" id="varopt-cancel">Cancel</button>
                    <button type="button" class="ess-modal-btn primary" id="varopt-save">Save &amp; Reload</button>
                </div>
            </div>`;

        this._overlay.querySelector('#varopt-close').addEventListener('click', () => this._close());
        this._overlay.querySelector('#varopt-cancel').addEventListener('click', () => this._close());
        this._overlay.addEventListener('click', (e) => {
            if (e.target === this._overlay) this._close();
        });
        this._overlay.querySelector('#varopt-add').addEventListener('click', () => {
            this.rows.push({ label: '', value: '' });
            this._renderRows();
            const inputs = this._overlay.querySelectorAll('.ess-varopt-value');
            inputs[inputs.length - 1]?.focus();
        });
        this._overlay.querySelector('#varopt-save').addEventListener('click', () => this._save());

        const rowsEl = this._overlay.querySelector('#varopt-rows');
        rowsEl.addEventListener('click', (e) => {
            const btn = e.target.closest('button[data-op]');
            if (!btn) return;
            const idx = parseInt(btn.dataset.idx, 10);
            this._syncRowsFromDom();
            switch (btn.dataset.op) {
                case 'up':
                    if (idx > 0) {
                        [this.rows[idx - 1], this.rows[idx]] = [this.rows[idx], this.rows[idx - 1]];
                    }
                    break;
                case 'down':
                    if (idx < this.rows.length - 1) {
                        [this.rows[idx + 1], this.rows[idx]] = [this.rows[idx], this.rows[idx + 1]];
                    }
                    break;
                case 'del':
                    this.rows.splice(idx, 1);
                    break;
                case 'test':
                    this._testRow(idx);
                    return;   // no re-render — keep focus/state
            }
            this._renderRows();
        });
    }

    _setTitle() {
        const t = this._overlay?.querySelector('#varopt-title');
        if (t) t.textContent = `Options: ${this.argName}  (variant ${this.variant})`;
    }

    _renderRows() {
        const el = this._overlay.querySelector('#varopt-rows');
        el.innerHTML = this.rows.map((r, i) => `
            <div class="ess-varopt-row" data-idx="${i}">
                <span class="ess-varopt-default">${i === 0 ? 'default' : ''}</span>
                <input type="text" class="ess-varopt-label" data-idx="${i}"
                    placeholder="label" value="${this._escapeHtml(r.label)}">
                <textarea class="ess-varopt-value" data-idx="${i}" rows="1"
                    placeholder="value">${this._escapeHtml(r.value)}</textarea>
                <button type="button" data-op="test" data-idx="${i}" title="Dry-run the loader with this value">&#9654;</button>
                <button type="button" data-op="up" data-idx="${i}" title="Move up">&#9650;</button>
                <button type="button" data-op="down" data-idx="${i}" title="Move down">&#9660;</button>
                <button type="button" data-op="del" data-idx="${i}" title="Remove">&times;</button>
            </div>`).join('');
    }

    _syncRowsFromDom() {
        const labels = this._overlay.querySelectorAll('.ess-varopt-label');
        const values = this._overlay.querySelectorAll('.ess-varopt-value');
        labels.forEach((inp) => {
            const i = parseInt(inp.dataset.idx, 10);
            if (this.rows[i]) this.rows[i].label = inp.value.trim();
        });
        values.forEach((inp) => {
            const i = parseInt(inp.dataset.idx, 10);
            if (this.rows[i]) this.rows[i].value = inp.value.trim();
        });
    }

    _showStatus(msg, isError = false) {
        const el = this._overlay?.querySelector('#varopt-status');
        if (!el) return;
        el.textContent = msg;
        el.classList.toggle('error', isError);
    }

    _showError(msg) { this._showStatus(msg, true); }

    // Dry-run the loader with row idx's value substituted for this arg
    // (all other args at their current selections).
    async _testRow(idx) {
        this._syncRowsFromDom();
        const row = this.rows[idx];
        if (!row || row.value === '') {
            this._showError('Nothing to test — value is empty');
            return;
        }
        const vi = this.essControl?.state?.variantInfo;
        if (!vi?.loader_proc || !vi.loader_arg_names || !vi.loader_args) {
            this._showError('No loader info available');
            return;
        }
        const name = row.label || row.value;
        this._showStatus(`Testing ${this.argName} = ${name} ...`);
        try {
            const parts = [];
            vi.loader_arg_names.forEach((a, i) => {
                const val = (a === this.argName) ? row.value : vi.loader_args[i];
                parts.push(this._tclBrace(a), this._tclBrace(val));
            });
            const cmd = `ess::test_loader ${this._tclBrace(vi.loader_proc)} {${parts.join(' ')}}`;
            // test_loader deletes the live stimdg before running the
            // loader, so it needs restoring even when the test fails.
            this.testDirty = true;
            const result = await this.connection.evalAsync(`send ess {${cmd}}`, 60000);
            this._showStatus(`✓ ${name}: ${result}`);
        } catch (err) {
            this._showError(`✗ ${name}: ${err.message}`);
        }
    }

    async _save() {
        this._syncRowsFromDom();
        this.rows = this.rows.filter(r => r.label !== '' || r.value !== '');
        if (!this.rows.length) {
            this._showError('At least one option is required');
            return;
        }
        const btn = this._overlay.querySelector('#varopt-save');
        btn.disabled = true;
        this._showStatus('Saving...');
        try {
            const parts = [];
            for (const r of this.rows) {
                parts.push(this._tclBrace(r.label), this._tclBrace(r.value || r.label));
            }
            const cmd = `ess::set_variant_options {${this._tclBrace(this.variant).slice(1, -1)}}`
                + ` {${this._tclBrace(this.argName).slice(1, -1)}}`
                + ` {${parts.join(' ')}}; ess::resource_variants`;
            const result = await this.connection.evalAsync(`send ess {${cmd}}`, 30000);
            this._showStatus('Saved — reloading variant...');
            await this.connection.evalAsync('ess::reload_variant', 60000);
            this.testDirty = false;
            this.log(`Options for '${this.argName}' saved (${result}) and variant reloaded`, 'info');
            this._close();
        } catch (err) {
            this._showError(`Save failed: ${err.message}`);
            btn.disabled = false;
        }
    }
}
