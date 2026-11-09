// session_browser.js — dg_viewer's list of the dserv host's sessions.
//
// The sessions come from dserv's df catalog (`list_sessions`: those whose
// obs or trials file is in the work directory); a session opens from
// dserv's read-only /data/trials/NAME.trials.dgz.  df deletes a trials
// file once it is exported (the obs file is the lasting copy), so opening
// a session without one first asks df to extract it (`viewer_trials`),
// which df does only while no experiment runs.  Otherwise everything here
// is a read: one catalog query when the panel opens or refreshes, one file
// fetch per session opened.
//
// The dserv is this page's own host, else ?host=HOST:PORT (and ?ssl=1),
// as for dserv's other pages.  Without one answering, the panel is not
// offered and the viewer works as before (Open, drop, ?file=).

const SessionBrowser = {
  app: null,
  host: null,          // host:port of the dserv
  secure: false,
  conn: null,
  sessions: [],
  available: false,
  current: null,       // name of the session shown
  el: null,

  // ready: the connection attempt, for others that ask dserv (Stim2View)
  init(app) {
    this.ready = this._init(app);
    return this.ready;
  },

  async _init(app) {
    this.app = app;
    const p = new URLSearchParams(window.location.search);
    this.host = p.get('host') || window.location.host;
    this.secure = p.has('ssl') ? p.get('ssl') === '1' : window.location.protocol === 'https:';
    this._build();
    try {
      this.conn = new DservConnection({
        subprocess: 'df', dservHost: this.host, forceSecure: this.secure,
        autoReconnect: false, connectTimeout: 3000,
      });
      await this.conn.connect();
      this.available = true;
    } catch (e) {
      this.available = false;
      console.log(`dserv session catalog at ${this.host}: not available (${e.message || e})`);
    }
    document.getElementById('btnSessions').style.display = this.available ? '' : 'none';
    return this.available;
  },

  dataUrl(name) {
    const path = `/data/trials/${encodeURIComponent(name)}.trials.dgz`;
    if (this.host === window.location.host) return path;
    return `${this.secure ? 'https:' : 'http:'}//${this.host}${path}`;
  },

  _build() {
    const main = document.getElementById('mainArea');
    const el = document.createElement('div');
    el.className = 'session-panel';
    el.id = 'sessionPanel';
    el.innerHTML = `
      <div class="sp-bar">
        <span class="sp-title">Sessions</span>
        <select id="spSubject"><option value="">all subjects</option></select>
        <select id="spSystem"><option value="">all systems</option></select>
        <input id="spText" type="search" placeholder="protocol, variant, name…">
        <span class="sp-count" id="spCount"></span>
        <button id="spRefresh" title="Ask dserv again">↻</button>
        <button id="spClose" title="Close (Esc)">✕</button>
      </div>
      <div class="sp-list" id="spList"></div>
      <div class="sp-foot" id="spFoot"></div>`;
    main.appendChild(el);
    this.el = el;
    for (const id of ['spSubject', 'spSystem']) el.querySelector('#' + id).addEventListener('change', () => this.render());
    el.querySelector('#spText').addEventListener('input', () => this.render());
    el.querySelector('#spRefresh').addEventListener('click', () => this.refresh());
    el.querySelector('#spClose').addEventListener('click', () => this.hide());
    el.addEventListener('keydown', e => { if (e.key === 'Escape') { e.stopPropagation(); this.hide(); } });
  },

  isOpen() { return this.el && this.el.classList.contains('open'); },

  async show() {
    if (!this.available) return;
    this.el.classList.add('open');
    document.getElementById('spClose').style.display = this.app.dg ? '' : 'none';
    if (!this.sessions.length) await this.refresh(); else this.render();
    this.el.querySelector('#spText').focus();
  },

  hide() {
    if (!this.app.dg) return;          // nothing behind it yet
    this.el.classList.remove('open');
  },

  async refresh() {
    const foot = this.el.querySelector('#spFoot');
    foot.textContent = `asking ${this.host}…`;
    try {
      if (!this.conn.connected) await this.conn.connect();   // it may have dropped since
      const r = await this.conn.evalAsync('send df {list_sessions}', 15000);
      this.sessions = JSON.parse(r);
      foot.textContent = '';
    } catch (e) {
      this.sessions = [];
      foot.textContent = /invalid command name/.test(e.message)
        ? `${this.host}'s dserv has no list_sessions: it needs the current config/dfconf.tcl`
        : `could not list sessions: ${e.message}`;
    }
    this._fillSelect('spSubject', 'subject', 'all subjects');
    this._fillSelect('spSystem', 'system', 'all systems');
    this.render();
  },

  _fillSelect(id, key, all) {
    const sel = this.el.querySelector('#' + id), keep = sel.value;
    const vals = [...new Set(this.sessions.map(s => s[key]).filter(Boolean))].sort();
    sel.innerHTML = `<option value="">${all}</option>` + vals.map(v => `<option>${esc(v)}</option>`).join('');
    if (vals.includes(keep)) sel.value = keep;
  },

  _filtered() {
    const subj = this.el.querySelector('#spSubject').value;
    const sys = this.el.querySelector('#spSystem').value;
    const words = this.el.querySelector('#spText').value.toLowerCase().split(/\s+/).filter(Boolean);
    return this.sessions.filter(s =>
      (!subj || s.subject === subj) && (!sys || s.system === sys) &&
      words.every(w => `${s.name} ${s.protocol} ${s.variant}`.toLowerCase().includes(w)));
  },

  render() {
    const list = this.el.querySelector('#spList');
    const rows = this._filtered();
    this.el.querySelector('#spCount').textContent =
      `${rows.length}${rows.length !== this.sessions.length ? ' of ' + this.sessions.length : ''} session${this.sessions.length === 1 ? '' : 's'}`;
    list.innerHTML = '';
    let lastDate = null;
    for (const s of rows) {
      if (s.date !== lastDate) {
        lastDate = s.date;
        const d = document.createElement('div');
        d.className = 'sp-date'; d.textContent = s.date || 'undated';
        list.appendChild(d);
      }
      const r = document.createElement('div');
      r.className = 'sp-row' + (s.name === this.current ? ' current' : '');
      r.title = s.name;
      r.innerHTML =
        `<span class="sp-time">${esc((s.time || '').slice(0, 5))}</span>` +
        `<span class="sp-subj">${esc(s.subject)}</span>` +
        `<span class="sp-exp">${esc(s.system)}<i>/</i>${esc(s.protocol)}<i>/</i>${esc(s.variant)}</span>` +
        `<span class="sp-n">${s.n_trials} trials</span>` +
        `<span class="sp-size">${s.has_trials ? (s.trials_size / 1048576).toFixed(1) + ' MB' : ''}</span>`;
      if (!s.has_obs) r.title += ' (no obs file here: no stim2 clips)';
      r.addEventListener('click', () => this.open(s.name));
      list.appendChild(r);
    }
    if (!rows.length && this.sessions.length) list.innerHTML = '<div class="sp-empty">no session matches</div>';
    else if (!this.sessions.length) list.innerHTML = '<div class="sp-empty">no sessions with trials files on this host</div>';
  },

  async open(name) {
    const url = this.dataUrl(name);
    const foot = this.el.querySelector('#spFoot');
    const s = this.sessions.find(x => x.name === name);
    if (s && !s.has_trials) {
      foot.textContent = `extracting ${name}'s trials…`;
      const err = await this._extract(name);
      if (err) { foot.textContent = `${name}: ${err}`; return; }
      s.has_trials = true;
    }
    foot.textContent = `loading ${name}…`;
    const ok = await this.app.loadURL(url);
    if (!ok) { foot.textContent = `could not load ${name}`; return; }
    this.current = name;
    this.el.querySelector('#spFoot').textContent = '';
    // the address names the session: reload or share it
    const p = new URLSearchParams(window.location.search);
    p.set('file', url);
    history.replaceState(null, '', `${location.pathname}?${p.toString().replace(/%2F/g, '/').replace(/%3A/g, ':')}`);
    this.el.classList.remove('open');
  },

  // The trials file is made in df, which says when in the datapoint
  // df/viewer_trials/NAME; sendNoReply, so dserv's main interp does not
  // wait on the extraction (its other pages would wait with it).  "" when
  // done, else why not.
  async _extract(name) {
    const token = Math.random().toString(36).slice(2, 10);
    try {
      if (!this.conn.connected) await this.conn.connect();
      await this.conn.evalAsync(`sendNoReply df {viewer_trials ${name} ${token}}`, 5000);
    } catch (e) {
      return /invalid command name/.test(e.message)
        ? `this dserv cannot extract trials for the viewer (it needs the current config/dfconf.tcl)`
        : e.message;
    }
    const until = Date.now() + 180000;
    while (Date.now() < until) {
      await new Promise(r => setTimeout(r, 300));
      let v;
      try {
        v = JSON.parse(await this.conn.evalAsync(`dservGet df/viewer_trials/${name}`, 5000));
      } catch (e) { continue; }                    // not there yet
      if (v.token !== token) continue;              // an earlier answer
      return v.status === 'ok' ? '' : (v.error || 'extraction failed');
    }
    return 'no answer from df after 3 minutes';
  },
};

function esc(s) {
  return String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}
