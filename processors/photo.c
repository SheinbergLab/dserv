/*
 * photo.c - online light-edge detector for a photodiode on an extio
 *           analog input
 *
 * A photodiode taped over a screen patch (stim2's photopatch, or the
 * stimulus itself) is digitized by an extio box and arrives as analog
 * sample blocks. This processor finds each change of light in that stream
 * and publishes ONE small event per edge, timed to the sample:
 *
 *   - the data file gets a handful of edges per trial instead of the whole
 *     stream (log the stream too in a validation session, so the detection
 *     can be re-run offline if the settings turn out wrong);
 *   - pairing an edge with the frame that caused it (stim/photopatch,
 *     stim/frame) is an in-order match, with no signal processing left to
 *     do at extraction;
 *   - ESS can see light arrive while the trial runs.
 *
 * Use the events for control and for timing; the raw stream, when logged,
 * stays the record of what the diode saw (the saccade.c rule).
 *
 * INPUT CONTRACT: an extio analog group block (wiznet-io/common/
 * pico_ain_group.h), DSERV_BYTE:
 *
 *   0 u8 ver (1) | 1 u8 mask | 2 u8 nchan | 3 u8 count | 4 u32 interval_us |
 *   8 u16 flags | 10 u16 reserved | 12 int16[count*nchan], scan-major
 *
 * The datapoint's timestamp is the box's time of scan 0, mapped onto the
 * dserv clock; scan k is at timestamp + k*interval_us. Those are hardware
 * sample times, good to about a microsecond, on the same clock as the obs
 * boundaries and the eye samples (emconf's process_analog uses the same
 * stamps). Little-endian, as every dserv host is.
 *
 * WHAT IS AN EDGE. The signal sits at a level, moves by at least
 * min_step counts (or noise_k noise sd, where the level is noisier than
 * that), and either settles at a new level or turns back:
 *
 *   level      tracked while nothing is happening (EMA, level_tau), with
 *              the noise around it
 *   armed      the signal is min_step away from the level. The departure
 *              is found by walking back to the last sample still inside
 *              the noise (noise_k * sd), and the OLD level is the mean of
 *              the base_dur of samples before it
 *   settled    over the last settle_dur the signal has stopped moving: the
 *              mean of its second half is within settle_frac of the step
 *              (or 3 sd of the noise) of the first half's. The NEW level is
 *              the second half's mean
 *   reversed   before settling, the signal turned back by min_step (a patch
 *              toggled faster than the panel can follow): the edge ends at
 *              its extreme, which is its new level, and the opposite edge
 *              starts there
 *   timeout    neither within max_dur: reported with settled 0
 *
 * The crossing times are interpolated between samples at 10, 50 and 90%
 * of old -> new. t10 is the nearest thing to "light started to change";
 * t90 - t10 is the panel's transition time for that pair of levels; t50
 * is the most precise single time (steepest part, least noise).
 *
 * A backlight driven by PWM ripples at kHz: set `smooth` to a whole number
 * of PWM periods. The smoothing is a boxcar, and each smoothed value is
 * given the MEAN time of its window, so it adds no delay to the reported
 * times.
 *
 * OUTPUT: proc/photo/edge (renamable with the "dpoint" param), DSERV_DOUBLE,
 * 10 values per edge -- one row for each edge the block finished, almost
 * always one:
 *
 *   [0] dir      +1 brighter, -1 darker (in ADC counts; a diode wired
 *                inverted reports the opposite)
 *   [1] t50      crossing of 50%, dserv clock, us (fractional)
 *   [2] t10      crossing of 10%
 *   [3] t90      crossing of 90%
 *   [4] old      level before the edge (counts)
 *   [5] new      level after it (counts): the settled mean, or the extreme
 *                reached when the edge reversed or timed out
 *   [6] settled  1 settled, 0 reversed or timed out
 *   [7] seq      edges since the last reset, from 1
 *   [8] t_arm    the sample at which the edge was detected (us)
 *   [9] t_done   the sample at which it was reported (us): settling, or
 *                the turn
 *
 * The datapoint's TIMESTAMP is the first row's t50, rounded: an edge is
 * logged at the moment the light changed, so it lands in the obs it
 * belongs to however late the block carrying it arrived.
 *
 * BATCHED BLOCKS. A box sends `batch` scans a block to save traffic. The
 * times above do not depend on it -- every sample carries its own time --
 * only WHEN an edge is published does: after the block that finishes it
 * (settling, or the turn) arrives. dserv takes one output datapoint per
 * input, so a block that finishes more than one edge sends them all in
 * that one datapoint, a row each (n_multi counts those blocks); none is
 * held back or lost however long the blocks are.
 *
 * Inactive by default: set active 1 from whatever wants edges. Inactive
 * costs an uncontended lock per block.
 *
 * ATTACHING AND GROUP. The block's name carries the box and the group
 * (extio/<box>/state/ain/<group>), so triggers.tcl attaches the processor
 * once, by pattern, to every analog group (extio/ * /state/ain/ *, without
 * the spaces; dpoint_process.c matches '*'), and the `group` param picks
 * the one to read:
 *
 *   photodiode     that group on any box (the default)
 *   box01/photo    that group on that box only
 *   ""             every block (a test, or a rig with one analog group)
 *
 * Blocks from other groups cost a name compare (n_other). So the group is
 * a setting (`photo group`, the gear), changed live, never a re-attach --
 * re-attaching frees the name the dispatch thread may be comparing. Two
 * live boxes announcing the same group would feed one detector two
 * interleaved streams; qualify it as box/group. dserv runs only the FIRST
 * processor attached to a datapoint, so nothing else can be attached to
 * an analog group while this is.
 *
 * THREADS. onProcess runs on whichever thread sets the block (the box's
 * transport), set/getProcessParams on an ess interp's; each instance has
 * its own mutex, held for a whole block and for each param call. The
 * "dpoint" rename is recorded and installed at the start of the next
 * block, after dserv has copied the previous event (see saccade.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#include <Datapoint.h>
#include <dpoint_process.h>
#include "prmutil.h"

enum { PH_IDLE = 0, PH_EDGE = 1 };

#define RING      4096		/* smoothed samples kept: 0.4 s at 10 kHz   */
#define RING_MASK (RING - 1)
#define RAW_MAX   128		/* longest smoothing window, in samples      */
#define QMAX      32		/* edges one block can finish                */
#define PMAX      512		/* longest strobe flash, in samples          */
#define PER_N     9		/* flash intervals in the period median      */
#define NOUT      10
#define HDR_LEN   12
#define BLOCK_VER 1

typedef struct {
  double v[NOUT];
} edge_out_t;

typedef struct process_params_s {
  /* ---- settable ---- */
  int    active;
  int    channel;		/* column of the block (0-based)              */
  double smooth;		/* s: boxcar before detection (0: none)       */
  double min_step;		/* counts a change must reach to be an edge   */
  double noise_k;		/* departure = noise_k * noise sd from level  */
  double base_dur;		/* s averaged for the old level               */
  double settle_dur;		/* s window that must be flat to settle       */
  double settle_frac;		/* still = halves differ by < this of the step */
  double max_dur;		/* s from departure before giving up          */
  double level_tau;		/* s: EMA of the level and noise when idle    */
  double max_gap;		/* s: a larger step between samples resets    */
  int    strobe;		/* 1: the backlight flashes once a refresh     */
  double period;		/* s: the refresh, strobe mode (0: learn it)   */
  double pulse_thresh;		/* counts above dark that make a flash         */
  char   group[64];		/* analog group to read; "" = any block        */

  /* ---- smoothing ---- */
  double rx[RAW_MAX], rt[RAW_MAX];
  int    r_head, r_n;

  /* ---- smoothed samples, by absolute index ---- */
  double x[RING], t[RING];
  int64_t n_total;		/* samples pushed since the reset             */
  int64_t first_ok;		/* oldest index usable for a backtrack        */
  double last_raw_t;		/* time of the last raw sample (-1: none)     */

  /* ---- strobe: flashes -> one value per refresh ---- */
  int    have_base, in_pulse, pn;
  double base;			/* dark between flashes                        */
  double px[PMAX], pt[PMAX];	/* the flash in progress                       */
  double prev_x, prev_t;
  int    have_prev;
  double last_on;		/* onset of the last item, real or missing    */
  double last_real_on;		/* onset of the last real flash               */
  double pers[PER_N];		/* accepted flash intervals (us)              */
  int    n_pers, pers_head;
  double period_est;		/* us */

  /* ---- level ---- */
  int    have_level;
  double level, nvar;
  int64_t idle_n;

  /* ---- the edge in progress ---- */
  int    state;
  int    dir;
  int64_t i_dep, i_ext;
  double old, ext, t_arm;

  /* ---- sample period and the windows as sample counts ---- */
  double dt_est;		/* us between samples */
  double item_dt;		/* us between detector inputs: dt_est, or the
				   refresh in strobe mode */
  int    n_smooth, n_base, n_settle;

  /* ---- finished edges ---- */
  edge_out_t q[QMAX];
  int    q_n;

  /* ---- counters (read-only) ---- */
  int n_blocks, n_samples, n_edges, n_rising, n_falling, n_unsettled;
  int n_gaps, n_bad, n_stale, n_multi, n_dropped;
  int n_pulses, n_missing, n_long;
  double last_t50;

  /* the group as names to compare: a suffix, or a whole name (box/group) */
  char   g_suffix[96];
  char   g_exact[160];
  char   g_out[64];		/* `group` as returned by getProcessParams     */
  int    n_other;		/* blocks from other groups, ignored           */

  pthread_mutex_t lock;
  char  *pending_name;

  int warned;
  int dummyInt;
  ds_datapoint_t event_dpoint;
} process_params_t;

static void defaults(process_params_t *p)
{
  p->active      = 0;
  p->channel     = 0;
  p->smooth      = 0.0;
  p->min_step    = 100.0;	/* of 4095 on an MCP3204 */
  p->noise_k     = 4.0;
  p->base_dur    = 0.002;
  p->settle_dur  = 0.004;
  p->settle_frac = 0.01;
  p->max_dur     = 0.100;
  p->level_tau   = 0.020;
  p->max_gap     = 0.050;
  p->strobe       = 0;
  p->period       = 0.0;
  p->pulse_thresh = 50.0;
}

/* which blocks are this detector's (see GROUP in the header) */
static void set_group(process_params_t *p, const char *g)
{
  while (*g == ' ' || *g == '\t') g++;
  snprintf(p->group, sizeof(p->group), "%s", g);
  size_t n = strlen(p->group);
  while (n && (p->group[n-1] == ' ' || p->group[n-1] == '\t')) p->group[--n] = 0;
  p->g_suffix[0] = p->g_exact[0] = 0;
  if (!p->group[0]) return;
  char *slash = strchr(p->group, '/');
  if (slash)
    snprintf(p->g_exact, sizeof(p->g_exact), "extio/%.*s/state/ain/%s",
	     (int) (slash - p->group), p->group, slash + 1);
  else
    snprintf(p->g_suffix, sizeof(p->g_suffix), "/state/ain/%s", p->group);
}

static int group_match(process_params_t *p, const char *name)
{
  if (!p->group[0]) return 1;
  if (!name) return 0;
  if (p->g_exact[0]) return !strcmp(name, p->g_exact);
  size_t n = strlen(name), m = strlen(p->g_suffix);
  return n > m && !strncmp(name, "extio/", 6) && !strcmp(name + n - m, p->g_suffix);
}

static int to_samples(double dur_s, double dt_us, int lo, int hi)
{
  int n = dt_us > 0.0 ? (int) lround(dur_s * 1e6 / dt_us) : lo;
  if (n < lo) n = lo;
  if (n > hi) n = hi;
  return n;
}

/* the refresh in strobe mode: the setting, else what was learned (us) */
static double strobe_period(process_params_t *p)
{
  return p->period > 0.0 ? p->period * 1e6 : p->period_est;
}

static void update_counts(process_params_t *p)
{
  /* in strobe mode the detector sees one value a refresh: its windows are
     in refreshes, and at least two of them */
  int lo = p->strobe ? 2 : 1;
  p->item_dt  = p->strobe ? strobe_period(p) : p->dt_est;
  p->n_smooth = to_samples(p->smooth,     p->dt_est,  1, RAW_MAX);
  p->n_base   = to_samples(p->base_dur,   p->item_dt, lo, RING / 8);
  p->n_settle = to_samples(p->settle_dur, p->item_dt, 2, RING / 8);
}

/* forget the signal, keep the counters */
static void reset_stream(process_params_t *p)
{
  p->r_head = p->r_n = 0;
  p->have_base = p->in_pulse = p->pn = p->have_prev = 0;
  p->last_on = p->last_real_on = -1.0;
  p->first_ok = p->n_total;
  p->have_level = 0;
  p->idle_n = 0;
  p->state = PH_IDLE;
}

static void reset_all(process_params_t *p)
{
  p->n_total = 0;
  p->last_raw_t = -1.0;
  reset_stream(p);
  p->q_n = 0;
  p->n_blocks = p->n_samples = p->n_edges = p->n_rising = p->n_falling = 0;
  p->n_unsettled = p->n_gaps = p->n_bad = p->n_stale = 0;
  p->n_multi = p->n_dropped = 0;
  p->n_pulses = p->n_missing = p->n_long = p->n_other = 0;
  p->last_t50 = 0.0;
  p->dt_est = 0.0;
  p->n_pers = p->pers_head = 0;
  p->period_est = 0.0;
  update_counts(p);
}

static inline double X(process_params_t *p, int64_t i) { return p->x[i & RING_MASK]; }
static inline double T(process_params_t *p, int64_t i) { return p->t[i & RING_MASK]; }

void *newProcessParams(void)
{
  process_params_t *p = calloc(1, sizeof(process_params_t));
  defaults(p);
  set_group(p, "photodiode");
  reset_all(p);
  p->warned = 0;

  p->event_dpoint.flags = 0;
  p->event_dpoint.varname = strdup("proc/photo/edge");
  p->event_dpoint.varlen = strlen(p->event_dpoint.varname);
  p->event_dpoint.data.type = DSERV_DOUBLE;
  p->event_dpoint.data.len = NOUT * sizeof(double);
  p->event_dpoint.data.buf = calloc(QMAX * NOUT, sizeof(double));
  pthread_mutex_init(&p->lock, NULL);
  p->pending_name = NULL;
  return p;
}

void freeProcessParams(void *pstruct)
{
  process_params_t *p = (process_params_t *) pstruct;
  pthread_mutex_destroy(&p->lock);
  free(p->pending_name);
  free(p->event_dpoint.varname);
  free(p->event_dpoint.data.buf);
  free(p);
}

#define PARAM_TABLE(p)							\
  { "active",      &p->active,      &p->dummyInt, PU_INT },		\
  { "channel",     &p->channel,     &p->dummyInt, PU_INT },		\
  { "smooth",      &p->smooth,      &p->dummyInt, PU_DOUBLE },		\
  { "min_step",    &p->min_step,    &p->dummyInt, PU_DOUBLE },		\
  { "noise_k",     &p->noise_k,     &p->dummyInt, PU_DOUBLE },		\
  { "base_dur",    &p->base_dur,    &p->dummyInt, PU_DOUBLE },		\
  { "settle_dur",  &p->settle_dur,  &p->dummyInt, PU_DOUBLE },		\
  { "settle_frac", &p->settle_frac, &p->dummyInt, PU_DOUBLE },		\
  { "max_dur",     &p->max_dur,     &p->dummyInt, PU_DOUBLE },		\
  { "level_tau",   &p->level_tau,   &p->dummyInt, PU_DOUBLE },		\
  { "max_gap",     &p->max_gap,     &p->dummyInt, PU_DOUBLE },		\
  { "strobe",      &p->strobe,      &p->dummyInt, PU_INT },		\
  { "period",      &p->period,      &p->dummyInt, PU_DOUBLE },		\
  { "pulse_thresh", &p->pulse_thresh, &p->dummyInt, PU_DOUBLE }

static int get_param_locked(dpoint_process_param_setting_t *pinfo)
{
  process_params_t *p = (process_params_t *) pinfo->params;
  /* read-only views computed on demand */
  double noise = sqrt(p->nvar);
  double level = p->have_level ? p->level : 0.0;
  PARAM_ENTRY params[] = {
    PARAM_TABLE(p),
    { "state",       &p->state,       &p->dummyInt, PU_INT },
    { "level",       &level,          &p->dummyInt, PU_DOUBLE },
    { "noise",       &noise,          &p->dummyInt, PU_DOUBLE },
    { "dt_est",      &p->dt_est,      &p->dummyInt, PU_DOUBLE },
    { "n_smooth",    &p->n_smooth,    &p->dummyInt, PU_INT },
    { "n_base",      &p->n_base,      &p->dummyInt, PU_INT },
    { "n_settle",    &p->n_settle,    &p->dummyInt, PU_INT },
    { "n_blocks",    &p->n_blocks,    &p->dummyInt, PU_INT },
    { "n_samples",   &p->n_samples,   &p->dummyInt, PU_INT },
    { "n_edges",     &p->n_edges,     &p->dummyInt, PU_INT },
    { "n_rising",    &p->n_rising,    &p->dummyInt, PU_INT },
    { "n_falling",   &p->n_falling,   &p->dummyInt, PU_INT },
    { "n_unsettled", &p->n_unsettled, &p->dummyInt, PU_INT },
    { "n_gaps",      &p->n_gaps,      &p->dummyInt, PU_INT },
    { "n_bad",       &p->n_bad,       &p->dummyInt, PU_INT },
    { "n_stale",     &p->n_stale,     &p->dummyInt, PU_INT },
    { "n_multi",     &p->n_multi,     &p->dummyInt, PU_INT },
    { "n_dropped",   &p->n_dropped,   &p->dummyInt, PU_INT },
    { "last_t50",    &p->last_t50,    &p->dummyInt, PU_DOUBLE },
    { "base",        &p->base,        &p->dummyInt, PU_DOUBLE },
    { "period_est",  &p->period_est,  &p->dummyInt, PU_DOUBLE },
    { "item_dt",     &p->item_dt,     &p->dummyInt, PU_DOUBLE },
    { "n_pulses",    &p->n_pulses,    &p->dummyInt, PU_INT },
    { "n_missing",   &p->n_missing,   &p->dummyInt, PU_INT },
    { "n_long",      &p->n_long,      &p->dummyInt, PU_INT },
    { "n_other",     &p->n_other,     &p->dummyInt, PU_INT },
    { "", NULL, NULL, PU_NULL }
  };
  if (!strcmp(pinfo->pname, "group")) {	/* a string: not in the table */
    snprintf(p->g_out, sizeof(p->g_out), "%s", p->group);
    if (pinfo->pval) *pinfo->pval = p->g_out;
    return 1;
  }
  char *result_str = puGetParamEntry(&params[0], pinfo->pname);
  if (result_str && pinfo->pval) {
    *pinfo->pval = result_str;
    return 1;
  }
  return 0;
}

int getProcessParams(dpoint_process_param_setting_t *pinfo)
{
  process_params_t *p = (process_params_t *) pinfo->params;
  pthread_mutex_lock(&p->lock);
  int rc = get_param_locked(pinfo);
  pthread_mutex_unlock(&p->lock);
  return rc;
}

static void clamp_params(process_params_t *p)
{
  if (p->channel < 0) p->channel = 0;
  if (p->smooth < 0.0) p->smooth = 0.0;
  if (p->min_step < 1.0) p->min_step = 1.0;
  if (p->noise_k < 0.0) p->noise_k = 0.0;
  if (p->settle_frac <= 0.0) p->settle_frac = 0.01;
  if (p->level_tau <= 0.0) p->level_tau = 1e-4;
  if (p->period < 0.0) p->period = 0.0;
  if (p->pulse_thresh < 1.0) p->pulse_thresh = 1.0;
  p->strobe = p->strobe ? 1 : 0;
  update_counts(p);
}

static int set_param_locked(dpoint_process_param_setting_t *pinfo)
{
  process_params_t *p = (process_params_t *) pinfo->params;
  char *name = pinfo->pname;
  char **vals = pinfo->pval;

  if (!strcmp(name, "dpoint")) {
    char *nm = malloc(strlen(vals[0]) + strlen("/edge") + 1);
    sprintf(nm, "%s/edge", vals[0]);
    free(p->pending_name);
    p->pending_name = nm;
    return DPOINT_PROCESS_IGNORE;
  }
  if (!strcmp(name, "reset")) {
    reset_all(p);
    return DPOINT_PROCESS_IGNORE;
  }
  if (!strcmp(name, "group")) {
    char was[64];
    snprintf(was, sizeof(was), "%s", p->group);
    set_group(p, vals[0] ? vals[0] : "");
    if (strcmp(was, p->group)) {	/* another signal: forget this one */
      reset_stream(p);
      p->last_raw_t = -1.0;
    }
    return DPOINT_PROCESS_IGNORE;
  }
  if (!strcmp(name, "defaults")) {
    int was_active = p->active;
    defaults(p);
    p->active = was_active;
    update_counts(p);
    return DPOINT_PROCESS_IGNORE;
  }

  int was_active = p->active;
  int was_channel = p->channel;
  int was_smooth = p->n_smooth;
  int was_strobe = p->strobe;
  PARAM_ENTRY params[] = { PARAM_TABLE(p), { "", NULL, NULL, PU_NULL } };
  puSetParamEntry(&params[0], name, 1, vals);
  clamp_params(p);

  /* a new start, a new signal, or a new filter: what is held is stale */
  if ((!was_active && p->active) || p->channel != was_channel ||
      p->n_smooth != was_smooth || p->strobe != was_strobe) {
    reset_stream(p);
    p->last_raw_t = -1.0;
  }
  return DPOINT_PROCESS_IGNORE;
}

int setProcessParams(dpoint_process_param_setting_t *pinfo)
{
  process_params_t *p = (process_params_t *) pinfo->params;
  pthread_mutex_lock(&p->lock);
  int rc = set_param_locked(pinfo);
  pthread_mutex_unlock(&p->lock);
  return rc;
}

/* first time, from i0 on, that the signal reaches `lev` going `dir`,
   interpolated between the samples on either side */
static double crossing(process_params_t *p, int64_t i0, int64_t i1,
		       int dir, double lev)
{
  if (i0 < p->first_ok) i0 = p->first_ok;
  if (dir * (X(p, i0) - lev) >= 0.0) return T(p, i0);
  for (int64_t k = i0 + 1; k <= i1; k++) {
    double xk = X(p, k);
    if (dir * (xk - lev) >= 0.0) {
      /* between two refreshes there was no light to interpolate: the
         answer is the first refresh past the level */
      if (p->strobe) return T(p, k);
      double xa = X(p, k - 1), ta = T(p, k - 1), tb = T(p, k);
      double f = xk != xa ? (lev - xa) / (xk - xa) : 1.0;
      return ta + f * (tb - ta);
    }
  }
  return T(p, i1);
}

/* the edge in progress ends at index i_end with level `nw` */
static void finish(process_params_t *p, int64_t i_end, double nw,
		   int settled, double t_done)
{
  int64_t i0 = p->i_dep - 1;
  double d = nw - p->old;
  edge_out_t e;
  e.v[0] = p->dir;
  e.v[1] = crossing(p, i0, i_end, p->dir, p->old + 0.5 * d);
  e.v[2] = crossing(p, i0, i_end, p->dir, p->old + 0.1 * d);
  e.v[3] = crossing(p, i0, i_end, p->dir, p->old + 0.9 * d);
  e.v[4] = p->old;
  e.v[5] = nw;
  e.v[6] = settled;
  e.v[7] = ++p->n_edges;
  e.v[8] = p->t_arm;
  e.v[9] = t_done;

  if (p->dir > 0) p->n_rising++; else p->n_falling++;
  if (!settled) p->n_unsettled++;
  p->last_t50 = e.v[1];

  if (p->q_n == QMAX) {		/* a block is 24 samples at most */
    p->n_dropped++;
    return;
  }
  p->q[p->q_n++] = e;
}

/* the change that counts as an edge: min_step, or more where the level is
   noisier than that -- a backlight's PWM ripple left by a smoothing window
   that does not span whole periods scales with the light, so the bright
   level of a patch can be far noisier than its dark one */
static double step_thr(process_params_t *p)
{
  double t = p->noise_k * sqrt(p->nvar);
  return t > p->min_step ? t : p->min_step;
}

static void arm(process_params_t *p, int64_t i, int dir)
{
  double thr = p->noise_k * sqrt(p->nvar);
  if (thr < 0.1 * p->min_step) thr = 0.1 * p->min_step;

  /* walk back to the last sample still inside the noise */
  int64_t j = i;
  while (j - 1 >= p->first_ok && i - (j - 1) < RING / 2 &&
	 dir * (X(p, j - 1) - p->level) > thr)
    j--;
  p->i_dep = j;

  /* the old level: the base window before the departure */
  double s = 0.0;
  int n = 0;
  for (int64_t k = j - 1; k >= p->first_ok && n < p->n_base; k--, n++)
    s += X(p, k);
  p->old = n ? s / n : p->level;

  p->dir = dir;
  p->ext = X(p, i);
  p->i_ext = i;
  p->t_arm = T(p, i);
  p->state = PH_EDGE;
}

static void to_idle(process_params_t *p, double lev, int64_t from)
{
  p->state = PH_IDLE;
  p->level = lev;
  p->have_level = 1;
  p->idle_n = p->n_base;	/* the new level is already known */
  p->first_ok = from;		/* no backtrack into the edge just ended */
}

/* one smoothed sample, already in the ring at index i */
static void detect(process_params_t *p, int64_t i)
{
  double x = X(p, i), t = T(p, i);

  if (!p->have_level) {
    p->level = x;
    p->nvar = 0.0;
    p->have_level = 1;
    p->idle_n = 1;
    return;
  }

  if (p->state == PH_IDLE) {
    double d = x - p->level;
    if (fabs(d) >= step_thr(p) && p->idle_n >= p->n_base) {
      arm(p, i, d > 0 ? 1 : -1);
      return;
    }
    double a = p->item_dt > 0.0 ? p->item_dt / (p->level_tau * 1e6) : 0.1;
    if (a > 1.0) a = 1.0;
    /* noise from the deviation BEFORE the level moves */
    p->nvar += a * (d * d - p->nvar);
    p->level += a * d;
    p->idle_n++;
    return;
  }

  /* ---- an edge in progress ---- */
  if (p->dir * (x - p->ext) > 0.0) {
    p->ext = x;
    p->i_ext = i;
  }

  /* turned back before settling: the extreme was this edge's level */
  if (p->dir * (p->ext - x) >= step_thr(p) &&
      fabs(p->ext - p->old) >= p->min_step) {
    double ext = p->ext;
    int64_t i_ext = p->i_ext;
    finish(p, i_ext, ext, 0, t);
    /* the opposite edge starts at that extreme */
    p->dir = -p->dir;
    p->old = ext;
    p->i_dep = i_ext + 1;
    p->ext = x;
    p->i_ext = i;
    p->t_arm = t;
    p->first_ok = i_ext;
    return;
  }

  /* settled: across the last settle window the signal no longer moves --
     the mean of its second half is within settle_frac of the step of the
     first half's (or within the noise). Not "the range is small": a
     panel's approach is exponential, and a range test calls it settled at
     ~1.5 time constants, 20% short of the level. */
  if (i - p->i_dep + 1 >= p->n_settle) {
    int h = p->n_settle / 2;
    double s1 = 0.0, s2 = 0.0;
    for (int64_t k = i - 2 * h + 1; k <= i - h; k++) s1 += X(p, k);
    for (int64_t k = i - h + 1; k <= i; k++) s2 += X(p, k);
    double m1 = s1 / h, m2 = s2 / h;
    /* the new level's own noise, from the second half: it may be much
       larger than the old level's (ripple scales with the light) */
    double v2 = 0.0;
    for (int64_t k = i - h + 1; k <= i; k++) {
      double dv = X(p, k) - m2;
      v2 += dv * dv;
    }
    v2 = h > 1 ? v2 / (h - 1) : 0.0;
    double nv = v2 > p->nvar ? v2 : p->nvar;
    double step = fabs(m2 - p->old);
    double tol = p->settle_frac * step;
    double nfl = 3.0 * sqrt(nv) * sqrt(2.0 / h);	/* 3 sd of m2-m1 */
    if (tol < nfl) tol = nfl;
    if (step >= p->min_step && fabs(m2 - m1) <= tol) {
      finish(p, i, m2, 1, t);
      to_idle(p, m2, i - h + 1);
      p->nvar = v2;
      return;
    }
  }

  /* gave up: too long, or about to run out of ring */
  if (t - T(p, p->i_dep) > p->max_dur * 1e6 ||
      i - p->i_dep > RING - p->n_base - 4) {
    if (fabs(p->ext - p->old) >= p->min_step)
      finish(p, p->i_ext, p->ext, 0, t);
    to_idle(p, x, i);
    p->nvar = 0.0;
  }
}

/* one detector input: a smoothed sample, or a refresh in strobe mode */
static void push_item(process_params_t *p, double t, double x)
{
  int64_t i = p->n_total++;
  p->x[i & RING_MASK] = x;
  p->t[i & RING_MASK] = t;
  /* the ring keeps the newest RING items: nothing older is usable */
  if (p->first_ok < p->n_total - RING) p->first_ok = p->n_total - RING;
  detect(p, i);
}

/* a flash interval, for the learned period: the median of the last PER_N
   that look like one refresh (a missed flash makes a double interval) */
static void add_interval(process_params_t *p, double iv)
{
  if (p->period_est > 0.0 &&
      (iv < 0.5 * p->period_est || iv > 1.5 * p->period_est)) return;
  p->pers[p->pers_head] = iv;
  p->pers_head = (p->pers_head + 1) % PER_N;
  if (p->n_pers < PER_N) p->n_pers++;
  double tmp[PER_N];
  for (int i = 0; i < p->n_pers; i++) {
    double v = p->pers[i];
    int j = i - 1;
    while (j >= 0 && tmp[j] > v) { tmp[j+1] = tmp[j]; j--; }
    tmp[j+1] = v;
  }
  /* three agreeing intervals before trusting it */
  if (p->n_pers >= 3 && tmp[p->n_pers / 2] != p->period_est) {
    p->period_est = tmp[p->n_pers / 2];
    update_counts(p);
  }
}

/* the flash that just ended, in px/pt: its peak above the dark, stamped
   where it rose through half of that */
static void flash_done(process_params_t *p)
{
  double pk = p->px[0];
  for (int k = 1; k < p->pn; k++) if (p->px[k] > pk) pk = p->px[k];
  double half = p->base + 0.5 * (pk - p->base);
  double t_on = p->pt[0];
  for (int k = 1; k < p->pn; k++) {
    if (p->px[k] >= half) {
      double xa = p->px[k-1], xb = p->px[k];
      double f = xb != xa ? (half - xa) / (xb - xa) : 1.0;
      if (f < 0.0) f = 0.0;
      t_on = p->pt[k-1] + f * (p->pt[k] - p->pt[k-1]);
      break;
    }
  }
  if (p->last_real_on >= 0.0) add_interval(p, t_on - p->last_real_on);
  p->last_real_on = p->last_on = t_on;
  p->n_pulses++;
  push_item(p, t_on, pk - p->base);
}

/*
 * Strobe mode. The backlight flashes once a refresh, so even a steady
 * patch is a pulse train, and the light a refresh showed is the size of
 * its flash. Each flash -- a run above dark + pulse_thresh, ended below
 * dark + pulse_thresh/2 -- becomes ONE detector input: its peak above the
 * dark level, at the time it rose through half that peak. A refresh with
 * no flash (a black patch, a flash under pulse_thresh) is a 0 at the time
 * the flash was due, once 1.5 periods pass without one; that needs the
 * period, learned from the flashes or set (`period`, which a dark start
 * needs). The edge logic then runs on those per-refresh values, and an
 * edge's times are the first REFRESH past 10/50/90% -- the question a
 * strobed display can answer.
 */
static void flash(process_params_t *p, double t, double x)
{
  if (!p->have_base) { p->base = x; p->have_base = 1; }
  double on_lev = p->base + p->pulse_thresh;
  double off_lev = p->base + 0.5 * p->pulse_thresh;

  if (!p->in_pulse && x >= on_lev) {
    p->in_pulse = 1;
    p->pn = 0;
    if (p->have_prev) {		/* the dark sample before, for the rise */
      p->px[0] = p->prev_x; p->pt[0] = p->prev_t; p->pn = 1;
    }
  }

  if (p->in_pulse) {
    if (p->pn == PMAX) {	/* no dark between: not a strobed display */
      p->n_long++;
      p->in_pulse = 0;
      p->pn = 0;
    }
    else {
      p->px[p->pn] = x; p->pt[p->pn] = t; p->pn++;
      if (x < off_lev) {
	p->in_pulse = 0;
	flash_done(p);
      }
    }
  }
  else {
    /* dark between flashes: the baseline follows it */
    double a = p->dt_est > 0.0 ? p->dt_est / 50000.0 : 0.01;
    if (a > 1.0) a = 1.0;
    p->base += a * (x - p->base);

    double per = strobe_period(p);
    /* a set period and no flash yet: count dark refreshes from here, so a
       patch that starts black has a level before its first flash */
    if (per > 0.0 && p->last_on < 0.0) p->last_on = t;
    if (per > 0.0) {
      while (t - p->last_on > 1.5 * per) {
	p->last_on += per;
	p->n_missing++;
	push_item(p, p->last_on, 0.0);
      }
    }
  }
  p->prev_x = x; p->prev_t = t; p->have_prev = 1;
}

/* one raw sample: gap check, smoothing, then detection */
static void sample(process_params_t *p, double t, double raw)
{
  if (p->last_raw_t >= 0.0) {
    double dt = t - p->last_raw_t;
    if (dt <= 0.0 || dt > p->max_gap * 1e6) {
      p->n_gaps++;
      if (p->state == PH_EDGE) p->n_dropped++;
      reset_stream(p);
    }
  }
  p->last_raw_t = t;
  p->n_samples++;

  p->rx[p->r_head] = raw;
  p->rt[p->r_head] = t;
  p->r_head = (p->r_head + 1) % RAW_MAX;
  if (p->r_n < RAW_MAX) p->r_n++;
  if (p->r_n < p->n_smooth) return;

  double sx = 0.0, st = 0.0;
  for (int k = 1; k <= p->n_smooth; k++) {
    int ix = (p->r_head - k + RAW_MAX) % RAW_MAX;
    sx += p->rx[ix];
    st += p->rt[ix];
  }
  if (p->strobe) flash(p, st / p->n_smooth, sx / p->n_smooth);
  else push_item(p, st / p->n_smooth, sx / p->n_smooth);
}

static int process_block(process_params_t *p, dpoint_process_info_t *pinfo)
{
  ds_datapoint_t *in = pinfo->input_dpoint;
  const unsigned char *b = in->data.buf;

  if (in->data.type != DSERV_BYTE || in->data.len < HDR_LEN ||
      b[0] != BLOCK_VER) {
    p->n_bad++;
    if (!p->warned) {
      p->warned = 1;
      fprintf(stderr, "photo: ignoring %s (type %d, %u bytes): this processor "
	      "takes extio analog blocks (DSERV_BYTE, ver %d)\n",
	      in->varname ? in->varname : "(unnamed dpoint)",
	      (int) in->data.type, in->data.len, BLOCK_VER);
    }
    return DPOINT_PROCESS_IGNORE;
  }

  int nchan = b[2], count = b[3];
  uint32_t ivl;
  memcpy(&ivl, b + 4, 4);
  if (nchan < 1 || count < 1 ||
      in->data.len < (uint32_t) (HDR_LEN + 2 * nchan * count) ||
      (count > 1 && ivl == 0)) {
    p->n_bad++;
    return DPOINT_PROCESS_IGNORE;
  }
  if (p->channel >= nchan) {
    p->n_bad++;
    if (p->warned != 2) {
      p->warned = 2;
      fprintf(stderr, "photo: channel %d, but %s has %d\n", p->channel,
	      in->varname ? in->varname : "(unnamed dpoint)", nchan);
    }
    return DPOINT_PROCESS_IGNORE;
  }
  p->n_blocks++;

  double t0 = (double) in->timestamp;
  double t_last = t0 + (double) (count - 1) * ivl;

  /* a block already seen (a dservTouch replays the stored one): skip it.
     A real jump back -- a clock re-anchored -- is further than max_gap and
     goes through as a gap. */
  if (p->last_raw_t >= 0.0 && t_last <= p->last_raw_t &&
      p->last_raw_t - t0 <= p->max_gap * 1e6) {
    p->n_stale++;
    goto out;
  }

  if (count > 1 && ivl != p->dt_est) {
    p->dt_est = ivl;
    update_counts(p);
  }
  else if (count == 1 && p->last_raw_t >= 0.0 && t0 > p->last_raw_t &&
	   p->dt_est == 0.0) {
    p->dt_est = t0 - p->last_raw_t;
    update_counts(p);
  }

  for (int k = 0; k < count; k++) {
    int16_t v;
    memcpy(&v, b + HDR_LEN + 2 * (k * nchan + p->channel), 2);
    sample(p, t0 + (double) k * ivl, (double) v);
  }

 out:
  /* every edge this block finished, one row each, in time order */
  if (!p->q_n) return DPOINT_PROCESS_IGNORE;
  memcpy(p->event_dpoint.data.buf, p->q, p->q_n * sizeof(edge_out_t));
  p->event_dpoint.data.len = p->q_n * NOUT * sizeof(double);
  p->event_dpoint.timestamp = (uint64_t) llround(p->q[0].v[1]);
  if (p->q_n > 1) p->n_multi++;
  p->q_n = 0;
  pinfo->dpoint = &p->event_dpoint;
  return DPOINT_PROCESS_DSERV;
}

int onProcess(dpoint_process_info_t *pinfo, void *params)
{
  process_params_t *p = (process_params_t *) params;

  pthread_mutex_lock(&p->lock);
  if (p->pending_name) {
    free(p->event_dpoint.varname);
    p->event_dpoint.varname = p->pending_name;
    p->event_dpoint.varlen = strlen(p->event_dpoint.varname);
    p->pending_name = NULL;
  }
  int rc = DPOINT_PROCESS_IGNORE;
  if (p->active) {
    if (group_match(p, pinfo->input_dpoint->varname)) rc = process_block(p, pinfo);
    else p->n_other++;
  }
  pthread_mutex_unlock(&p->lock);
  return rc;
}
