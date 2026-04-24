/**
 * dg_reader.js — DG/DGZ binary format reader
 *
 * Reads DYN_GROUP files (.dg, .dgz) into a DynGroup structure.
 * Supports gzip-compressed files via DecompressionStream.
 *
 * Usage:
 *   const dg = await readDGAuto(arrayBuffer, filename);
 *   const nRows = countRows(dg);
 *   const val = getScalar(dg, 'system', 0);
 *   const arr = getSublistData(dg, 'em_h_deg', 0);
 */

// Data type constants
const DF_CHAR = 2, DF_LONG = 3, DF_SHORT = 4, DF_FLOAT = 5, DF_STRING = 7, DF_LIST = 12;
const DG_MAGIC = new Uint8Array([0x21, 0x12, 0x36, 0x63]);

// Type name map for display
const DF_TYPE_NAMES = {
  [DF_CHAR]: 'char', [DF_SHORT]: 'short', [DF_LONG]: 'int32',
  [DF_FLOAT]: 'float', [DF_STRING]: 'string', [DF_LIST]: 'list'
};

class DynList {
  constructor() {
    this.name = '';
    this.dataType = null;
    this.increment = 10;
    this.flags = 0;
    this.data = null;
  }
  get n() {
    return this.data ? (Array.isArray(this.data) ? this.data.length : this.data.length) : 0;
  }
  get typeName() {
    return DF_TYPE_NAMES[this.dataType] || '?';
  }
}

class DynGroup {
  constructor() {
    this.name = '';
    this.lists = [];
  }
  get nlists() { return this.lists.length; }
  getList(n) { return this.lists.find(l => l.name === n); }
}

// Binary reader
class BR {
  constructor(b) {
    this.buf = b; this.v = new DataView(b); this.u8 = new Uint8Array(b);
    this.o = 0; this.le = true;
  }
  get eof() { return this.o >= this.buf.byteLength; }
  r8() { return this.u8[this.o++]; }
  r32() { const v = this.v.getInt32(this.o, this.le); this.o += 4; return v; }
  r16() { const v = this.v.getInt16(this.o, this.le); this.o += 2; return v; }
  rf() { const v = this.v.getFloat32(this.o, this.le); this.o += 4; return v; }
  rs(n) {
    const b = this.u8.slice(this.o, this.o + n); this.o += n;
    let e = b.indexOf(0); if (e === -1) e = n;
    return new TextDecoder().decode(b.slice(0, e));
  }
  ri32(n) { const a = new Int32Array(n); for (let i = 0; i < n; i++) a[i] = this.r32(); return a; }
  ri16(n) { const a = new Int16Array(n); for (let i = 0; i < n; i++) a[i] = this.r16(); return a; }
  rf32(n) { const a = new Float32Array(n); for (let i = 0; i < n; i++) a[i] = this.rf(); return a; }
  ru8(n) { const a = this.u8.slice(this.o, this.o + n); this.o += n; return new Uint8Array(a); }
}

// DG file reader
class DGR {
  read(buf) {
    const r = new BR(buf);
    for (let i = 0; i < 4; i++) if (r.r8() !== DG_MAGIC[i]) throw new Error('Not a DG file');
    const dg = new DynGroup();
    while (!r.eof) {
      const t = r.r8();
      if (t === 255) break;
      if (t === 0) {
        r.le = true;
        let v = r.v.getFloat32(r.o, true);
        if (v !== 1.0) { v = r.v.getFloat32(r.o, false); if (v !== 1.0) throw new Error('Bad version'); r.le = false; }
        r.o += 4;
      } else if (t === 1) { this._rg(r, dg); }
      else throw new Error('tag ' + t);
    }
    return dg;
  }
  _rg(r, dg) {
    while (!r.eof) {
      const t = r.r8();
      if (t === 255) return;
      if (t === 0) { const n = r.r32(); dg.name = r.rs(n); }
      else if (t === 1) r.r32();
      else if (t === 2) { const dl = new DynList(); this._rl(r, dl); dg.lists.push(dl); }
      else throw new Error('gtag ' + t);
    }
  }
  _rl(r, dl) {
    while (!r.eof) {
      const t = r.r8();
      if (t === 255) return;
      switch (t) {
        case 0: { const n = r.r32(); dl.name = r.rs(n); break; }
        case 1: dl.increment = r.r32(); break;
        case 10: dl.flags = r.r32(); break;
        case 2: break;
        case 4: { const n = r.r32(); dl.dataType = DF_CHAR; dl.data = r.ru8(n); break; }
        case 5: { const n = r.r32(); dl.dataType = DF_SHORT; dl.data = r.ri16(n); break; }
        case 6: { const n = r.r32(); dl.dataType = DF_LONG; dl.data = r.ri32(n); break; }
        case 7: { const n = r.r32(); dl.dataType = DF_FLOAT; dl.data = r.rf32(n); break; }
        case 3: {
          const n = r.r32(); dl.dataType = DF_STRING;
          const s = []; for (let i = 0; i < n; i++) { const l = r.r32(); s.push(r.rs(l)); }
          dl.data = s; break;
        }
        case 8: {
          const n = r.r32(); dl.dataType = DF_LIST;
          const subs = [];
          for (let i = 0; i < n; i++) {
            if (r.r8() !== 9) throw new Error('sublist');
            const sub = new DynList(); this._rl(r, sub); subs.push(sub);
          }
          dl.data = subs; break;
        }
        default: throw new Error('ltag ' + t);
      }
    }
  }
}

function readDG(b) { return new DGR().read(b); }

async function readDGZ(b) {
  const s = new Response(b).body.pipeThrough(new DecompressionStream('gzip'));
  return readDG(await new Response(s).arrayBuffer());
}

async function readDGAuto(b, f = '') {
  const u = new Uint8Array(b);
  if ((u[0] === 0x1f && u[1] === 0x8b) || f.toLowerCase().endsWith('.dgz')) return readDGZ(b);
  return readDG(b);
}

// ── Helpers ──

function getSublistData(dg, colName, rowIdx) {
  const list = dg.getList(colName);
  if (!list) return null;
  if (list.dataType === DF_LIST && Array.isArray(list.data) && rowIdx < list.data.length) {
    return list.data[rowIdx].data;
  }
  return null;
}

function getScalar(dg, colName, rowIdx) {
  const list = dg.getList(colName);
  if (!list || !list.data) return undefined;
  if (list.dataType === DF_LIST) return undefined;
  if (Array.isArray(list.data)) return list.data[rowIdx];
  return rowIdx < list.data.length ? list.data[rowIdx] : undefined;
}

function countRows(dg) {
  for (const l of dg.lists) {
    if (l.dataType === DF_LIST && Array.isArray(l.data)) return l.data.length;
    if (l.data) return Array.isArray(l.data) ? l.data.length : l.data.length;
  }
  return 0;
}

/** Detect whether this DG has trial structure (extract script output) */
function isTrialDG(dg) {
  const required = ['filename', 'system', 'protocol', 'variant'];
  return required.every(name => dg.getList(name) != null);
}

/** Check for eye movement data */
function hasEyeData(dg) {
  return dg.getList('em_h_deg') != null
      && dg.getList('em_v_deg') != null
      && dg.getList('em_seconds') != null;
}

/** Check for spike data produced by the neuropixel Tcl package */
function hasSpikeData(dg) {
  return dg.getList('spike_times') != null
      && dg.getList('spike_unit') != null
      && dg.getList('spike_src_trial') != null;
}

/** Format a cell value for display */
function formatCellValue(list, rowIdx) {
  if (!list || !list.data) return '';
  if (list.dataType === DF_LIST) {
    if (!Array.isArray(list.data) || rowIdx >= list.data.length) return '';
    const sub = list.data[rowIdx];
    return `[${sub.typeName} \u00D7 ${sub.n}]`;
  }
  const v = Array.isArray(list.data) ? list.data[rowIdx] : list.data[rowIdx];
  if (v === null || v === undefined) return '';
  if (typeof v === 'string') return v;
  if (typeof v === 'number') {
    if (Number.isInteger(v)) return String(v);
    if (Math.abs(v) < 0.001 || Math.abs(v) >= 10000) return v.toExponential(3);
    return Number(v.toPrecision(6)).toString();
  }
  return String(v);
}
