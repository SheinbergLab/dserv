/* Replay one trial in the wasm stim and return a 60 fps mp4.
   The steps are the same sequence trial_video writes into play.tcl. */
(function (root) {
  const glOpts = {
    alpha: false,
    antialias: false,
    depth: false,
    stencil: false,
    premultipliedAlpha: false,
    preserveDrawingBuffer: true
  };

  let canvas = null;
  let gl = null;
  let shapeMod = null;
  let starting = null;
  let present = function () {};
  let dropShapeTextures = function () {};
  let paintClear = function () {};
  let background = [0, 0, 0];
  let screenW = 1280;
  let screenH = 800;
  let spanX = 1;
  let spanY = 1;

  function f32(v) {
    const a = new Float32Array(1);
    a[0] = v;
    return a[0];
  }
  function fadd(a, b) { return f32(f32(a) + f32(b)); }
  function fsub(a, b) { return f32(f32(a) - f32(b)); }
  function fmul(a, b) { return f32(f32(a) * f32(b)); }
  function fdiv(a, b) { return f32(f32(a) / f32(b)); }
  function degx(d) { return fmul(fadd(0.5, fdiv(d, spanX)), screenW); }
  function degy(d) { return fmul(fsub(0.5, fdiv(d, spanY)), screenH); }

  function shader(type, source) {
    const s = gl.createShader(type);
    gl.shaderSource(s, source);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
      throw new Error(gl.getShaderInfoLog(s));
    }
    return s;
  }

  function initGl() {
    canvas = document.getElementById("view");
    if (!canvas) throw new Error("the viewer has no render canvas");
    gl = canvas.getContext("webgl2", glOpts);
    if (!gl) throw new Error("this browser has no WebGL 2");

    const program = gl.createProgram();
    gl.attachShader(program, shader(gl.VERTEX_SHADER, `#version 300 es
in vec2 a;
void main() { gl_Position = vec4(a, 0.0, 1.0); }`));
    gl.attachShader(program, shader(gl.FRAGMENT_SHADER, `#version 300 es
precision mediump float;
uniform vec3 uColor;
out vec4 o;
void main() { o = vec4(uColor, 1.0); }`));
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      throw new Error(gl.getProgramInfoLog(program));
    }
    gl.useProgram(program);
    const buf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    const loc = gl.getAttribLocation(program, "a");
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);

    const texProgram = gl.createProgram();
    gl.attachShader(texProgram, shader(gl.VERTEX_SHADER, `#version 300 es
in vec2 a;
in vec2 t;
out vec2 v;
void main() { v = t; gl_Position = vec4(a, 0.0, 1.0); }`));
    gl.attachShader(texProgram, shader(gl.FRAGMENT_SHADER, `#version 300 es
precision mediump float;
uniform sampler2D u;
uniform float uOpacity;
in vec2 v;
out vec4 o;
void main() { o = texture(u, v) * uOpacity; }`));
    gl.linkProgram(texProgram);
    if (!gl.getProgramParameter(texProgram, gl.LINK_STATUS)) {
      throw new Error(gl.getProgramInfoLog(texProgram));
    }
    const aLoc = gl.getAttribLocation(texProgram, "a");
    const tLoc = gl.getAttribLocation(texProgram, "t");
    const opLoc = gl.getUniformLocation(texProgram, "uOpacity");
    const texBuf = gl.createBuffer();
    const texVerts = new Float32Array(16);
    const shapeTex = new Map();

    function uploadTex(bytes, side) {
      const tex = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, tex);
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, side, side, 0, gl.RGBA, gl.UNSIGNED_BYTE, bytes);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      return tex;
    }
    function argbToRgba(src, side, stride) {
      const out = new Uint8Array(side * side * 4);
      for (let y = 0; y < side; y++) {
        for (let x = 0; x < side; x++) {
          const s = y * stride + x * 4;
          const d = (y * side + x) * 4;
          out[d] = src[s + 2];
          out[d + 1] = src[s + 1];
          out[d + 2] = src[s];
          out[d + 3] = src[s + 3];
        }
      }
      return out;
    }
    function drawTexturedQuad(tex, x, y, w, h, opacity) {
      const W = canvas.width;
      const H = canvas.height;
      const x0 = x / W * 2 - 1;
      const x1 = (x + w) / W * 2 - 1;
      const y0 = 1 - y / H * 2;
      const y1 = 1 - (y + h) / H * 2;
      texVerts.set([
        x0, y0, 0, 1,
        x1, y0, 1, 1,
        x0, y1, 0, 0,
        x1, y1, 1, 0
      ]);
      gl.useProgram(texProgram);
      gl.bindBuffer(gl.ARRAY_BUFFER, texBuf);
      gl.bufferData(gl.ARRAY_BUFFER, texVerts, gl.DYNAMIC_DRAW);
      gl.enableVertexAttribArray(aLoc);
      gl.enableVertexAttribArray(tLoc);
      gl.vertexAttribPointer(aLoc, 2, gl.FLOAT, false, 16, 0);
      gl.vertexAttribPointer(tLoc, 2, gl.FLOAT, false, 16, 8);
      gl.bindTexture(gl.TEXTURE_2D, tex);
      gl.uniform1f(opLoc, opacity);
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    }
    dropShapeTextures = function () {
      for (const tex of shapeTex.values()) gl.deleteTexture(tex);
      shapeTex.clear();
    };
    function drawVisible() {
      gl.disable(gl.CULL_FACE);
      gl.disable(gl.DEPTH_TEST);
      gl.disable(gl.SCISSOR_TEST);
      gl.viewport(0, 0, canvas.width, canvas.height);
      const n = shapeMod._stim_frame();
      gl.bindVertexArray(null);
      gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
      for (let i = 0; i < n; i++) {
        const cx = f32(shapeMod._stim_draw_cx(i));
        const cy = f32(shapeMod._stim_draw_cy(i));
        const hx = f32(shapeMod._stim_draw_hx(i));
        const hy = f32(shapeMod._stim_draw_hy(i));
        const side = shapeMod._stim_draw_side(i);
        const stride = shapeMod._stim_draw_stride(i);
        const opacity = shapeMod._stim_draw_opacity(i);
        const p = shapeMod._stim_draw_bytes(i);
        let tex = shapeTex.get(p);
        if (!tex) {
          const src = shapeMod.HEAPU8.subarray(p, p + stride * side);
          tex = uploadTex(argbToRgba(src, side, stride), side);
          shapeTex.set(p, tex);
        }
        const x0 = degx(fsub(cx, hx));
        const y0 = degy(fadd(cy, hy));
        const x1 = degx(fadd(cx, hx));
        const y1 = degy(fsub(cy, hy));
        drawTexturedQuad(tex, x0, y0, x1 - x0, y1 - y0, opacity);
      }
      gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
      gl.disableVertexAttribArray(tLoc);
      gl.useProgram(program);
      gl.bindBuffer(gl.ARRAY_BUFFER, buf);
      gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
    }
    paintClear = function (r, g, b) {
      gl.clearColor(r, g, b, 1);
      gl.clear(gl.COLOR_BUFFER_BIT);
    };
    present = function () {
      paintClear(background[0] / 255, background[1] / 255, background[2] / 255);
      drawVisible();
    };
  }

  function loadScript(src) {
    return new Promise(function (resolve, reject) {
      const s = document.createElement("script");
      s.src = src;
      s.onload = function () { resolve(); };
      s.onerror = function () { reject(new Error("Could not load " + src)); };
      document.head.appendChild(s);
    });
  }

  function ensureModule() {
    if (shapeMod) return Promise.resolve();
    if (starting) return starting;
    starting = (async function () {
      initGl();
      if (typeof createShapeRaster !== "function") {
        await loadScript("shape_raster.js?v=12");
      }
      const mod = await createShapeRaster({
        locateFile: function (path) { return path + "?v=12"; },
        canvas: canvas,
        preinitializedWebGLContext: gl
      });
      shapeMod = mod;
      if (shapeMod._stim_gl_init() !== 0) {
        const why = readCString(shapeMod._stim_error()) || "stim_gl_init failed";
        shapeMod = null;
        throw new Error(why);
      }
    })();
    return starting.catch(function (err) {
      starting = null;
      throw err;
    });
  }

  function cString(text) {
    const bytes = new TextEncoder().encode(text + "\0");
    const ptr = shapeMod._malloc(bytes.length);
    shapeMod.HEAPU8.set(bytes, ptr);
    return ptr;
  }
  function readCString(ptr) {
    if (!ptr) return "";
    const mem = shapeMod.HEAPU8;
    let s = "";
    for (let i = ptr; mem[i]; i++) s += String.fromCharCode(mem[i]);
    return s;
  }
  function evalScript(script) {
    const ptr = cString(script);
    const code = shapeMod._stim_eval(ptr);
    shapeMod._free(ptr);
    if (code !== 0) throw new Error(readCString(shapeMod._stim_error()) || script);
    return readCString(shapeMod._stim_result());
  }
  function tclBrace(text) {
    const s = String(text);
    if (/[{}\\]/.test(s)) throw new Error("cannot quote " + s);
    return "{" + s + "}";
  }
  function loadDg(bytes) {
    const ptr = shapeMod._malloc(bytes.length);
    shapeMod.HEAPU8.set(bytes, ptr);
    const code = shapeMod._stim_load_dg(ptr, bytes.length);
    shapeMod._free(ptr);
    if (code !== 0) throw new Error(readCString(shapeMod._stim_error()) || "stimdg");
  }
  function stimTime() {
    const text = evalScript("set ::_replay_seen $::StimTimeF").trim();
    const n = Number(text);
    if (!Number.isFinite(n)) throw new Error("StimTimeF " + text);
    return n;
  }

  function applyTcl(text) {
    const bg = /^setBackground\s+(\d+)\s+(\d+)\s+(\d+)\s*$/.exec(text);
    if (bg) background = [Number(bg[1]), Number(bg[2]), Number(bg[3])];
    if (/^nexttrial\b/.test(text)) dropShapeTextures();
    evalScript(text);
  }

  function installStandins() {
    evalScript([
      "array unset ::dsCmds",
      "set ::dservhost browser",
      "package provide qpcs 1.0",
      "namespace eval qpcs {}",
      "proc qpcs::dsStimRegister {host} {}",
      "proc qpcs::dsStimUnregister {host} {}",
      "proc qpcs::dsStimAddMatch {host name} {return 0}",
      "proc qpcs::dsSet {args} {}",
      "proc qpcs::dsGet {server var} {",
      "  if {$var eq \"ess/system_path\"} {",
      "    return [list {} {} {} {} {} /stim]",
      "  }",
      "  return -1",
      "}",
      "proc qpcs::dsSocketSendBytes {args} {}",
      "proc dserv_send_string {args} {}",
      "proc _tape_ds {raw} {",
      "  set name [lindex $raw 0]",
      "  if {![info exists ::dsCmds($name)]} {",
      "    error \"no handler for $name\"",
      "  }",
      "  eval $::dsCmds($name) $raw",
      "}"
    ].join("\n"));
  }

  function setScreen(replay) {
    screenW = Number(replay.width);
    screenH = Number(replay.height);
    const halfx = Number(replay.halfx);
    const halfy = Number(replay.halfy);
    const refresh = Number(replay.refresh);
    if (!(screenW > 0) || !(screenH > 0) || !(halfx > 0) || !(halfy > 0)) {
      throw new Error("replay is missing the screen");
    }
    spanX = f32(halfx * 2);
    spanY = f32(halfy * 2);
    canvas.width = screenW;
    canvas.height = screenH;
    gl.viewport(0, 0, screenW, screenH);
    shapeMod._stim_set_screen(halfx, halfy, screenW, screenH, refresh > 0 ? refresh : 60);
    const bg = replay.background || [0, 0, 0];
    background = [Number(bg[0]) || 0, Number(bg[1]) || 0, Number(bg[2]) || 0];
  }

  function blobBytes(blob) {
    return new Promise(function (resolve, reject) {
      const reader = new FileReader();
      reader.onload = function () { resolve(new Uint8Array(reader.result)); };
      reader.onerror = function () { reject(reader.error || new Error("could not read a frame")); };
      reader.readAsArrayBuffer(blob);
    });
  }

  async function encodeStill(replay, onProgress, saveFrame) {
    const showPin = replay.show_pin === "" || replay.show_pin == null ? null : Number(replay.show_pin);
    const showTape = replay.show_tape === "" || replay.show_tape == null ? null : Number(replay.show_tape);
    const stepMs = Number(replay.step_ms);
    if (showPin != null && showTape != null && Math.abs(showPin - showTape) > stepMs + 1) {
      throw new Error("show_response pin " + showPin + " is more than a frame from tape time " + showTape);
    }
    let held = 0;
    let wiped = 0;
    let captures = 0;
    let outIndex = 0;
    const total = Number(replay.frames) || replay.steps.length;
    const steps = replay.steps;

    function notePin(pin, count) {
      const seen = stimTime();
      if (Math.abs(seen - pin) < 0.05) {
        if (count) held += 1;
        return;
      }
      if (Math.abs(seen) < 0.5) {
        if (count) {
          wiped += 1;
          return;
        }
      }
      throw new Error("StimTimeF " + seen + " did not hold tape pin " + pin);
    }

    for (let i = 0; i < steps.length; i++) {
      const step = steps[i];
      if (step.tcl != null) {
        applyTcl(step.tcl);
        if (step.pin != null && !Number(step.clock)) notePin(Number(step.pin), false);
        continue;
      }
      present();
      if (i > 0 && steps[i - 1].pin != null && Number(steps[i - 1].clock)) {
        notePin(Number(steps[i - 1].pin), true);
      }
      const nrep = Number(step.capture) || 0;
      if (nrep > 0) {
        const blob = await new Promise(function (resolve) {
          canvas.toBlob(resolve, "image/jpeg", 0.92);
        });
        if (!blob) throw new Error("could not read the frame");
        await saveFrame(await blobBytes(blob), nrep);
        outIndex += nrep;
      }
      captures += 1;
      if (onProgress && captures % 10 === 0) {
        onProgress(captures, total);
        await new Promise(function (resolve) { requestAnimationFrame(resolve); });
      }
    }
    if (outIndex < 1) throw new Error("playback produced no frames");
    if (captures < 1 || held < 1) throw new Error("StimTimeF never held the tape pin");
    if (wiped > 4) {
      throw new Error("StimTimeF was cleared on " + wiped + " frames; the tape pin is not sticking");
    }
    return {
      server: true,
      frames: captures,
      clock_held: held,
      reset_frames: wiped
    };
  }

  async function encodeTrial(replay, onProgress) {
    if (typeof VideoEncoder !== "function") {
      throw new Error("this browser has no VideoEncoder, so it cannot build an mp4");
    }
    const base = {
      width: canvas.width,
      height: canvas.height,
      bitrate: 2500000,
      framerate: 60,
      latencyMode: "quality",
      avc: { format: "avc" }
    };
    let chosen = null;
    for (const codec of ["avc1.640028", "avc1.4d0028", "avc1.420028"]) {
      const cfg = Object.assign({ codec: codec }, base);
      try {
        const q = await VideoEncoder.isConfigSupported(cfg);
        if (q.supported) {
          chosen = q.config || cfg;
          break;
        }
      } catch (err) {}
    }
    if (!chosen) throw new Error("this browser cannot encode " + canvas.width + "×" + canvas.height + " h264");

    const samples = [];
    let description = null;
    let encError = null;
    const encoder = new VideoEncoder({
      output: function (chunk, meta) {
        if (meta && meta.decoderConfig && meta.decoderConfig.description) {
          description = new Uint8Array(meta.decoderConfig.description);
        }
        const data = new Uint8Array(chunk.byteLength);
        chunk.copyTo(data);
        samples.push({ data: data, key: chunk.type === "key" });
      },
      error: function (err) { encError = err; }
    });
    encoder.configure(chosen);

    const showPin = replay.show_pin === "" || replay.show_pin == null ? null : Number(replay.show_pin);
    const showTape = replay.show_tape === "" || replay.show_tape == null ? null : Number(replay.show_tape);
    const stepMs = Number(replay.step_ms);
    if (showPin != null && showTape != null && Math.abs(showPin - showTape) > stepMs + 1) {
      throw new Error("show_response pin " + showPin + " is more than a frame from tape time " + showTape);
    }

    let held = 0;
    let wiped = 0;
    let captures = 0;
    let outIndex = 0;
    const total = Number(replay.frames) || replay.steps.length;
    const steps = replay.steps;

    function notePin(pin, count) {
      const seen = stimTime();
      if (Math.abs(seen - pin) < 0.05) {
        if (count) held += 1;
        return;
      }
      if (Math.abs(seen) < 0.5) {
        if (count) {
          wiped += 1;
          return;
        }
      }
      throw new Error("StimTimeF " + seen + " did not hold tape pin " + pin);
    }

    for (let i = 0; i < steps.length; i++) {
      const step = steps[i];
      if (encError) throw encError;
      if (step.tcl != null) {
        applyTcl(step.tcl);
        if (step.pin != null && !Number(step.clock)) notePin(Number(step.pin), false);
        continue;
      }
      present();
      if (i > 0 && steps[i - 1].pin != null && Number(steps[i - 1].clock)) {
        notePin(Number(steps[i - 1].pin), true);
      }
      const nrep = Number(step.capture) || 0;
      for (let r = 0; r < nrep; r++) {
        if (encoder.encodeQueueSize > 30) {
          await new Promise(function (resolve) { encoder.ondequeue = resolve; });
        }
        const frame = new VideoFrame(canvas, {
          timestamp: Math.round(outIndex * 1000000 / 60),
          duration: Math.round(1000000 / 60)
        });
        encoder.encode(frame, { keyFrame: outIndex % 60 === 0 });
        frame.close();
        outIndex += 1;
      }
      captures += 1;
      if (onProgress && captures % 10 === 0) {
        onProgress(captures, total);
        await new Promise(function (resolve) { requestAnimationFrame(resolve); });
      }
    }
    if (encError) throw encError;
    if (outIndex < 1) throw new Error("playback produced no frames");
    if (captures < 1 || held < 1) throw new Error("StimTimeF never held the tape pin");
    if (wiped > 4) {
      throw new Error("StimTimeF was cleared on " + wiped + " frames; the tape pin is not sticking");
    }
    await encoder.flush();
    encoder.close();
    if (encError) throw encError;
    if (!description) throw new Error("encoder did not return an avcC description");
    return {
      mp4: root.muxMp4(samples, description, canvas.width, canvas.height, 60),
      frames: captures,
      clock_held: held,
      reset_frames: wiped
    };
  }

  async function render(opts) {
    await ensureModule();
    dropShapeTextures();
    setScreen(opts.replay);
    installStandins();
    evalScript(opts.stimText);
    const need = opts.replay.need || [];
    for (let i = 0; i < need.length; i++) {
      const name = need[i];
      const got = evalScript(
        "set ::_need_name " + tclBrace(name) + "\nexpr {[info exists ::dsCmds($::_need_name)]}"
      ).trim();
      if (got !== "1") {
        throw new Error("trial_video: stim file registered no handler for " + name);
      }
    }
    loadDg(opts.stimdg);
    if (typeof VideoEncoder !== "function") {
      if (typeof opts.saveFrame !== "function") {
        throw new Error("this browser has no VideoEncoder, so it cannot build an mp4");
      }
      return encodeStill(opts.replay, opts.onProgress, opts.saveFrame);
    }
    return encodeTrial(opts.replay, opts.onProgress);
  }

  root.TrialWasm = { render: render };
})(typeof globalThis !== "undefined" ? globalThis : this);
