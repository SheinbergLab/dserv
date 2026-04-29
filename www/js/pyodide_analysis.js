/**
 * pyodide_analysis.js — in-browser statistical analysis kernel
 *
 * Lazy-loads Pyodide (CPython + numpy/pandas/scipy/statsmodels/scikit-learn)
 * the first time an analysis is requested, then exposes a small API for
 * the kinds of quantitative follow-ups the spike_explorer 2D summary
 * naturally raises:
 *
 *   • twoWayANOVA(trials)       — selectivity for dim1, dim2, dim1×dim2
 *                                 on a per-trial scalar (e.g. spike count
 *                                 in a window). Reports F, p, df, and
 *                                 partial η² for each effect.
 *
 *   • decodePopulation(X, y)    — cross-validated population decoding
 *                                 with a "shuffle within condition"
 *                                 control, so the gap between the two
 *                                 quantifies information carried by
 *                                 simultaneous (vs independent) trials.
 *
 * Inputs are plain JS arrays / typed arrays. Outputs are plain JS
 * objects. The module never touches App state directly — callers extract
 * trial-level numbers however they like (the helper countSpikesInWindow
 * below mirrors what the heatmap pass already does) and hand them in.
 *
 * Dispatches CustomEvents on window for UI progress:
 *   'pyodide-analysis:status' { detail: { phase, message } }
 *
 * Usage:
 *   await PyodideAnalysis.ready();             // first call: ~10MB download
 *   const r = await PyodideAnalysis.twoWayANOVA({
 *     dim1: ['A','A','B','B', ...],
 *     dim2: ['1','2','1','2', ...],
 *     y:    [12, 8, 4, 6,  ...],
 *   });
 *   console.log(r.effects);  // [{name:'dim1', F, p, df, partial_eta2}, ...]
 */

(function () {
  'use strict';

  // Pin a version so we get reproducible behavior; bump when intentional.
  const PYODIDE_VERSION = '0.26.4';
  const PYODIDE_CDN = `https://cdn.jsdelivr.net/pyodide/v${PYODIDE_VERSION}/full/`;

  // Packages loaded eagerly once Pyodide boots. statsmodels pulls in
  // patsy + scipy + numpy; scikit-learn pulls in joblib + threadpoolctl.
  const PY_PACKAGES = ['numpy', 'pandas', 'scipy', 'statsmodels', 'scikit-learn'];

  // Python module installed into the Pyodide FS once. Keeping it as a
  // single string makes the JS file self-contained.
  const PY_MODULE = `
import numpy as np
import pandas as pd
from scipy import stats as _scistats

def two_way_anova(dim1, dim2, y):
    """Type-II two-way ANOVA. Returns a list of effects with F, p, df,
    and partial eta-squared. Falls back to one-way if a dimension is
    constant. Empty cells are tolerated by statsmodels' formula path."""
    import statsmodels.formula.api as smf
    import statsmodels.api as sm
    df = pd.DataFrame({'d1': dim1, 'd2': dim2, 'y': np.asarray(y, dtype=float)})
    df = df.dropna(subset=['y'])
    n = len(df)
    n1 = df['d1'].nunique()
    n2 = df['d2'].nunique()
    out = {'n': int(n), 'n_levels': {'dim1': int(n1), 'dim2': int(n2)},
           'effects': [], 'note': None}
    if n < 4 or (n1 < 2 and n2 < 2):
        out['note'] = 'not enough data for ANOVA'
        return out
    if n1 < 2:
        formula, terms = 'y ~ C(d2)', [('dim2', 'C(d2)')]
    elif n2 < 2:
        formula, terms = 'y ~ C(d1)', [('dim1', 'C(d1)')]
    else:
        formula = 'y ~ C(d1) + C(d2) + C(d1):C(d2)'
        terms = [('dim1', 'C(d1)'), ('dim2', 'C(d2)'),
                 ('dim1:dim2', 'C(d1):C(d2)')]
    try:
        model = smf.ols(formula, data=df).fit()
    except Exception as e:
        out['note'] = f'fit failed: {e}'
        return out
    # Type II is the standard choice for unbalanced designs.
    table = sm.stats.anova_lm(model, typ=2)
    ss_resid = float(table.loc['Residual', 'sum_sq'])
    for label, term in terms:
        if term not in table.index:
            continue
        ss = float(table.loc[term, 'sum_sq'])
        df_term = float(table.loc[term, 'df'])
        F = float(table.loc[term, 'F'])
        p = float(table.loc[term, 'PR(>F)'])
        denom = ss + ss_resid
        peta2 = float(ss / denom) if denom > 0 else float('nan')
        out['effects'].append({
            'name': label, 'F': F, 'p': p,
            'df': df_term, 'partial_eta2': peta2, 'sum_sq': ss,
        })
    out['ss_residual'] = ss_resid
    out['df_residual'] = float(table.loc['Residual', 'df'])
    return out

def decode_population(X, y, n_folds=5, n_shuffles=200, classifier='logreg', seed=0):
    """Stratified-CV decoding accuracy on (N_trials × N_units) X.

    Compares 'real' (simultaneous) trials vs 'shuffled' (each unit
    independently re-paired with a trial of the same label). The gap is
    the contribution of simultaneous-trial structure beyond per-unit
    selectivity. Permutation p-value: fraction of shuffles >= real."""
    from sklearn.linear_model import LogisticRegression
    from sklearn.discriminant_analysis import LinearDiscriminantAnalysis
    from sklearn.model_selection import StratifiedKFold, cross_val_score
    from sklearn.preprocessing import StandardScaler
    from sklearn.pipeline import Pipeline

    X = np.asarray(X, dtype=float)
    y = np.asarray(y)
    if X.ndim != 2 or X.shape[0] != y.shape[0]:
        raise ValueError(f'X must be (N_trials, N_units); got {X.shape} vs y {y.shape}')

    # Drop classes with too few trials for stratified CV.
    classes, counts = np.unique(y, return_counts=True)
    keep_classes = classes[counts >= n_folds]
    mask = np.isin(y, keep_classes)
    X, y = X[mask], y[mask]
    if len(np.unique(y)) < 2:
        return {'note': 'fewer than 2 classes with enough trials',
                'n_trials': int(X.shape[0]), 'n_units': int(X.shape[1])}

    if classifier == 'lda':
        clf = Pipeline([('s', StandardScaler()),
                        ('m', LinearDiscriminantAnalysis())])
    else:
        clf = Pipeline([('s', StandardScaler()),
                        ('m', LogisticRegression(max_iter=1000,
                                                 multi_class='auto'))])
    cv = StratifiedKFold(n_splits=n_folds, shuffle=True, random_state=seed)

    acc_real = cross_val_score(clf, X, y, cv=cv, scoring='accuracy')

    rng = np.random.default_rng(seed)
    shuffled = np.empty(n_shuffles, dtype=float)
    Xs = X.copy()
    n_units = X.shape[1]
    # Within-condition shuffle: for each class, permute the trial order
    # of each unit independently. Per-unit marginals are preserved; the
    # joint trial-by-trial structure is broken.
    class_idx = {c: np.where(y == c)[0] for c in np.unique(y)}
    for s in range(n_shuffles):
        for idx in class_idx.values():
            for u in range(n_units):
                Xs[idx, u] = X[rng.permutation(idx), u]
        shuffled[s] = cross_val_score(clf, Xs, y, cv=cv,
                                      scoring='accuracy').mean()

    real_mean = float(acc_real.mean())
    shuf_mean = float(shuffled.mean())
    chance = float(1.0 / len(np.unique(y)))
    # Permutation p: how often does the null reach the observed real mean?
    p = float((np.sum(shuffled >= real_mean) + 1) / (n_shuffles + 1))

    def ci(arr):
        lo, hi = np.percentile(arr, [2.5, 97.5])
        return [float(lo), float(hi)]

    return {
        'n_trials': int(X.shape[0]),
        'n_units': int(X.shape[1]),
        'n_classes': int(len(np.unique(y))),
        'classes': [str(c) for c in np.unique(y).tolist()],
        'chance': chance,
        'acc_real_mean': real_mean,
        'acc_real_folds': [float(v) for v in acc_real],
        'acc_real_ci': ci(acc_real) if len(acc_real) >= 4 else None,
        'acc_shuffled_mean': shuf_mean,
        'acc_shuffled_ci': ci(shuffled),
        'gain': real_mean - shuf_mean,
        'p_value': p,
        'n_shuffles': int(n_shuffles),
    }
`;

  // ────────────────────────────────────────────────────────────────────
  // Pyodide bootstrap (lazy, single-flight)
  // ────────────────────────────────────────────────────────────────────

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
      emit('loading-packages', 'installing scientific stack');
      await py.loadPackage(PY_PACKAGES);
      emit('initializing', 'compiling analysis module');
      py.runPython(PY_MODULE);
      _py = py;
      emit('ready', 'analysis kernel ready');
      return py;
    })();
    return _pyPromise;
  }

  // ────────────────────────────────────────────────────────────────────
  // JS-side helpers (extract trial-level numbers from spike arrays).
  // These mirror the count-in-window pass the heatmap already does, so
  // the analyses see the same data the user sees.
  // ────────────────────────────────────────────────────────────────────

  /**
   * Count spikes in [tMin, tMax] (and optional [dLo, dHi]) for one trial.
   *   spkT, spkD: typed arrays of spike times / depths for this trial's
   *               canonical source row.
   *   align:      time origin (e.g. stim_on) in same clock as spkT.
   * Returns integer count.
   */
  function countSpikesInWindow(spkT, spkD, align, tMin, tMax,
                                dLo, dHi, missing) {
    if (!spkT) return 0;
    const useDepth = (dLo != null && dHi != null);
    const miss = (missing == null) ? -Infinity : missing;
    let n = 0;
    for (let k = 0; k < spkT.length; k++) {
      const at = spkT[k] - align;
      if (at < tMin || at >= tMax) continue;
      if (useDepth) {
        const d = spkD ? spkD[k] : 0;
        if (d <= miss || d < dLo || d >= dHi) continue;
      }
      n++;
    }
    return n;
  }

  /**
   * Build a (N_trials × N_depthBins) firing-rate matrix for population
   * decoding. Treats each depth bin as a pseudo-unit (same scheme used
   * by the heatmap). Rates are spikes/second.
   *   trialSpikes: array of {spkT, spkD, align} per trial
   *   tMin, tMax in ms relative to align
   *   dLo, dHi, nDbins, missing as in the heatmap
   */
  function buildPopulationMatrix(trialSpikes, tMin, tMax,
                                  dLo, dHi, nDbins, missing) {
    const N = trialSpikes.length;
    const X = new Float32Array(N * nDbins);
    const tWinSec = Math.max(1e-9, (tMax - tMin) / 1000);
    const dRange = (dHi - dLo) || 1;
    const binDw = dRange / nDbins;
    const miss = (missing == null) ? -Infinity : missing;
    for (let i = 0; i < N; i++) {
      const { spkT, spkD, align } = trialSpikes[i];
      if (!spkT || !Number.isFinite(align)) continue;
      const row = i * nDbins;
      for (let k = 0; k < spkT.length; k++) {
        const at = spkT[k] - align;
        if (at < tMin || at >= tMax) continue;
        const d = spkD ? spkD[k] : 0;
        if (d <= miss || d < dLo || d >= dHi) continue;
        const dbi = Math.min(nDbins - 1, ((d - dLo) / binDw) | 0);
        X[row + dbi] += 1;
      }
      // Convert counts to rate.
      for (let b = 0; b < nDbins; b++) X[row + b] /= tWinSec;
    }
    return { X, shape: [N, nDbins] };
  }

  // ────────────────────────────────────────────────────────────────────
  // Public API
  // ────────────────────────────────────────────────────────────────────

  /**
   * Two-way ANOVA on per-trial scalar response.
   *   trials: { dim1: array, dim2: array, y: array } — equal length
   * Optional names for prettier output:
   *   labels: { dim1Name, dim2Name, responseName }
   */
  async function twoWayANOVA(trials, labels) {
    const py = await bootPyodide();
    const { dim1, dim2, y } = trials;
    if (!dim1 || !dim2 || !y) throw new Error('need {dim1, dim2, y}');
    if (dim1.length !== dim2.length || dim1.length !== y.length) {
      throw new Error('dim1, dim2, y must be the same length');
    }
    // Stringify factor levels (numeric levels are fine, but Python side
    // treats everything categorical via C(...)).
    const d1 = Array.from(dim1, v => String(v));
    const d2 = Array.from(dim2, v => String(v));
    const yy = Float64Array.from(y, v => Number.isFinite(+v) ? +v : NaN);
    const ns = py.toPy({ d1, d2, y: yy });
    py.globals.set('_in', ns);
    const res = py.runPython(
      'two_way_anova(_in["d1"], _in["d2"], _in["y"])'
    ).toJs({ dict_converter: Object.fromEntries });
    ns.destroy();
    if (labels) {
      const map = { dim1: labels.dim1Name, dim2: labels.dim2Name,
                    'dim1:dim2': `${labels.dim1Name}:${labels.dim2Name}` };
      for (const e of res.effects || []) if (map[e.name]) e.name = map[e.name];
      res.response_name = labels.responseName || 'y';
    }
    return res;
  }

  /**
   * Population decoding with simultaneous-vs-shuffled comparison.
   *   X: Float32Array | number[][]  (N × U)  — typed-array form must
   *      be paired with `shape: [N, U]` in opts.
   *   y: array of N labels
   *   opts: { shape?, nFolds=5, nShuffles=200, classifier='logreg', seed=0 }
   */
  async function decodePopulation(X, y, opts = {}) {
    const py = await bootPyodide();
    const nFolds = opts.nFolds ?? 5;
    const nShuffles = opts.nShuffles ?? 200;
    const classifier = opts.classifier ?? 'logreg';
    const seed = opts.seed ?? 0;

    let Xpy;
    if (ArrayBuffer.isView(X)) {
      if (!opts.shape || opts.shape.length !== 2) {
        throw new Error('typed-array X requires opts.shape = [N, U]');
      }
      // Ship the flat buffer + shape; reshape on the Python side.
      py.globals.set('_xflat', py.toPy(Array.from(X)));
      py.globals.set('_xshape', py.toPy(opts.shape));
      Xpy = py.runPython(
        'np.asarray(_xflat, dtype=float).reshape(_xshape)'
      );
    } else {
      Xpy = py.toPy(X);
    }
    const ypy = py.toPy(Array.from(y, v => String(v)));
    py.globals.set('_X', Xpy);
    py.globals.set('_y', ypy);
    py.globals.set('_kw', py.toPy({
      n_folds: nFolds, n_shuffles: nShuffles,
      classifier, seed,
    }));
    emit('running', `decoding (${nShuffles} shuffles)`);
    const res = py.runPython(
      'decode_population(_X, _y, **_kw)'
    ).toJs({ dict_converter: Object.fromEntries });
    Xpy.destroy(); ypy.destroy();
    emit('idle', 'done');
    return res;
  }

  // True iff Pyodide is fully loaded and ready (no work pending).
  function isReady() { return _py != null; }

  // Pre-warm — useful to call from a "Load analysis kernel" button so
  // the heavy download doesn't block the first analysis click.
  async function ready() { await bootPyodide(); return true; }

  window.PyodideAnalysis = {
    ready,
    isReady,
    twoWayANOVA,
    decodePopulation,
    // Helpers exposed so callers can shape data without duplicating logic.
    countSpikesInWindow,
    buildPopulationMatrix,
    version: PYODIDE_VERSION,
  };
})();
