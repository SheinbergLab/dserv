/* extio-setup UI.
 *
 * One model + renderer, two drivers behind a small seam:
 *   serial -- the local USB console: full config read via `dump`, CLI writes,
 *             interactive console, profiles. (v1, hardware-validated.)
 *   dserv  -- a rig's dserv datapoint table: box picker from extio/boxes,
 *             live pin map from the box's announced manifest + di/do frames,
 *             writes via %set extio/<box>/config|cmd. No console; pulse/
 *             debounce/active_low are write-only (the box doesn't announce
 *             them), and in_pullup renders as plain "in" (pins/in doesn't
 *             distinguish) -- the serial dump stays the full source of truth.
 */
"use strict";

const $ = (id) => document.getElementById(id);

// Compare two firmware version strings (git describe: "X.Y.Z[-N-gHASH][-dirty]"
// or an override like "0.47.16-OTA1"). Returns >0 if a is newer, <0 if b is newer,
// 0 if equal, by leading X.Y.Z then the git commit count (-N). Fuzzy by design --
// good enough to avoid calling an OLDER shelf image an "update".
function cmpFw(a, b) {
  const parse = (s) => {
    const m = String(s).match(/(\d+)\.(\d+)\.(\d+)(?:-(\d+))?/);
    return m ? [+m[1], +m[2], +m[3], m[4] ? +m[4] : 0] : [0, 0, 0, 0];
  };
  const pa = parse(a), pb = parse(b);
  for (let i = 0; i < 4; i++) { const d = pa[i] - pb[i]; if (d) return Math.sign(d); }
  return String(a) === String(b) ? 0 : 0;   // same version tuple -> treat as equal
}
const api = async (path, body) => {
  const opts = body === undefined ? {} :
    { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) };
  const r = await fetch(path, opts);
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error || r.statusText);
  return j;
};

/* Board map: W6300-EVB-Pico2 (dual image) from wiznet-io/PINMAP.md.
 * "claimed" pins depend on live config (mcp/oled), reflected from the box's
 * announced state/{mcp_en,oled_en} (dserv) or `show` output (serial). MCP3204
 * and OLED share SPI0 SCK/MOSI (GP2/3), so when both are on the OLED note wins
 * for those two -- pinRole checks oled first. */
const NPINS = 29;
const FIXED = { 0:"UART0 TX", 1:"UART0 RX", 15:"W6300", 16:"W6300", 17:"W6300",
  18:"W6300", 19:"W6300", 20:"W6300", 21:"W6300", 22:"W6300",
  23:"SMPS", 24:"VBUS", 28:"mode strap" };
const OLED_PINS = { 2:"OLED CLK", 3:"OLED DATA", 6:"OLED CS", 7:"OLED DC", 8:"OLED RST" };
const MCP_PINS = { 2:"MCP SCK", 3:"MCP DIN", 4:"MCP DOUT", 5:"MCP CS" };

let cfg = emptyCfg();
let selPin = null;
let connected = false;
let es = null;  // console EventSource
let esd = null; // data-events EventSource
let reloadT = null;      // debounce for config re-reads (console OKs, manifest frames)
const liveDI = new Map(); // pin -> {v, count}: live di/do state from the event stream
let beats = 0, lastBeat = 0;
let obsLive = false; // box is currently in an observation (state/in_obs = 1)

function emptyCfg() {
  return { name: "", desc: "", mode: "", obs: null, sync: null,
    mcp: false, oled: false, pins: {}, dgroups: {}, again: {}, raw: [],
    // firmware identity: fw=running version, build=shelf image line
    // (BOX_BUILD_TARGET), board=compat key (BOX_BOARD_ID), channel=update track.
    // build/board/channel arrive over dserv (state/*) or serial (`show` trailer);
    // absent on boxes predating the firmware change -> loadShelf falls back.
    fw: "", build: "", board: "", channel: "" };
}
const pin = (n) => (cfg.pins[n] ??= { mode: "", pulse: 0, debounce: 0, actlow: false, label: "" });

function scheduleReload(ms = 700) {
  clearTimeout(reloadT);
  reloadT = setTimeout(() => reload(selPin).catch(() => {}), ms);
}

/* ================= drivers ================= */

/* ---- serial: the box's USB console (config, CLI, dump/show, diagnostics).
 * Events come from the box's own data CDC -- UNLESS "events via dserv" is on
 * (combined mode): then the console stays here but events arrive over a dserv
 * that already owns the box's data port, sidestepping the two-reader
 * contention. eventsVia records which. ---- */
const SerialDriver = {
  id: "serial",
  hasConsole: true,   // interactive CLI pane
  hasProfiles: true,  // dump-based save/apply
  fullConfig: true,   // pulse/debounce/active_low/in_pullup readable
  eventsVia: "local", // "local" (box data CDC) or "dserv" (combined mode)

  async connect(port) {
    const combined = $("viaDserv").checked;
    this.eventsVia = combined ? "dserv" : "local";
    const j = await api("/api/connect", { port, noData: combined });
    openConsoleStream();
    if (combined) {
      const host = $("viaDservHost").value.trim() || "localhost";
      try {
        const dj = await api("/api/dserv/connect", { host, keepSerial: true });
        this.dservHost = dj.host;
        openEventStream();
        conLog(`events via dserv @ ${dj.host} (${(dj.boxes || []).join(", ") || "no live boxes"})`);
      } catch (e) {
        this.eventsVia = "local"; // dserv failed; console still usable, just no events
        conLog("events via dserv failed: " + e.message + " (console still active)");
      }
      return { label: shortPort(port) + " + dserv" };
    }
    if (j.data) openEventStream();
    else if (j.dataError) conLog("data interface: " + j.dataError);
    return { label: shortPort(port) };
  },
  async disconnect() {
    await api("/api/disconnect", {}).catch(() => {});
    if (this.eventsVia === "dserv") await api("/api/dserv/disconnect", {}).catch(() => {});
    this.eventsVia = "local";
  },

  async read() {
    const j = await api("/api/dump");
    const c = parseDump(j.lines);
    // effective identity: `dump` emits only non-defaults (a default-named box
    // has no `name` line), so `show` fills name/fw. But `show` is a second
    // console command and can lose the single-flight race (errExecBusy) if
    // another exec is in flight -- retry once, then keep the PRIOR identity
    // rather than blanking the panel (dump already refreshed pins/labels).
    for (let attempt = 0; attempt < 2; attempt++) {
      try {
        const s = await api("/api/exec", { cmd: "show" });
        const txt = (s.lines || []).join(" ");
        if (!c.name) c.name = txt.match(/name=(\S+)/)?.[1] || "";
        c.fw = txt.match(/fw=(\S+)/)?.[1] || "";
        // build/board/channel: present only on firmware with the extended `show`
        // trailer; older boxes leave these "" and loadShelf falls back to dev.
        c.build = txt.match(/build=(\S+)/)?.[1] || "";
        c.board = txt.match(/board=(\S+)/)?.[1] || "";
        c.channel = txt.match(/channel=(\S+)/)?.[1] || "";
        c.info = [txt.match(/transport=(\w+)/)?.[1], c.fw,
          c.mode && "mode " + c.mode].filter(Boolean).join(" · ");
        break;
      } catch {
        if (attempt === 0) { await new Promise((r) => setTimeout(r, 250)); continue; }
        if (!c.name) c.name = cfg.name;                       // preserve prior identity
        if (!c.info) c.info = cfg.info || (c.mode ? "mode " + c.mode : "");
      }
    }
    return c;
  },

  async applyPin(n, want, p, wantObs, wantSync) {
    const cmds = [];
    if (want.mode !== (p.mode || "off")) cmds.push(`pin ${n} mode ${want.mode}`);
    if (want.label !== (p.label || "")) cmds.push(`label ${n} ${want.label || "off"}`);
    if (want.debounce !== (p.debounce || 0)) cmds.push(`pin ${n} debounce ${want.debounce}`);
    if (want.pulse !== (p.pulse || 0)) cmds.push(`pin ${n} pulse ${want.pulse}`);
    if (want.actlow !== !!p.actlow) cmds.push(`pin ${n} active_low ${want.actlow ? 1 : 0}`);
    if (wantObs !== (cfg.obs === n)) cmds.push(wantObs ? `obs pin ${n}` : "obs off");
    if (wantSync !== (cfg.sync === n)) cmds.push(wantSync ? `sync pin ${n}` : "sync off");
    for (const c of cmds) await execChecked(c);
    return cmds.length;
  },

  async testPulse(n, us) { await execChecked(`do ${n} pulse ${us}`); },
  async setBoxField(field, v) { await execChecked(`${field} ${v}`); }, // name|desc
  async setFeature(name, on) { await execChecked(`${name} enable ${on ? 1 : 0}`); }, // mcp/oled
  async applyGroup(slot, g) {
    if (!g.pins) { await execChecked(`group ${slot} off`); return; }
    await execChecked(`group ${slot} pins ${g.pins}`);
    await execChecked(`group ${slot} label ${g.label || "off"}`);
    await execChecked(`group ${slot} settle ${g.settle || 0}`);
    await execChecked(`group ${slot} quiet ${g.quiet ? 1 : 0}`);
  },
  async applyAinGroup(slot, g) {
    if (!g.channels) { await execChecked(`ain group ${slot} off`); return; }
    await execChecked(`ain group ${slot} channels ${g.channels}`);
    await execChecked(`ain group ${slot} label ${g.label || "off"}`);
    await execChecked(`ain group ${slot} mode ${g.mode || "onchange"}`);
    await execChecked(`ain group ${slot} deadband ${g.deadband || 0}`);
    await execChecked(`ain group ${slot} decimate ${g.decimate || 1}`);
    await execChecked(`ain group ${slot} batch ${g.batch || 1}`);
    await execChecked(`ain group ${slot} average ${g.average ? 1 : 0}`);
  },
  async save() { await execChecked("save"); },
  async reboot() { await execChecked("reboot"); },
};

/* ---- dserv: the datapoint-table view of a rig's boxes ---- */
const DservDriver = {
  id: "dserv",
  hasConsole: false,
  hasProfiles: false,
  fullConfig: false,
  host: "",
  box: "",

  async connect(host) {
    const j = await api("/api/dserv/connect", { host });
    this.host = j.host;
    this.box = j.primary || j.boxes[0] || "";
    setBoxChoices(j.boxes, this.box);
    if (!j.boxes.length)
      conLog("no live boxes on " + host + " (extio/boxes is empty -- box off, or extio subprocess not loaded)");
    openEventStream();
    return { label: (this.box || "?") + "@" + this.host };
  },
  async disconnect() { await api("/api/dserv/disconnect", {}); this.host = this.box = ""; },

  async read() {
    const j = await api("/api/dserv/state?box=" + encodeURIComponent(this.box));
    setBoxChoices(j.boxes, this.box);
    return cfgFromState(this.box, j.state || {});
  },

  set(leaf, value) {
    return api("/api/dserv/set", { key: `extio/${this.box}/${leaf}`, value: String(value) })
      .then(() => conLog(`> %set ${leaf} = ${value}`));
  },

  async setFeature(name, on) { await this.set(`config/${name}/enable`, on ? 1 : 0); }, // mcp/oled
  async applyGroup(slot, g) {
    if (!g.pins) { await this.set(`config/group/${slot}/pins`, "off"); return; }
    await this.set(`config/group/${slot}/pins`, g.pins);
    await this.set(`config/group/${slot}/label`, g.label || "off");
    await this.set(`config/group/${slot}/settle_ms`, g.settle || 0);
    await this.set(`config/group/${slot}/quiet`, g.quiet ? 1 : 0);
  },
  async applyAinGroup(slot, g) {
    if (!g.channels) { await this.set(`config/ain/group/${slot}/off`, 1); return; }
    await this.set(`config/ain/group/${slot}/channels`, g.channels);
    await this.set(`config/ain/group/${slot}/label`, g.label || "off");
    await this.set(`config/ain/group/${slot}/mode`, g.mode || "onchange");
    await this.set(`config/ain/group/${slot}/deadband`, g.deadband || 0);
    await this.set(`config/ain/group/${slot}/decimate`, g.decimate || 1);
    await this.set(`config/ain/group/${slot}/batch`, g.batch || 1);
    await this.set(`config/ain/group/${slot}/average`, g.average ? 1 : 0);
  },

  async applyPin(n, want, p, wantObs, wantSync) {
    const sets = [];
    if (want.mode !== (p.mode || "off")) sets.push([`config/pin/${n}/mode`, want.mode]);
    if (want.label !== (p.label || "")) sets.push([`config/pin/${n}/label`, want.label || "off"]);
    // write-only fields: blank means "leave alone", a value means "set it"
    if (want.debounce) sets.push([`config/pin/${n}/debounce_ms`, want.debounce]);
    if (want.pulse) sets.push([`config/pin/${n}/pulse_us`, want.pulse]);
    if (want.actlow) sets.push([`config/pin/${n}/active_low`, 1]);
    if (wantObs !== (cfg.obs === n)) sets.push(["config/obs/pin", wantObs ? n : "off"]);
    if (wantSync !== (cfg.sync === n)) sets.push(["config/sync/pin", wantSync ? n : "off"]);
    for (const [k, v] of sets) await this.set(k, v);
    return sets.length;
  },

  async testPulse(n, us) { await this.set(`cmd/do/${n}/pulse_us`, us); },
  async setBoxField(field, v) {
    await this.set(`config/${field}`, v); // name|desc
    if (field === "name")
      conLog("renamed: the box re-announces under the new name -- reselect it in the box list");
  },
  async save() { await this.set("cmd/save", 1); conLog("cmd/save sent"); },
  async reboot() { await this.set("cmd/reboot", 1); },

  // Network OTA: no local USB, so kick the box's own A/B updater -- it pulls the
  // image from the shelf, trials it (try-before-you-buy), self-tests, and commits
  // or auto-reverts. The dserv-side extioconf turns this cmd/ota/pull into
  // extio_ota_push_shelf; value = "<channel> ?<version>?" (empty version = latest).
  async ota(channel, version) {
    await this.set("cmd/ota/pull", version ? `${channel} ${version}` : (channel || "dev"));
  },
};

let drv = SerialDriver;
const shortPort = (p) => p.replace("/dev/cu.", "").replace("/dev/", "");

/* ---- dump parsing: mirrors the pico_cli grammar (serial driver) ---- */
function parseDump(lines) {
  const c = emptyCfg();
  c.raw = lines;
  let m;
  for (const raw of lines) {
    const s = raw.trim();
    if (!s || s.startsWith("#")) continue;
    if ((m = s.match(/^name (.+)$/))) c.name = m[1];
    else if ((m = s.match(/^desc (.+)$/))) c.desc = m[1];
    else if ((m = s.match(/^mode (\w+)$/))) c.mode = m[1];
    else if ((m = s.match(/^pin (\d+) mode (\w+)$/))) (c.pins[+m[1]] ??= {}).mode = m[2];
    else if ((m = s.match(/^pin (\d+) pulse (\d+)$/))) (c.pins[+m[1]] ??= {}).pulse = +m[2];
    else if ((m = s.match(/^pin (\d+) debounce (\d+)$/))) (c.pins[+m[1]] ??= {}).debounce = +m[2];
    else if ((m = s.match(/^pin (\d+) active_low 1$/))) (c.pins[+m[1]] ??= {}).actlow = true;
    else if ((m = s.match(/^label (\d+) (.+)$/))) (c.pins[+m[1]] ??= {}).label = m[2];
    else if ((m = s.match(/^group (\d+) pins (\S+)$/))) (c.dgroups[+m[1]] ??= {}).pins = m[2];
    else if ((m = s.match(/^group (\d+) label (\S+)$/))) (c.dgroups[+m[1]] ??= {}).label = m[2];
    else if ((m = s.match(/^group (\d+) settle (\d+)$/))) (c.dgroups[+m[1]] ??= {}).settle = +m[2];
    else if ((m = s.match(/^group (\d+) quiet 1$/))) (c.dgroups[+m[1]] ??= {}).quiet = true;
    else if ((m = s.match(/^ain group (\d+) channels (\S+)$/))) (c.again[+m[1]] ??= {}).channels = m[2];
    else if ((m = s.match(/^ain group (\d+) label (\S+)$/))) (c.again[+m[1]] ??= {}).label = m[2];
    else if ((m = s.match(/^ain group (\d+) mode (\S+)$/))) (c.again[+m[1]] ??= {}).mode = m[2];
    else if ((m = s.match(/^ain group (\d+) deadband (\d+)$/))) (c.again[+m[1]] ??= {}).deadband = +m[2];
    else if ((m = s.match(/^ain group (\d+) decimate (\d+)$/))) (c.again[+m[1]] ??= {}).decimate = +m[2];
    else if ((m = s.match(/^ain group (\d+) batch (\d+)$/))) (c.again[+m[1]] ??= {}).batch = +m[2];
    else if ((m = s.match(/^ain group (\d+) average 1$/))) (c.again[+m[1]] ??= {}).average = true;
    else if ((m = s.match(/^mcp rate (\d+)$/))) c.mcprate = +m[1];
    else if ((m = s.match(/^obs pin (\d+)$/))) c.obs = +m[1];
    else if ((m = s.match(/^sync pin (\d+)$/))) c.sync = +m[1];
    else if (s === "mcp enable 1") c.mcp = true;
    else if (s === "oled enable 1") c.oled = true;
  }
  return c;
}

/* ---- manifest parsing: the box's announced state/* leaves (dserv driver) ---- */
function cfgFromState(box, st) {
  const c = emptyCfg();
  c.name = box;
  c.desc = typeof st["desc"] === "string" ? st["desc"] : "";
  for (const p of String(st["pins/in"] ?? "").split(",").filter(Boolean))
    (c.pins[+p] ??= {}).mode = "in"; // in vs in_pullup not announced
  for (const p of String(st["pins/out"] ?? "").split(",").filter(Boolean))
    (c.pins[+p] ??= {}).mode = "out";
  // Groups are announced by NAME (label, or g<idx>/a<idx> when unlabeled); the
  // slot is announced separately as .../idx so we can map a labeled group back to
  // `group <idx>` / `ain group <idx>` for editing. Collect by name, then re-key.
  let m; const dgByName = {}, ainByName = {};
  for (const [k, v] of Object.entries(st)) {
    if ((m = k.match(/^label\/(\d+)$/)) && v) (c.pins[+m[1]] ??= {}).label = String(v);
    else if ((m = k.match(/^group\/([^/]+)\/([a-z_]+)$/))) {
      const g = (dgByName[m[1]] ??= { name: m[1] });
      if (m[2] === "pins") g.pins = String(v);
      else if (m[2] === "settle_ms") g.settle = +v;
      else if (m[2] === "quiet") g.quiet = +v === 1;
      else if (m[2] === "idx") g.idx = +v;
    } else if ((m = k.match(/^ain\/([^/]+)\/([a-z_]+)$/))) {   // ain/rate has no 3rd seg -> skipped
      const g = (ainByName[m[1]] ??= { name: m[1] });
      if (m[2] === "channels") g.channels = String(v);
      else if (m[2] === "mode") g.mode = String(v);
      else if (m[2] === "decimate") g.decimate = +v;
      else if (m[2] === "batch") g.batch = +v;
      else if (m[2] === "deadband") g.deadband = +v;
      else if (m[2] === "average") g.average = +v === 1;
      else if (m[2] === "idx") g.idx = +v;
    }
  }
  c.mcprate = +st["ain/rate"] || 0;
  const rekey = (byName, dst, pfx) => {
    for (const g of Object.values(byName)) {
      const auto = g.name.match(new RegExp("^" + pfx + "(\\d+)$"));  // g2 / a1 = unlabeled
      const slot = Number.isInteger(g.idx) ? g.idx : (auto ? +auto[1] : null);
      if (slot === null) continue;                    // older fw w/o idx + labeled -> not editable
      dst[slot] = { ...g, label: auto ? "" : g.name };
    }
  };
  rekey(dgByName, c.dgroups, "g");
  rekey(ainByName, c.again, "a");
  if (typeof st["obs_pin"] === "number" && st["obs_pin"] >= 0) c.obs = st["obs_pin"];
  if (typeof st["sync_pin"] === "number" && st["sync_pin"] >= 0) c.sync = st["sync_pin"];
  // analog/display feature flags (state/mcp_en, state/oled_en) -> claimed pins.
  // Absent on boxes predating the manifest change -> stays false (pin map just
  // won't shade them, same as before).
  c.mcp = +st["mcp_en"] === 1;
  c.oled = +st["oled_en"] === 1;
  c.fw = st["fw"] || "";
  // build/board announced today (state/build, state/board); channel added with
  // the firmware channel-policy change -- absent -> loadShelf falls back to dev.
  c.build = st["build"] || "";
  c.board = st["board"] || "";
  c.channel = st["channel"] || "";
  c.info = [st["fw"], st["transport"],
    st["ip"] && st["ip"] !== "0.0.0.0" ? st["ip"] : null,
    st["boot"] ? "boot " + st["boot"] : null].filter(Boolean).join(" · ");
  return c;
}

/* ---- pin map ---- */
function pinRole(n) {
  if (n in FIXED) return { cls: "fixed", note: FIXED[n], locked: true };
  if (cfg.oled && n in OLED_PINS) return { cls: "claimed", note: OLED_PINS[n], locked: true };
  if (cfg.mcp && n in MCP_PINS) return { cls: "claimed", note: MCP_PINS[n], locked: true };
  const lbl = cfg.pins[n]?.label;
  if (n === cfg.obs) return { cls: "special", note: lbl ? lbl + " · obs" : "obs mirror", strong: !!lbl, locked: false };
  if (n === cfg.sync) return { cls: "special", note: lbl ? lbl + " · sync" : "sync input", strong: !!lbl, locked: false };
  const p = cfg.pins[n];
  if (p && (p.mode === "in" || p.mode === "in_pullup"))
    return { cls: "in", note: p.label || p.mode, strong: !!p.label, locked: false };
  if (p && p.mode === "out")
    return { cls: "out", note: p.label || "out", strong: !!p.label, locked: false };
  const extra = n === 25 ? "LED" : (n === 26 || n === 27) ? "free (ADC)" : "free";
  if (p && p.label) // labeled but mode unset: show it, it's configuration
    return { cls: "free", note: p.label + " (no mode)", strong: true, locked: false };
  return { cls: "free", note: extra, locked: false };
}

function renderPins() {
  const el = $("pinmap");
  el.innerHTML = "";
  for (let n = 0; n < NPINS; n++) {
    const r = pinRole(n);
    const div = document.createElement("div");
    div.className = "pin";
    const chip = document.createElement("button");
    chip.type = "button";
    chip.className = "chip " + r.cls + (r.locked ? " dis" : "") + (n === selPin ? " sel" : "")
      + (liveDI.get(n)?.v ? " hi" : "") + (obsLive && n === cfg.obs ? " obslive" : "");
    chip.dataset.pin = n;
    chip.textContent = n;
    chip.title = r.locked ? r.note + " (reserved)" : "configure GPIO " + n;
    chip.setAttribute("aria-label", "GPIO " + n + (r.locked ? " reserved: " + r.note : ""));
    if (!r.locked) chip.onclick = () => selectPin(n);
    else chip.disabled = true;
    const note = document.createElement("span");
    note.className = "note";
    if (r.strong) { const b = document.createElement("b"); b.textContent = r.note; note.append(b); }
    else note.textContent = r.note;
    const c = liveDI.get(n)?.count;
    if (c) note.append(` ×${c}`);
    div.append(chip, note);
    el.append(div);
  }
}

/* ---- box + groups panels ---- */
function renderBox() {
  $("boxname").value = cfg.name;
  $("boxdesc").value = cfg.desc;
  $("boxinfo").textContent = cfg.info || (cfg.mode ? "mode " + cfg.mode : "—");
  // one-line summary (both group kinds); the Groups panel does the editing
  const gs = [];
  for (const [g, v] of Object.entries(cfg.dgroups))
    gs.push(`${v.label || "g" + g}[${v.pins || ""}]`);
  for (const [g, v] of Object.entries(cfg.again))
    gs.push(`${v.label || "a" + g}{${v.channels || ""}}`);
  $("groups").textContent = gs.length ? gs.join("  ") : "—";
  renderMcp();
  renderGroups();
}

/* ---- Analog / MCP3204 panel: enable/disable the SPI ADC (claims GP2-5). Works
 * in both drivers via drv.setFeature (serial: `mcp enable N`; dserv:
 * config/mcp/enable). MCP is compiled into every image, so show when connected.
 * Optimistic: shade the pins immediately; the "save + reboot" note sets the
 * expectation that the claim only takes effect on the next boot. ---- */
function renderMcp() {
  $("mcppanel").hidden = !connected;
  if (!connected) return;
  $("mcpStatus").textContent = cfg.mcp ? "enabled — GP2-5 claimed" : "disabled";
  $("mcpEnable").disabled = cfg.mcp;
  $("mcpDisable").disabled = !cfg.mcp;
}

async function mcpSet(on) {
  try {
    await drv.setFeature("mcp", on);
    cfg.mcp = on;                       // optimistic: reflect in panel + pin map now
    $("mcpMsg").textContent = `${on ? "enabled" : "disabled"} — Save to flash + reboot to apply`;
    renderMcp(); renderPins();
  } catch (e) {
    $("mcpMsg").textContent = "err: " + e.message;
  }
}
$("mcpEnable").onclick = () => mcpSet(true);
$("mcpDisable").onclick = () => mcpSet(false);

/* ---- Groups panel: DI chord groups + analog (MCP3204) channel groups. Both are
 * named member-sets published as ONE atomic datapoint; edited by SLOT (0..N-1),
 * which we map from the manifest's announced idx (labeled groups lose the slot in
 * their name). Shows when connected; the analog section is gated on mcp. A shared
 * editor opens for a chosen (type, slot); .gdi/.gain fields toggle by type. ---- */
const NDGROUPS = 4, NAGROUPS_UI = 4;
let gedit = null;               // { type: 'di'|'ain', slot } or null
let gedMembers = new Set();     // live member selection in the open editor

const groupSummaryDI = (v) =>
  `${v.pins || "—"}${v.settle ? " · settle " + v.settle : ""}${v.quiet ? " · quiet" : ""}`;
function groupSummaryAin(v) {
  const p = [v.mode || "onchange"];
  if ((v.mode || "onchange") === "onchange") { if (v.deadband) p.push("db " + v.deadband); }
  else { if (v.decimate > 1) p.push("dec " + v.decimate); if (v.batch > 1) p.push("batch " + v.batch);
         if (v.average) p.push("avg"); }
  return `ch ${v.channels || "—"} · ${p.join(" · ")}`;
}

function renderGroups() {
  const panel = $("grouppanel");
  if (!connected) { panel.hidden = true; return; }
  panel.hidden = false;
  const dl = $("dgroupList"); dl.innerHTML = "";
  for (let s = 0; s < NDGROUPS; s++) {
    const v = cfg.dgroups[s];
    dl.append(groupRow("di", s, v, v && (v.label || "g" + s), v && groupSummaryDI(v)));
  }
  const showAin = cfg.mcp;
  $("ainGroupHead").hidden = !showAin;
  $("ainGroupList").hidden = !showAin;
  if (showAin) {
    const al = $("ainGroupList"); al.innerHTML = "";
    for (let s = 0; s < NAGROUPS_UI; s++) {
      const v = cfg.again[s];
      al.append(groupRow("ain", s, v, v && (v.label || "a" + s), v && groupSummaryAin(v)));
    }
  }
}

function groupRow(type, slot, v, name, summary) {
  const row = document.createElement("div");
  row.className = "grow" + (gedit && gedit.type === type && gedit.slot === slot ? " sel" : "");
  const nm = document.createElement("span"); nm.className = "gname";
  nm.textContent = v ? name : `slot ${slot}`;
  const meta = document.createElement("span"); meta.className = "gmeta";
  meta.textContent = v ? summary : "empty — click to add";
  row.append(nm, meta);
  row.onclick = () => openGroupEditor(type, slot);
  return row;
}

function openGroupEditor(type, slot) {
  gedit = { type, slot };
  const v = (type === "di" ? cfg.dgroups : cfg.again)[slot] || {};
  $("geditor").hidden = false;
  $("gedTitle").textContent = `${type === "di" ? "Digital" : "Analog"} group ${slot}`;
  $("gedMsg").textContent = "";
  for (const el of document.querySelectorAll(".gdi"))  el.style.display = type === "di"  ? "" : "none";
  for (const el of document.querySelectorAll(".gain")) el.style.display = type === "ain" ? "" : "none";
  $("gedLabel").value = v.label || "";
  if (type === "di") {
    $("gedMemLbl").textContent = "pins";
    $("gedSettle").value = v.settle || "";
    $("gedQuiet").checked = !!v.quiet;
    renderMemberChips("di", v.pins);
  } else {
    $("gedMemLbl").textContent = "channels";
    setGmode(v.mode || "onchange");
    $("gedDead").value = v.deadband ?? "";
    $("gedDec").value = v.decimate || "";
    $("gedBatch").value = v.batch || "";
    $("gedAvg").checked = !!v.average;
    renderMemberChips("ain", v.channels);
  }
  renderGroups();   // re-highlight the selected row
}

function renderMemberChips(type, csv) {
  gedMembers = new Set(String(csv || "").split(",").filter(Boolean).map(Number));
  const box = $("gedMembers"); box.innerHTML = "";
  const items = type === "di"
    ? Object.keys(cfg.pins).map(Number)
        .filter((n) => { const md = cfg.pins[n]?.mode; return md === "in" || md === "in_pullup"; })
        .sort((a, b) => a - b)
    : [0, 1, 2, 3];
  if (type === "di" && !items.length) {
    box.className = "chips hint";
    box.textContent = "no input pins — set some GPIO to input first";
    return;
  }
  box.className = "chips";
  for (const n of items) {
    const c = document.createElement("span");
    c.className = "mchip" + (gedMembers.has(n) ? " on" : "");
    c.textContent = type === "di" ? (cfg.pins[n]?.label ? `${n}·${cfg.pins[n].label}` : `${n}`) : `ch${n}`;
    c.onclick = () => { gedMembers.has(n) ? gedMembers.delete(n) : gedMembers.add(n); c.classList.toggle("on"); };
    box.append(c);
  }
}

function setGmode(mode) {
  for (const b of $("gedModeBtns").children) b.classList.toggle("active", b.dataset.gmode === mode);
}
$("gedModeBtns").onclick = (e) => { if (e.target.dataset.gmode) setGmode(e.target.dataset.gmode); };

function closeGroupEditor() { gedit = null; $("geditor").hidden = true; renderGroups(); }

async function applyGroupEdit() {
  if (!gedit) return;
  const { type, slot } = gedit;
  const members = [...gedMembers].sort((a, b) => a - b).join(",");
  if (!members) { $("gedMsg").textContent = "pick at least one member (or Delete)"; return; }
  try {
    if (type === "di") {
      await drv.applyGroup(slot, { pins: members, label: $("gedLabel").value.trim(),
        settle: +$("gedSettle").value || 0, quiet: $("gedQuiet").checked });
    } else {
      const mode = [...$("gedModeBtns").children].find((b) => b.classList.contains("active"))?.dataset.gmode || "onchange";
      await drv.applyAinGroup(slot, { channels: members, label: $("gedLabel").value.trim(), mode,
        deadband: +$("gedDead").value || 0, decimate: +$("gedDec").value || 1,
        batch: +$("gedBatch").value || 1, average: $("gedAvg").checked });
    }
    $("gedMsg").textContent = "applied — Save to flash to persist";
    scheduleReload();
  } catch (e) { $("gedMsg").textContent = "err: " + e.message; }
}

async function deleteGroupEdit() {
  if (!gedit) return;
  const { type, slot } = gedit;
  try {
    if (type === "di") await drv.applyGroup(slot, {});     // no pins -> off
    else               await drv.applyAinGroup(slot, {});  // no channels -> off
    closeGroupEditor(); scheduleReload();
  } catch (e) { $("gedMsg").textContent = "err: " + e.message; }
}

$("gedApply").onclick = applyGroupEdit;
$("gedOff").onclick = deleteGroupEdit;
$("gedCancel").onclick = closeGroupEditor;

/* ---- LAN discovery: list Ethernet boxes broadcasting their UDP beacon (the Go
 * server collects them on :5011). Independent of connect state — this is how you
 * FIND a fresh box before adopting it. Default-named boxes show "(unnamed)", so
 * the IP + board/fw disambiguate. Polls every ~2.5s. ---- */
async function refreshDiscovery() {
  try { const j = await api("/api/discover"); renderDiscovery(j.boxes || [], j.enabled); }
  catch { /* transient — the next tick retries */ }
}
function renderDiscovery(boxes, enabled) {
  const hint = $("discohint"), list = $("discolist");
  if (enabled === false) { hint.textContent = "off — UDP :5011 unavailable"; list.innerHTML = ""; return; }
  boxes.sort((a, b) => String(a.ip).localeCompare(String(b.ip), undefined, { numeric: true }));
  hint.textContent = boxes.length ? `${boxes.length} on the LAN` : "listening…";
  list.innerHTML = "";
  for (const b of boxes) {
    const row = document.createElement("div");
    row.className = "grow" + (daBox === b.ip ? " sel" : "");
    const nm = document.createElement("span"); nm.className = "gname";
    const named = b.name && b.name !== "pico";
    nm.textContent = named ? b.name : "(unnamed)";
    const meta = document.createElement("span"); meta.className = "gmeta";
    meta.textContent = `${b.ip} · ${b.board || "?"}/${b.build || "?"} · fw ${b.fw || "?"}`;
    const badge = document.createElement("span");
    badge.className = "badge " + (b.configured ? "on" : "off");
    badge.textContent = b.configured ? "configured" : "unconfigured";
    row.append(nm, meta, badge);
    row.onclick = () => openAssign(b);
    list.append(row);
  }
}

/* ---- adopt a discovered box: point it at a rig's dserv + save, pushed straight
 * to the box's config server (:5010) via /api/discover/assign. ---- */
let daBox = null;   // ip of the box the assign form is open for
function openAssign(b) {
  daBox = b.ip;
  $("discoAssign").hidden = false;
  $("daBox").textContent = `${b.name || "pico"} @ ${b.ip}`;
  $("daMsg").textContent = "";
  // sensible default: if the page was loaded from a rig by IP, target that host
  if (!$("daIP").value) {
    const h = location.hostname;
    if (/^\d+\.\d+\.\d+\.\d+$/.test(h)) $("daIP").value = h;
  }
  $("discoAssign")._box = b;
  refreshDiscovery();   // highlight the selected row
  $("daIP").focus();
}
function closeAssign() { daBox = null; $("discoAssign").hidden = true; refreshDiscovery(); }
$("daCancel").onclick = closeAssign;
$("daAssign").onclick = async () => {
  const b = $("discoAssign")._box; if (!b) return;
  const dservIP = $("daIP").value.trim();
  const dservPort = +$("daPort").value || 4620;
  if (!/^\d+\.\d+\.\d+\.\d+$/.test(dservIP)) { $("daMsg").textContent = "enter the rig's dserv IP"; return; }
  $("daMsg").textContent = "assigning…";
  try {
    const j = await api("/api/discover/assign", { ip: b.ip, name: b.name || "pico", dservIP, dservPort });
    $("daMsg").textContent = `assigned → ${j.assigned}. The box will connect & appear in dserv mode.`;
    setTimeout(refreshDiscovery, 1500);   // beacon target flips to configured shortly
  } catch (e) { $("daMsg").textContent = "err: " + e.message; }
};

setInterval(refreshDiscovery, 2500);
refreshDiscovery();

/* ---- Handheld / BLE panel (dserv mode: pairing is a rig operation, and the
 * state/ble/* telemetry only exists over dserv). Every action is a config/cmd
 * datapoint; status comes from state/ble/{bonds,encrypted,pairing}. ---- */
function renderBleFrom(st) {
  const panel = $("blepanel");
  // This panel is the receiver's (BLE CENTRAL) pairing control. A handheld is a
  // peripheral -- it transports over the radio (transport=ble) and publishes no
  // state/ble/* -- so never show pairing there. A radio-capable board on a wired
  // transport (usb/eth) is a receiver, even before BLE has been enabled.
  const has = Object.keys(st).some((k) => k.startsWith("ble/"));
  const peripheral = String(st["transport"] || "") === "ble";
  const radioBoard = /pico2_w|thingplus/.test(String(st["board"] || ""));
  if (drv.id !== "dserv" || peripheral || (!has && !radioBoard)) { panel.hidden = true; return; }
  panel.hidden = false;

  const bonds = st["ble/bonds"], enc = +st["ble/encrypted"], pairing = +st["ble/pairing"] || 0;
  let status;
  if (!has) status = "radio off — Enable relay to bring it up";
  else if (pairing > 0) status = `pairing window open — ${pairing}s left`;
  else if (enc === 1) status = "handheld bonded + encrypted";
  else if (+bonds > 0) status = `${bonds} bond(s) — waiting for the handheld`;
  else status = "radio up — no bond yet";
  $("bleStatus").textContent = status;
  $("bleBonds").textContent = has ? `${bonds ?? 0}${enc === 1 ? " · encrypted" : ""}` : "—";
  const pairing0 = pairing === 0;
  $("blePair").textContent = pairing0 ? "Pair handheld" : `Pairing… ${pairing}s`;
  $("blePair").classList.toggle("primary", pairing0);
  $("blePair").disabled = !pairing0;
}

async function refreshBle() {
  if (drv.id !== "dserv" || !connected) { $("blepanel").hidden = true; return; }
  try {
    const j = await api("/api/dserv/state?box=" + encodeURIComponent(drv.box));
    renderBleFrom(j.state || {});
  } catch { /* transient; the poll retries */ }
}

async function bleSet(leaf, value, msg) {
  try {
    await drv.set(leaf, value);
    if (msg) $("bleMsg").textContent = msg;
  } catch (e) {
    $("bleMsg").textContent = "err: " + e.message;
  }
}

$("bleEnable").onclick = async () => {
  await bleSet("config/ble/enable", 1);
  await bleSet("config/ble/pipe", 1, "radio + relay enabled — Save to flash to persist");
  setTimeout(refreshBle, 900);
};
$("blePair").onclick = async () => {
  const secs = Math.max(1, Math.min(300, parseInt($("blePairSecs").value, 10) || 60));
  await bleSet("cmd/ble/pair", secs, `pairing window open ${secs}s — bring the handheld in range`);
  refreshBle();
};
$("bleForget").onclick = async () => {
  if (!confirm("Forget all bonded handhelds? Each will need to be paired again.")) return;
  await bleSet("cmd/ble/forget", 1, "bonds cleared");
  setTimeout(refreshBle, 900);
};
// keep the panel (countdown / bonds / encrypted) live while it's visible
setInterval(() => { if (!$("blepanel").hidden) refreshBle(); }, 1500);

/* ---- pin editor ---- */
function selectPin(n) {
  selPin = n;
  const p = pin(n);
  $("editor").hidden = false;
  $("pinno").textContent = n;
  $("pinrole").textContent = n === cfg.obs ? "(obs mirror)" : n === cfg.sync ? "(sync input)" : "";
  for (const b of $("modebtns").children)
    b.classList.toggle("active", (p.mode || "off") === b.dataset.mode);
  $("pinlabel").value = p.label || "";
  // write-only over dserv: leave blank = untouched (placeholder explains)
  $("pindebounce").value = p.debounce || "";
  $("pinpulse").value = p.pulse || "";
  $("pinactlow").checked = !!p.actlow;
  $("pindebounce").placeholder = drv.fullConfig ? "" : "n/a";
  $("pinpulse").placeholder = drv.fullConfig ? "" : "n/a";
  $("pinobs").checked = cfg.obs === n;
  $("pinsync").checked = cfg.sync === n;
  $("pintest").disabled = p.mode !== "out";
  $("pinmsg").textContent = "";
  $("drvhint").hidden = drv.fullConfig;
  renderPins();
}

$("modebtns").onclick = (e) => {
  if (e.target.dataset.mode === undefined) return;
  for (const b of $("modebtns").children) b.classList.toggle("active", b === e.target);
};

async function execChecked(cmd) {
  const j = await api("/api/exec", { cmd });
  const resp = (j.lines || []).join(" / ");
  conLog("> " + cmd + "  → " + resp);
  if (resp.startsWith("ERR")) throw new Error(resp);
  return resp;
}

$("pinapply").onclick = async () => {
  if (selPin === null) return;
  const n = selPin, p = pin(n);
  const want = {
    mode: [...$("modebtns").children].find((b) => b.classList.contains("active"))?.dataset.mode || "off",
    label: $("pinlabel").value.trim(),
    debounce: +$("pindebounce").value || 0,
    pulse: +$("pinpulse").value || 0,
    actlow: $("pinactlow").checked,
  };
  try {
    const sent = await drv.applyPin(n, want, p, $("pinobs").checked, $("pinsync").checked);
    if (!sent) { $("pinmsg").textContent = "no changes"; return; }
    $("pinmsg").textContent = "applied (unsaved until 'Save to flash')";
    await reload(n);
    // Over dserv the box re-announces its pin state asynchronously (a round-trip
    // the immediate reload above races), so schedule a catch-up read to pick up
    // the settled pins/in + labels. Serial mode's dump is already authoritative.
    if (drv.id === "dserv") scheduleReload(700);
  } catch (e) { $("pinmsg").textContent = e.message; }
};

$("pintest").onclick = async () => {
  if (selPin === null) return;
  const us = pin(selPin).pulse || 100000;
  try { await drv.testPulse(selPin, us); $("pinmsg").textContent = `pulsed ${us}µs`; }
  catch (e) { $("pinmsg").textContent = e.message; }
};

document.querySelectorAll("[data-cmdfrom]").forEach((b) => {
  b.onclick = async () => {
    const v = $(b.dataset.cmdfrom).value.trim();
    if (!v) return;
    try { await drv.setBoxField(b.dataset.verb, v); await reload(selPin); }
    catch (e) { conLog(e.message); }
  };
});

/* ---- profiles (serial only: they ride the dump format) ---- */
$("profsave").onclick = async () => {
  try {
    const j = await api("/api/dump");
    const text = j.lines.join("\n") + "\n";
    const name = (cfg.name || "extio") + "-profile.txt";
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([text], { type: "text/plain" }));
    a.download = name;
    a.click();
    URL.revokeObjectURL(a.href);
    $("profmsg").textContent = "saved " + name;
  } catch (e) { $("profmsg").textContent = e.message; }
};

$("profapply").onclick = () => $("proffile").click();
$("proffile").onchange = async () => {
  const f = $("proffile").files[0];
  if (!f) return;
  const text = await f.text();
  const lines = text.split(/\r?\n/);
  const hasName = lines.some((l) => /^name /.test(l.trim()));
  let newName = "";
  if (hasName) {
    newName = prompt("Profile sets a box name. New name for THIS box (empty = keep profile's):", "") || "";
  }
  $("profmsg").textContent = "applying " + f.name + "…";
  try {
    const j = await api("/api/apply", { lines, newName });
    const bad = j.results.filter((r) => !r.ok);
    $("profmsg").textContent = bad.length
      ? `stopped at "${bad[0].cmd}": ${bad[0].resp}`
      : `applied ${j.results.length} commands from ${f.name} (includes save)`;
    await reload(selPin);
  } catch (e) { $("profmsg").textContent = e.message; }
  $("proffile").value = "";
};

/* ---- firmware ---- */
async function loadFirmware() {
  try {
    const j = await api("/api/firmware");
    const sel = $("fwfiles");
    sel.innerHTML = "";
    for (const f of j.files) {
      const o = document.createElement("option");
      o.value = f.name;
      o.textContent = `${f.name} (${(f.size / 1024).toFixed(0)}k)`;
      sel.append(o);
    }
    $("flash").disabled = !j.files.length;
    if (!j.files.length) $("flashmsg").textContent = j.dir ? "no .uf2 in " + j.dir : "no firmware dir (-fw)";
  } catch (e) { $("flashmsg").textContent = e.message; }
}

$("flash").onclick = async () => {
  const file = $("fwfiles").value;
  if (!file) return;
  // Local flash is a USB mass-storage copy -- it can't reach a box over the
  // network. Over dserv, send the user to the shelf OTA below instead of the
  // old "a board already in BOOTSEL" dead-end.
  if (drv.id === "dserv") {
    $("flashmsg").textContent = "local flash needs a serial (USB) connection — use “flash from shelf” below for an over-the-air update";
    return;
  }
  const target = (connected && drv.id === "serial")
    ? `the connected box (${cfg.name || "unnamed"})` : "a board already in BOOTSEL";
  if (!confirm(`Flash ${file} to ${target}?`)) return;
  $("flash").disabled = true;
  $("flashmsg").textContent = "flashing… (box reboots; watch for re-enumeration)";
  try {
    const j = await api("/api/flash", { file });
    $("flashmsg").textContent = (j.ok ? "done: " : "FAILED: ") + j.steps.join(" → ") + (j.error ? " — " + j.error : "");
    await refreshPorts();
  } catch (e) { $("flashmsg").textContent = e.message; }
  $("flash").disabled = false;
};

/* ---- firmware shelf (pull from dserv.net) ---- */
// reload() calls loadShelf() on every config change, so cache the network
// fetch briefly; the box-fw compare below is recomputed cheaply each time.
// Cache is keyed by channel too -- a different box (or channel change) refetches.
let _shelf = { t: 0, ch: null, st: null, j: null };
async function loadShelf() {
  const wrap = $("shelfwrap"), msg = $("shelfmsg");
  try {
    // The box tells us which channel to track (state/channel or `show`); default
    // dev for boxes predating the firmware channel field.
    const ch = cfg.channel || "dev";
    if (Date.now() - _shelf.t > 30000 || !_shelf.j || _shelf.ch !== ch) {
      _shelf.st = await api("/api/status");
      _shelf.j = _shelf.st.shelf ? await api("/api/shelf?channel=" + encodeURIComponent(ch)) : null;
      _shelf.ch = ch;
      _shelf.t = Date.now();
    }
    const st = _shelf.st, j = _shelf.j;
    if (!st.shelf || !j) { wrap.style.display = "none"; return; } // -shelf="" disables
    const sel = $("shelffiles");
    sel.innerHTML = "";
    // Flatten versions × images, newest first (server already sorts versions).
    const allRows = [];
    for (const v of (j.versions || []))
      for (const img of (v.images || []))
        allRows.push({ version: v.version, file: img.file, build: img.build, board: img.board || "",
                       dirty: v.dirty, ota: !!img.otaCapable, bin: img.bin || "" });

    // Default view: only images for THIS box's build line (BOX_BUILD_TARGET), so
    // the dropdown is "updates for what I'm running", not every board's image.
    // "all" checkbox (or an unknown build, e.g. old fw / no box) drops the filter.
    const boxBuild = cfg.build || "";
    const showAll = $("shelfall").checked || !boxBuild;
    const compatible = (r) => r.build === boxBuild &&
      (!r.board || !cfg.board || r.board === cfg.board);
    const rows = showAll ? allRows : allRows.filter(compatible);

    // The box's update line: newest version among compatible images, regardless of
    // the display filter -- the compare is always about the running build.
    const lineRows = boxBuild ? allRows.filter(compatible) : allRows;
    const lineLatest = lineRows.length ? lineRows[0].version : (j.latest || "");
    const boxFw = cfg.fw || "";

    // Recommend the newest displayed image that's an actual upgrade (and, over
    // dserv, OTA-capable so the box can pull it). Falls back to newest displayed.
    let recommended = null;
    for (const r of rows) {
      if (boxFw && cmpFw(r.version, boxFw) <= 0) continue;   // not an upgrade
      if (drv.id === "dserv" && (!r.ota || !r.bin)) continue; // OTA needs a flat .bin
      recommended = r; break;
    }
    for (const r of rows) {
      const o = document.createElement("option");
      o.value = JSON.stringify({ channel: ch, version: r.version, file: r.file,
                                 build: r.build, ota: r.ota, bin: r.bin });
      o.textContent = `${r.build} · ${r.version}${r.dirty ? " (dirty)" : ""}${r.ota ? " · OTA" : ""}`;
      if (recommended && r === recommended) o.selected = true;
      sel.append(o);
    }
    $("shelfflash").disabled = !rows.length;
    // Over dserv it's an over-the-air update (box pulls it itself); over serial
    // it's a local download + BOOTSEL flash.
    $("shelfflash").textContent = drv.id === "dserv" ? "OTA…" : "Flash…";
    wrap.style.display = "";
    // Host-side "update available": box fw vs its line's latest on this channel.
    // Compare by VERSION (leading X.Y.Z + git-describe commit count), not raw
    // inequality -- else a box that's NEWER than the shelf (e.g. an image staged
    // straight to the slot, or a stale shelf) reads as "update available" and would
    // silently downgrade.
    const line = boxBuild ? `${ch}/${lineLatest} (${boxBuild})` : `${ch}/${lineLatest}`;
    if (boxFw && lineLatest) {
      const cmp = cmpFw(lineLatest, boxFw);   // >0 shelf newer, <0 box newer, 0 same
      if (cmp > 0)      msg.textContent = `update available: ${line} — box has ${boxFw}`;
      else if (cmp < 0) msg.textContent = `box is NEWER than shelf: ${boxFw} (shelf latest ${line})`;
      else              msg.textContent = `box matches shelf ${line}`;
    } else {
      msg.textContent = rows.length ? `${rows.length} image(s) on ${st.shelf} (${ch})` : `no images for ${ch}`;
    }
  } catch (e) {
    wrap.style.display = "none";
    msg.textContent = "shelf: " + e.message;
  }
}

// Toggling "all" only changes the display filter -- re-render from cache, no refetch.
$("shelfall").onchange = () => loadShelf().catch(() => {});

$("shelfflash").onclick = async () => {
  const raw = $("shelffiles").value;
  if (!raw) return;
  const spec = JSON.parse(raw);

  // dserv (network) mode: there's no local USB, so trigger the box's OWN A/B OTA
  // -- it pulls this image from the shelf, trials it, self-tests, and commits or
  // auto-reverts. (The local mass-storage flash below only works over serial.)
  if (drv.id === "dserv") {
    const box = drv.box;
    if (!box) { $("shelfmsg").textContent = "select a box first"; return; }
    if (!spec.ota || !spec.bin) {
      $("shelfmsg").textContent = `${spec.version} has no OTA image for over-the-air update (bench-flash only) — pick one marked “OTA”`;
      return;
    }
    if (!confirm(`OTA ${box} to ${spec.version} (${spec.build})?\n\nThe box pulls it from the shelf, boots it as a trial, self-tests (transport + dserv + registration), then commits — or auto-reverts to the current image if the trial can't check in.`)) return;
    $("shelfflash").disabled = true;
    $("shelfmsg").textContent = `OTA ${box}: triggering shelf pull of ${spec.version}…`;
    try {
      await drv.ota(spec.channel, spec.version);
      $("shelfmsg").textContent = `OTA ${box}: box pulling ${spec.version} — watch the OTA state (trial boot ~30 s, then it reconnects on the new image, or reverts)`;
    } catch (e) { $("shelfmsg").textContent = "OTA trigger failed: " + e.message; }
    $("shelfflash").disabled = false;
    return;
  }

  const target = (connected && drv.id === "serial")
    ? `the connected box (${cfg.name || "unnamed"})` : "a board already in BOOTSEL";
  if (!confirm(`Fetch ${spec.file} (${spec.version}) from dserv.net and flash it to ${target}?`)) return;
  $("shelfflash").disabled = true;
  $("shelfmsg").textContent = "fetching + verifying sha256, then flashing…";
  try {
    const j = await api("/api/shelf/flash", spec);
    $("shelfmsg").textContent = (j.ok ? "done: " : "FAILED: ") + j.steps.join(" → ") + (j.error ? " — " + j.error : "");
    await refreshPorts();
  } catch (e) { $("shelfmsg").textContent = e.message; }
  $("shelfflash").disabled = false;
};

/* ---- console ---- */
function conLog(s) {
  const c = $("console");
  const div = document.createElement("div");
  if (/ERR|FAILED|timeout|refus/.test(s)) div.className = "err";
  else if (/^OK|→ OK/.test(s)) div.className = "ok";
  div.textContent = s;
  c.append(div);
  while (c.childElementCount > 800) c.firstChild.remove();
  c.scrollTop = c.scrollHeight;
}

function openConsoleStream() {
  if (es) es.close();
  es = new EventSource("/api/console");
  es.onmessage = (e) => {
    conLog(e.data);
    // Any successful command -- typed here, in picocom, or by a profile --
    // may have changed config: converge the UI on the box's truth.
    if (/^OK /.test(e.data)) scheduleReload();
  };
  es.onerror = () => { es.close(); es = null; };
}

/* Reflect one pin's live state in place: solid green while active, a pop
 * on every edge, and a running press count on the label. */
function updateChipLive(pin, blink = true) {
  const chip = document.querySelector(`.chip[data-pin="${pin}"]`);
  if (!chip) return;
  const s = liveDI.get(pin);
  chip.classList.toggle("hi", !!(s && s.v));
  if (blink) {
    chip.classList.remove("blink");
    void chip.offsetWidth; // restart the animation
    chip.classList.add("blink");
  }
  const note = chip.parentElement.querySelector(".note");
  if (note && s && s.count) {
    const r = pinRole(pin);
    note.innerHTML = "";
    const b = document.createElement("b");
    b.textContent = r.note;
    note.append(b, ` ×${s.count}`);
  }
}

/* Reflect the box's live obs state: a header badge (always, even with no obs
 * pin configured) plus a pulse on the obs-mirror chip. */
function setObsLive(on) {
  if (on === obsLive) return;
  obsLive = on;
  const badge = $("obs");
  badge.textContent = on ? "◉ obs" : "";
  badge.className = "badge" + (on ? " obson" : "");
  if (cfg.obs != null) {
    const chip = document.querySelector(`.chip[data-pin="${cfg.obs}"]`);
    if (chip) chip.classList.toggle("obslive", on);
  }
}

/* ---- live events (serial data CDC or dserv push -- same frames) ---- */

/* Manifest leaves: a change means the box's announced config moved
 * (a %set we sent, another UI, or a reflash) -- re-read and re-render. */
const MANIFEST_LEAF = /^(pins\/|label\/|desc$|obs_pin$|sync_pin$|group\/[^/]+\/(pins|settle_ms)$)/;

/* Which box's events to show. dserv mode: the picked box. Combined mode: the
 * box the console is plugged into (cfg.name). Pure serial: null -- the local
 * data CDC only ever carries this one box. */
function activeEventBox() {
  if (drv.id === "dserv") return drv.box;
  if (drv.id === "serial" && SerialDriver.eventsVia === "dserv") return cfg.name || null;
  return null;
}

function openEventStream() {
  if (esd) esd.close();
  esd = new EventSource("/api/events");
  esd.onmessage = (e) => {
    let ev;
    try { ev = JSON.parse(e.data); } catch { return; }
    // obs marker: the canonical event-driven signal (ess/obs_active, set by the
    // BEGINOBS/ENDOBS triggers) -- not per-box, so handle before the extio regex.
    if (ev.name === "ess/obs_active") {
      setObsLive(!!(+ev.val));
      if (!ev.snap) conLog(`⚡ obs_active = ${ev.val}`);
      return;
    }
    const m0 = ev.name.match(/^extio\/([^/]+)\/state\/(.+)$/);
    if (!m0) return; // extio/boxes, decoded/*, ...: not per-box state
    // a dserv rig can host several boxes; show only the active one
    const box = activeEventBox();
    if (box && m0[1] !== box) return;
    const leaf = m0[2];
    let m;
    if ((m = leaf.match(/^d[io]\/(\d+)$/))) {
      const pin = +m[1], v = +ev.val || 0;
      const s = liveDI.get(pin) || { v: 0, count: 0 };
      s.v = v;
      if (v && !ev.snap) s.count++; // snapshots repaint state, they aren't presses
      liveDI.set(pin, s);
      updateChipLive(pin, !ev.snap);
      if (!ev.snap) conLog(`⚡ ${leaf} = ${ev.val}`);
    } else if (/^group\/[^/]+$/.test(leaf) || /^timer\//.test(leaf)) {
      conLog(`⚡ ${leaf} = ${ev.val}`);
    } else if (/watchdog|heartbeat/.test(leaf)) {
      beats++; lastBeat = Date.now();
      $("beat").textContent = "♥ " + beats;
    } else if (!ev.snap && MANIFEST_LEAF.test(leaf) &&
               (drv.id === "dserv" || SerialDriver.eventsVia === "dserv")) {
      scheduleReload(500); // manifest re-announce: converge on the box's truth
    }
    // everything else (telemetry, sync) stays off the console
  };
  // Do NOT close on error: EventSource auto-reconnects, and the server
  // replays a di/do snapshot on every (re)subscribe, self-healing any pin
  // state that went stale during the gap. Just show the stream is down.
  esd.onerror = () => { $("beat").textContent = "♥ …"; };
}

$("coninput").addEventListener("keydown", async (e) => {
  if (e.key !== "Enter") return;
  const cmd = $("coninput").value.trim();
  if (!cmd) return;
  $("coninput").value = "";
  try { await api("/api/consolewrite", { data: cmd + "\n" }); }
  catch (err) { conLog(err.message); }
});

/* ---- connection lifecycle ---- */
async function refreshPorts() {
  const ports = await api("/api/ports").catch(() => []);
  const sel = $("ports");
  const prev = sel.value;
  sel.innerHTML = "";
  for (const p of ports) {
    const o = document.createElement("option");
    o.value = p.console;
    o.textContent = shortPort(p.console);
    sel.append(o);
  }
  if (!ports.length) {
    const o = document.createElement("option");
    o.value = "";
    o.textContent = "no extio box found";
    sel.append(o);
  } else if ([...sel.options].some((o) => o.value === prev)) sel.value = prev;
  return ports;
}

function setBoxChoices(boxes, keep) {
  const sel = $("dboxes");
  const cur = keep ?? sel.value;
  const have = [...sel.options].map((o) => o.value);
  if (have.join("\x00") !== boxes.join("\x00")) {
    sel.innerHTML = "";
    for (const b of boxes) {
      const o = document.createElement("option");
      o.value = o.textContent = b;
      sel.append(o);
    }
  }
  if (boxes.includes(cur)) sel.value = cur;
  sel.hidden = !boxes.length;
}

async function reload(reselect = null) {
  cfg = await drv.read();
  renderPins();
  renderBox();
  refreshBle(); // Handheld/BLE panel (dserv only; hides itself otherwise)
  if (reselect !== null) selectPin(reselect);
  loadShelf(); // refresh the "update available" compare against the box's fw
}

function setConnected(on, label) {
  connected = on;
  $("status").className = "badge " + (on ? "on" : "off");
  $("status").textContent = on ? label : "no box";
  $("connect").textContent = on ? "Disconnect" : "Connect";
  for (const id of ["refresh", "save", "reboot"]) $(id).disabled = !on;
  for (const id of ["profsave", "profapply"]) $(id).disabled = !on || !drv.hasProfiles;
  $("coninput").disabled = !on || !drv.hasConsole;
  $("conhint").textContent = !on ? "connect a box to interact"
    : drv.hasConsole ? "" : "live events only (the CLI console needs the USB serial driver)";
  if (!on) {
    $("editor").hidden = true; $("blepanel").hidden = true; $("mcppanel").hidden = true;
    $("grouppanel").hidden = true; $("geditor").hidden = true; gedit = null;
    selPin = null; cfg = emptyCfg();
    liveDI.clear(); beats = 0; $("beat").textContent = "";
    setObsLive(false);
    SerialDriver.eventsVia = "local"; // next connect starts clean
    if (esd) { esd.close(); esd = null; }
    if (es) { es.close(); es = null; }
    renderPins(); renderBox();
  }
}

/* mode picker: which driver the Connect button drives */
function applyMode(m) {
  drv = m === "dserv" ? DservDriver : SerialDriver;
  $("mode").value = m;
  $("serialctl").hidden = m !== "serial";
  $("dservctl").hidden = m !== "dserv";
}

$("mode").onchange = async () => {
  if (connected) { // switching modes ends the current session
    try { await drv.disconnect(); } catch { /* server may already be down */ }
    setConnected(false);
  }
  applyMode($("mode").value);
};

$("dboxes").onchange = async () => {
  if (drv.id !== "dserv" || !connected) return;
  drv.box = $("dboxes").value;
  liveDI.clear(); beats = 0; setObsLive(false); // per-box live state
  $("status").textContent = drv.box + "@" + drv.host;
  openEventStream(); // resubscribe: snapshot repaints the new box's pins
  await reload().catch((e) => conLog(e.message));
};

$("connect").onclick = async () => {
  if (connected) {
    await drv.disconnect().catch(() => {});
    setConnected(false);
    return;
  }
  try {
    if (drv.id === "serial") {
      const port = $("ports").value;
      if (!port) return;
      const info = await drv.connect(port);
      setConnected(true, info.label);
    } else {
      const host = $("dhost").value.trim();
      if (!host) return;
      const info = await drv.connect(host);
      setConnected(true, info.label);
    }
    await reload();
  } catch (e) { conLog("connect: " + e.message); setConnected(false); }
};

$("rescan").onclick = refreshPorts;
$("refresh").onclick = () => reload(selPin).catch((e) => conLog(e.message));
$("save").onclick = () => drv.save().catch((e) => conLog(e.message));
$("reboot").onclick = async () => {
  if (!confirm("Reboot the box?")) return;
  try { await drv.reboot(); } catch (e) { conLog(e.message); }
  // Serial console can't survive the reboot's re-enumeration -> tear down (and
  // the dserv side too, in combined mode). Pure dserv mode rides it out.
  if (drv.id === "serial") { await drv.disconnect().catch(() => {}); setConnected(false); }
};

/* "events via dserv" toggle: reveal the host box; a live session must
 * reconnect to change its event source, so nudge if flipped while connected. */
$("viaDserv").onchange = () => {
  $("viaDservHost").hidden = !$("viaDserv").checked;
  if (connected && drv.id === "serial")
    conLog("reconnect to " + ($("viaDserv").checked ? "switch events to dserv" : "use the box's data CDC"));
};

/* Poll status so an unplug / dserv drop flips the UI on its own. */
let lastDataNote = "";
setInterval(async () => {
  try {
    const st = await api("/api/status");
    if (st.dataNote && st.dataNote !== lastDataNote) {
      lastDataNote = st.dataNote;
      conLog("data interface: " + st.dataNote);
    }
    const combined = drv.id === "serial" && SerialDriver.eventsVia === "dserv";
    if (connected && drv.id === "serial" && !st.connected) {
      conLog("(box disconnected)");
      if (combined) await drv.disconnect().catch(() => {}); // tear down the dserv side too
      setConnected(false);
    }
    if (connected && drv.id === "dserv") {
      if (st.mode !== "dserv") { conLog("(dserv session ended)"); setConnected(false); return; }
      setBoxChoices(st.dserv.boxes, drv.box); // boxes come and go with their watchdogs
      if (st.dserv.pushes === 0) $("beat").textContent = "♥ re-reg…"; // self-heal in progress
    }
    // Resurrect a dead event stream (browsers kill EventSource on 4xx, e.g.
    // a retry that landed mid-flash); the snapshot replay repairs pin state.
    const haveSource = drv.id === "dserv" ? st.mode === "dserv" : combined ? !!st.dserv : st.data;
    if (connected && haveSource && (!esd || esd.readyState === 2)) openEventStream();
  } catch { /* server gone; leave UI as-is */ }
}, 2000);

/* ---- boot ---- */
(async () => {
  renderPins();
  renderBox();
  loadFirmware();
  loadShelf();
  const ports = await refreshPorts();
  const st = await api("/api/status").catch(() => ({}));
  if (st.mode === "dserv") { // server already holds a dserv session (page reload)
    applyMode("dserv");
    DservDriver.host = st.dserv.host;
    DservDriver.box = st.dserv.primary || st.dserv.boxes[0] || "";
    setBoxChoices(st.dserv.boxes, DservDriver.box);
    $("dhost").value = st.dserv.host;
    setConnected(true, (DservDriver.box || "?") + "@" + DservDriver.host);
    openEventStream();
    reload().catch(() => {});
  } else if (st.connected) {
    applyMode("serial");
    openConsoleStream();
    if (st.dserv) { // combined session (console + dserv events) survived a reload
      SerialDriver.eventsVia = "dserv";
      SerialDriver.dservHost = st.dserv.host;
      $("viaDserv").checked = true;
      $("viaDservHost").value = st.dserv.host;
      $("viaDservHost").hidden = false;
      setConnected(true, shortPort(st.port) + " + dserv");
      openEventStream();
    } else {
      setConnected(true, shortPort(st.port));
      if (st.data) openEventStream();
    }
    reload().catch(() => {});
  } else {
    applyMode("serial");
    // Smart default: if a dserv is answering locally, this is a rig where its
    // usbio owns the box's data CDC -- so take events over dserv (combined
    // mode) instead of colliding on the data port. On a bare bench box (no
    // dserv) stay plain serial. This also steers the single-box auto-connect.
    try {
      const p = await api("/api/dserv/probe");
      if (p.available) {
        $("viaDserv").checked = true;
        $("viaDservHost").hidden = false;
        conLog("dserv detected -- events will come via dserv (uncheck 'events via dserv' for a bare bench box)");
      }
    } catch { /* no dserv reachable: plain serial is right */ }
    if (ports.length === 1) $("connect").click(); // exactly one box: connect on load
  }
})();
