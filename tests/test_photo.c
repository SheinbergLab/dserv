/*
 * test_photo.c -- processors/photo.c on synthetic extio analog blocks.
 *
 * The signals are analytic, so every edge has a true time:
 *   - an LCD: a first-order (exponential) approach to each new level;
 *   - a strobed display (a Display++): a trapezoid flash every refresh,
 *     its height the patch's level when it fires;
 * with deterministic Gaussian noise, a PWM ripple, two channels, replayed
 * blocks and a gap. Also the '*' in an attached name (dpoint_process.c),
 * through the real attach and dispatch.
 *
 * photo.c is #included so its statics and its process_params_t are in
 * reach; dpoint_process.c and prmutil.c are linked.
 */

#include "../processors/photo.c"

/* dpoint_process.c: registers a processor without a shared object */
void add_process_function_info(char *name, void *handle,
                               DPOINT_PROCESS_FUNC pfunc,
                               DPOINT_PROCESS_NEWPARAM_FUNC newparamfunc,
                               DPOINT_PROCESS_FREEPARAM_FUNC freeparamfunc,
                               DPOINT_PROCESS_SETPARAM_FUNC setparamfunc,
                               DPOINT_PROCESS_SETPARAM_FUNC getparamfunc);

static int fails = 0;
#define CHECK(cond, ...) do {						\
    if (cond) { printf("  ok: "); printf(__VA_ARGS__); printf("\n"); }	\
    else { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } \
  } while (0)

/* ---------- noise ---------- */
static uint64_t rng = 88172645463325252ull;
static double urand(void)
{
  rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
  return ((rng >> 11) + 0.5) / 9007199254740992.0;
}
static double gauss(void)
{
  return sqrt(-2.0 * log(urand())) * cos(2.0 * M_PI * urand());
}

/* ---------- signals ---------- */
#define MAXSTEP 512
typedef struct {
  int    n;
  double t[MAXSTEP];		/* us: when the patch changed       */
  double lev[MAXSTEP];		/* the level it changed to          */
  double lev0;			/* before the first change          */
  double tau;			/* us: LCD time constant            */
  double pwm_hz, pwm_frac;	/* backlight ripple, a fraction of level */
  /* strobe */
  int    strobe;
  double per, phase;		/* us */
  double dark;			/* counts between flashes           */
  double boxcar;		/* us: the truth is this smoothed   */
} lsig_t;

static double cmd_at(const lsig_t *s, double t)
{
  double v = s->lev0;
  for (int i = 0; i < s->n && s->t[i] <= t; i++) v = s->lev[i];
  return v;
}

/* the noiseless LCD output: a sum of exponential approaches */
static double lcd(const lsig_t *s, double t)
{
  double v = s->lev0, prev = s->lev0;
  for (int i = 0; i < s->n && s->t[i] < t; i++) {
    v += (s->lev[i] - prev) * (1.0 - exp(-(t - s->t[i]) / s->tau));
    prev = s->lev[i];
  }
  if (s->pwm_hz > 0.0) {
    double ph = fmod(t * 1e-6 * s->pwm_hz, 1.0);
    v += (ph < 0.5 ? 1.0 : -1.0) * s->pwm_frac * v;
  }
  return v;
}

/* a flash: 0.4 ms up, 1 ms flat, 0.4 ms down; half height 0.2 ms in */
#define FL_RISE 400.0
#define FL_TOP  1000.0
static double flash_shape(double u)
{
  if (u < 0.0) return 0.0;
  if (u < FL_RISE) return u / FL_RISE;
  if (u < FL_RISE + FL_TOP) return 1.0;
  if (u < 2 * FL_RISE + FL_TOP) return 1.0 - (u - FL_RISE - FL_TOP) / FL_RISE;
  return 0.0;
}

/* refresh n fires at phase + n*per with the level the patch had then */
static double strobed(const lsig_t *s, double t)
{
  double n = floor((t - s->phase) / s->per);
  double tr = s->phase + n * s->per;
  return s->dark + cmd_at(s, tr) * flash_shape(t - tr);
}

static double clean(const lsig_t *s, double t)
{
  if (s->boxcar > 0.0) {	/* what a `smooth` window makes of it */
    double sum = 0.0;
    int n = 0;
    for (double u = -0.5 * s->boxcar; u <= 0.5 * s->boxcar; u += 10.0, n++)
      sum += s->strobe ? strobed(s, t + u) : lcd(s, t + u);
    return sum / n;
  }
  return s->strobe ? strobed(s, t) : lcd(s, t);
}

/* first time after t0 the noiseless signal reaches lev going dir (1 us) */
static double true_cross(const lsig_t *s, double t0, double t1, int dir, double lev)
{
  for (double t = t0; t <= t1; t += 1.0)
    if (dir * (clean(s, t) - lev) >= 0.0) return t;
  return -1.0;
}

/* ---------- feeding blocks ---------- */
#define MAXEDGE 1024
typedef struct {
  int n;
  double e[MAXEDGE][NOUT];
  uint64_t ts[MAXEDGE];
} edges_t;

static unsigned char blockbuf[HDR_LEN + 2 * 4 * 255];

static int feed_block(process_params_t *p, const char *name, uint64_t t0,
		      uint32_t ivl, int count, int nchan, const int16_t *v,
		      edges_t *out)
{
  blockbuf[0] = BLOCK_VER; blockbuf[1] = (1 << nchan) - 1;
  blockbuf[2] = nchan; blockbuf[3] = count;
  memcpy(blockbuf + 4, &ivl, 4);
  memset(blockbuf + 8, 0, 4);
  memcpy(blockbuf + HDR_LEN, v, 2 * nchan * count);

  ds_datapoint_t in = { 0 };
  in.timestamp = t0;
  in.varname = (char *) name;
  in.data.type = DSERV_BYTE;
  in.data.len = HDR_LEN + 2 * nchan * count;
  in.data.buf = blockbuf;

  dpoint_process_info_t pi = { 0 };
  pi.input_dpoint = &in;
  int rc = onProcess(&pi, p);
  if (rc == DPOINT_PROCESS_DSERV && out) {
    /* a row per edge the block finished */
    int rows = pi.dpoint->data.len / (NOUT * sizeof(double));
    const double *d = (const double *) pi.dpoint->data.buf;
    for (int r = 0; r < rows && out->n < MAXEDGE; r++) {
      memcpy(out->e[out->n], d + r * NOUT, NOUT * sizeof(double));
      out->ts[out->n] = r == 0 ? pi.dpoint->timestamp : 0;
      out->n++;
    }
  }
  return rc;
}

/* sample the signal from t_start for dur us at `rate` Hz, `batch` scans a
   block; channel `chan` of `nchan` carries it, the others noise */
static void run(process_params_t *p, const lsig_t *s, double t_start,
		double dur, double rate, int batch, double noise,
		int nchan, int chan, edges_t *out)
{
  uint32_t ivl = (uint32_t) lround(1e6 / rate);
  int16_t v[4 * 255];
  double t = t_start;
  while (t < t_start + dur) {
    for (int k = 0; k < batch; k++) {
      double tk = t + k * (double) ivl;
      for (int c = 0; c < nchan; c++) {
	double x = c == chan ? clean(s, tk) + noise * gauss()
	                     : 2000.0 + 1500.0 * (urand() < 0.01) + 50 * gauss();
	if (x < 0) x = 0;
	if (x > 4095) x = 4095;
	v[k * nchan + c] = (int16_t) lround(x);
      }
    }
    feed_block(p, "extio/box01/state/ain/photodiode", (uint64_t) t, ivl,
	       batch, nchan, v, out);
    t += batch * (double) ivl;
  }
}

static process_params_t *fresh(void)
{
  process_params_t *p = newProcessParams();
  p->active = 1;
  return p;
}

static void set(process_params_t *p, const char *name, const char *val)
{
  char *v = (char *) val;
  dpoint_process_param_setting_t ps = { 0 };
  ps.pname = (char *) name;
  ps.pval = &v;
  ps.params = p;
  setProcessParams(&ps);
}

/* each reported crossing against the true curve at the reported level */
static void check_times(const lsig_t *s, const edges_t *E, int i, double t_from,
			double tol10, double tol50, double tol90, const char *what)
{
  const double *e = E->e[i];
  int dir = (int) e[0];
  double old = e[4], nw = e[5];
  double w50 = true_cross(s, t_from, t_from + 60000, dir, old + 0.5 * (nw - old));
  double w10 = true_cross(s, t_from, t_from + 60000, dir, old + 0.1 * (nw - old));
  double w90 = true_cross(s, t_from, t_from + 60000, dir, old + 0.9 * (nw - old));
  CHECK(fabs(e[1] - w50) <= tol50, "%s: t50 %+.1f us from truth (tol %.0f)",
	what, e[1] - w50, tol50);
  CHECK(fabs(e[2] - w10) <= tol10, "%s: t10 %+.1f us from truth (tol %.0f)",
	what, e[2] - w10, tol10);
  CHECK(fabs(e[3] - w90) <= tol90, "%s: t90 %+.1f us from truth (tol %.0f)",
	what, e[3] - w90, tol90);
}

#define T0 1795127461000000.0	/* a dserv clock in 2026, us */

int main(void)
{
  edges_t E;

  /* ---------------- LCD: one step up, one down ---------------- */
  printf("LCD steps (tau 2 ms, 10 kHz, batch 10, noise sd 5):\n");
  {
    lsig_t s = { 0 };
    s.lev0 = 500; s.tau = 2000;
    s.n = 2;
    s.t[0] = T0 + 100000.3; s.lev[0] = 3000;
    s.t[1] = T0 + 200000.7; s.lev[1] = 500;
    process_params_t *p = fresh();
    E.n = 0;
    run(p, &s, T0, 300000, 10000, 10, 5.0, 1, 0, &E);
    CHECK(E.n == 2, "two edges (got %d)", E.n);
    if (E.n == 2) {
      CHECK(E.e[0][0] == 1 && E.e[1][0] == -1, "up then down");
      CHECK(E.e[0][6] == 1 && E.e[1][6] == 1, "both settled");
      CHECK(fabs(E.e[0][4] - 500) < 10, "old level %.1f ~ 500", E.e[0][4]);
      CHECK(fabs(E.e[0][5] - 3000) < 0.03 * 2500, "new level %.1f within 3%% of 3000",
	    E.e[0][5]);
      check_times(&s, &E, 0, s.t[0], 60, 30, 150, "rise");
      check_times(&s, &E, 1, s.t[1], 60, 30, 150, "fall");
      CHECK(E.ts[0] == (uint64_t) llround(E.e[0][1]), "timestamp is t50");
      CHECK(E.e[0][7] == 1 && E.e[1][7] == 2, "seq 1, 2");
      CHECK(E.e[0][9] >= E.e[0][8] && E.e[0][9] >= E.e[0][3], "t_arm, t90 <= t_done");
    }
    freeProcessParams(p);
  }

  /* ---------------- noise only ---------------- */
  printf("noise only (sd 20, 2 s):\n");
  {
    lsig_t s = { 0 };
    s.lev0 = 1200; s.tau = 2000;
    process_params_t *p = fresh();
    E.n = 0;
    run(p, &s, T0, 2e6, 10000, 10, 20.0, 1, 0, &E);
    CHECK(E.n == 0, "no edges (got %d)", E.n);
    freeProcessParams(p);
  }

  /* ---------------- inactive ---------------- */
  printf("inactive:\n");
  {
    lsig_t s = { 0 };
    s.lev0 = 500; s.tau = 2000; s.n = 1; s.t[0] = T0 + 50000; s.lev[0] = 3000;
    process_params_t *p = newProcessParams();
    E.n = 0;
    run(p, &s, T0, 100000, 10000, 10, 5.0, 1, 0, &E);
    CHECK(E.n == 0 && p->n_samples == 0, "nothing seen while inactive");
    freeProcessParams(p);
  }

  /* ---------------- toggling faster than the panel settles ---------------- */
  printf("toggle every refresh at 120 Hz (tau 3 ms: never settles):\n");
  {
    lsig_t s = { 0 };
    s.lev0 = 400; s.tau = 3000;
    double per = 1e6 / 120.0;
    s.n = 60;
    for (int i = 0; i < s.n; i++) {
      s.t[i] = T0 + 50000 + i * per;
      s.lev[i] = (i % 2 == 0) ? 3200 : 400;
    }
    process_params_t *p = fresh();
    E.n = 0;
    run(p, &s, T0, 50000 + 61 * per, 10000, 10, 5.0, 1, 0, &E);
    CHECK(E.n >= 59 && E.n <= 60, "an edge per toggle (got %d of 60)", E.n);
    int alt = 1, worst = 0;
    double worst_d = 0;
    for (int i = 1; i < E.n; i++) if (E.e[i][0] == E.e[i-1][0]) alt = 0;
    CHECK(alt, "directions alternate");
    /* t50 of each against the true curve after its own toggle */
    for (int i = 0; i < E.n && i < s.n; i++) {
      int dir = (int) E.e[i][0];
      double lev = E.e[i][4] + 0.5 * (E.e[i][5] - E.e[i][4]);
      double w = true_cross(&s, s.t[i], s.t[i] + per, dir, lev);
      double d = fabs(E.e[i][1] - w);
      if (d > worst_d) { worst_d = d; worst = i; }
    }
    CHECK(worst_d <= 40, "every t50 within 40 us of truth (worst %.1f, edge %d)",
	  worst_d, worst);
    CHECK(p->n_dropped == 0, "none dropped");
    freeProcessParams(p);
  }

  /* ---------------- PWM ripple on the backlight ---------------- */
  printf("2033 Hz PWM ripple of +-10%% (smooth 2 ms, ~4 periods):\n");
  {
    lsig_t s = { 0 };
    s.lev0 = 300; s.tau = 2000; s.pwm_hz = 2033; s.pwm_frac = 0.10;
    s.n = 2; s.t[0] = T0 + 100000; s.lev[0] = 3000;
    s.t[1] = T0 + 200000; s.lev[1] = 300;
    process_params_t *p = fresh();
    set(p, "smooth", "0.002");
    set(p, "active", "1");
    E.n = 0;
    run(p, &s, T0, 300000, 10000, 10, 5.0, 1, 0, &E);
    CHECK(E.n == 2, "smoothed: two edges (got %d)", E.n);
    if (E.n == 2) {
      /* against the ripple-free curve, smoothed the same way: a window
	 moves the crossings of a curved edge (here t50 by ~+125 us), so
	 the times describe the smoothed light */
      lsig_t s2 = s; s2.pwm_hz = 0; s2.boxcar = 2000;
      /* the ripple the window leaves (4.07 periods in 20 samples: ~+-25
	 counts at the top) is the error: small where the edge is steep,
	 large at 90% of a slow approach */
      check_times(&s2, &E, 0, s.t[0] - 1500, 60, 80, 600, "rise");
      check_times(&s2, &E, 1, s.t[1] - 1500, 60, 80, 600, "fall");
    }
    freeProcessParams(p);
  }

  /* ---------------- channel select ---------------- */
  printf("two channels, the diode on channel 1:\n");
  {
    lsig_t s = { 0 };
    s.lev0 = 500; s.tau = 2000; s.n = 1; s.t[0] = T0 + 100000; s.lev[0] = 3000;
    process_params_t *p = fresh();
    set(p, "channel", "1");
    E.n = 0;
    run(p, &s, T0, 200000, 10000, 10, 5.0, 2, 1, &E);
    CHECK(E.n == 1 && E.e[0][0] == 1, "one rising edge (got %d)", E.n);
    freeProcessParams(p);

    p = fresh();
    set(p, "channel", "2");
    E.n = 0;
    run(p, &s, T0, 20000, 10000, 10, 5.0, 2, 1, &E);
    CHECK(E.n == 0 && p->n_bad > 0, "a channel the block lacks is refused");
    freeProcessParams(p);
  }

  /* ---------------- replayed block, then a gap ---------------- */
  printf("a replayed block and a gap:\n");
  {
    process_params_t *p = fresh();
    set(p, "group", "");		/* the blocks here are named "x" */
    int16_t v[10];
    for (int k = 0; k < 10; k++) v[k] = 500;
    feed_block(p, "x", (uint64_t) T0, 100, 10, 1, v, NULL);
    feed_block(p, "x", (uint64_t) T0, 100, 10, 1, v, NULL);	/* dservTouch */
    CHECK(p->n_stale == 1 && p->n_gaps == 0, "replay skipped, not a gap");
    feed_block(p, "x", (uint64_t) (T0 + 1000000), 100, 10, 1, v, NULL);
    CHECK(p->n_gaps == 1, "1 s later: a gap");
    freeProcessParams(p);
  }

  /* ---------------- slow blocks: two edges in one block ---------------- */
  printf("edges closer than a block (1 kHz, batch 24):\n");
  {
    lsig_t s = { 0 };
    s.lev0 = 500; s.tau = 300;
    s.n = 20;
    for (int i = 0; i < s.n; i++) {
      s.t[i] = T0 + 100000 + i * 12000.0;
      s.lev[i] = (i % 2 == 0) ? 3000 : 500;
    }
    process_params_t *p = fresh();
    E.n = 0;
    run(p, &s, T0, 400000, 1000, 24, 5.0, 1, 0, &E);
    int inorder = 1;
    for (int i = 1; i < E.n; i++) if (E.e[i][7] != E.e[i-1][7] + 1) inorder = 0;
    CHECK(E.n == 20, "all 20 delivered (got %d)", E.n);
    CHECK(p->n_multi > 0 && inorder, "some blocks carried two, rows in order (%d)",
	  p->n_multi);
    CHECK(p->n_dropped == 0, "none dropped");
    freeProcessParams(p);
  }

  /* ---------------- strobed display ---------------- */
  printf("strobed (Display++-like: a flash every 8.33 ms):\n");
  {
    double per = 1e6 / 120.0;
    lsig_t s = { 0 };
    s.strobe = 1; s.per = per; s.phase = T0 + 1234.5; s.dark = 80;
    s.lev0 = 0;			/* a black patch: no flash */
    s.n = 6;
    for (int i = 0; i < s.n; i++) {
      s.t[i] = T0 + 200000 + i * 150000.0;
      s.lev[i] = (i % 2 == 0) ? 2000 : 0;
    }

    /* without strobe mode every flash is an edge */
    process_params_t *p = fresh();
    E.n = 0;
    run(p, &s, T0, 1100000, 10000, 10, 5.0, 1, 0, &E);
    CHECK(E.n > 50, "continuous mode: each flash an edge (%d)", E.n);
    freeProcessParams(p);

    /* strobe mode, period set: a black start has a level */
    p = fresh();
    set(p, "strobe", "1");
    set(p, "period", "0.0083333333");
    E.n = 0;
    run(p, &s, T0, 1100000, 10000, 10, 5.0, 1, 0, &E);
    CHECK(E.n == 6, "strobe mode: one edge per change (got %d)", E.n);
    CHECK(p->n_long == 0, "every flash ended (n_long %d)", p->n_long);
    for (int i = 0; i < E.n && i < s.n; i++) {
      /* the first refresh at or after the change, at its half height */
      double n = ceil((s.t[i] - s.phase) / per);
      double want = s.phase + n * per + 0.5 * FL_RISE;
      int dir = s.lev[i] > 0 ? 1 : -1;
      CHECK(E.e[i][0] == dir && fabs(E.e[i][1] - want) <= 60,
	    "%s: t50 %+.1f us from the first changed refresh",
	    dir > 0 ? "on " : "off", E.e[i][1] - want);
    }
    if (E.n >= 1)
      CHECK(fabs(E.e[0][5] - (2000 + 0) ) < 60 && fabs(E.e[0][4]) < 1,
	    "levels are flash peaks above dark: %.0f -> %.0f", E.e[0][4], E.e[0][5]);
    freeProcessParams(p);

    /* strobe mode, period learned, a grey patch (always a flash) */
    s.lev0 = 600;
    for (int i = 0; i < s.n; i++) s.lev[i] = (i % 2 == 0) ? 2000 : 600;
    p = fresh();
    set(p, "strobe", "1");
    E.n = 0;
    run(p, &s, T0, 1100000, 10000, 10, 5.0, 1, 0, &E);
    CHECK(fabs(p->period_est - per) < 0.01 * per, "period learned: %.1f us",
	  p->period_est);
    CHECK(E.n == 6, "grey <-> white: one edge per change (got %d)", E.n);
    for (int i = 0; i < E.n && i < s.n; i++) {
      double n = ceil((s.t[i] - s.phase) / per);
      double want = s.phase + n * per + 0.5 * FL_RISE;
      CHECK(fabs(E.e[i][1] - want) <= 60, "edge %d: t50 %+.1f us", i,
	    E.e[i][1] - want);
    }
    CHECK(p->n_missing == 0, "no refresh missing");
    freeProcessParams(p);
  }

  /* ---------------- '*' in an attached name ---------------- */
  printf("attach by pattern (dpoint_process.c):\n");
  {
    add_process_function_info("photo", NULL, onProcess, newProcessParams,
			      (DPOINT_PROCESS_FREEPARAM_FUNC) freeProcessParams,
			      setProcessParams, getProcessParams);
    /* as triggers.tcl does it: every analog group, `group` picks one */
    CHECK(process_attach("ph", "extio/*/state/ain/*", "photo") == 0,
	  "attached");
    ds_datapoint_t *out = NULL;
    process_set_param("ph", "active", "1", 0, 0, &out);

    lsig_t s = { 0 };
    s.lev0 = 500; s.tau = 2000; s.n = 1; s.t[0] = T0 + 50000; s.lev[0] = 3000;
    int hits = 0, others = 0;
    uint32_t ivl = 100;
    for (double t = T0; t < T0 + 100000; t += 1000) {
      int16_t v[10];
      for (int k = 0; k < 10; k++) v[k] = (int16_t) lround(clean(&s, t + k * ivl));
      blockbuf[0] = BLOCK_VER; blockbuf[1] = 1; blockbuf[2] = 1; blockbuf[3] = 10;
      memcpy(blockbuf + 4, &ivl, 4);
      memcpy(blockbuf + HDR_LEN, v, 20);
      ds_datapoint_t in = { 0 };
      in.timestamp = (uint64_t) t;
      in.data.type = DSERV_BYTE;
      in.data.len = HDR_LEN + 20;
      in.data.buf = blockbuf;
      in.varname = "extio/box07/state/ain/photodiode";
      if (process_dpoint(&in, &out) == DPOINT_PROCESS_DSERV) hits++;
      in.varname = "extio/box07/state/ain/eye";
      if (process_dpoint(&in, &out) != DPOINT_PROCESS_IGNORE) others++;
    }
    CHECK(hits == 1, "any box's photodiode group gives its edge (%d)", hits);
    CHECK(others == 0, "another group gives nothing");
    char *g = process_get_param("ph", "n_blocks", 0);
    CHECK(g && atoi(g) == 100, "100 photodiode blocks read (%s)", g ? g : "none");
    g = process_get_param("ph", "n_other", 0);
    CHECK(g && atoi(g) == 100, "100 eye blocks passed over (%s)", g ? g : "none");
    g = process_get_param("ph", "group", 0);
    CHECK(g && !strcmp(g, "photodiode"), "group reads back (%s)", g ? g : "none");
    ds_datapoint_t meta = { 0 };
    meta.varname = "extio/box07/state/ain/group/photodiode/chans";
    meta.data.type = DSERV_STRING;
    meta.data.len = 1;
    meta.data.buf = (unsigned char *) "0";
    process_dpoint(&meta, &out);
    g = process_get_param("ph", "n_bad", 0);
    CHECK(g && atoi(g) == 0, "the group's metadata is not mistaken for it");
  }

  /* ---------------- group ---------------- */
  printf("group:\n");
  {
    process_params_t *p = fresh();
    CHECK(group_match(p, "extio/box01/state/ain/photodiode") &&
	  group_match(p, "extio/b2/state/ain/photodiode") &&
	  !group_match(p, "extio/box01/state/ain/eye") &&
	  !group_match(p, "extio/box01/state/ain/label/photodiode") &&
	  !group_match(p, "extio/box01/state/ain/photodiode2") &&
	  !group_match(p, "other/state/ain/photodiode"),
	  "photodiode: that group on any box, nothing else");
    set(p, "group", " box01/photodiode ");
    CHECK(group_match(p, "extio/box01/state/ain/photodiode") &&
	  !group_match(p, "extio/box02/state/ain/photodiode"),
	  "box01/photodiode: that box only (trimmed)");
    set(p, "group", "");
    CHECK(group_match(p, "anything/at/all"), "empty: every block");

    /* changing it forgets the signal it was reading */
    set(p, "group", "photodiode");
    lsig_t s = { 0 };
    s.lev0 = 500; s.tau = 2000;
    E.n = 0;
    run(p, &s, T0, 20000, 10000, 10, 5.0, 1, 0, &E);
    CHECK(p->have_level, "reading");
    set(p, "group", "photo2");
    CHECK(!p->have_level && p->last_raw_t < 0, "a new group starts clean");
    set(p, "defaults", "1");
    CHECK(!strcmp(p->group, "photo2"), "defaults leave the group alone");
    freeProcessParams(p);
  }

  /* ---------------- invert: fewer counts = more light ---------------- */
  printf("invert (an LM393 module's analog out: light pulls it down):\n");
  {
    lsig_t s = { 0 };
    s.lev0 = 2200; s.tau = 2000;	/* dark */
    s.n = 2;
    s.t[0] = T0 + 100000.3; s.lev[0] = 1550;	/* lit: counts fall */
    s.t[1] = T0 + 200000.7; s.lev[1] = 2200;	/* dark again       */
    process_params_t *p = fresh();
    E.n = 0;
    run(p, &s, T0, 300000, 5000, 20, 5.0, 1, 0, &E);
    CHECK(E.n == 2 && E.e[0][0] == -1 && E.e[1][0] == 1,
	  "without it: darker, then brighter -- backwards (got %d edges)", E.n);
    freeProcessParams(p);

    p = fresh();
    set(p, "invert", "1");
    E.n = 0;
    run(p, &s, T0, 300000, 5000, 20, 5.0, 1, 0, &E);
    CHECK(E.n == 2, "two edges (got %d)", E.n);
    if (E.n == 2) {
      CHECK(E.e[0][0] == 1 && E.e[1][0] == -1, "brighter when lit, then darker");
      CHECK(fabs(E.e[0][4] - 2200) < 10 && fabs(E.e[0][5] - 1550) < 0.03 * 650,
	    "old/new in the box's counts (%.0f -> %.0f)", E.e[0][4], E.e[0][5]);
      double old = E.e[0][4], nw = E.e[0][5];
      double w50 = true_cross(&s, s.t[0], s.t[0] + 60000, -1, old + 0.5 * (nw - old));
      CHECK(fabs(E.e[0][1] - w50) <= 60, "lit: t50 %+.1f us from truth", E.e[0][1] - w50);
      old = E.e[1][4]; nw = E.e[1][5];
      w50 = true_cross(&s, s.t[1], s.t[1] + 60000, 1, old + 0.5 * (nw - old));
      CHECK(fabs(E.e[1][1] - w50) <= 60, "dark: t50 %+.1f us from truth", E.e[1][1] - w50);
    }
    char *lv = NULL;
    dpoint_process_param_setting_t ps = { 0 };
    ps.pname = "level"; ps.pval = &lv; ps.params = p;
    getProcessParams(&ps);
    CHECK(lv && fabs(atof(lv) - 2200) < 20, "level reads in the box's counts (%s)",
	  lv ? lv : "none");
    CHECK(p->have_level, "reading");
    set(p, "invert", "0");
    CHECK(!p->have_level && p->last_raw_t < 0, "changing invert starts clean");
    freeProcessParams(p);
  }

  if (fails) {
    printf("%d FAILURE(S)\n", fails);
    return 1;
  }
  printf("all checks passed\n");
  return 0;
}
