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
const api = async (path, body) => {
  const opts = body === undefined ? {} :
    { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) };
  const r = await fetch(path, opts);
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error || r.statusText);
  return j;
};

/* Board map: W6300-EVB-Pico2 (dual image) from wiznet-io/PINMAP.md.
 * "claimed" pins depend on live config (ain/oled) -- resolved in render.
 * TODO: replace with a reserved-mask announced by the firmware manifest. */
const NPINS = 29;
const FIXED = { 0:"UART0 TX", 1:"UART0 RX", 15:"W6300", 16:"W6300", 17:"W6300",
  18:"W6300", 19:"W6300", 20:"W6300", 21:"W6300", 22:"W6300",
  23:"SMPS", 24:"VBUS", 28:"mode strap" };
const OLED_PINS = { 2:"OLED CLK", 3:"OLED DATA", 6:"OLED CS", 7:"OLED DC", 8:"OLED RST" };
const AIN_PINS = { 4:"I2C SDA", 5:"I2C SCL" };

let cfg = emptyCfg();
let selPin = null;
let connected = false;
let es = null;  // console EventSource
let esd = null; // data-events EventSource
let reloadT = null;      // debounce for config re-reads (console OKs, manifest frames)
const liveDI = new Map(); // pin -> {v, count}: live di/do state from the event stream
let beats = 0, lastBeat = 0;

function emptyCfg() {
  return { name: "", desc: "", mode: "", obs: null, sync: null,
    ain: false, oled: false, pins: {}, groups: {}, raw: [] };
}
const pin = (n) => (cfg.pins[n] ??= { mode: "", pulse: 0, debounce: 0, actlow: false, label: "" });

function scheduleReload(ms = 700) {
  clearTimeout(reloadT);
  reloadT = setTimeout(() => reload(selPin).catch(() => {}), ms);
}

/* ================= drivers ================= */

/* ---- serial: wraps the /api console+dump surface (v1 behavior, unchanged) ---- */
const SerialDriver = {
  id: "serial",
  hasConsole: true,   // interactive CLI pane
  hasProfiles: true,  // dump-based save/apply
  fullConfig: true,   // pulse/debounce/active_low/in_pullup readable

  async connect(port) {
    const j = await api("/api/connect", { port });
    openConsoleStream();
    if (j.data) openEventStream();
    else if (j.dataError) conLog("data interface: " + j.dataError);
    return { label: shortPort(port) };
  },
  async disconnect() { await api("/api/disconnect", {}); },

  async read() {
    const j = await api("/api/dump");
    const c = parseDump(j.lines);
    try { // effective identity: dump emits only non-defaults, show has the rest
      const s = await api("/api/exec", { cmd: "show" });
      const txt = (s.lines || []).join(" ");
      if (!c.name) c.name = txt.match(/name=(\S+)/)?.[1] || "";
      c.info = [txt.match(/transport=(\w+)/)?.[1], txt.match(/fw=(\S+)/)?.[1],
        c.mode && "mode " + c.mode].filter(Boolean).join(" · ");
    } catch { /* show is cosmetic; dump already succeeded */ }
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
    else if ((m = s.match(/^group (\d+) pins (\S+)$/))) (c.groups[+m[1]] ??= {}).pins = m[2];
    else if ((m = s.match(/^group (\d+) label (\S+)$/))) (c.groups[+m[1]] ??= {}).label = m[2];
    else if ((m = s.match(/^group (\d+) settle (\d+)$/))) (c.groups[+m[1]] ??= {}).settle = +m[2];
    else if ((m = s.match(/^group (\d+) quiet 1$/))) (c.groups[+m[1]] ??= {}).quiet = true;
    else if ((m = s.match(/^obs pin (\d+)$/))) c.obs = +m[1];
    else if ((m = s.match(/^sync pin (\d+)$/))) c.sync = +m[1];
    else if (s === "ain enable 1") c.ain = true;
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
  let m;
  for (const [k, v] of Object.entries(st)) {
    if ((m = k.match(/^label\/(\d+)$/)) && v) (c.pins[+m[1]] ??= {}).label = String(v);
    else if ((m = k.match(/^group\/([^/]+)\/pins$/))) (c.groups[m[1]] ??= {}).pins = String(v);
    else if ((m = k.match(/^group\/([^/]+)\/settle_ms$/))) (c.groups[m[1]] ??= {}).settle = +v;
  }
  if (typeof st["obs_pin"] === "number" && st["obs_pin"] >= 0) c.obs = st["obs_pin"];
  if (typeof st["sync_pin"] === "number" && st["sync_pin"] >= 0) c.sync = st["sync_pin"];
  c.info = [st["fw"], st["transport"],
    st["ip"] && st["ip"] !== "0.0.0.0" ? st["ip"] : null,
    st["boot"] ? "boot " + st["boot"] : null].filter(Boolean).join(" · ");
  return c;
}

/* ---- pin map ---- */
function pinRole(n) {
  if (n in FIXED) return { cls: "fixed", note: FIXED[n], locked: true };
  if (cfg.oled && n in OLED_PINS) return { cls: "claimed", note: OLED_PINS[n], locked: true };
  if (cfg.ain && n in AIN_PINS) return { cls: "claimed", note: AIN_PINS[n], locked: true };
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
      + (liveDI.get(n)?.v ? " hi" : "");
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
  // groups: serial keys are indexes with a label field; dserv keys ARE the label
  const gs = Object.entries(cfg.groups).map(([g, v]) => {
    const name = v.label || (/^\d+$/.test(g) ? "g" + g : g);
    return `${name}[${v.pins || ""}]${v.settle ? " settle=" + v.settle : ""}`;
  });
  $("groups").textContent = gs.length ? gs.join("  ") : "—";
}

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

/* ---- live events (serial data CDC or dserv push -- same frames) ---- */

/* Manifest leaves: a change means the box's announced config moved
 * (a %set we sent, another UI, or a reflash) -- re-read and re-render. */
const MANIFEST_LEAF = /^(pins\/|label\/|desc$|obs_pin$|sync_pin$|group\/[^/]+\/(pins|settle_ms)$)/;

function openEventStream() {
  if (esd) esd.close();
  esd = new EventSource("/api/events");
  esd.onmessage = (e) => {
    let ev;
    try { ev = JSON.parse(e.data); } catch { return; }
    const m0 = ev.name.match(/^extio\/([^/]+)\/state\/(.+)$/);
    if (!m0) return; // extio/boxes, decoded/*, ...: not per-box state
    // a dserv rig can host several boxes; show only the selected one
    if (drv.id === "dserv" && drv.box && m0[1] !== drv.box) return;
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
    } else if (drv.id === "dserv" && !ev.snap && MANIFEST_LEAF.test(leaf)) {
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
  if (reselect !== null) selectPin(reselect);
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
    $("editor").hidden = true; selPin = null; cfg = emptyCfg();
    liveDI.clear(); beats = 0; $("beat").textContent = "";
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
  liveDI.clear(); beats = 0; // per-box live state
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
  if (drv.id === "serial") setConnected(false); // dserv session survives a box reboot
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
    if (connected && drv.id === "serial" && !st.connected) {
      conLog("(box disconnected)");
      setConnected(false);
    }
    if (connected && drv.id === "dserv") {
      if (st.mode !== "dserv") { conLog("(dserv session ended)"); setConnected(false); return; }
      setBoxChoices(st.dserv.boxes, drv.box); // boxes come and go with their watchdogs
      if (st.dserv.pushes === 0) $("beat").textContent = "♥ re-reg…"; // self-heal in progress
    }
    // Resurrect a dead event stream (browsers kill EventSource on 4xx, e.g.
    // a retry that landed mid-flash); the snapshot replay repairs pin state.
    const haveSource = drv.id === "serial" ? st.data : st.mode === "dserv";
    if (connected && haveSource && (!esd || esd.readyState === 2)) openEventStream();
  } catch { /* server gone; leave UI as-is */ }
}, 2000);

/* ---- boot ---- */
(async () => {
  renderPins();
  renderBox();
  loadFirmware();
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
    setConnected(true, shortPort(st.port));
    openConsoleStream();
    if (st.data) openEventStream();
    reload().catch(() => {});
  } else {
    applyMode("serial");
    if (ports.length === 1) $("connect").click(); // exactly one box: connect on load
  }
})();
