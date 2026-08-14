/**
 * NetStatusModal.js — Network path / AP picker (netmon subprocess)
 *
 * Lists interfaces with IPs, and — on an explicit Scan — the visible
 * BSSIDs for SSIDs that already have NetworkManager credentials. Click
 * an Ethernet card or a Wi-Fi AP row to switch the registry path.
 *
 * Transport (the SyncModal split — see netmonconf.tcl's header for why):
 *   fast:  `send netmon {netmon_choices}` awaited via evalAsync —
 *          sub-100ms, safe to block on.
 *   slow:  scans and switches take seconds to a minute, and a blocking
 *          `send` would stall the MAIN interp (every client) for the
 *          duration. So they fire with `sendNoReply netmon {...}` and
 *          resolve from netmon/scan_result / netmon/switch_result,
 *          matched by a per-request id. Subscribe before firing so a
 *          fast result can't be missed.
 *
 * Scanning is never automatic: an active rescan hops off-channel and
 * perturbs live wifi traffic, so checking status must not trigger one.
 */

class NetStatusModal {
    constructor(options = {}) {
        this.connection = options.connection;
        this.dpManager = options.dpManager;
        this.log = options.log || console.log;
        this._overlay = null;
        this._loading = false;
        this._switching = false;
        this._data = null;                     // {current, interfaces, error}
        this._scan = { state: 'idle', aps: [], error: '' };
        this._gen = 0;                         // stale-async guard
        this._reqSeq = 0;
    }

    open() {
        if (!this.connection || !this.connection.connected) {
            this.log('Cannot open network status: not connected to dserv', 'error');
            return;
        }

        this._data = null;
        this._scan = { state: 'idle', aps: [], error: '' };
        this._switching = false;
        this._buildModal();
        document.body.appendChild(this._overlay);
        this._onKeyDown = (e) => {
            if (e.key === 'Escape') {
                e.preventDefault();
                this._close();
            }
        };
        document.addEventListener('keydown', this._onKeyDown);
        this._load();
    }

    _close() {
        this._gen += 1;
        if (this._onKeyDown) {
            document.removeEventListener('keydown', this._onKeyDown);
            this._onKeyDown = null;
        }
        this._overlay?.remove();
        this._overlay = null;
        this._loading = false;
        this._switching = false;
    }

    _escapeHtml(str) {
        if (str == null) return '';
        return String(str)
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;');
    }

    // Strip characters that could break out of a braced Tcl word.
    _tclSafe(str) {
        return String(str ?? '').replace(/[{}\[\]$\\;]/g, '');
    }

    _nextReq() {
        this._reqSeq = (this._reqSeq + 1) % 1000;
        return `${Date.now()}${this._reqSeq}`;
    }

    // Fast command: evaluate in netmon, await the reply.
    async _execJson(cmd, timeoutMs = 15000) {
        const raw = await this.connection.evalAsync(`send netmon {${cmd}}`, timeoutMs);
        try {
            return JSON.parse(raw);
        } catch (e) {
            throw new Error(`Bad response from netmon: ${String(raw).slice(0, 200)}`);
        }
    }

    // Slow command: sendNoReply + resolve from a result datapoint whose
    // payload carries our request id. Subscribe before firing.
    _execAwaitResult(cmd, dpName, req, timeoutMs) {
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
                if (String(parsed.req) !== String(req)) return;
                finish(resolve, parsed);
            });

            this.connection.evalAsync(`sendNoReply netmon {${cmd}}`)
                .catch(err => finish(reject, err));
        });
    }

    _buildModal() {
        this._overlay = document.createElement('div');
        this._overlay.className = 'ess-modal-overlay ess-net-modal-overlay';
        this._overlay.innerHTML = `
            <div class="ess-modal ess-net-modal">
                <div class="ess-modal-header">
                    <span class="ess-modal-title">Network</span>
                    <button type="button" class="ess-modal-close" id="net-modal-close">&times;</button>
                </div>
                <div class="ess-modal-body" id="net-modal-body">
                    <div class="ess-modal-loading">Loading interfaces…</div>
                </div>
                <div class="ess-modal-footer ess-net-modal-footer">
                    <p class="ess-net-modal-hint">Select a connection to switch · Scan APs lists access points for saved networks (briefly disturbs Wi‑Fi traffic)</p>
                    <div class="ess-net-modal-footer-actions">
                        <button type="button" class="ess-modal-btn cancel" id="net-modal-scan" hidden>Scan APs</button>
                        <button type="button" class="ess-modal-btn cancel" id="net-modal-refresh">Refresh</button>
                        <div class="ess-modal-footer-spacer"></div>
                        <button type="button" class="ess-modal-btn cancel" id="net-modal-done">Close</button>
                    </div>
                </div>
            </div>
        `;

        this._overlay.querySelector('#net-modal-close').addEventListener('click', () => this._close());
        this._overlay.querySelector('#net-modal-done').addEventListener('click', () => this._close());
        this._overlay.querySelector('#net-modal-refresh').addEventListener('click', () => this._load());
        this._overlay.querySelector('#net-modal-scan').addEventListener('click', () => this._runScan());
        this._overlay.addEventListener('click', (e) => {
            if (e.target === this._overlay) this._close();
        });
    }

    _setBusyButtons() {
        if (!this._overlay) return;
        const busy = this._loading || this._switching || this._scan.state === 'scanning';
        const scanBtn = this._overlay.querySelector('#net-modal-scan');
        const refreshBtn = this._overlay.querySelector('#net-modal-refresh');
        const hasWifi = !!(this._data?.interfaces || []).some((r) => r.type === 'wifi');
        if (scanBtn) {
            scanBtn.hidden = !hasWifi;
            scanBtn.disabled = busy;
        }
        if (refreshBtn) refreshBtn.disabled = busy;
    }

    // Load interfaces + current path (fast; no scan).
    async _load() {
        if (!this._overlay || this._loading || this._switching) return;
        this._loading = true;
        const gen = ++this._gen;
        this._setBusyButtons();

        const body = this._overlay.querySelector('#net-modal-body');
        if (body && !this._data) {
            body.innerHTML = '<div class="ess-modal-loading">Loading interfaces…</div>';
        }

        try {
            const ifaceData = await this._execJson('netmon_choices', 15000);
            if (gen !== this._gen || !this._overlay) return;
            this._data = ifaceData;
            this._render();
        } catch (e) {
            if (gen !== this._gen || !this._overlay) return;
            this.log(`Network interfaces failed: ${e.message}`, 'error');
            if (body) {
                body.innerHTML = `<div class="ess-net-modal-error">${this._escapeHtml(e.message)}</div>`;
            }
        } finally {
            if (gen === this._gen) {
                this._loading = false;
                this._setBusyButtons();
            }
        }
    }

    // Explicit AP scan (slow; via netmon/scan_result).
    async _runScan() {
        if (!this._overlay || this._loading || this._switching) return;
        if (this._scan.state === 'scanning') return;
        const gen = this._gen;
        this._scan = { state: 'scanning', aps: [], error: '' };
        this._render();
        this._setBusyButtons();

        try {
            const req = this._nextReq();
            const result = await this._execAwaitResult(
                `netmon_scan ${req}`, 'netmon/scan_result', req, 90000);
            if (gen !== this._gen || !this._overlay) return;
            this._scan = {
                state: 'done',
                aps: Array.isArray(result.access_points) ? result.access_points : [],
                error: result.error || ''
            };
        } catch (e) {
            if (gen !== this._gen || !this._overlay) return;
            this.log(`Network scan failed: ${e.message}`, 'error');
            this._scan = { state: 'done', aps: [], error: e.message };
        }
        this._render();
        this._setBusyButtons();
    }

    async _switch(kind, payload) {
        if (!this._overlay || this._loading || this._switching) return;

        this._switching = true;
        this._setBusyButtons();
        const body = this._overlay.querySelector('#net-modal-body');
        if (body) {
            const note = document.createElement('div');
            note.className = 'ess-net-modal-switching';
            note.id = 'net-modal-switching';
            note.textContent = 'Switching…';
            body.prepend(note);
            body.querySelectorAll('.is-clickable').forEach((el) => {
                el.classList.add('is-disabled');
            });
        }

        try {
            const req = this._nextReq();
            let cmd;
            if (kind === 'iface') {
                const iface = this._tclSafe(payload.iface);
                cmd = `netmon_switch_iface ${iface} ${req}`;
            } else if (kind === 'ap') {
                const profile = this._tclSafe(payload.profile);
                const device = this._tclSafe(payload.device);
                const bssid = this._tclSafe(payload.bssid);
                cmd = `netmon_switch_ap {${profile}} ${device} ${bssid} ${req}`;
            } else {
                throw new Error('unknown switch kind');
            }

            const result = await this._execAwaitResult(
                cmd, 'netmon/switch_result', req, 120000);
            if (!result.ok) {
                throw new Error(result.error || 'switch failed');
            }
            this._switching = false;
            // Scan data predates the switch — retire it rather than show
            // stale in-use marks.
            this._scan = { state: 'idle', aps: [], error: '' };
            await this._load();
        } catch (e) {
            this.log(`Network switch failed: ${e.message}`, 'error');
            this._switching = false;
            if (body) {
                const note = body.querySelector('#net-modal-switching');
                if (note) {
                    note.className = 'ess-net-modal-error';
                    note.textContent = e.message;
                } else {
                    body.insertAdjacentHTML(
                        'afterbegin',
                        `<div class="ess-net-modal-error">${this._escapeHtml(e.message)}</div>`
                    );
                }
                body.querySelectorAll('.is-clickable').forEach((el) => {
                    el.classList.remove('is-disabled');
                });
            }
            this._setBusyButtons();
        }
    }

    _signalClass(signalNum) {
        if (!Number.isFinite(signalNum)) return '';
        if (signalNum >= 70) return 'ess-net-signal-good';
        if (signalNum >= 45) return 'ess-net-signal-fair';
        return 'ess-net-signal-weak';
    }

    _bindClicks() {
        if (!this._overlay || this._switching) return;
        const body = this._overlay.querySelector('#net-modal-body');
        if (!body) return;

        body.querySelectorAll('[data-net-switch="iface"]').forEach((el) => {
            el.addEventListener('click', (e) => {
                e.preventDefault();
                if (el.classList.contains('is-disabled')) return;
                this._switch('iface', { iface: el.getAttribute('data-iface') });
            });
        });

        body.querySelectorAll('[data-net-switch="ap"]').forEach((el) => {
            el.addEventListener('click', (e) => {
                e.preventDefault();
                if (el.classList.contains('is-disabled')) return;
                this._switch('ap', {
                    profile: el.getAttribute('data-profile'),
                    device: el.getAttribute('data-device'),
                    bssid: el.getAttribute('data-bssid')
                });
            });
        });
    }

    _renderApTable(aps, device, registryIface) {
        const sameIface = registryIface && registryIface === device;
        const apTitle = sameIface
            ? 'Switch to this access point.'
            : 'Switch to this interface using this access point.';

        let html = '<table class="ess-net-modal-table ess-net-modal-ap-table"><thead><tr>';
        html += '<th></th><th>SSID</th><th>BSSID</th><th>Signal</th>';
        html += '</tr></thead><tbody>';
        for (const row of aps) {
            const inUse = !!row.in_use;
            const signalNum = Number(row.signal);
            const hasSignal = Number.isFinite(signalNum);
            const sig = hasSignal ? `${signalNum}%` : '—';
            const strengthClass = this._signalClass(signalNum);
            // The in-use AP stays clickable while this device is NOT the
            // registry path: that click means "make Wi-Fi primary" (route
            // metrics flip; the association doesn't change).
            const clickable = !this._switching && !(inUse && sameIface);
            const rowTitle = inUse
                ? 'Make this interface primary (keeps this access point).'
                : apTitle;
            const classes = [
                inUse ? 'ess-net-modal-current' : '',
                clickable ? 'is-clickable' : ''
            ].filter(Boolean).join(' ');

            const attrs = clickable
                ? ` data-net-switch="ap" data-profile="${this._escapeHtml(row.profile || row.ssid || '')}" data-device="${this._escapeHtml(device)}" data-bssid="${this._escapeHtml(row.bssid || '')}" title="${this._escapeHtml(rowTitle)}" role="button" tabindex="0"`
                : '';

            html += `<tr class="${classes}"${attrs}>`;
            html += `<td class="ess-net-modal-mark">${inUse ? '●' : ''}</td>`;
            html += `<td>${this._escapeHtml(row.ssid || '')}</td>`;
            html += `<td class="ess-net-modal-mono">${this._escapeHtml(row.bssid || '')}</td>`;
            html += `<td class="ess-net-modal-signal ${strengthClass}">${this._escapeHtml(sig)}</td>`;
            html += '</tr>';
        }
        html += '</tbody></table>';
        return html;
    }

    _render() {
        const body = this._overlay?.querySelector('#net-modal-body');
        if (!body) return;
        const data = this._data || {};

        const interfaces = Array.isArray(data.interfaces) ? data.interfaces : [];
        const error = data.error ? String(data.error) : '';
        const registryIface = (data.current && data.current.iface)
            || (interfaces.find((r) => r.current) || {}).iface
            || '';
        const linkDown = (data.current && data.current.state) === 'down';

        let html = '';
        if (error) {
            html += `<div class="ess-net-modal-error">${this._escapeHtml(error)}</div>`;
        }
        if (linkDown) {
            html += '<div class="ess-net-modal-error">No usable route — link appears down</div>';
        }

        if (!interfaces.length) {
            html += '<div class="ess-net-modal-empty">No interfaces with an IPv4 address</div>';
            body.innerHTML = html;
            return;
        }

        html += '<div class="ess-net-modal-cards">';
        for (const row of interfaces) {
            const cur = !!row.current;
            const iface = row.iface || '';
            const type = row.type || '';
            const typeLabel = type === 'wifi' ? 'Wi-Fi' : (type === 'ethernet' ? 'Ethernet' : type);
            const childAps = this._scan.aps.filter((ap) => (ap.device || '') === iface);
            const ethClickable = type === 'ethernet' && !cur && !this._switching;

            const cardClass = [
                'ess-net-iface-card',
                cur ? 'is-current' : '',
                ethClickable ? 'is-clickable' : ''
            ].filter(Boolean).join(' ');

            const cardAttrs = ethClickable
                ? ` data-net-switch="iface" data-iface="${this._escapeHtml(iface)}" title="Switch to this interface." role="button" tabindex="0"`
                : '';

            html += `<div class="${cardClass}"${cardAttrs}>`;
            html += '<div class="ess-net-iface-header">';
            html += `<span class="ess-net-modal-mark">${cur ? '●' : ''}</span>`;
            html += `<span class="ess-net-iface-name">${this._escapeHtml(iface)}</span>`;
            html += `<span class="ess-net-iface-meta">${this._escapeHtml(typeLabel)}</span>`;
            if (type === 'wifi' && row.ssid) {
                html += `<span class="ess-net-iface-meta">${this._escapeHtml(row.ssid)}</span>`;
            }
            if (type === 'wifi' && row.bssid) {
                html += `<span class="ess-net-iface-meta ess-net-modal-mono">${this._escapeHtml(row.bssid)}</span>`;
            }
            html += `<span class="ess-net-iface-ip ess-net-modal-mono">${this._escapeHtml(row.ip || '')}</span>`;
            html += '</div>';

            if (type === 'wifi') {
                if (this._scan.state === 'scanning') {
                    html += '<div class="ess-net-modal-scanning">Scanning access points…</div>';
                } else if (this._scan.state === 'done') {
                    if (this._scan.error) {
                        html += `<div class="ess-net-modal-error">${this._escapeHtml(this._scan.error)}</div>`;
                    } else if (childAps.length) {
                        html += this._renderApTable(childAps, iface, registryIface);
                    } else {
                        html += '<div class="ess-net-modal-empty">No access points for saved Wi‑Fi credentials</div>';
                    }
                } else {
                    html += '<div class="ess-net-modal-empty">Use Scan APs to list access points</div>';
                }
            }

            html += '</div>';
        }
        html += '</div>';

        body.innerHTML = html;
        this._bindClicks();
    }
}
