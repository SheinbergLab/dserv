// stim2_view.js — dg_viewer's stim2 mode.
//
// The world panel plays a frame-locked movie of the selected trial,
// rendered by stim2 from the session's own data (stim2's
// tools/render/render_server.tcl, docs/render.md), in step with the trace:
// while it plays the video is the clock, and a scrub, a key or the slider
// seeks it.  The gaze is drawn here, over the video, from the trials DG's
// em_h_deg / em_v_deg.
//
// The render server is ?render=URL, else this page's host on port 8140.
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
    this.server = (p.get('render') || `${location.protocol}//${location.hostname}:8140`).replace(/\/+$/, '');
    this._build();
    try {
      const r = await fetch(this.server + '/', { signal: AbortSignal.timeout(1500) });
      this.available = r.ok;
    } catch (e) {
      this.available = false;
    }
    console.log(`stim2 render server ${this.server}: ${this.available ? 'available' : 'not answering'}`);
    return this.available;
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
    const zeroMs = (this.app.hasEye && j.timing.em_zero_ms != null) ? j.timing.em_zero_ms : j.timing.obs_begin_ms;
    this.zero = zeroMs / 1000 - (this.app.td ? this.app.td.tStart : 0);
    if (!this.app.hasEye && this.app.td) {
      // no em_seconds: the trial is BEGINOBS to ENDOBS
      const td = this.app.td;
      td.tStart = 0; td.dur = td.tEnd = (j.timing.obs_end_ms - j.timing.obs_begin_ms) / 1000;
      this.zero = j.timing.obs_begin_ms / 1000;
      this.app.renderTrial();
    }
    this._status(j.cached ? '' : 'loading…');
    this.video.playbackRate = this.app.speed || 1;
    this.video.src = this.server + j.clip;
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
    this.loading = false;
    if (!ok) { this._status(`the clip did not load (${this.server + j.clip})`); return; }
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
      if (my !== this.waiting || !s.rendering) return;
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

  _seekTo(t) {
    if (this.video.seeking) { this.pendingSeek = t; return; }
    const d = this.video.duration || 0;
    const c = Math.max(0, Math.min(d > 0 ? d - this._frame() / 2 : 0, t));
    if (Math.abs(this.video.currentTime - c) > this._frame() / 2) this.video.currentTime = c;
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
      if (tn >= 0) { this._seekTo(tn); this._play(); }
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
    if (this.showGaze) this._drawGaze(ctx, R, pt);
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
