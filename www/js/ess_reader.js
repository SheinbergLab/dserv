/* Read one trial out of an .ess (a dslog) in the browser.
   The page already plays a script, a stimdg, and a list of replay steps.
   This produces those three from the file the user picked. */
(function (root) {
  const BYTE = 0, STRING = 1, FLOAT = 2, DOUBLE = 3, SHORT = 4, INT = 5;
  const DG = 6, SCRIPT = 7, EVT = 9, JSON = 11, INT64 = 16;
  const FRAME_MS = 1000 / 60;

  function asBytes(input) {
    if (input instanceof ArrayBuffer) return new Uint8Array(input);
    if (ArrayBuffer.isView(input)) {
      return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
    }
    throw new Error("ess is not a file");
  }

  function f32(n) {
    const a = new Float32Array(1);
    a[0] = n;
    return a[0];
  }

  function cString(bytes, start, len) {
    let end = start + len;
    for (let i = start; i < start + len && i < bytes.length; i++) {
      if (bytes[i] === 0) {
        end = i;
        break;
      }
    }
    return new TextDecoder("utf-8").decode(bytes.subarray(start, end));
  }

  function textPayload(payload) {
    return cString(payload, 0, payload.length);
  }

  function firstNumber(type, payload) {
    if (!payload.length) return null;
    const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
    if (type === FLOAT && payload.length >= 4) return view.getFloat32(0, true);
    if (type === DOUBLE && payload.length >= 8) return view.getFloat64(0, true);
    if (type === INT && payload.length >= 4) return view.getInt32(0, true);
    if (type === SHORT && payload.length >= 2) return view.getInt16(0, true);
    if (type === INT64 && payload.length >= 8) return Number(view.getBigInt64(0, true));
    if (type === BYTE) return payload[0];
    return null;
  }

  function decodeValue(type, payload) {
    if (type === STRING || type === JSON || type === SCRIPT) {
      return { kind: "text", text: textPayload(payload) };
    }
    if (type === DG || type === BYTE) {
      return { kind: "bytes", bytes: payload.slice() };
    }
    const n = firstNumber(type, payload);
    if (n != null && Number.isFinite(n)) return { kind: "num", n: n };
    return null;
  }

  function readEss(input) {
    const bytes = asBytes(input);
    if (bytes.length < 16) throw new Error("not an ess file");
    if (bytes[0] !== 100 || bytes[1] !== 115 || bytes[2] !== 108 ||
        bytes[3] !== 111 || bytes[4] !== 103) {
      throw new Error("not an ess file");
    }
    if (!bytes[5]) throw new Error("ess file has no version");
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const headerUs = view.getBigUint64(8, true);
    const begins = [];
    const ends = [];
    const inbox = [];
    const stimdg = [];
    const scripts = [];
    const paths = [];
    const screens = {
      "ess/screen_w": [],
      "ess/screen_h": [],
      "ess/screen_halfx": [],
      "ess/screen_halfy": [],
      "ess/screen_refresh_rate": []
    };
    const backgrounds = [];
    const extractedTexts = [];
    const touches = [];
    const rawEye = [];
    const eyeSources = [];
    const eyeSettings = [];
    let subjectName = "";
    let variantLine = "";
    let openTime = 0;
    let sawInbox = false;
    let o = 16;
    while (o + 2 <= bytes.length) {
      const nameLen = view.getUint16(o, true);
      o += 2;
      if (o + nameLen + 8 + 4 + 4 + 4 > bytes.length) break;
      let name = cString(bytes, o, nameLen);
      o += nameLen;
      const pointUs = view.getBigUint64(o, true);
      o += 8;
      o += 4;
      const dtype = bytes[o];
      const evType = bytes[o + 1];
      const evSubtype = bytes[o + 2];
      const puttype = bytes[o + 3];
      o += 4;
      const len = view.getUint32(o, true);
      o += 4;
      if (len < 0 || o + len > bytes.length) throw new Error("ess record is truncated");
      const payload = bytes.subarray(o, o + len);
      o += len;
      const t = f32(Number(pointUs - headerUs) / 1e6);
      if (dtype === EVT) {
        name = "evt:" + evType + ":" + evSubtype;
        if (evType === 19) begins.push(t);
        else if (evType === 20) ends.push(t);
        else if (evType === 8 && evSubtype === 0 && !openTime) {
          const opened = eventNumber(puttype, payload);
          if (opened > 0) openTime = opened;
        } else if (evType === 18 && evSubtype === 1 && !subjectName) {
          const value = decodeValue(puttype, payload);
          if (value && value.kind === "text" && value.text.trim()) subjectName = value.text.trim();
        } else if (evType === 18 && evSubtype === 3 && !variantLine) {
          const value = decodeValue(puttype, payload);
          if (value && value.kind === "text" && value.text.trim()) variantLine = value.text.trim();
        } else if (evType === 48 && (evSubtype === 0 || evSubtype === 1)) {
          const nums = numberList(puttype, payload);
          if (nums && nums.length >= 2 && Number.isFinite(nums[0]) && Number.isFinite(nums[1])) {
            touches.push({
              t: t,
              kind: evSubtype === 0 ? "press" : "release",
              x: nums[0],
              y: nums[1]
            });
          }
        }
        continue;
      }
      if (name === "stim/inbox") {
        sawInbox = true;
        const value = decodeValue(dtype, payload);
        if (value && value.kind === "text") inbox.push({ t: t, text: value.text });
        continue;
      }
      if (name === "stimdg") {
        const value = decodeValue(dtype === EVT ? puttype : dtype, payload);
        if (value && value.kind === "bytes") stimdg.push({ t: t, bytes: value.bytes });
        continue;
      }
      if (name === "ess/stim_script_body") {
        const value = decodeValue(dtype, payload);
        if (value && value.kind === "text" && value.text) scripts.push({ t: t, text: value.text });
        continue;
      }
      if (name === "ess/stim_script_path") {
        const value = decodeValue(dtype, payload);
        if (value && value.kind === "text" && value.text) paths.push({ t: t, text: value.text });
        continue;
      }
      if (screens[name]) {
        const value = decodeValue(dtype, payload);
        if (value) screens[name].push({ t: t, value: value });
        continue;
      }
      if (name === "ess/stim_background") {
        const value = decodeValue(dtype, payload);
        if (value) backgrounds.push({ t: t, value: value });
        continue;
      }
      if (name === "ess/extracted") {
        const value = decodeValue(dtype, payload);
        if (value && value.kind === "text" && value.text) extractedTexts.push(value.text);
        continue;
      }
      if (name === "eyetracking/raw") {
        const nums = numberList(dtype, payload);
        if (nums && nums.length >= 2) rawEye.push({ t: t, floats: nums });
        continue;
      }
      if (name === "em/source_active") {
        const value = decodeValue(dtype, payload);
        const word = value && value.kind === "text" ? value.text.trim().split(/\s+/)[0] : "";
        if (word === "video" || word === "analog" || word === "virtual") {
          eyeSources.push({ t: t, word: word });
        }
        continue;
      }
      if (name === "em/settings") {
        const value = decodeValue(dtype, payload);
        if (value && value.kind === "text" && value.text) eyeSettings.push({ t: t, text: value.text });
      }
    }
    if (!sawInbox) throw new Error("no stim/inbox tape in this file");
    rawEye.sort(byTime);
    eyeSources.sort(byTime);
    eyeSettings.sort(byTime);
    const kind = eyeKind(rawEye, eyeSources);
    const layers = [];
    if (touches.length) layers.push("touch");
    if (kind) layers.push("eye");
    const halfx = latestPositive(screens["ess/screen_halfx"], Infinity);
    const halfy = latestPositive(screens["ess/screen_halfy"], Infinity);
    return {
      begins: begins,
      ends: ends,
      attempts: ends,
      inbox: inbox,
      stimdg: stimdg,
      scripts: scripts,
      paths: paths,
      screens: screens,
      backgrounds: backgrounds,
      extracted: lastExtracted(extractedTexts),
      touches: touches,
      rawEye: rawEye,
      eyeSources: eyeSources,
      eyeSettings: eyeSettings,
      layers: layers,
      eyeVirtual: kind === "virtual",
      halfx: halfx ? halfx.n : null,
      halfy: halfy ? halfy.n : null,
      subject: subjectName,
      variantLine: variantLine,
      openTime: openTime
    };
  }

  function eventNumber(type, payload) {
    const n = firstNumber(type, payload);
    if (n != null && Number.isFinite(n)) return n;
    const value = decodeValue(type, payload);
    if (!value || value.kind !== "text") return 0;
    const parsed = Number(value.text.trim());
    return Number.isFinite(parsed) ? parsed : 0;
  }

  function catalogRow(ess, filename) {
    const parts = String(ess.variantLine || "").split(":");
    let system = "";
    let protocol = "";
    let variant = "";
    if (parts.length >= 3) {
      system = parts[0];
      protocol = parts[1];
      variant = parts[2];
    } else if (parts.length === 2) {
      system = parts[0];
      protocol = parts[1];
    }
    let date = "";
    let time = "";
    const timestamp = ess.openTime || 0;
    if (timestamp > 0) {
      const when = new Date(timestamp * 1000);
      const pad = function (n) { return String(n).padStart(2, "0"); };
      date = when.getFullYear() + "-" + pad(when.getMonth() + 1) + "-" + pad(when.getDate());
      time = pad(when.getHours()) + ":" + pad(when.getMinutes()) + ":" + pad(when.getSeconds());
    }
    return {
      filename: filename,
      subject: ess.subject || "",
      system: system,
      protocol: protocol,
      variant: variant,
      date: date,
      time: time,
      timestamp: timestamp,
      n_obs: ess.attempts.length,
      ess: ess
    };
  }

  function byTime(a, b) {
    return a.t < b.t ? -1 : a.t > b.t ? 1 : 0;
  }

  function numberList(type, payload) {
    if (!payload || !payload.length) return null;
    const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
    const out = [];
    if (type === FLOAT) {
      const n = Math.floor(payload.length / 4);
      for (let i = 0; i < n; i++) out.push(view.getFloat32(i * 4, true));
      return out;
    }
    if (type === DOUBLE) {
      const n = Math.floor(payload.length / 8);
      for (let i = 0; i < n; i++) out.push(view.getFloat64(i * 8, true));
      return out;
    }
    return null;
  }

  function eyeKind(raws, sources) {
    if (!raws.length || !sources.length) return "";
    let kind = "";
    for (let i = 0; i < raws.length; i++) {
      const src = valueAt(sources, raws[i].t);
      const word = src ? src.word : "";
      if (word !== "video" && word !== "analog" && word !== "virtual") continue;
      if (word === "virtual") {
        if (kind === "") kind = "virtual";
      } else {
        kind = "eye";
      }
    }
    return kind;
  }

  function extractedMap(text) {
    let obj;
    try {
      obj = globalThis.JSON.parse(text);
    } catch (e) {
      return null;
    }
    if (!obj || typeof obj !== "object" || Array.isArray(obj)) return null;
    const map = {};
    const keys = Object.keys(obj);
    for (let i = 0; i < keys.length; i++) {
      const span = obj[keys[i]];
      if (!Array.isArray(span) || span.length < 2) return null;
      const lo = Number(span[0]);
      const hi = Number(span[1]);
      if (!Number.isInteger(lo) || !Number.isInteger(hi)) return null;
      map[keys[i]] = [lo, hi];
    }
    return map;
  }

  function lastExtracted(texts) {
    for (let i = texts.length - 1; i >= 0; i--) {
      const map = extractedMap(texts[i]);
      if (map) return map;
    }
    return null;
  }

  function latest(rows, end) {
    let best = null;
    let bestT = -Infinity;
    for (let i = 0; i < rows.length; i++) {
      const row = rows[i];
      if (row.t <= end && row.t >= bestT) {
        best = row;
        bestT = row.t;
      }
    }
    return best;
  }

  function positiveOf(value) {
    if (!value) return null;
    if (value.kind === "num" && value.n > 0) return { n: value.n, text: String(value.n) };
    if (value.kind === "text") {
      const raw = value.text.trim().split(/\s+/)[0];
      const n = Number(raw);
      if (raw && Number.isFinite(n) && n > 0) return { n: n, text: raw };
    }
    return null;
  }

  function latestPositive(rows, end) {
    let best = null;
    let bestT = -Infinity;
    for (let i = 0; i < rows.length; i++) {
      if (rows[i].t > end || rows[i].t < bestT) continue;
      const got = positiveOf(rows[i].value);
      if (!got) continue;
      best = got;
      bestT = rows[i].t;
    }
    return best;
  }

  function tclWords(text) {
    const out = [];
    let i = 0;
    const n = text.length;
    while (i < n) {
      while (i < n && /\s/.test(text[i])) i++;
      if (i >= n) break;
      if (text[i] === "{") {
        let depth = 1;
        let j = i + 1;
        let word = "";
        while (j < n && depth > 0) {
          const c = text[j];
          if (c === "{") {
            depth++;
            word += c;
          } else if (c === "}") {
            depth--;
            if (depth > 0) word += c;
          } else {
            word += c;
          }
          j++;
        }
        out.push(word);
        i = j;
        continue;
      }
      let j = i;
      while (j < n && !/\s/.test(text[j])) j++;
      out.push(text.slice(i, j));
      i = j;
    }
    return out;
  }

  function bracesBalanced(text) {
    let depth = 0;
    for (let i = 0; i < text.length; i++) {
      if (text[i] === "{") depth++;
      else if (text[i] === "}") {
        depth--;
        if (depth < 0) return false;
      }
    }
    return depth === 0;
  }

  function tclWord(text) {
    const s = String(text);
    if (s === "") return "{}";
    if (/^[^\s{}\\\[\]$"';]+$/.test(s)) return s;
    if (s.indexOf("\\") < 0 && bracesBalanced(s)) return "{" + s + "}";
    let out = "";
    for (let i = 0; i < s.length; i++) {
      const c = s[i];
      if (c === " " || c === "\t" || c === "{" || c === "}" || c === "\\" ||
          c === "$" || c === "[" || c === "]" || c === ";" || c === "\"") {
        out += "\\" + c;
      } else {
        out += c;
      }
    }
    return out;
  }

  function tclList(words) {
    return words.map(tclWord).join(" ");
  }

  function parseInbox(text) {
    const nl = text.indexOf("\n");
    if (nl < 0) throw new Error("stim/inbox record has no body");
    const header = tclWords(text.slice(0, nl));
    if (header.length < 6) throw new Error("bad stim/inbox header");
    const nbytes = Number(header[5]);
    if (!Number.isInteger(nbytes) || nbytes < 0) throw new Error("bad stim/inbox nbytes");
    const body = text.slice(nl + 1, nl + 1 + nbytes);
    if (body.length !== nbytes) throw new Error("stim/inbox record is short");
    return {
      seq: Number(header[0]),
      kind: header[1],
      ticks: Number(header[2]),
      timef: Number(header[3]),
      body: body
    };
  }

  function storedBackground(value) {
    let parts = [];
    if (value.kind === "text") {
      const words = tclWords(value.text.trim());
      parts = words.length === 1 ? tclWords(words[0]) : words;
    } else if (value.kind === "num") {
      return null;
    }
    if (parts.length !== 3) return null;
    const rgb = [];
    for (let i = 0; i < 3; i++) {
      const n = Number(parts[i]);
      if (!Number.isInteger(n) || n < 0 || n > 255) return null;
      rgb.push(n);
    }
    return rgb;
  }

  function commandBackground(kind, body) {
    if (kind !== "cmd") return null;
    const words = tclWords(body);
    if (words.length !== 4 || words[0] !== "setBackground") return null;
    const rgb = [];
    for (let i = 1; i < 4; i++) {
      const n = Number(words[i]);
      if (!Number.isInteger(n) || n < 0 || n > 255) return null;
      rgb.push(n);
    }
    return rgb;
  }

  function recordLine(rec) {
    if (rec.kind === "cmd") {
      if (rec.body.startsWith("omitted ")) return null;
      const word = tclWords(rec.body)[0];
      return { line: rec.body, code: word === "show_response" ? 2 : 1 };
    }
    if (rec.kind === "ds") {
      if (rec.body === "stimdg" || rec.body.startsWith("stimdg ")) return null;
      return { line: tclList(["_tape_ds", rec.body]), code: 1 };
    }
    return null;
  }

  function beginLogger(begins, start, end) {
    let found = null;
    for (let i = 0; i < begins.length; i++) {
      if (begins[i] > start && begins[i] <= end) found = begins[i];
    }
    return found;
  }

  function ticksAt(pairs, loggerT) {
    const n = pairs.length;
    if (!n) throw new Error("no clock");
    if (n === 1) return pairs[0].ticks + (loggerT - pairs[0].t) * 1000;
    let lo = 0;
    for (let i = 1; i < n; i++) {
      if (pairs[i].t <= loggerT) lo = i;
      else break;
    }
    let a;
    let b;
    if (lo === 0 && loggerT < pairs[0].t) {
      a = 0;
      b = 1;
    } else if (lo >= n - 1) {
      a = n - 2;
      b = n - 1;
    } else {
      a = lo;
      b = lo + 1;
    }
    const dt = pairs[b].t - pairs[a].t;
    if (Math.abs(dt) < 1e-12) return pairs[a].ticks;
    return pairs[a].ticks + (loggerT - pairs[a].t) * (pairs[b].ticks - pairs[a].ticks) / dt;
  }

  function clockStepPins(base, baseTicks, from, to, stepMs) {
    const pins = [];
    let n = 0;
    let T = from;
    for (;;) {
      n += 1;
      const next = from + n * stepMs;
      if (next >= to - 0.5) {
        if (to > T + 0.01) pins.push(base + (to - baseTicks));
        return pins;
      }
      pins.push(base + (next - baseTicks));
      T = next;
    }
  }

  function buildSteps(recs, bgMissing, bg, stepMs, beginT, end) {
    const tFirst = recs[0].ticks;
    const tSwap = recs[recs.length - 1].ticks;
    const pairs = recs.map(function (rec) { return { t: rec.t, ticks: rec.ticks }; });
    pairs.sort(function (a, b) { return a.t < b.t ? -1 : a.t > b.t ? 1 : 0; });
    let tEnd = ticksAt(pairs, end);
    if (tEnd < tSwap) tEnd = tSwap;
    let tBeginTick = tFirst;
    if (beginT != null) {
      const at = ticksAt(pairs, beginT);
      if (at < tSwap) tBeginTick = at;
    }
    const t0 = tBeginTick;
    const t1 = tSwap;
    const times = [t0];
    let nstep = 0;
    let Tcur = t0;
    for (;;) {
      nstep += 1;
      const next = t0 + nstep * stepMs;
      if (next >= t1 - 0.5) {
        if (t1 > Tcur + 0.01) times.push(t1);
        break;
      }
      times.push(next);
      Tcur = next;
    }
    const nNative = times.length;
    let nOut = Math.ceil((tEnd - t0) / FRAME_MS) + 1;
    if (nOut < 1) nOut = 1;
    const reps = [];
    let outK = 0;
    for (let i = 0; i < nNative; i++) {
      const limit = i + 1 < nNative ? times[i + 1] : t1 + FRAME_MS;
      let nrep = 0;
      while (outK < nOut) {
        const tk = t0 + outK * FRAME_MS;
        if (i + 1 < nNative && tk >= limit) break;
        nrep += 1;
        outK += 1;
      }
      reps.push(nrep);
    }

    const steps = [];
    function addTcl(text, extra) {
      const step = { tcl: text };
      if (extra) {
        step.pin = extra.pin;
        step.clock = extra.clock;
        step.mark = extra.mark;
      }
      steps.push(step);
    }
    function addTime(pin, clock, mark) {
      addTcl("stimTimeSet " + pin, { pin: String(pin), clock: clock, mark: mark });
    }
    addTcl("stimPlayback 1");
    addTcl(bgMissing ? "setBackground 0 0 0" : tclList(["setBackground"].concat(bg)));

    let base = recs[0].timef;
    let baseTicks = recs[0].ticks;
    if (recs[0].kind === "reset") base = 0;
    let showTape = "";
    let showPin = "";
    let idx = 0;
    let stepped = null;
    while (idx < recs.length) {
      const rec = recs[idx];
      if (rec.ticks >= t0) break;
      const rt = rec.ticks;
      if (stepped != null && rt > stepped + 0.01) {
        const pins = clockStepPins(base, baseTicks, stepped, rt, stepMs);
        for (let p = 0; p < pins.length; p++) addTime(pins[p], 0, 0);
      }
      baseTicks = rt;
      base = rec.kind === "reset" ? 0 : rec.timef;
      const line = recordLine(rec);
      if (line) addTcl(line.line);
      addTime(base + (rt - baseTicks), 0, 0);
      stepped = rt;
      idx += 1;
    }
    if (stepped != null && t0 > stepped + 0.01) {
      const pins = clockStepPins(base, baseTicks, stepped, t0, stepMs);
      for (let p = 0; p < pins.length; p++) addTime(pins[p], 0, 0);
    }
    for (let fi = 0; fi < nNative; fi++) {
      const T = times[fi];
      let marked = 0;
      while (idx < recs.length) {
        const rec = recs[idx];
        if (rec.ticks > T) break;
        baseTicks = rec.ticks;
        base = rec.kind === "reset" ? 0 : rec.timef;
        const line = recordLine(rec);
        if (line) addTcl(line.line);
        if (line && line.code === 2) {
          marked = 1;
          showTape = String(rec.timef);
        }
        idx += 1;
      }
      const pin = base + (T - baseTicks);
      if (marked) showPin = String(pin);
      addTcl("set ::tape_mark " + marked);
      addTime(pin, 1, marked);
      steps.push({ capture: reps[fi] });
    }
    return {
      steps: steps,
      show_pin: showPin,
      show_tape: showTape,
      frames: nNative,
      step_ms: stepMs
    };
  }

  function readEssTrial(ess, trial) {
    const n = ess.ends.length;
    if (!Number.isInteger(trial) || trial < 0 || trial >= n) {
      throw new Error("trial " + trial + " outside 0.." + (n - 1));
    }
    const start = trial > 0 ? ess.ends[trial - 1] : -1e300;
    const end = ess.ends[trial];
    const beginT = beginLogger(ess.begins, start, end);
    const recs = [];
    for (let i = 0; i < ess.inbox.length; i++) {
      const row = ess.inbox[i];
      if (row.t > start && row.t <= end) {
        const rec = parseInbox(row.text);
        rec.t = row.t;
        recs.push(rec);
      }
    }
    if (!recs.length) throw new Error("trial " + trial + " has no stim/inbox records");
    recs.sort(function (a, b) { return a.seq - b.seq; });

    let nextT = end;
    for (let i = 0; i < recs.length; i++) {
      if (recs[i].kind === "cmd" && tclWords(recs[i].body)[0] === "nexttrial") {
        nextT = recs[i].t;
        break;
      }
    }
    let dg = null;
    let dgT = -Infinity;
    for (let i = 0; i < ess.stimdg.length; i++) {
      if (ess.stimdg[i].t >= dgT) {
        dg = ess.stimdg[i];
        dgT = ess.stimdg[i].t;
      }
    }
    if (!dg || dgT > nextT) {
      dg = null;
      dgT = -Infinity;
      for (let i = 0; i < ess.stimdg.length; i++) {
        if (ess.stimdg[i].t <= nextT && ess.stimdg[i].t >= dgT) {
          dg = ess.stimdg[i];
          dgT = ess.stimdg[i].t;
        }
      }
    }
    if (!dg) throw new Error("no stimdg at or before trial " + trial);

    const script = latest(ess.scripts, end);
    const path = latest(ess.paths, end);
    if (!script && !path) throw new Error("tape has no stim script");
    if (!script && path) {
      throw new Error("stim script is not in the file: " + path.text);
    }

    const need = [];
    for (let i = 0; i < recs.length; i++) {
      if (recs[i].kind !== "ds") continue;
      const body = recs[i].body;
      if (body === "stimdg" || body.startsWith("stimdg ")) continue;
      const pname = tclWords(body)[0];
      if (need.indexOf(pname) < 0) need.push(pname);
    }

    let firstT = null;
    for (let i = 0; i < recs.length; i++) {
      if (firstT == null || recs[i].t < firstT) firstT = recs[i].t;
    }
    const limit = beginT == null ? firstT : beginT;
    let bg = null;
    let bgT = -Infinity;
    for (let i = 0; i < ess.inbox.length; i++) {
      const row = ess.inbox[i];
      if (row.t > limit || row.t < bgT) continue;
      const rec = parseInbox(row.text);
      const rgb = commandBackground(rec.kind, rec.body);
      if (rgb) {
        bg = rgb;
        bgT = row.t;
      }
    }
    if (!bg) {
      for (let i = 0; i < ess.backgrounds.length; i++) {
        const row = ess.backgrounds[i];
        if (row.t > limit || row.t < bgT) continue;
        const rgb = storedBackground(row.value);
        if (rgb) {
          bg = rgb;
          bgT = row.t;
        }
      }
    }
    const bgMissing = bg ? 0 : 1;

    const width = latestPositive(ess.screens["ess/screen_w"], end);
    const height = latestPositive(ess.screens["ess/screen_h"], end);
    const halfx = latestPositive(ess.screens["ess/screen_halfx"], end);
    const halfy = latestPositive(ess.screens["ess/screen_halfy"], end);
    const refresh = latestPositive(ess.screens["ess/screen_refresh_rate"], end);
    if (!width || !height || !halfx || !halfy) {
      throw new Error("ess has no screen width or height");
    }
    const refreshN = refresh ? refresh.n : 60;
    const stepMs = 1000 / refreshN;
    const built = buildSteps(recs, bgMissing, bg || [0, 0, 0], stepMs, beginT, end);
    return {
      stimText: script.text,
      stimdg: dg.bytes,
      replay: {
        width: Math.trunc(width.n),
        height: Math.trunc(height.n),
        halfx: halfx.text,
        halfy: halfy.text,
        refresh: refresh ? refresh.text : "60",
        background: bgMissing ? [0, 0, 0] : bg,
        background_missing: bgMissing,
        need: need,
        step_ms: built.step_ms,
        frames: built.frames,
        show_pin: built.show_pin,
        show_tape: built.show_tape,
        steps: built.steps
      }
    };
  }

  function valueAt(rows, t) {
    let found = null;
    for (let i = 0; i < rows.length; i++) {
      if (rows[i].t <= t) found = rows[i];
      else break;
    }
    return found;
  }

  function tclTrue(text) {
    if (text == null) return false;
    const raw = String(text).trim();
    if (raw === "") return false;
    const n = Number(raw);
    if (Number.isFinite(n)) return n !== 0;
    const low = raw.toLowerCase();
    return low === "true" || low === "yes" || low === "on";
  }

  function settingsDict(text) {
    let words = tclWords(String(text).trim());
    if (words.length === 1) words = tclWords(words[0]);
    const dict = {};
    for (let i = 0; i + 1 < words.length; i += 2) dict[words[i]] = words[i + 1];
    return dict;
  }

  function coeffList(text) {
    if (text == null) return null;
    const parts = String(text).trim().split(/\s+/);
    if (parts.length !== 9) return null;
    const nums = [];
    let any = false;
    for (let i = 0; i < 9; i++) {
      const n = Number(parts[i]);
      if (!Number.isFinite(n)) return null;
      if (n !== 0) any = true;
      nums.push(n);
    }
    return any ? nums : null;
  }

  function biquad(x, y, c) {
    const x2 = x * x;
    const y2 = y * y;
    const xy = x * y;
    return c[0] + c[1] * x + c[2] * y + c[3] * x2 + c[4] * y2 + c[5] * xy +
      c[6] * x2 * y + c[7] * x * y2 + c[8] * x2 * y2;
  }

  function numOr(text, fallback) {
    const n = Number(text);
    return Number.isFinite(n) ? n : fallback;
  }

  function eyeToDeg(source, spec, h, v) {
    if (source === "virtual") return [h, v];
    if (tclTrue(spec.use_biquadratic)) {
      const bh = coeffList(spec.bq_h_coeffs);
      const bv = coeffList(spec.bq_v_coeffs);
      if (bh && bv) return [biquad(h, v, bh), biquad(h, v, bv)];
    }
    const hd0 = numOr(spec.scale_h, 1) * (h - numOr(spec.raw_center_h, 0));
    const vd0 = numOr(spec.scale_v, 1) * (v - numOr(spec.raw_center_v, 0));
    let hd = hd0;
    let vd = vd0;
    if (source === "analog") {
      if (tclTrue(spec.invert_h)) hd = -hd;
      if (tclTrue(spec.invert_v)) vd = -vd;
    }
    return [hd, vd];
  }

  function expandEyeBatches(batches) {
    const samples = [];
    for (let i = 0; i < batches.length; i++) {
      const t = batches[i].t;
      const floats = batches[i].floats;
      const pairs = Math.floor(floats.length / 2);
      if (pairs < 1) continue;
      let dt = 0;
      if (i + 1 < batches.length) {
        const nextT = batches[i + 1].t;
        if (nextT > t) dt = (nextT - t) / pairs;
      } else if (i > 0) {
        const prev = batches[i - 1];
        const prevPairs = Math.floor(prev.floats.length / 2);
        if (prevPairs > 0 && t > prev.t) dt = (t - prev.t) / prevPairs;
      }
      for (let k = 0; k < pairs; k++) {
        samples.push({
          t: t + k * dt,
          h: floats[k * 2],
          v: floats[k * 2 + 1]
        });
      }
    }
    return samples;
  }

  function trialClock(ess, trial) {
    const n = ess.ends.length;
    if (!Number.isInteger(trial) || trial < 0 || trial >= n) {
      throw new Error("trial " + trial + " outside 0.." + (n - 1));
    }
    const start = trial > 0 ? ess.ends[trial - 1] : -1e300;
    const end = ess.ends[trial];
    const beginT = beginLogger(ess.begins, start, end);
    const recs = [];
    for (let i = 0; i < ess.inbox.length; i++) {
      const row = ess.inbox[i];
      if (row.t > start && row.t <= end) {
        const rec = parseInbox(row.text);
        rec.t = row.t;
        recs.push(rec);
      }
    }
    if (!recs.length) throw new Error("trial " + trial + " has no stim/inbox records");
    recs.sort(function (a, b) { return a.seq - b.seq; });
    const pairs = recs.map(function (rec) { return { t: rec.t, ticks: rec.ticks }; });
    pairs.sort(byTime);
    const firstTick = recs[0].ticks;
    const lastTick = recs[recs.length - 1].ticks;
    let t0 = firstTick;
    if (beginT != null) {
      const at = ticksAt(pairs, beginT);
      if (at < lastTick) t0 = at;
    }
    return { start: start, end: end, pairs: pairs, t0: t0 };
  }

  function videoSeconds(pairs, t0, loggerT) {
    return (ticksAt(pairs, loggerT) - t0) / 1000;
  }

  function trialMarks(ess, trial) {
    if (!ess._marks) ess._marks = {};
    if (ess._marks[trial]) return ess._marks[trial];
    const clock = trialClock(ess, trial);
    const widthRow = latestPositive(ess.screens["ess/screen_w"], clock.end);
    const heightRow = latestPositive(ess.screens["ess/screen_h"], clock.end);
    const halfxRow = latestPositive(ess.screens["ess/screen_halfx"], clock.end);
    const halfyRow = latestPositive(ess.screens["ess/screen_halfy"], clock.end);
    const screenW = widthRow ? widthRow.n : 1280;
    const screenH = heightRow ? heightRow.n : 800;
    const screenHx = halfxRow ? halfxRow.n : 16;
    const screenHy = halfyRow ? halfyRow.n : 16;
    const frameW = 1280;
    const frameH = 800;

    const touches = [];
    for (let i = 0; i < ess.touches.length; i++) {
      const ev = ess.touches[i];
      if (ev.t <= clock.start || ev.t > clock.end) continue;
      touches.push({
        kind: ev.kind,
        t: videoSeconds(clock.pairs, clock.t0, ev.t),
        x: ev.x * frameW / screenW,
        y: ev.y * frameH / screenH
      });
    }
    touches.sort(function (a, b) {
      const d = a.t - b.t;
      if (Math.abs(d) > 1e-9) return d < 0 ? -1 : 1;
      if (a.kind === b.kind) return 0;
      return a.kind === "press" ? -1 : 1;
    });

    const batches = [];
    let later = null;
    for (let i = 0; i < ess.rawEye.length; i++) {
      const row = ess.rawEye[i];
      if (row.t > clock.start && row.t <= clock.end) batches.push(row);
      else if (row.t > clock.end && (later == null || row.t < later.t)) later = row;
    }
    if (later) batches.push(later);
    batches.sort(byTime);
    const rawSamples = expandEyeBatches(batches);
    const eyes = [];
    for (let i = 0; i < rawSamples.length; i++) {
      const s = rawSamples[i];
      if (s.t <= clock.start || s.t > clock.end) continue;
      if (!Number.isFinite(s.h) || !Number.isFinite(s.v)) continue;
      const sourceRow = valueAt(ess.eyeSources, s.t);
      const source = sourceRow ? sourceRow.word : "";
      if (source !== "video" && source !== "analog" && source !== "virtual") continue;
      const settingsRow = valueAt(ess.eyeSettings, s.t);
      let spec = null;
      if (settingsRow) spec = settingsDict(settingsRow.text);
      if (source !== "virtual" && (!spec || !Object.keys(spec).length)) continue;
      if (!spec) spec = {};
      const deg = eyeToDeg(source, spec, s.h, s.v);
      const px = screenW / 2 + deg[0] / screenHx * (screenW / 2);
      const py = screenH / 2 - deg[1] / screenHy * (screenH / 2);
      eyes.push({
        t: videoSeconds(clock.pairs, clock.t0, s.t),
        x: px * frameW / screenW,
        y: py * frameH / screenH
      });
    }
    eyes.sort(function (a, b) { return a.t < b.t ? -1 : a.t > b.t ? 1 : 0; });

    const marks = { width: frameW, height: frameH, touches: touches, eyes: eyes };
    ess._marks[trial] = marks;
    return marks;
  }

  root.EssReader = {
    readEss: readEss,
    readEssTrial: readEssTrial,
    trialMarks: trialMarks,
    catalogRow: catalogRow
  };
})(typeof globalThis !== "undefined" ? globalThis : this);
