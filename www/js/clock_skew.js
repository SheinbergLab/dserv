/* clock_skew.js -- is dserv's clock the same clock as this browser's?
 *
 * WHO NEEDS THIS. Only a page that ages datapoints against their SERVER
 * timestamp (`msg.timestamp`). Such a page is doing a subtraction across two
 * machines, and it is meaningful only while the two clocks agree.
 *
 * extio-config.html does that deliberately -- before you edit a value you need
 * to know whether the value is current ON THE SERVER, not merely whether the
 * box is talking. extio.html deliberately does NOT: every age there is
 * Date.now() minus a LOCAL arrival time, which answers "is this box talking to
 * me right now". Both are correct; they answer different questions. Do not add
 * this to a page of the second kind -- it would be warning about a clock that
 * page never consults.
 *
 * WHAT WENT WRONG WITHOUT IT (rig Pi, 2026-08-09). The host was put into the PTP
 * client role on a segment with no real grandmaster, phc2sys steered
 * CLOCK_REALTIME down to a from-zero PHC, and the machine ran in 1970 with
 * chrony stopped. dserv was immune -- it stamps from CLOCK_MONOTONIC and never
 * steps -- but it had captured its epoch constant at startup from the stale
 * clock systemd restored at boot, so every datapoint carried a timestamp ~29 h
 * in the past. extio-config.html then showed EVERY box as "offline 29h" while
 * the rig was healthy and publishing watchdogs at 1 Hz. The symptom pointed at
 * the boxes and the network. The fault was a clock, and nothing said so.
 *
 * See also ptp/clock_skew_us and ptp/clock_error, which config/ptpconf.tcl
 * publishes for the same reason on the server side.
 *
 * Usage:
 *     const skew = ClockSkew.create({ elementId: 'clockwarn' });
 *     ... on every datapoint:   skew.note(msg.timestamp);
 *     ... on your 1 Hz tick:    skew.render();
 *     ... if you want the number: skew.us()   // null | signed microseconds
 */
window.ClockSkew = (() => {
  'use strict';

  const DEFAULTS = {
    elementId: 'clockwarn',
    windowMs:  60000,
    warnUs:    60 * 1e6,   /* same threshold as ptpconf's ptp/clock_error */
  };

  function fmtDur(us) {
    const s = Math.floor(us / 1e6);
    if (s < 90) return s + 's';
    const m = Math.floor(s / 60);
    if (m < 120) return m + 'm';
    const h = Math.floor(m / 60);
    return h < 48 ? h + 'h' : Math.floor(h / 24) + 'd ' + (h % 24) + 'h';
  }

  return {
    create(opts = {}) {
      const cfg = { ...DEFAULTS, ...opts };
      let samples = [];
      let lastHtml = null;

      return {
        /* Feed every datapoint that carries a server timestamp. Cheap: one
         * push and an amortised shift. */
        note(ts) {
          if (!ts) return;                  /* no server timestamp = nothing to compare */
          const at = Date.now();
          samples.push({ at, d: at * 1000 - ts });
          const cutoff = at - cfg.windowMs;
          while (samples.length && samples[0].at < cutoff) samples.shift();
        },

        /* THE ESTIMATOR IS A MIN OVER A ROLLING WINDOW. Both halves are
         * load-bearing.
         *
         * MIN, not mean and not latest: `now - msg.timestamp` is
         * (clock skew) + (how stale that datapoint already was) + (transport
         * delay). Only the first term is wanted, and the other two are strictly
         * non-negative, so the minimum over recent arrivals is the tightest
         * available bound on the skew. A mean would be dragged arbitrarily far
         * by the retained backlog that lands on every subscribe -- dserv retains
         * datapoints forever, so that backlog can be days old.
         *
         * ROLLING, not all-time: an all-time minimum can never recover from a
         * server clock that steps BACKWARD. The older, smaller sample would
         * stand for the life of the page and this would stay silent through
         * exactly the fault it exists to catch. */
        us() {
          if (!samples.length) return null;
          let m = Infinity;
          for (const s of samples) if (s.d < m) m = s.d;
          return m;
        },

        render() {
          const el = document.getElementById(cfg.elementId);
          if (!el) return;
          const sk = this.us();
          if (sk === null || Math.abs(sk) < cfg.warnUs) {
            if (lastHtml !== null) { el.innerHTML = ''; lastHtml = null; }
            el.className = 'hide';
            return;
          }
          el.className = '';
          /* sk > 0 means dserv's timestamps are OLDER than our clock, i.e. dserv
           * is behind -- the direction that makes everything read as offline. */
          const dir = sk > 0 ? 'behind' : 'ahead of';
          const html =
            `<b>dserv's clock is ${fmtDur(Math.abs(sk))} ${dir} this browser's.</b> ` +
            `Every age and liveness state below is measured against it, so boxes may ` +
            `read <b>offline</b> while they are in fact healthy — check the watchdog ` +
            `counter itself, not the pill.<br>` +
            `Fix the host clock (its NTP daemon, and <code>dserv-ptp-setup status</code> — ` +
            `a PTP client following a grandmaster that was never set will do this), then ` +
            `<b>restart dserv</b>: its epoch anchor is captured once at startup and is ` +
            `never recomputed.`;
          /* Memoized: this is called on a 1 Hz tick and the text only changes
           * when fmtDur crosses a bucket. */
          if (html !== lastHtml) { el.innerHTML = html; lastHtml = html; }
        },
      };
    },
  };
})();
