/* Minimal avc1 mp4 from WebCodecs chunks.
   Each chunk is already AVCC (4-byte lengths). description is the
   AVCDecoderConfigurationRecord. */
(function (root) {
  function u8(parts) {
    let n = 0;
    for (let i = 0; i < parts.length; i++) n += parts[i].length;
    const out = new Uint8Array(n);
    let o = 0;
    for (let i = 0; i < parts.length; i++) {
      out.set(parts[i], o);
      o += parts[i].length;
    }
    return out;
  }
  function be(n, bytes) {
    const a = new Uint8Array(bytes);
    for (let i = bytes - 1; i >= 0; i--) {
      a[i] = n & 0xff;
      n = Math.floor(n / 256);
    }
    return a;
  }
  function str(s) {
    return new TextEncoder().encode(s);
  }
  function box(type, payload) {
    return u8([be(8 + payload.length, 4), str(type), payload]);
  }
  function fullbox(type, version, flags, payload) {
    return box(type, u8([be(version, 1), be(flags, 3), payload]));
  }

  function muxMp4(samples, description, width, height, fps) {
    const timescale = 60000;
    const delta = timescale / fps;
    if (delta !== Math.floor(delta)) throw new Error("fps does not divide 60000");
    const duration = samples.length * delta;
    const desc = description instanceof Uint8Array ? description : new Uint8Array(description);

    const matrix = u8([
      be(0x00010000, 4), be(0, 4), be(0, 4),
      be(0, 4), be(0x00010000, 4), be(0, 4),
      be(0, 4), be(0, 4), be(0x40000000, 4)
    ]);
    const mvhd = fullbox("mvhd", 0, 0, u8([
      be(0, 4), be(0, 4), be(timescale, 4), be(duration, 4),
      be(0x00010000, 4), be(0x0100, 2), be(0, 2), be(0, 8),
      matrix, be(0, 24), be(2, 4)
    ]));
    const tkhd = fullbox("tkhd", 0, 3, u8([
      be(0, 4), be(0, 4), be(1, 4), be(0, 4), be(duration, 4),
      be(0, 8), be(0, 2), be(0, 2), be(0, 2), be(0, 2),
      matrix, be(width, 2), be(0, 2), be(height, 2), be(0, 2)
    ]));
    const mdhd = fullbox("mdhd", 0, 0, u8([
      be(0, 4), be(0, 4), be(timescale, 4), be(duration, 4), be(0x55c4, 2), be(0, 2)
    ]));
    const hdlr = fullbox("hdlr", 0, 0, u8([
      be(0, 4), str("vide"), be(0, 12), str("VideoHandler"), new Uint8Array([0])
    ]));
    const vmhd = fullbox("vmhd", 0, 1, u8([be(0, 2), be(0, 2), be(0, 2), be(0, 2)]));
    const url = fullbox("url ", 0, 1, new Uint8Array(0));
    const dref = fullbox("dref", 0, 0, u8([be(1, 4), url]));
    const dinf = box("dinf", dref);

    const name = new Uint8Array(32);
    const avc1Body = u8([
      be(0, 6), be(1, 2),
      be(0, 2), be(0, 2), be(0, 12),
      be(width, 2), be(height, 2),
      be(0x00480000, 4), be(0x00480000, 4),
      be(0, 4), be(1, 2), name,
      be(0x0018, 2), be(0xffff, 2),
      box("avcC", desc)
    ]);
    const avc1 = box("avc1", avc1Body);
    const stsd = fullbox("stsd", 0, 0, u8([be(1, 4), avc1]));
    const stts = fullbox("stts", 0, 0, u8([
      be(1, 4), be(samples.length, 4), be(delta, 4)
    ]));
    const keys = [];
    for (let i = 0; i < samples.length; i++) if (samples[i].key) keys.push(i + 1);
    if (!keys.length) keys.push(1);
    const keyBytes = [be(keys.length, 4)];
    for (let i = 0; i < keys.length; i++) keyBytes.push(be(keys[i], 4));
    const stss = fullbox("stss", 0, 0, u8(keyBytes));
    const stsc = fullbox("stsc", 0, 0, u8([
      be(1, 4), be(1, 4), be(samples.length, 4), be(1, 4)
    ]));
    const sizes = [be(0, 4), be(samples.length, 4)];
    let dataLen = 0;
    for (let i = 0; i < samples.length; i++) {
      sizes.push(be(samples[i].data.length, 4));
      dataLen += samples[i].data.length;
    }
    const stsz = fullbox("stsz", 0, 0, u8(sizes));
    const stco = fullbox("stco", 0, 0, u8([be(1, 4), be(0, 4)]));
    const stbl = box("stbl", u8([stsd, stts, stss, stsc, stsz, stco]));
    const minf = box("minf", u8([vmhd, dinf, stbl]));
    const mdia = box("mdia", u8([mdhd, hdlr, minf]));
    const trak = box("trak", u8([tkhd, mdia]));
    let moov = box("moov", u8([mvhd, trak]));
    const ftyp = box("ftyp", u8([str("isom"), be(0, 4), str("isom"), str("iso2"), str("avc1"), str("mp41")]));

    const sampleOffset = ftyp.length + moov.length + 8;
    const patched = new Uint8Array(moov);
    const view = new DataView(patched.buffer);
    let found = false;
    for (let o = 0; o + 8 < patched.length; o++) {
      if (patched[o] === 0x73 && patched[o + 1] === 0x74 && patched[o + 2] === 0x63 && patched[o + 3] === 0x6f) {
        view.setUint32(o + 12, sampleOffset);
        found = true;
        break;
      }
    }
    if (!found) throw new Error("stco missing");
    moov = patched;

    const dataParts = samples.map(function (s) { return s.data; });
    const mdat = box("mdat", u8(dataParts));
    if (mdat.length !== 8 + dataLen) throw new Error("mdat size");
    return u8([ftyp, moov, mdat]);
  }

  root.muxMp4 = muxMp4;
  if (typeof module !== "undefined" && module.exports) module.exports = { muxMp4: muxMp4 };
})(typeof globalThis !== "undefined" ? globalThis : this);
