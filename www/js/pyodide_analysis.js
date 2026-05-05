/**
 * pyodide_analysis.js — in-browser ANOVA kernel for Spike Explorer.
 *
 * Deliberately scoped to "quick-pass statistical support for what the user
 * is already looking at" — not a substitute for a real analysis notebook.
 * Two functions, both vectorized in numpy:
 *
 *   anova({dim1, dim2?, y})
 *     Per-trial scalar response (e.g. spike count in window).
 *     - dim2 omitted → 1-way ANOVA on dim1.
 *     - both given  → 2-way Type II ANOVA on dim1, dim2, dim1:dim2.
 *     Returns F, df, p, partial η² per effect.
 *
 *   anovaOverTime(X3d_flat, [N,U,T], dim1, dim2?)
 *     Per-(unit,time) firing rate, fit independently for each (u,t) cell
 *     by a single batched-OLS pass per design. Same Type II hierarchy.
 *     Returns η² traces only — median across units with 25–75% band per
 *     effect. No per-cell p arrays, no heatmaps. The trace is what the
 *     spike_explorer overlays on its raster/PSTH view to read latency
 *     to selectivity.
 *
 * Inputs: plain JS arrays / typed arrays. Outputs: plain JS objects.
 * All factor levels are treated as categorical — continuous / circular
 * variables (orientation, position) belong in a notebook with the right
 * basis; this kernel doesn't try to guess.
 *
 * Status events on window for UI: 'pyodide-analysis:status'.
 */

(function () {
  'use strict';

  const PYODIDE_VERSION = '0.26.4';
  const PYODIDE_CDN = `https://cdn.jsdelivr.net/pyodide/v${PYODIDE_VERSION}/full/`;
  // numpy + scipy is all we need — no statsmodels, no sklearn.
  const PY_PACKAGES = ['numpy', 'scipy'];

  const PY_MODULE = `
import numpy as np
from scipy import stats as _sps

# ─── shared helpers ────────────────────────────────────────────────────

def _design_1way(d1):
    """Treatment-coded 1-way design: intercept + (n1-1) contrast columns.
    Returns (D, info). info.A holds the contrast-column indices for d1."""
    d1 = np.asarray(d1)
    cats = np.unique(d1)
    n1 = len(cats) - 1
    N = len(d1)
    cols = [np.ones(N)]
    for c in cats[1:]:
        cols.append((d1 == c).astype(float))
    D = np.column_stack(cols) if cols else np.ones((N, 1))
    return D, {'N': N, 'n1': n1,
               'A': list(range(1, 1 + n1))}

def _design_2way(d1, d2):
    """Treatment-coded 2-way design with interaction:
    intercept + (n1-1) A + (n2-1) B + (n1-1)*(n2-1) AB."""
    d1 = np.asarray(d1); d2 = np.asarray(d2)
    cats1 = np.unique(d1); cats2 = np.unique(d2)
    n1, n2 = len(cats1) - 1, len(cats2) - 1
    N = len(d1)
    cols = [np.ones(N)]
    for c in cats1[1:]: cols.append((d1 == c).astype(float))
    for c in cats2[1:]: cols.append((d2 == c).astype(float))
    for ca in cats1[1:]:
        for cb in cats2[1:]:
            cols.append(((d1 == ca) & (d2 == cb)).astype(float))
    D = np.column_stack(cols)
    return D, {'N': N, 'n1': n1, 'n2': n2,
               'A':  list(range(1, 1 + n1)),
               'B':  list(range(1 + n1, 1 + n1 + n2)),
               'AB': list(range(1 + n1 + n2, 1 + n1 + n2 + n1*n2))}

def _ss_resid(D, cols, Y):
    """SS-residual for OLS fit of Y on D[:, cols]. Y can be (N,) or (N,M);
    returns scalar or (M,) accordingly."""
    Dm = D[:, cols]
    beta, _, _, _ = np.linalg.lstsq(Dm, Y, rcond=None)
    r = Y - Dm @ beta
    return (r * r).sum(axis=0)

# ─── aggregate (per-trial scalar) ANOVA ────────────────────────────────

def anova_aggregate(dim1, dim2, y):
    """One- or two-way ANOVA on a per-trial scalar response.

    dim2 may be None (or len==0) for the 1-way case.
    Hierarchical Type II SS:
      1-way: SS(A) = SS_total - SS(intercept+A) is just the obvious split.
      2-way: SS(A) = SSR(B)   - SSR(A+B);  SS(B) = SSR(A) - SSR(A+B);
             SS(AB) = SSR(A+B) - SSR(A+B+AB).
    Returns {n, n_levels, effects:[{name,F,df,p,partial_eta2,sum_sq}], note}.
    """
    y = np.asarray(y, dtype=float)
    finite = np.isfinite(y)
    y = y[finite]
    d1 = np.asarray(dim1)[finite]
    has_d2 = dim2 is not None and len(dim2) == len(finite)
    if has_d2:
        d2 = np.asarray(dim2)[finite]

    n = len(y)
    n1 = int(len(np.unique(d1))) if n else 0
    n2 = int(len(np.unique(d2))) if (has_d2 and n) else 0
    out = {'n': int(n),
           'n_levels': {'dim1': n1, 'dim2': n2 if has_d2 else None},
           'effects': [], 'note': None}

    if n < 4 or n1 < 2:
        out['note'] = 'not enough data / levels'
        return out

    if has_d2 and n2 < 2:
        # Degenerate 2-way → fall through to 1-way on dim1.
        has_d2 = False
        out['n_levels']['dim2'] = n2

    if not has_d2:
        D, info = _design_1way(d1)
        intercept = [0]
        full   = intercept + info['A']
        df_full = n - len(full)
        if df_full <= 0:
            out['note'] = f'too few trials ({n}) for {len(full)} parameters'
            return out
        ss_int  = _ss_resid(D, intercept, y)
        ss_full = _ss_resid(D, full,      y)
        ss_a    = max(0.0, float(ss_int - ss_full))
        ss_full = float(ss_full)
        df_a    = info['n1']
        mse     = ss_full / df_full
        if mse <= 0:
            out['note'] = 'zero residual variance'
            return out
        F = (ss_a / df_a) / mse
        p = float(_sps.f.sf(F, df_a, df_full))
        out['effects'].append({
            'name': 'dim1', 'F': float(F), 'df': int(df_a),
            'df_resid': int(df_full),
            'p': p,
            'partial_eta2': ss_a / (ss_a + ss_full) if (ss_a + ss_full) > 0 else float('nan'),
            'sum_sq': ss_a,
        })
        out['ss_residual'] = ss_full
        out['df_residual'] = int(df_full)
        return out

    # 2-way
    D, info = _design_2way(d1, d2)
    intercept = [0]
    full      = intercept + info['A'] + info['B'] + info['AB']
    no_a      = intercept + info['B']
    no_b      = intercept + info['A']
    no_ab     = intercept + info['A'] + info['B']
    df_full   = n - len(full)
    if df_full <= 0:
        out['note'] = f'too few trials ({n}) for {len(full)} parameters'
        return out
    ss_full   = float(_ss_resid(D, full,   y))
    ss_no_a   = float(_ss_resid(D, no_a,   y))
    ss_no_b   = float(_ss_resid(D, no_b,   y))
    ss_no_ab  = float(_ss_resid(D, no_ab,  y))
    ss_a  = max(0.0, ss_no_a  - ss_no_ab)
    ss_b  = max(0.0, ss_no_b  - ss_no_ab)
    ss_ab = max(0.0, ss_no_ab - ss_full)
    df_a, df_b, df_ab = info['n1'], info['n2'], info['n1'] * info['n2']
    mse = ss_full / df_full
    if mse <= 0:
        out['note'] = 'zero residual variance'
        return out
    for label, ss, df_t in [
        ('dim1',      ss_a,  df_a),
        ('dim2',      ss_b,  df_b),
        ('dim1:dim2', ss_ab, df_ab),
    ]:
        F = (ss / df_t) / mse
        p = float(_sps.f.sf(F, df_t, df_full))
        out['effects'].append({
            'name': label, 'F': float(F),
            'df': int(df_t), 'df_resid': int(df_full),
            'p': p,
            'partial_eta2': ss / (ss + ss_full) if (ss + ss_full) > 0 else float('nan'),
            'sum_sq': ss,
        })
    out['ss_residual'] = ss_full
    out['df_residual'] = int(df_full)
    return out

# ─── time-resolved per-(unit,time) ANOVA ────────────────────────────────

def _quantiles_across_units(eta_ut):
    """Per-timepoint q25/q50/q75 across units (NaN-aware). eta_ut is (U,T)."""
    U, T = eta_ut.shape
    out = np.full((3, T), np.nan)
    for t in range(T):
        col = eta_ut[:, t]
        col = col[np.isfinite(col)]
        if col.size == 0:
            continue
        out[0, t] = np.percentile(col, 25)
        out[1, t] = np.percentile(col, 50)
        out[2, t] = np.percentile(col, 75)
    return out  # rows: q25, q50, q75

def anova_over_time(X_flat, shape, dim1, dim2):
    """Per-(unit,time) one- or two-way ANOVA via batched OLS.

    X_flat: 1D length N*U*T (row-major: trial, unit, time).
    shape:  [N, U, T]
    dim2:   None or empty → 1-way; else 2-way with interaction.

    Returns η² traces (median across units + q25/q75 band) per effect:
      {'shape':[U,T], 'effects':[{name, q25, q50, q75}, ...],
       'eta2_per_unit':{name: (U,T) list-of-lists}}   ← retained but small;
                                                        callers can ignore.
    """
    N, U, T = int(shape[0]), int(shape[1]), int(shape[2])
    Y = np.asarray(X_flat, dtype=float).reshape(N, U, T)
    Y2 = Y.reshape(N, U * T)

    has_d2 = dim2 is not None and len(dim2) == N
    if has_d2:
        d2_arr = np.asarray(dim2)
        if len(np.unique(d2_arr)) < 2:
            has_d2 = False

    if has_d2:
        D, info = _design_2way(np.asarray(dim1), d2_arr)
    else:
        D, info = _design_1way(np.asarray(dim1))

    intercept = [0]
    if has_d2:
        full   = intercept + info['A'] + info['B'] + info['AB']
        no_a   = intercept + info['B']
        no_b   = intercept + info['A']
        no_ab  = intercept + info['A'] + info['B']
    else:
        full   = intercept + info['A']

    df_full = N - len(full)
    if df_full <= 0:
        return {'note': f'too few trials ({N}) for {len(full)} parameters'}
    if (has_d2 and (info['n1'] < 1 or info['n2'] < 1)) or \
       (not has_d2 and info['n1'] < 1):
        return {'note': 'need >=2 levels'}

    ss_full = _ss_resid(D, full, Y2)        # (U*T,)
    if has_d2:
        ss_no_a   = _ss_resid(D, no_a,  Y2)
        ss_no_b   = _ss_resid(D, no_b,  Y2)
        ss_no_ab  = _ss_resid(D, no_ab, Y2)
        ss_a  = np.maximum(0.0, ss_no_a  - ss_no_ab)
        ss_b  = np.maximum(0.0, ss_no_b  - ss_no_ab)
        ss_ab = np.maximum(0.0, ss_no_ab - ss_full)
        ss_by_eff = [('dim1', ss_a), ('dim2', ss_b), ('dim1:dim2', ss_ab)]
    else:
        ss_int = _ss_resid(D, intercept, Y2)
        ss_a   = np.maximum(0.0, ss_int - ss_full)
        ss_by_eff = [('dim1', ss_a)]

    # η² per (unit, time). Mark zero-variance cells NaN so they don't
    # pollute the across-unit summary.
    var_ut = Y.var(axis=0)            # (U, T)
    bad = (var_ut == 0) | ~np.isfinite(var_ut)

    effects = []
    eta_per_eff = {}
    for label, ss in ss_by_eff:
        eta = ss / (ss + ss_full + 1e-30)
        eta_ut = eta.reshape(U, T)
        eta_ut[bad] = np.nan
        eta_per_eff[label] = eta_ut
        q = _quantiles_across_units(eta_ut)
        # Per-unit η² (U flat × T) — enables JS-side resampling by depth
        # band without re-running the fit. None for non-finite (zero-variance
        # cells, etc.) so the JSON stays clean.
        eta_flat = []
        for u in range(U):
            eta_flat.append([None if not np.isfinite(v) else float(v)
                             for v in eta_ut[u]])
        effects.append({
            'name': label,
            'q25': [None if not np.isfinite(v) else float(v) for v in q[0]],
            'q50': [None if not np.isfinite(v) else float(v) for v in q[1]],
            'q75': [None if not np.isfinite(v) else float(v) for v in q[2]],
            'eta_per_unit': eta_flat,    # shape: (U, T), nullable
        })

    return {
        'shape': [int(U), int(T)],
        'n_trials': int(N),
        'df_resid': int(df_full),
        'effects': effects,
    }
`;

  // ─── Pyodide bootstrap ────────────────────────────────────────────────

  let _pyPromise = null;
  let _py = null;

  function emit(phase, message) {
    window.dispatchEvent(new CustomEvent('pyodide-analysis:status', {
      detail: { phase, message },
    }));
  }

  function loadScript(url) {
    return new Promise((resolve, reject) => {
      const s = document.createElement('script');
      s.src = url;
      s.onload = resolve;
      s.onerror = () => reject(new Error('failed to load ' + url));
      document.head.appendChild(s);
    });
  }

  async function bootPyodide() {
    if (_py) return _py;
    if (_pyPromise) return _pyPromise;
    _pyPromise = (async () => {
      emit('loading-runtime', 'fetching Pyodide runtime');
      if (typeof loadPyodide !== 'function') {
        await loadScript(PYODIDE_CDN + 'pyodide.js');
      }
      const py = await loadPyodide({ indexURL: PYODIDE_CDN });
      emit('loading-packages', 'installing numpy + scipy');
      await py.loadPackage(PY_PACKAGES);
      emit('initializing', 'compiling analysis module');
      py.runPython(PY_MODULE);
      _py = py;
      emit('ready', 'analysis kernel ready');
      return py;
    })();
    return _pyPromise;
  }

  // ─── Public API ───────────────────────────────────────────────────────

  /**
   * One- or two-way ANOVA on a per-trial scalar.
   *   trials: { dim1, dim2?, y }    (dim2 optional → 1-way)
   *   labels: { dim1Name, dim2Name?, responseName? }   (display only)
   */
  async function anova(trials, labels) {
    const py = await bootPyodide();
    const { dim1, y } = trials;
    let { dim2 } = trials;
    if (!dim1 || !y) throw new Error('need {dim1, y}');
    if (dim1.length !== y.length) throw new Error('dim1 and y length mismatch');
    if (dim2 != null && dim2.length !== dim1.length) {
      throw new Error('dim2 length mismatch');
    }
    const d1 = Array.from(dim1, v => String(v));
    const d2 = dim2 ? Array.from(dim2, v => String(v)) : null;
    const yy = Float64Array.from(y, v => Number.isFinite(+v) ? +v : NaN);
    py.globals.set('_d1', py.toPy(d1));
    py.globals.set('_d2', d2 ? py.toPy(d2) : py.toPy(null));
    py.globals.set('_y',  py.toPy(yy));
    const res = py.runPython('anova_aggregate(_d1, _d2, _y)')
                  .toJs({ dict_converter: Object.fromEntries });
    if (labels) {
      const map = { dim1: labels.dim1Name,
                    dim2: labels.dim2Name,
                    'dim1:dim2': labels.dim2Name
                      ? `${labels.dim1Name}:${labels.dim2Name}` : null };
      for (const e of res.effects || []) if (map[e.name]) e.name = map[e.name];
      res.response_name = labels.responseName || 'y';
    }
    return res;
  }

  /**
   * Time-resolved per-(unit,time) ANOVA. Returns η² traces only.
   *   X3d:  Float32Array (or number[]) length N*U*T, row-major (trial, unit, time)
   *   dim1: array length N
   *   opts: { shape: [N,U,T], dim2?: array length N }
   */
  async function anovaOverTime(X3d, dim1, opts) {
    const py = await bootPyodide();
    if (!opts || !opts.shape || opts.shape.length !== 3) {
      throw new Error('anovaOverTime requires opts.shape = [N, U, T]');
    }
    const dim2 = opts.dim2 ?? null;
    py.globals.set('_xflat',  py.toPy(Array.from(X3d)));
    py.globals.set('_xshape', py.toPy(opts.shape));
    py.globals.set('_d1', py.toPy(Array.from(dim1, v => String(v))));
    py.globals.set('_d2', dim2 ? py.toPy(Array.from(dim2, v => String(v))) : py.toPy(null));
    emit('running',
      `time-resolved ANOVA: ${opts.shape[1]} units × ${opts.shape[2]} timebins`);
    const res = py.runPython('anova_over_time(_xflat, _xshape, _d1, _d2)')
                  .toJs({ dict_converter: Object.fromEntries });
    emit('idle', 'done');
    return res;
  }

  function isReady() { return _py != null; }
  async function ready() { await bootPyodide(); return true; }

  window.PyodideAnalysis = {
    ready,
    isReady,
    anova,
    anovaOverTime,
    version: PYODIDE_VERSION,
  };
})();
