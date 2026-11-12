// stim2_view.js — dg_viewer's stim2 mode.
//
// The world panel plays a frame-locked movie of the selected trial,
// rendered by stim2 from the session's own data (stim2's
// tools/render/render_server.tcl, docs/render.md), in step with the trace:
// while it plays the video is the clock, and a scrub, a key or the slider
// seeks it.  The gaze is drawn here, over the video, from the trials DG's
// em_h_deg / em_v_deg.
//
// The render server is ?render=URL, else the dserv's `df render_url`
// setting (a rig whose render host is another machine), else this page's
// host on port 8140: https there when this page is https (a render server
// run with -cert/-key), falling back to http.  Safari will not let an https
// page use an http server at all, even on localhost, so with Safari an
// https dserv needs an https render server.
// A trials DG row names its clip by `filename` and `obsid`.
//
// Time: the viewer's clock is App.pt, seconds from td.tStart (em_seconds,
// which counts from the trial's first eye sample).  The clip's timing JSON
// gives where em_seconds is 0 on the clip's timeline (em_zero_ms; else
// obs_begin_ms, BEGINOBS, a few ms earlier).

const Stim2View = {
  app: null,
  server: '',
  available: false,     // the server answered
  enabled: true,        // the stim2 checkbox
  showGaze: true,
  showTouch: true,      // the session's touchscreen presses (the clip timing's touches)
  box: null, video: null, overlay: null, octx: null, statusEl: null,
  timing: null, ready: false, req: 0,
  zero: 0,              // clip seconds at td.tStart + 0
  lastPt: null,         // the pt advance() last returned (to spot a jump)
  pendingSeek: null,

  async init(app) {
    this.app = app;
    this._setNoClip(null);                       // a new file: try again
    if (this._probed) return this.available;     // once per page
    this._probed = true;
    const p = new URLSearchParams(window.location.search);
    // A named server is used as named.  Otherwise port 8140 of this host,
    // in this page's scheme first: an https page tries https (a render
    // server with -cert/-key), then http, which Chrome allows from an https
    // page on localhost only (clips then come through _blob, below) and
    // Safari never does.
    const named = p.get('render') || await this._rigServer();
    const here = location.hostname;
    const tries = named ? [named]
                : location.protocol === 'https:' ? [`https://${here}:8140`, `http://${here}:8140`]
                : [`http://${here}:8140`];
    this._build();
    this.available = false;
    for (const t of tries) {
      this.server = t.replace(/\/+$/, '');
      try {
        const r = await fetch(this.server + '/', { signal: AbortSignal.timeout(1500) });
        this.available = r.ok;
      } catch (e) {
        this.available = false;
      }
      if (this.available) break;
    }
    if (!this.available) this.server = tries[0].replace(/\/+$/, '');
    console.log(`stim2 render server ${this.server}: ${this.available ? 'available' : 'not answering'}` +
                (!this.available && location.protocol === 'https:' && this.server.startsWith('http:')
                 ? ' (an http server from an https page: Safari blocks this; run the render server with -cert/-key)' : ''));
    return this.available;
  },

  // the rig's own say: dserv's `df render_url` setting, through the
  // Sessions panel's connection ("" when unset, or a dserv without it)
  async _rigServer() {
    if (typeof SessionBrowser === 'undefined' || !SessionBrowser.ready) return '';
    try {
      if (!await SessionBrowser.ready) return '';
      return (await SessionBrowser.conn.evalAsync('dservGet settings/df/render_url', 3000)).trim();
    } catch (e) {
      return '';
    }
  },

  active() { return this.available && this.enabled && !this.noClip && this.app && this.app.isTrial; },

  // A session stim2 cannot replay (the server's 422: recorded before
  // stim2 kept frame records, or without its script): the world panel
  // goes back to the plugin's own drawing for this file, and the stim2
  // checkbox says why.
  _setNoClip(reason) {
    this.noClip = reason;
    const chk = document.getElementById('chkStim2');
    if (!chk) return;
    const label = chk.parentElement;
    chk.disabled = !!reason;
    label.lastChild.textContent = reason ? ' stim2: no clip' : ' stim2';
    label.title = reason || '';
    label.style.opacity = reason ? '0.6' : '';
  },

  _build() {
    if (this.box) return;
    const wp = document.getElementById('worldPanel');
    const box = document.createElement('div');
    box.className = 'stim2-box';
    box.innerHTML = '<video muted playsinline preload="auto"></video><canvas></canvas><div class="stim2-status"></div>';
    wp.insertBefore(box, wp.firstChild);
    this.box = box;
    this.video = box.querySelector('video');
    this.overlay = box.querySelector('canvas');
    this.octx = this.overlay.getContext('2d');
    this.statusEl = box.querySelector('.stim2-status');
    this.video.addEventListener('seeked', () => {
      if (this.pendingSeek != null) { const t = this.pendingSeek; this.pendingSeek = null; this._seekTo(t); }
      this.draw();
    });
    this.video.addEventListener('loadedmetadata', () => this.resize());
  },

  // the controls: stim2 on/off and the gaze, in the controls bar
  controls(container) {
    if (!this.available || document.getElementById('chkStim2')) return;
    const mk = (id, label, checked, fn) => {
      const l = document.createElement('label'); l.className = 'bt';
      const i = document.createElement('input'); i.type = 'checkbox'; i.id = id; i.checked = checked;
      i.addEventListener('change', () => fn(i.checked));
      l.appendChild(i); l.appendChild(document.createTextNode(' ' + label));
      container.appendChild(l);
    };
    mk('chkStim2', 'stim2', this.enabled, on => {
      this.enabled = on;
      this.app.applyWorldMode();
      if (on) this.load(this.app.cur); else this.video.pause();
    });
    mk('chkStim2Gaze', 'gaze', this.showGaze, on => { this.showGaze = on; this.draw(); });
    mk('chkStim2Touch', 'touch', this.showTouch, on => { this.showTouch = on; this.draw(); });
    const b = document.createElement('button');
    b.id = 'btnStim2Export'; b.className = 'stim2-export-btn'; b.textContent = 'mp4';
    b.title = 'Export this trial as a movie (stim2 draws the gaze and caption into it)';
    b.addEventListener('click', e => { e.stopPropagation(); this._exportPanel(b); });
    container.appendChild(b);
  },

  // ── export: the selected trial as one movie, with the gaze shown here
  // and a caption, drawn by stim2 (the render server's POST /export) ──
  _exportPanel(btn) {
    let p = document.getElementById('stim2Export');
    if (p) { p.remove(); return; }
    p = document.createElement('div');
    p.id = 'stim2Export'; p.className = 'stim2-export';
    const hasGaze = !!(this.app.hasEye && this.app.td && this.app.td.eyeT && this.app.td.eyeT.length);
    // touches: what the render server found in the session for this clip
    const hasTouch = !!(this.ready && this.timing && this.timing.touches && this.timing.touches.length);
    const dg = this.app.dg, i = this.app.cur;
    const obsid = getScalar(dg, 'obsid', i);
    p.innerHTML = `
      <div class="x-title">Export trial ${obsid != null ? Number(obsid) + 1 : ''}</div>
      <input type="text" class="x-cap" maxlength="500" placeholder="caption (optional)">
      <div class="x-row">
        <select class="x-at" title="Where the caption goes">
          <option value="nw">top left</option><option value="n">top</option><option value="ne">top right</option>
          <option value="sw">bottom left</option><option value="s">bottom</option><option value="se">bottom right</option>
        </select>
        <label class="bt"><input type="checkbox" class="x-gaze"${hasGaze && this.showGaze ? ' checked' : ''}${hasGaze ? '' : ' disabled'}> gaze</label>
        <label class="bt"><input type="checkbox" class="x-touch"${hasTouch && this.showTouch ? ' checked' : ''}${hasTouch ? '' : ' disabled'}> touch</label>
      </div>
      <div class="x-row"><button class="primary x-go">Export</button><button class="x-close">Close</button></div>
      <div class="x-status"></div>`;
    document.body.appendChild(p);
    const r = btn.getBoundingClientRect();
    p.style.left = Math.max(8, Math.min(window.innerWidth - p.offsetWidth - 8, r.right - p.offsetWidth)) + 'px';
    p.style.bottom = (window.innerHeight - r.top + 6) + 'px';
    const close = () => { p.remove(); document.removeEventListener('mousedown', outside); document.removeEventListener('keydown', esc); };
    const outside = e => { if (!p.contains(e.target) && e.target !== btn) close(); };
    const esc = e => { if (e.key === 'Escape') close(); };
    document.addEventListener('mousedown', outside);
    document.addEventListener('keydown', esc);
    p.querySelector('.x-close').addEventListener('click', close);
    const cap = p.querySelector('.x-cap');
    cap.addEventListener('keydown', e => { e.stopPropagation(); if (e.key === 'Enter') go.click(); if (e.key === 'Escape') close(); });
    const go = p.querySelector('.x-go');
    go.addEventListener('click', async () => {
      go.disabled = true;
      try {
        await this.exportTrial(i, {
          caption: cap.value.trim(),
          caption_at: p.querySelector('.x-at').value,
          gaze: p.querySelector('.x-gaze').checked,
          touch: p.querySelector('.x-touch').checked,
        }, msg => { p.querySelector('.x-status').textContent = msg; });
      } finally { go.disabled = false; }
    });
    cap.focus();
  },

  // row i's reel item: the trial, its caption, and the gaze as this page
  // draws it (em_seconds from the trial's first sample; blinks left out
  // when they are masked here)
  _trialItem(i, opts) {
    const dg = this.app.dg;
    const fn = String(getScalar(dg, 'filename', i)).split(/[\\/]/).pop();
    const item = { type: 'trial', file: fn, obsid: Number(getScalar(dg, 'obsid', i)) };
    if (opts.caption) { item.caption = opts.caption; item.caption_at = opts.caption_at || 'nw'; }
    const td = i === this.app.cur ? this.app.td : null;
    if (opts.gaze && td && td.eyeT && td.eyeH && td.eyeV) {
      const t = [], x = [], y = [];
      const mask = this.app.maskBlinks && td.blink;
      for (let k = 0; k < td.eyeT.length; k++) {
        if (mask && td.blink[k] !== 0) continue;
        const h = td.eyeH[k], v = td.eyeV[k];
        if (!Number.isFinite(h) || !Number.isFinite(v)) continue;
        t.push(Math.round((td.eyeT[k] - td.tStart) * 1e4) / 1e4);
        x.push(Math.round(h * 1e3) / 1e3); y.push(Math.round(v * 1e3) / 1e3);
      }
      if (t.length) item.gaze = { t, x, y };
    }
    // the server reads the touches from the session itself
    if (opts.touch) item.touch = true;
    return item;
  },

  // render and download; say(msg) reports progress
  async exportTrial(i, opts, say) {
    if (!this.available) { say('no render server'); return; }
    const dg = this.app.dg;
    if (getScalar(dg, 'filename', i) == null || getScalar(dg, 'obsid', i) == null) {
      say('no filename/obsid columns: cannot find this trial'); return;
    }
    const item = this._trialItem(i, opts);
    const name = `${item.file.replace(/\.(trials\.dgz|obs\.dgz|ess|dgz)$/, '')}_trial${item.obsid + 1}.mp4`;
    say('rendering…');
    const poll = setInterval(async () => {
      try {
        const s = await (await fetch(this.server + '/status')).json();
        if (s.rendering && s.rendering.job.startsWith('export/')) {
          say(`rendering… ${Math.round(s.rendering.progress * 100)}%`);
        } else if (s.rendering) {
          say(`waiting for trial ${s.rendering.job.split('/').pop()} to finish…`);
        }
      } catch (e) { /* the answer will come */ }
    }, 400);
    let j, r;
    try {
      r = await fetch(this.server + '/export', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ fps: 60, items: [item] }),
      });
      j = await r.json();
    } catch (e) {
      j = { error: `render server ${this.server}: ${e.message}` }; r = { ok: false };
    } finally { clearInterval(poll); }
    if (!r.ok || j.error) { say(j.error || `export failed (${r.status})`); return; }
    say('downloading…');
    try {
      const blob = await (await fetch(this.server + j.clip)).blob();
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob); a.download = name;
      document.body.appendChild(a); a.click(); a.remove();
      setTimeout(() => URL.revokeObjectURL(a.href), 10000);
      const size = blob.size < 1048576 ? `${Math.max(1, Math.round(blob.size / 1024))} KB`
                                       : `${(blob.size / 1048576).toFixed(1)} MB`;
      say(`saved ${name} (${size})`);
    } catch (e) {
      say(`the movie did not download: ${e.message}`);
    }
  },

  // show the video (stim2 on) or the plugin's canvas
  applyMode() {
    const on = this.active();
    this.box.style.display = on ? 'block' : 'none';
    const cw = document.getElementById('cvsWorld');
    if (cw) cw.style.display = on ? 'none' : '';
    const wp = document.getElementById('worldPanel');
    wp.classList.toggle('stim2-on', on);
    if (on) this.resize();
  },

  _status(msg) {
    this.statusEl.textContent = msg || '';
    this.statusEl.style.display = msg ? 'block' : 'none';
  },

  // ── the clip for row i ──
  async load(i) {
    if (!this.active() || i < 0) return;
    const my = ++this.req;
    this.ready = false; this.timing = null; this.lastPt = null; this.pendingSeek = null;
    this.loading = true;      // playback waits for the clip meanwhile
    this.video.pause();
    this.video.removeAttribute('src'); this.video.load();
    if (this._blob) { URL.revokeObjectURL(this._blob); this._blob = null; }
    this.draw();
    const dg = this.app.dg;
    let fn = getScalar(dg, 'filename', i), obsid = getScalar(dg, 'obsid', i);
    if (fn == null || obsid == null) {
      this.loading = false;
      this._status(`no ${fn == null ? 'filename' : 'obsid'} column: cannot find this trial's data`);
      return;
    }
    fn = String(fn).split(/[\\/]/).pop();
    this._status(`rendering trial ${Number(obsid) + 1}…`);
    this.waiting = my;
    const poll = setInterval(() => this._progress(my, Number(obsid) + 1), 300);
    let j, r;
    try {
      // then=: the next rows' obsids, for the server to render meanwhile
      const then = [];
      for (let k = i + 1; k < this.app.nRows && then.length < 2; k++) {
        const o = getScalar(dg, 'obsid', k);
        if (o != null) then.push(Number(o));
      }
      r = await fetch(`${this.server}/trial?file=${encodeURIComponent(fn)}&obsid=${Number(obsid)}` +
                      (then.length ? `&then=${then.join(',')}` : ''));
      j = await r.json();
    } catch (e) {
      j = { error: `render server ${this.server}: ${e.message}` }; r = { ok: false };
    } finally {
      clearInterval(poll);
      if (this.waiting === my) this.waiting = 0;   // a poll still in flight must not write
    }
    if (my !== this.req) return;           // another row was chosen meanwhile
    if (r.status === 422) {
      this.loading = false;
      this._status('');
      this._setNoClip(j.error || 'stim2 cannot replay this session');
      console.log('stim2 render server: no clip for this session:', this.noClip);
      this.app.applyWorldMode();
      return;
    }
    if (!r.ok || j.error) {
      this.loading = false;     // play on without the clip
      this._status(j.error || `render failed (${r.status})`);
      return;
    }
    this.timing = j.timing;
    // The clip is timed by the rig's frames; the trials DG only places the
    // viewer's clock on it.  The trial runs BEGINOBS to ENDOBS whatever the
    // eye record covers, so the clip always plays to its end.
    const td = this.app.td;
    const obsDur = (j.timing.obs_end_ms - j.timing.obs_begin_ms) / 1000;
    const eyeTimed = this.app.hasEye && td && td.eyeT && td.eyeT.length > 1 && j.timing.em_zero_ms != null;
    if (eyeTimed) {
      // em_seconds: 0 at the period's first eye record (em_zero_ms)
      this.zero = j.timing.em_zero_ms / 1000 - td.tStart;
      const end = (j.timing.obs_end_ms - j.timing.em_zero_ms) / 1000;
      if (end > td.tEnd) { td.tEnd = end; td.dur = td.tEnd - td.tStart; this.app.renderTrial(); }
    } else if (td) {
      // no eye times (no eye data, or positions without em_seconds, as from
      // a virtual eye's eyetracking/raw alone): obs time, BEGINOBS = 0
      td.tStart = 0; td.dur = td.tEnd = obsDur;
      this.zero = j.timing.obs_begin_ms / 1000;
      this.app.renderTrial();
    }
    this._status(j.cached ? '' : 'loading…');
    let src = this.server + j.clip;
    if (location.protocol === 'https:' && src.startsWith('http:')) {
      // An http render server from an https page (Chrome, localhost only):
      // the <video> would ask for the clip over https (browsers upgrade
      // mixed-content media), which an http server does not speak; fetch()
      // is not upgraded, so fetch the clip and play it from memory.  Clips
      // are a few MB at most.  An https server needs none of this.
      try {
        const b = await (await fetch(src)).blob();
        if (my !== this.req) return;
        src = this._blob = URL.createObjectURL(b);
      } catch (e) {
        if (my !== this.req) return;
        this.loading = false;
        this._status(`the clip did not load (${src}): ${e.message}`);
        return;
      }
    }
    this.video.playbackRate = this.app.speed || 1;
    this.video.src = src;
    // ready once the duration is known: Safari may load no picture until
    // something seeks or plays, which is what comes next
    const ok = await new Promise(res => {
      const v = this.video;
      const done = e => {
        v.removeEventListener('loadedmetadata', done); v.removeEventListener('error', done);
        res(e.type === 'loadedmetadata');
      };
      v.addEventListener('loadedmetadata', done); v.addEventListener('error', done);
    });
    if (my !== this.req) return;
    if (!ok) { this.loading = false; this._status(`the clip did not load (${this.server + j.clip})`); return; }
    // Prime the clip: play it for a moment and stop, before anything uses
    // it.  Safari (WebKit) otherwise leaves a never-played video's picture
    // on its first frame when play() starts it from the start of the clip:
    // the video's clock runs, the picture does not, until a later seek.
    // The playback clock waits meanwhile (loading).
    await this._prime();
    if (my !== this.req) return;
    this.loading = false;
    this.ready = true;
    this._status('');
    document.getElementById('worldLabel').textContent =
      `stim2 · trial ${j.timing.trial} · ${j.timing.fps} fps`;
    this.resize();
    if (this.app.playing) this.setPlaying(true); else this.seek(this.app.pt);
  },

  async _progress(my, trial) {
    if (my !== this.waiting) return;
    try {
      const s = await (await fetch(this.server + '/status')).json();
      if (my !== this.waiting) return;
      if (s.fetching && s.fetching.length && !s.rendering) {
        this._status('copying the session from the rig…');
        return;
      }
      if (!s.rendering) return;
      if (s.rendering.job.endsWith('/' + trial)) {
        this._status(`rendering trial ${trial}… ${Math.round(s.rendering.progress * 100)}%`);
      } else {
        this._status(`waiting for trial ${s.rendering.job.split('/').pop()} to finish…`);
      }
    } catch (e) { /* the answer will come */ }
  },

  // ── time ──
  clipT(pt) { return this.zero + (this.app.td ? this.app.td.tStart : 0) + pt; },
  ptOf(clipT) { return clipT - this.zero - (this.app.td ? this.app.td.tStart : 0); },
  _frame() { return this.timing ? 1 / this.timing.fps : 1 / 60; },

  // force: seek even when already within half a frame of t (see advance)
  _seekTo(t, force = false) {
    if (this.video.seeking) { this.pendingSeek = t; return; }
    const d = this.video.duration || 0;
    const c = Math.max(0, Math.min(d > 0 ? d - this._frame() / 2 : 0, t));
    if (force || Math.abs(this.video.currentTime - c) > this._frame() / 2) this.video.currentTime = c;
  },

  // play the fresh clip briefly, then back to its start (see load).  Muted
  // inline video may always play; a refusal or a stall just skips it.
  async _prime() {
    const v = this.video;
    try {
      await Promise.race([v.play(), new Promise((_, no) => setTimeout(() => no(new Error('timeout')), 750))]);
    } catch (e) { /* refused or slow: unprimed, as before */ }
    v.pause();
    if (v.currentTime !== 0) {
      await new Promise(res => {
        const t = setTimeout(res, 750);
        v.addEventListener('seeked', () => { clearTimeout(t); res(); }, { once: true });
        v.currentTime = 0;
      });
    }
  },

  // paused: put the video at pt
  seek(pt) {
    if (!this.ready) return;
    this._seekTo(this.clipT(pt));
  },

  setPlaying(on) {
    if (!this.ready) return;
    if (on) {
      this.lastPt = this.app.pt;
      const t = this.clipT(this.app.pt);
      if (t >= 0 && t < this.video.duration) { this._seekTo(t); this._play(); }
    } else {
      this.video.pause();
      this.seek(this.app.pt);
    }
  },

  setRate(r) { if (this.video) this.video.playbackRate = r; },

  _play() {
    this.video.play().catch(e => { if (e.name !== 'AbortError') console.warn('stim2 clip: play() refused:', e.name, e.message); });
  },

  // playing: the next pt.  The video is the clock where the clip covers
  // pt; before its first frame and after its last, the wall clock.  While
  // the clip renders or loads, the clock waits for it.
  advance(pt, dt, speed) {
    if (!this.ready) return this.loading ? pt : pt + dt * speed;
    const v = this.video;
    if (this.lastPt != null && Math.abs(pt - this.lastPt) > 0.05) {
      // the scrub bar or a key moved pt while playing
      const t = this.clipT(pt);
      if (t >= 0 && t < v.duration) { this._seekTo(t); if (v.paused) this._play(); }
      else v.pause();
      this.lastPt = pt;
      return pt;
    }
    let next;
    const t = this.clipT(pt);
    if (t < 0) {
      next = pt + dt * speed;
      const tn = this.clipT(next);
      // the clip starts: a real seek even when tn is within half a frame of
      // where the video sits (its start), then play -- the same order as
      // the loop's restart, which never froze, where the seek is long
      if (tn >= 0) { this._seekTo(tn, true); this._play(); }
    } else if (v.ended || t >= v.duration) {
      next = pt + dt * speed;
    } else if (v.paused) {
      this._seekTo(t); this._play();
      next = pt;
    } else {
      next = this.ptOf(v.currentTime);
    }
    this.lastPt = next;
    return next;
  },

  // ── drawing ──
  resize() {
    if (!this.box) return;
    const r = this.box.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
    this.overlay.width = Math.max(1, r.width * dpr); this.overlay.height = Math.max(1, r.height * dpr);
    this.overlay.style.width = r.width + 'px'; this.overlay.style.height = r.height + 'px';
    this.octx.setTransform(dpr, 0, 0, dpr, 0, 0);
    this.draw();
  },

  // the video's picture inside the box (object-fit: contain)
  _rect() {
    const bw = this.overlay.width / (window.devicePixelRatio || 1), bh = this.overlay.height / (window.devicePixelRatio || 1);
    const vw = (this.timing && this.timing.width) || this.video.videoWidth || 16;
    const vh = (this.timing && this.timing.height) || this.video.videoHeight || 9;
    const s = Math.min(bw / vw, bh / vh);
    return { x: (bw - vw * s) / 2, y: (bh - vh * s) / 2, w: vw * s, h: vh * s, bw, bh };
  },

  draw() {
    if (!this.octx) return;
    const ctx = this.octx, R = this._rect();
    ctx.clearRect(0, 0, R.bw, R.bh);
    if (!this.ready || !this.timing || !this.app.td) return;
    const pt = this.app.pt, t = this.clipT(pt);
    if (t < 0) {
      // Before the trial's first new frame.  stim2 draws only when
      // something changes, so the rig was still showing what the last
      // trial left, which this clip does not have: show the rig's
      // background, not the clip's first frame (which comes later), and
      // say when the first frame comes, on the viewer's time axis.
      const bg = (this.timing.screen && this.timing.screen.bg) || [0, 0, 0];
      ctx.fillStyle = `rgb(${bg[0]},${bg[1]},${bg[2]})`;
      ctx.fillRect(R.x, R.y, R.w, R.h);
      ctx.fillStyle = 'rgba(136,153,180,0.85)'; ctx.font = '10px "JetBrains Mono", monospace';
      ctx.textAlign = 'right'; ctx.textBaseline = 'bottom';
      ctx.fillText('rig screen not redrawn yet', R.x + R.w - 8, R.y + R.h - 20);
      ctx.fillText(`first frame at ${this.ptOf(0).toFixed(3)} s`, R.x + R.w - 8, R.y + R.h - 6);
    }
    if (this.showTouch) this._drawTouch(ctx, R, t * 1000);
    if (this.showGaze) this._drawGaze(ctx, R, pt);
  },

  // the touch strokes, as the export draws them (replaylib's _touch_at):
  // a 1.6 deg ring where the finger is, with a trail of its path over the
  // last 500 ms, from touch-down until the release (250 ms past the last
  // sample when none was logged), then at the release point fading over
  // 400 ms.  A stroke without a path (a tap, or a file from before ESS
  // logged drags) is a ring at the press, then at the release.
  _drawTouch(ctx, R, ms) {
    const scr = this.timing.screen || {}, marks = this.timing.touches;
    if (!marks || !marks.length || !scr.halfx || !scr.halfy) return;
    const toX = h => R.x + (0.5 + h / (2 * scr.halfx)) * R.w;
    const toY = v => R.y + (0.5 - v / (2 * scr.halfy)) * R.h;
    const rpx = Math.max(4, 0.8 / (2 * scr.halfx) * R.w);
    const HOLD = 250, FADE = 400, TRAIL = 500;
    const col = a => `rgba(255,77,230,${a.toFixed(3)})`;
    ctx.save();
    ctx.beginPath(); ctx.rect(R.x, R.y, R.w, R.h); ctx.clip();
    ctx.lineCap = 'round';
    for (const [on, x, y, up0, xu0, yu0, path] of marks) {
      if (on > ms) break;
      const n = path ? path.length / 3 : 0;
      const last = n ? path[3 * (n - 1)] : on;
      const up = up0 == null ? last + HOLD : up0;
      if (ms >= up + FADE) continue;
      const a = ms < up ? 1 : 1 - (ms - up) / FADE;
      const tt = Math.min(ms, up);
      let px = x, py = y;
      if (n) {
        // the finger at tt, and its trail back TRAIL ms, newest first
        const tr = [];
        for (let i = n - 1; i >= 0; i--) {
          const ts = path[3 * i];
          if (ts > tt) continue;
          if (tt - ts > TRAIL) break;
          tr.push([ts, path[3 * i + 1], path[3 * i + 2]]);
        }
        if (tr.length) { px = tr[0][1]; py = tr[0][2]; }
        ctx.lineWidth = Math.max(1, rpx * 0.16);
        for (let k = 1; k < tr.length; k++) {
          ctx.strokeStyle = col(0.7 * 0.95 * a * (1 - (tt - tr[k][0]) / TRAIL));
          ctx.beginPath();
          ctx.moveTo(toX(tr[k - 1][1]), toY(tr[k - 1][2]));
          ctx.lineTo(toX(tr[k][1]), toY(tr[k][2]));
          ctx.stroke();
        }
      } else if (ms >= up && up0 != null) {
        px = xu0; py = yu0;
      }
      ctx.lineWidth = Math.max(1.5, rpx * 0.24);
      ctx.strokeStyle = col(0.95 * a);
      ctx.beginPath(); ctx.arc(toX(px), toY(py), rpx, 0, Math.PI * 2); ctx.stroke();
    }
    ctx.restore();
  },

  _drawGaze(ctx, R, pt) {
    const td = this.app.td, scr = this.timing.screen || {};
    if (!td.eyeH || !td.eyeV || !td.eyeT || !scr.halfx || !scr.halfy) return;
    const toX = h => R.x + (0.5 + h / (2 * scr.halfx)) * R.w;
    const toY = v => R.y + (0.5 - v / (2 * scr.halfy)) * R.h;
    const now = td.tStart + pt;
    let i = this.app.t2i(now);
    if (i > 0 && td.eyeT[i] > now) i--;
    if (i < 0 || Math.abs(td.eyeT[i] - now) > 0.1) return;
    const blink = k => this.app.maskBlinks && td.blink && td.blink[k] !== 0;
    // a fading trail of the last 150 ms, then the disc
    ctx.save();
    ctx.beginPath(); ctx.rect(R.x, R.y, R.w, R.h); ctx.clip();
    ctx.lineWidth = 2; ctx.strokeStyle = 'rgba(255,140,26,0.35)';
    ctx.beginPath();
    let pen = false;
    for (let k = i; k >= 0 && now - td.eyeT[k] <= 0.15; k--) {
      if (blink(k)) { pen = false; continue; }
      const x = toX(td.eyeH[k]), y = toY(td.eyeV[k]);
      if (!pen) { ctx.moveTo(x, y); pen = true; } else ctx.lineTo(x, y);
    }
    ctx.stroke();
    if (!blink(i)) {
      const rpx = Math.max(3, 0.35 / (2 * scr.halfx) * R.w);   // a 0.7 deg disc
      ctx.fillStyle = 'rgba(255,140,26,0.9)';
      ctx.beginPath(); ctx.arc(toX(td.eyeH[i]), toY(td.eyeV[i]), rpx, 0, Math.PI * 2); ctx.fill();
    }
    ctx.restore();
  },
};
