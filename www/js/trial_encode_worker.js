/* One x264 encoder. The page runs several of these so different
   seconds of a trial compress at the same time. A new enc_open
   starts the chunk on a keyframe. */
let mod = null;
let pending = [];
let samples = [];
const queue = [];
let pumping = false;

self.onmessage = function (ev) {
  queue.push(ev.data);
  if (!pumping) pump();
};

function pump() {
  pumping = true;
  const step = function () {
    if (!queue.length) {
      pumping = false;
      return;
    }
    const msg = queue.shift();
    let result;
    try {
      result = handle(msg);
    } catch (err) {
      fail(msg, err);
      return;
    }
    if (result && typeof result.then === "function") {
      result.then(step, function (err) { fail(msg, err); });
      return;
    }
    step();
  };
  step();
}

function fail(msg, err) {
  pumping = false;
  queue.length = 0;
  self.postMessage({
    type: "error",
    id: msg && msg.id,
    error: String((err && err.message) || err)
  });
}

function handle(msg) {
  if (msg.type === "init") return init(msg);
  if (msg.type === "open") return openChunk(msg);
  if (msg.type === "frame") return addFrame(msg);
  if (msg.type === "flush") return flushChunk(msg);
  throw new Error("unknown encoder message");
}

function init(msg) {
  importScripts(msg.scriptUrl);
  return createTrialEncode({
    locateFile: function () { return msg.wasmUrl; }
  }).then(function (created) {
    mod = created;
    self.postMessage({ type: "ready", id: msg.id });
  });
}

function errText() {
  const ptr = mod._enc_error();
  if (!ptr) return "";
  const bytes = mod.HEAPU8;
  let end = ptr;
  while (bytes[end]) end++;
  return new TextDecoder().decode(bytes.subarray(ptr, end));
}

function openChunk(msg) {
  mod._enc_close();
  pending = [];
  samples = [];
  if (mod._enc_open(msg.w, msg.h, msg.preset) !== 0) {
    throw new Error(errText() || "encoder failed to open");
  }
  const n = mod._enc_avcc_size();
  const src = mod._enc_avcc();
  const description = new Uint8Array(mod.HEAPU8.subarray(src, src + n));
  self.postMessage({ type: "opened", id: msg.id, description: description.buffer }, [description.buffer]);
}

function takeSample() {
  const n = mod._enc_sample_size();
  if (n <= 0) throw new Error("encoder sample is missing");
  if (!pending.length) throw new Error("encoder returned an extra frame");
  const src = mod._enc_sample();
  const data = new Uint8Array(n);
  data.set(mod.HEAPU8.subarray(src, src + n));
  samples.push({
    data: data.buffer,
    key: mod._enc_sample_key() !== 0,
    dur: pending.shift()
  });
}

function addFrame(msg) {
  const bytes = new Uint8Array(msg.buffer);
  const ptr = mod._malloc(bytes.length);
  if (!ptr) throw new Error("encoder is out of memory");
  mod.HEAPU8.set(bytes, ptr);
  pending.push(msg.dur);
  const ready = mod._enc_rgba(ptr, msg.dur);
  mod._free(ptr);
  if (ready < 0) throw new Error(errText() || "encode failed");
  if (ready === 1) takeSample();
}

function flushChunk(msg) {
  for (;;) {
    const n = mod._enc_flush();
    if (n < 0) throw new Error(errText() || "encode failed");
    if (n === 0) break;
    takeSample();
  }
  if (pending.length) throw new Error("encoder held a frame it never returned");
  mod._enc_close();
  const out = samples;
  samples = [];
  const transfers = [];
  for (let i = 0; i < out.length; i++) transfers.push(out[i].data);
  self.postMessage({ type: "flushed", id: msg.id, samples: out }, transfers);
}
