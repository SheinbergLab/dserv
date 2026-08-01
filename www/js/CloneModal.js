/**
 * CloneModal.js — Create a new protocol or system by cloning
 *
 * Opened from the "+" next to the System / Protocol selectors. Creation
 * is local-first: scripts::clone_protocol / scripts::clone_system copy
 * an existing directory with the identifier renamed throughout, nothing
 * touches the registry. On success the clone is loaded immediately
 * (guarded ess::load_system); it appears in the Sync Tasks modal as
 * local-new files and pushes (with -add) once tested.
 */

class CloneModal {
    constructor(options = {}) {
        this.connection = options.connection;
        this.essControl = options.essControl;
        this.log = options.log || console.log;

        this.mode = 'protocol';   // 'protocol' | 'system'
        this._overlay = null;
    }

    open(mode) {
        if (!this.connection || !this.connection.connected) {
            this.log('Cannot clone: not connected to dserv', 'error');
            return;
        }
        this.mode = mode === 'system' ? 'system' : 'protocol';

        const st = this.essControl?.state || {};
        let sources;
        let current;
        if (this.mode === 'system') {
            sources = Array.isArray(st.systems) ? st.systems : [];
            current = st.currentSystem || '';
        } else {
            sources = Array.isArray(st.protocols) ? st.protocols : [];
            current = st.currentProtocol || '';
        }
        if (!sources.length) {
            this.log(`No ${this.mode}s available to clone from`, 'error');
            return;
        }

        this._buildModal(sources, current);
        document.body.appendChild(this._overlay);
        this._overlay.querySelector('#clone-name').focus();
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

    _buildModal(sources, current) {
        const kind = this.mode === 'system' ? 'System' : 'Protocol';
        const sysNote = this.mode === 'protocol'
            ? ` in ${this._escapeHtml(this.essControl?.state?.currentSystem || '?')}`
            : '';
        const opts = sources.map(s =>
            `<option value="${this._escapeHtml(s)}"${s === current ? ' selected' : ''}>${this._escapeHtml(s)}</option>`
        ).join('');

        this._overlay = document.createElement('div');
        this._overlay.className = 'ess-modal-overlay ess-clone-modal-overlay';
        this._overlay.innerHTML = `
            <div class="ess-modal ess-clone-modal">
                <div class="ess-modal-header">
                    <span class="ess-modal-title">New ${kind}${sysNote}</span>
                    <button type="button" class="ess-modal-close" id="clone-close">&times;</button>
                </div>
                <div class="ess-modal-body">
                    <div class="ess-modal-section">
                        <label class="ess-modal-label">Clone from</label>
                        <select class="ess-modal-select" id="clone-from">${opts}</select>
                    </div>
                    <div class="ess-modal-section">
                        <label class="ess-modal-label">New ${kind.toLowerCase()} name</label>
                        <input type="text" class="ess-modal-input" id="clone-name"
                            placeholder="letters, digits, _ and -" spellcheck="false">
                    </div>
                    <div class="ess-varopt-hint">Copies the canonical script files with the
                        ${kind.toLowerCase()} name renamed throughout, then loads the new copy.
                        Local until pushed from Sync Tasks.</div>
                    <div class="ess-varopt-status" id="clone-status"></div>
                </div>
                <div class="ess-modal-footer">
                    <div class="ess-modal-footer-spacer"></div>
                    <button type="button" class="ess-modal-btn cancel" id="clone-cancel">Cancel</button>
                    <button type="button" class="ess-modal-btn primary" id="clone-create">Create &amp; Load</button>
                </div>
            </div>`;

        this._overlay.querySelector('#clone-close').addEventListener('click', () => this._close());
        this._overlay.querySelector('#clone-cancel').addEventListener('click', () => this._close());
        this._overlay.addEventListener('click', (e) => {
            if (e.target === this._overlay) this._close();
        });
        this._overlay.querySelector('#clone-create').addEventListener('click', () => this._create());
        this._overlay.querySelector('#clone-name').addEventListener('keydown', (e) => {
            if (e.key === 'Enter') this._create();
        });
    }

    _status(msg, isError = false) {
        const el = this._overlay?.querySelector('#clone-status');
        if (!el) return;
        el.textContent = msg;
        el.classList.toggle('error', isError);
    }

    async _create() {
        const from = this._overlay.querySelector('#clone-from').value;
        const name = this._overlay.querySelector('#clone-name').value.trim();
        if (!name) {
            this._status('Name required', true);
            return;
        }
        if (!/^[A-Za-z0-9][A-Za-z0-9_-]*$/.test(name)) {
            this._status('Name may use letters, digits, _ and - only', true);
            return;
        }
        const btn = this._overlay.querySelector('#clone-create');
        btn.disabled = true;
        this._status('Cloning...');
        try {
            let cmd;
            let loadCmd;
            if (this.mode === 'system') {
                cmd = `scripts::clone_system {${from}} {${name}}`;
                loadCmd = `ess::load_system {${name}}`;
            } else {
                const sys = this.essControl?.state?.currentSystem || '';
                if (!sys) throw new Error('No system loaded');
                cmd = `scripts::clone_protocol {${sys}} {${from}} {${name}}`;
                loadCmd = `ess::load_system {${sys}} {${name}}`;
            }
            const raw = await this.connection.evalAsync(`send scripts {${cmd}}`, 30000);
            const result = JSON.parse(raw);
            this._status(`Created ${result.files.length} files — loading...`);
            await this.connection.evalAsync(loadCmd, 60000);
            this.log(`Cloned ${this.mode} '${from}' → '${name}' (${result.files.length} files)`
                + (result.skipped?.length ? `; skipped: ${result.skipped.join(', ')}` : ''), 'info');
            this._close();
        } catch (err) {
            this._status(err.message, true);
            btn.disabled = false;
        }
    }
}
