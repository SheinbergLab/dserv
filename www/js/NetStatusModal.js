/**
 * NetStatusModal.js — Network path / AP picker
 *
 * Lists interfaces with IPs and scanned BSSIDs for SSIDs that already
 * have NetworkManager credentials. Click Ethernet cards or Wi‑Fi APs
 * to switch the registry path (mesh_net_switch_iface / mesh_net_switch_ap).
 *
 * Interfaces load first (mesh_net_iface_choices); AP scan follows
 * (mesh_net_ap_scan) so the modal is useful while nmcli rescans.
 */

class NetStatusModal {
    constructor(options = {}) {
        this.connection = options.connection;
        this.log = options.log || console.log;
        this._overlay = null;
        this._loading = false;
        this._switching = false;
        this._data = null;
        this._scanGen = 0;
    }

    open() {
        if (!this.connection || !this.connection.connected) {
            this.log('Cannot open network status: not connected to dserv', 'error');
            return;
        }

        this._data = null;
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
        this._scanGen += 1;
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

    async _execJson(cmd, timeoutMs = 60000) {
        const raw = await this.connection.evalAsync(`send mesh {${cmd}}`, timeoutMs);
        try {
            return JSON.parse(raw);
        } catch (e) {
            throw new Error(`Bad response from mesh: ${String(raw).slice(0, 200)}`);
        }
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
                    <p class="ess-net-modal-hint">Select a connection to switch</p>
                    <div class="ess-net-modal-footer-actions">
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
        this._overlay.addEventListener('click', (e) => {
            if (e.target === this._overlay) this._close();
        });
    }

    async _load() {
        if (!this._overlay || this._loading || this._switching) return;
        this._loading = true;
        const gen = ++this._scanGen;

        const body = this._overlay.querySelector('#net-modal-body');
        const refreshBtn = this._overlay.querySelector('#net-modal-refresh');
        if (refreshBtn) refreshBtn.disabled = true;
        if (body) {
            body.innerHTML = '<div class="ess-modal-loading">Loading interfaces…</div>';
        }

        try {
            const ifaceData = await this._execJson('mesh_net_iface_choices', 15000);
            if (gen !== this._scanGen || !this._overlay) return;

            this._data = {
                ...ifaceData,
                access_points: [],
                error: ifaceData.error || '',
                scanning: true
            };
            this._render(this._data);

            try {
                const scanData = await this._execJson('mesh_net_ap_scan', 60000);
                if (gen !== this._scanGen || !this._overlay) return;

                this._data = {
                    ...this._data,
                    access_points: Array.isArray(scanData.access_points)
                        ? scanData.access_points
                        : [],
                    error: scanData.error || '',
                    scanning: false
                };
                this._render(this._data);
            } catch (scanErr) {
                if (gen !== this._scanGen || !this._overlay) return;
                this.log(`Network scan failed: ${scanErr.message}`, 'error');
                this._data = {
                    ...this._data,
                    access_points: [],
                    error: scanErr.message,
                    scanning: false
                };
                this._render(this._data);
            }
        } catch (e) {
            if (gen !== this._scanGen || !this._overlay) return;
            this.log(`Network interfaces failed: ${e.message}`, 'error');
            if (body) {
                body.innerHTML = `<div class="ess-net-modal-error">${this._escapeHtml(e.message)}</div>`;
            }
        } finally {
            if (gen === this._scanGen) {
                this._loading = false;
                if (refreshBtn) refreshBtn.disabled = false;
            }
        }
    }

    async _switch(kind, payload) {
        if (!this._overlay || this._loading || this._switching) return;

        this._switching = true;
        const body = this._overlay.querySelector('#net-modal-body');
        const refreshBtn = this._overlay.querySelector('#net-modal-refresh');
        if (refreshBtn) refreshBtn.disabled = true;
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
            let cmd;
            if (kind === 'iface') {
                const iface = this._tclSafe(payload.iface);
                cmd = `mesh_net_switch_iface ${iface}`;
            } else if (kind === 'ap') {
                const device = this._tclSafe(payload.device);
                const ssid = this._tclSafe(payload.ssid);
                const bssid = this._tclSafe(payload.bssid);
                cmd = `mesh_net_switch_ap ${device} {${ssid}} {${bssid}}`;
            } else {
                throw new Error('unknown switch kind');
            }

            const result = await this._execJson(cmd, 90000);
            if (!result.ok) {
                throw new Error(result.error || 'switch failed');
            }
            this._switching = false;
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
            if (refreshBtn) refreshBtn.disabled = false;
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
                    device: el.getAttribute('data-device'),
                    ssid: el.getAttribute('data-ssid'),
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
            const clickable = !inUse && !this._switching;
            const classes = [
                inUse ? 'ess-net-modal-current' : '',
                clickable ? 'is-clickable' : ''
            ].filter(Boolean).join(' ');

            const attrs = clickable
                ? ` data-net-switch="ap" data-device="${this._escapeHtml(device)}" data-ssid="${this._escapeHtml(row.ssid || '')}" data-bssid="${this._escapeHtml(row.bssid || '')}" title="${this._escapeHtml(apTitle)}" role="button" tabindex="0"`
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

    _render(data) {
        const body = this._overlay?.querySelector('#net-modal-body');
        if (!body) return;

        const interfaces = Array.isArray(data.interfaces) ? data.interfaces : [];
        const aps = Array.isArray(data.access_points) ? data.access_points : [];
        const error = data.error ? String(data.error) : '';
        const scanning = !!data.scanning;
        const registryIface = (data.current && data.current.iface)
            || (interfaces.find((r) => r.current) || {}).iface
            || '';

        let html = '';
        if (error && !scanning) {
            html += `<div class="ess-net-modal-error">${this._escapeHtml(error)}</div>`;
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
            const childAps = aps.filter((ap) => (ap.device || '') === iface);
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
            html += `<span class="ess-net-iface-ip ess-net-modal-mono">${this._escapeHtml(row.ip || '')}</span>`;
            html += '</div>';

            if (type === 'wifi') {
                if (scanning) {
                    html += '<div class="ess-net-modal-scanning">Scanning access points…</div>';
                } else if (childAps.length) {
                    html += this._renderApTable(childAps, iface, registryIface);
                } else if (!error) {
                    html += '<div class="ess-net-modal-empty">No access points for saved Wi‑Fi credentials</div>';
                }
            }

            html += '</div>';
        }
        html += '</div>';

        body.innerHTML = html;
        this._bindClicks();
    }
}
