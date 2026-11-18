/*
 * saccade.c - online (causal) saccade detector
 *
 * Fires an event the moment a saccade is under way, for saccade-contingent
 * displays (change an element DURING the saccade). This is the realtime
 * counterpart of the offline detector in dlsh/pkgs/em/em.c, and it uses the
 * same parameter names where the meaning is the same (vel_thresh,
 * vel_thresh_low, coast_dur, min_isi). It CANNOT do what the
 * offline one does after onset -- the PSO split by signed-velocity zero
 * crossing and the median landing windows need samples that have not
 * arrived yet -- so the offset event here is a coasted estimate, and the
 * offline extraction stays the record of what the eye did. Use this
 * processor's events for control, and the dg for analysis. Logging the
 * events lets you check them against em::extract_events offline.
 *
 * INPUT CONTRACT: eyetracking/sample, DSERV_DOUBLE [h_deg, v_deg, t_s, valid],
 * published by every eye source in config/emconf.tcl next to
 * eyetracking/position.
 *
 *   t_s    the sample's OWN time in seconds (camera frame time for video,
 *          the box's scan time for analog). Velocity is a displacement over
 *          a time difference, so it has to be the capture time, not the
 *          arrival time: at 250 Hz, 0.5 ms of transport jitter is a 12%
 *          speed error, right at threshold. The clock may jump (VideoStream
 *          re-anchors at file open); a non-positive or too-large step is
 *          treated as a gap and resets the history.
 *   valid  0 while the source has no real position (P1/P4 lost, blink).
 *          emconf HOLDS the last position through a loss, so the first
 *          sample after reacquisition jumps -- without this flag that
 *          jump reads as a saccade.
 *
 * Why not eyetracking/position: that is a 2-float contract other consumers
 * log and decode (windows.c, stim2 gazewin, planko's gaze_source), and
 * process_dpoint only runs the FIRST processor attached to a varname, so
 * windows already owns it.
 *
 * OUTPUT: proc/saccade/event (renamable with the "dpoint" param), DSERV_DOUBLE[11]
 *
 *   [0] event      1 = onset    speed stayed >= vel_thresh for run_dur
 *                  2 = landed   primary saccade over: velocity along the onset
 *                               direction reversed (the PSO overshoot) -- the
 *                               causal half of em.c's primary/PSO split. Fires
 *                               well before offset, so "change on landing"
 *                               paradigms should key on this, not offset.
 *                  3 = offset   net speed over the last coast_dur is below
 *                               vel_thresh_low: saccade AND PSO are over
 *                  4 = aborted  signal lost, clock gap, or max_dur exceeded
 *   [1] t_sample   the sample time (s, input clock) at which the event fired
 *   [2] t_onset    estimated movement onset (s): the last sample before
 *                  speed rose above vel_thresh_low
 *   [3] from_h     pre-saccade position (deg): the mean of the base_dur
 *   [4] from_v     of samples ending just before onset
 *   [5] h          position at the firing sample (deg)
 *   [6] v
 *   [7] amp        |(h,v) - from| (deg). At onset: distance covered so far.
 *                  At landed: pre-PSO landing (em.c sac_to_*_raw). At offset:
 *                  settled post-PSO landing (em.c sac_amps).
 *   [8] dir        atan2(v - from_v, h - from_h) in degrees. At onset it is
 *                  already a usable direction estimate.
 *   [9] peak_vel   peak speed so far (deg/s)
 *  [10] dur        at onset: t_sample - t_onset (detection delay within the
 *                  data stream); at landed/offset: primary duration (onset to
 *                  the last sample still moving forward)
 *
 * The datapoint's timestamp is the input sample's dserv timestamp, so
 * [now] minus it gives the latency from the sample reaching dserv to the
 * event reaching a consumer.
 *
 * Inactive by default (like windows): set active 1 from the system that
 * wants events. Inactive costs one branch per sample.
 *
 * SAMPLE-RATE INDEPENDENCE. Every parameter is in seconds, deg or deg/s,
 * never in samples, so the same settings mean the same thing at 250 Hz and
 * at 400 Hz. Speeds use each sample's own time. The four windows that work
 * on whole samples (run_dur, vel_win, base_dur, holdoff_dur) are converted
 * to sample counts with dt_est, the median of the last DT_N sample steps:
 * a median ignores a dropped frame, and it follows a rate change within a
 * few samples. Defaults equal the old 250 Hz counts exactly (2, 1, 4 and 3
 * samples). Read n_run, n_span, n_base, n_holdoff and dt_est to see what
 * they came to. (em.c's `runlength` is a sample count, so at 250 Hz
 * runlength 2 matches run_dur 0.008 here.)
 *
 * vel_win also keeps velocity NOISE roughly rate-independent: a one-sample
 * difference has noise ~ sqrt(2) * position noise / dt, so it grows with
 * the rate; differencing over a fixed time instead holds it steady.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include <Datapoint.h>
#include <dpoint_process.h>
#include "prmutil.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { SAC_FIX = 0, SAC_IN_FLIGHT = 1 };
enum { SAC_EV_ONSET = 1, SAC_EV_LANDED = 2, SAC_EV_OFFSET = 3, SAC_EV_ABORT = 4 };

#define HIST     64		/* ring length: base + backtrack, even at 1 kHz      */
#define DT_N     9		/* sample steps in the dt_est median                 */
#define MAX_SPAN 16
#define MAX_BASE 32
#define NOUT     11

typedef struct process_params_s {
  /* ---- settable ---- */
  int    active;
  double vel_thresh;		/* deg/s: speed that counts toward an onset      */
  double vel_thresh_low;	/* deg/s: onset backtrack and coasted offset     */
  double run_dur;		/* s of speed >= vel_thresh before onset fires   */
  double vel_win;		/* s spanned by the backward velocity difference */
  double coast_dur;		/* s window whose net speed < vel_thresh_low ends it */
  double min_isi;		/* s after an offset/abort before a new onset    */
  double max_dur;		/* s cap from onset; longer is not a saccade     */
  double max_gap;		/* s: a larger sample step resets the history    */
  double holdoff_dur;		/* s skipped after the signal returns            */
  double base_dur;		/* s of pre-onset samples averaged for `from`    */
  double min_onset_disp;	/* deg moved from `from` before onset may fire   */

  /* ---- state ---- */
  int    state;
  int    n_hist;		/* contiguous valid samples in the ring          */
  int    head;			/* index of the newest sample                    */
  double h[HIST], v[HIST], t[HIST], speed[HIST];
  int    run;
  int    holdoff_left;
  double last_end_t;		/* time of the last offset/abort (-inf if none)  */
  int    have_end;
  double onset_t, detect_t, from_h, from_v, peak;
  double ux, uy;		/* unit onset direction, for signed velocity     */
  int    landed;		/* landed event already sent for this saccade    */
  double prim_end_t;		/* last sample still moving forward              */
  double last_speed;

  /* ---- sample period, and the windows converted to samples ---- */
  double dts[DT_N];		/* persistent across resets: the rate is a     */
  int    n_dts, dts_head;	/* property of the source, not of a stretch    */
  double dt_est;
  int    n_run, n_span, n_base, n_holdoff;

  /* ---- counters (read-only) ---- */
  int n_samples, n_onsets, n_landed, n_offsets, n_aborts, n_invalid, n_gaps;

  int warned_type;
  int dummyInt;
  ds_datapoint_t event_dpoint;
} process_params_t;

static void defaults(process_params_t *p)
{
  p->active          = 0;
  p->vel_thresh      = 30.0;
  p->vel_thresh_low  = 10.0;
  p->run_dur         = 0.008;	/* 2 samples at 250 Hz */
  p->vel_win         = 0.004;	/* 1 sample  at 250 Hz */
  p->coast_dur       = 0.012;
  p->min_isi         = 0.020;
  p->max_dur         = 0.150;
  p->max_gap         = 0.050;
  p->holdoff_dur     = 0.012;	/* 3 samples at 250 Hz */
  p->base_dur        = 0.016;	/* 4 samples at 250 Hz */
  p->min_onset_disp  = 0.0;
}

static void reset_history(process_params_t *p)
{
  p->n_hist = 0;
  p->run = 0;
  p->state = SAC_FIX;
}

static void reset_all(process_params_t *p)
{
  reset_history(p);
  p->holdoff_left = 0;
  p->have_end = 0;
  p->last_speed = 0.0;
  p->n_samples = p->n_onsets = p->n_landed = p->n_offsets = p->n_aborts = 0;
  p->n_invalid = p->n_gaps = 0;
  p->n_dts = p->dts_head = 0;
  p->dt_est = 0.0;
}

/* a duration as a whole number of samples at the current rate */
static int to_samples(double dur, double dt, int lo, int hi)
{
  int n = dt > 0.0 ? (int) lround(dur / dt) : lo;
  if (n < lo) n = lo;
  if (n > hi) n = hi;
  return n;
}

static void update_counts(process_params_t *p)
{
  p->n_run     = to_samples(p->run_dur,     p->dt_est, 1, HIST / 2);
  p->n_span    = to_samples(p->vel_win,     p->dt_est, 1, MAX_SPAN);
  p->n_base    = to_samples(p->base_dur,    p->dt_est, 1, MAX_BASE);
  p->n_holdoff = to_samples(p->holdoff_dur, p->dt_est, 0, HIST);
}

/* add one sample step and re-take the median */
static void add_dt(process_params_t *p, double dt)
{
  double tmp[DT_N];
  p->dts[p->dts_head] = dt;
  p->dts_head = (p->dts_head + 1) % DT_N;
  if (p->n_dts < DT_N) p->n_dts++;
  for (int i = 0; i < p->n_dts; i++) {	/* insertion sort, n <= 9 */
    double x = p->dts[i];
    int j = i - 1;
    while (j >= 0 && tmp[j] > x) { tmp[j+1] = tmp[j]; j--; }
    tmp[j+1] = x;
  }
  double est = tmp[p->n_dts / 2];
  if (est != p->dt_est) {
    p->dt_est = est;
    update_counts(p);
  }
}

/* k samples back from the newest (0 = newest); caller checks k < n_hist */
static inline int idx_back(process_params_t *p, int k)
{
  return (p->head - k + HIST) % HIST;
}

void *newProcessParams(void)
{
  process_params_t *p = calloc(1, sizeof(process_params_t));
  defaults(p);
  reset_all(p);
  update_counts(p);
  p->warned_type = -1;

  p->event_dpoint.flags = 0;
  p->event_dpoint.varname = strdup("proc/saccade/event");
  p->event_dpoint.varlen = strlen(p->event_dpoint.varname);
  p->event_dpoint.data.type = DSERV_DOUBLE;
  p->event_dpoint.data.len = NOUT * sizeof(double);
  p->event_dpoint.data.buf = calloc(NOUT, sizeof(double));
  return p;
}

void freeProcessParams(void *pstruct)
{
  process_params_t *p = (process_params_t *) pstruct;
  free(p->event_dpoint.varname);
  free(p->event_dpoint.data.buf);
  free(p);
}

#define PARAM_TABLE(p)							\
  { "active",          &p->active,          &p->dummyInt, PU_INT },	\
  { "vel_thresh",      &p->vel_thresh,      &p->dummyInt, PU_DOUBLE },	\
  { "vel_thresh_low",  &p->vel_thresh_low,  &p->dummyInt, PU_DOUBLE },	\
  { "run_dur",         &p->run_dur,         &p->dummyInt, PU_DOUBLE },	\
  { "vel_win",         &p->vel_win,         &p->dummyInt, PU_DOUBLE },	\
  { "coast_dur",       &p->coast_dur,       &p->dummyInt, PU_DOUBLE },	\
  { "min_isi",         &p->min_isi,         &p->dummyInt, PU_DOUBLE },	\
  { "max_dur",         &p->max_dur,         &p->dummyInt, PU_DOUBLE },	\
  { "max_gap",         &p->max_gap,         &p->dummyInt, PU_DOUBLE },	\
  { "holdoff_dur",     &p->holdoff_dur,     &p->dummyInt, PU_DOUBLE },	\
  { "base_dur",        &p->base_dur,        &p->dummyInt, PU_DOUBLE },	\
  { "min_onset_disp",  &p->min_onset_disp,  &p->dummyInt, PU_DOUBLE }

int getProcessParams(dpoint_process_param_setting_t *pinfo)
{
  process_params_t *p = (process_params_t *) pinfo->params;
  PARAM_ENTRY params[] = {
    PARAM_TABLE(p),
    { "state",      &p->state,      &p->dummyInt, PU_INT },
    { "n_samples",  &p->n_samples,  &p->dummyInt, PU_INT },
    { "n_onsets",   &p->n_onsets,   &p->dummyInt, PU_INT },
    { "n_landed",   &p->n_landed,   &p->dummyInt, PU_INT },
    { "n_offsets",  &p->n_offsets,  &p->dummyInt, PU_INT },
    { "n_aborts",   &p->n_aborts,   &p->dummyInt, PU_INT },
    { "n_invalid",  &p->n_invalid,  &p->dummyInt, PU_INT },
    { "n_gaps",     &p->n_gaps,     &p->dummyInt, PU_INT },
    { "last_speed", &p->last_speed, &p->dummyInt, PU_DOUBLE },
    { "dt_est",     &p->dt_est,     &p->dummyInt, PU_DOUBLE },
    { "n_run",      &p->n_run,      &p->dummyInt, PU_INT },
    { "n_span",     &p->n_span,     &p->dummyInt, PU_INT },
    { "n_base",     &p->n_base,     &p->dummyInt, PU_INT },
    { "n_holdoff",  &p->n_holdoff,  &p->dummyInt, PU_INT },
    { "", NULL, NULL, PU_NULL }
  };
  char *result_str = puGetParamEntry(&params[0], pinfo->pname);
  if (result_str && pinfo->pval) {
    *pinfo->pval = result_str;
    return 1;
  }
  return 0;
}

static void clamp_params(process_params_t *p)
{
  /* sample-count bounds are applied in to_samples */
  if (p->run_dur < 0.0) p->run_dur = 0.0;
  if (p->vel_win < 0.0) p->vel_win = 0.0;
  if (p->base_dur < 0.0) p->base_dur = 0.0;
  if (p->holdoff_dur < 0.0) p->holdoff_dur = 0.0;
  update_counts(p);
}

int setProcessParams(dpoint_process_param_setting_t *pinfo)
{
  process_params_t *p = (process_params_t *) pinfo->params;
  char *name = pinfo->pname;
  char **vals = pinfo->pval;

  if (!strcmp(name, "dpoint")) {
    free(p->event_dpoint.varname);
    p->event_dpoint.varname = malloc(strlen(vals[0]) + strlen("/event") + 1);
    sprintf(p->event_dpoint.varname, "%s/event", vals[0]);
    p->event_dpoint.varlen = strlen(p->event_dpoint.varname);
    return DPOINT_PROCESS_IGNORE;
  }
  if (!strcmp(name, "reset")) {
    reset_all(p);
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
  PARAM_ENTRY params[] = { PARAM_TABLE(p), { "", NULL, NULL, PU_NULL } };
  puSetParamEntry(&params[0], name, 1, vals);
  clamp_params(p);

  /* switching on starts from a clean slate: history from before it was
     off may be arbitrarily old */
  if (!was_active && p->active) reset_history(p);
  return DPOINT_PROCESS_IGNORE;
}

static int emit(process_params_t *p, dpoint_process_info_t *pinfo, int event,
		double t, double h, double v, double dur)
{
  double dh = h - p->from_h, dv = v - p->from_v;
  double *o = (double *) p->event_dpoint.data.buf;
  o[0]  = event;
  o[1]  = t;
  o[2]  = p->onset_t;
  o[3]  = p->from_h;
  o[4]  = p->from_v;
  o[5]  = h;
  o[6]  = v;
  o[7]  = sqrt(dh*dh + dv*dv);
  o[8]  = atan2(dv, dh) * (180.0 / M_PI);
  o[9]  = p->peak;
  o[10] = dur;
  p->event_dpoint.timestamp = pinfo->input_dpoint->timestamp;
  pinfo->dpoint = &p->event_dpoint;
  return DPOINT_PROCESS_DSERV;
}

/* end an in-flight saccade without an offset (signal loss, gap, timeout) */
static int abort_flight(process_params_t *p, dpoint_process_info_t *pinfo,
			double t, double h, double v)
{
  p->n_aborts++;
  p->state = SAC_FIX;
  p->run = 0;
  p->last_end_t = t;
  p->have_end = 1;
  return emit(p, pinfo, SAC_EV_ABORT, t, h, v, t - p->onset_t);
}

int onProcess(dpoint_process_info_t *pinfo, void *params)
{
  process_params_t *p = (process_params_t *) params;
  ds_datapoint_t *in = pinfo->input_dpoint;

  if (!p->active) return DPOINT_PROCESS_IGNORE;

  if (in->data.type != DSERV_DOUBLE || in->data.len < 4 * sizeof(double)) {
    if (p->warned_type != (int) in->data.type) {
      p->warned_type = (int) in->data.type;
      fprintf(stderr,
	      "saccade: ignoring %s (type %d, %u bytes): this processor requires "
	      "DSERV_DOUBLE (%d) [h_deg v_deg t_s valid]\n",
	      in->varname ? in->varname : "(unnamed dpoint)",
	      (int) in->data.type, in->data.len, (int) DSERV_DOUBLE);
    }
    return DPOINT_PROCESS_IGNORE;
  }

  const double *d = (const double *) in->data.buf;
  double h = d[0], v = d[1], t = d[2];
  int valid = d[3] != 0.0 && isfinite(h) && isfinite(v) && isfinite(t);
  p->n_samples++;

  /* ---- signal loss: no position, so no velocity ---- */
  if (!valid) {
    p->n_invalid++;
    int rc = DPOINT_PROCESS_IGNORE;
    if (p->state == SAC_IN_FLIGHT) {
      /* report the last known position, not the held one */
      int k = idx_back(p, 0);
      rc = abort_flight(p, pinfo, p->t[k], p->h[k], p->v[k]);
    }
    reset_history(p);
    p->holdoff_left = p->n_holdoff;
    return rc;
  }

  /* ---- clock step: a gap, a reset, or a backwards jump ---- */
  if (p->n_hist > 0) {
    double dt = t - p->t[idx_back(p, 0)];
    if (dt <= 0.0 || dt > p->max_gap) {
      p->n_gaps++;
      int rc = DPOINT_PROCESS_IGNORE;
      if (p->state == SAC_IN_FLIGHT) rc = abort_flight(p, pinfo, t, h, v);
      reset_history(p);
      /* the clock may not be the same clock any more: an end time from
         before the step would gate min_isi against the wrong timeline
         (a backwards reset would suppress onsets until it caught up) */
      p->have_end = 0;
      /* this sample starts the new history */
      p->head = 0;
      p->h[0] = h; p->v[0] = v; p->t[0] = t; p->speed[0] = 0.0;
      p->n_hist = 1;
      return rc;
    }
  }

  if (p->n_hist > 0) add_dt(p, t - p->t[idx_back(p, 0)]);

  /* ---- push ---- */
  p->head = (p->head + 1) % HIST;
  p->h[p->head] = h; p->v[p->head] = v; p->t[p->head] = t;
  if (p->n_hist < HIST) p->n_hist++;

  /* settling after reacquisition: keep only the newest sample, so the
     first velocity computed afterwards spans post-holdoff samples only */
  if (p->holdoff_left > 0) {
    p->holdoff_left--;
    p->speed[p->head] = 0.0;
    p->n_hist = 1;
    return DPOINT_PROCESS_IGNORE;
  }

  /* ---- speed: causal backward difference ---- */
  double speed = 0.0;
  {
    int span = p->n_span < p->n_hist - 1 ? p->n_span : p->n_hist - 1;
    if (span >= 1) {
      int k = idx_back(p, span);
      double dt = t - p->t[k];
      double dh = h - p->h[k], dv = v - p->v[k];
      if (dt > 0.0) speed = sqrt(dh*dh + dv*dv) / dt;
    }
  }
  p->speed[p->head] = speed;
  p->last_speed = speed;

  /* need at least one real velocity before anything can be decided */
  if (p->n_hist < 2) return DPOINT_PROCESS_IGNORE;

  if (p->state == SAC_FIX) {
    if (p->have_end && t - p->last_end_t < p->min_isi) {
      p->run = 0;
      return DPOINT_PROCESS_IGNORE;
    }
    if (speed >= p->vel_thresh) p->run++;
    else p->run = 0;
    if (p->run < p->n_run) return DPOINT_PROCESS_IGNORE;

    /* onset: walk back while speed stays above the low threshold. Speed at
       sample j is the displacement from j-span to j, so the eye was still
       at j-1 (exactly for span 1; for a wider span the rise is smeared over
       span samples and this lands up to span-1 samples early). */
    int k = 0;
    while (k + 1 < p->n_hist - 1 &&
	   p->speed[idx_back(p, k + 1)] >= p->vel_thresh_low) k++;
    int pre = k + 1;		/* first sample (back) before the rise */
    if (pre > p->n_hist - 1) pre = p->n_hist - 1;

    double fh = 0.0, fv = 0.0;
    int nb = 0;
    for (int j = pre; j < p->n_hist && nb < p->n_base; j++, nb++) {
      int ix = idx_back(p, j);
      fh += p->h[ix];
      fv += p->v[ix];
    }
    fh /= nb; fv /= nb;

    if (p->min_onset_disp > 0.0) {
      double dh = h - fh, dv = v - fv;
      if (sqrt(dh*dh + dv*dv) < p->min_onset_disp)
	return DPOINT_PROCESS_IGNORE;	/* keep run; recheck next sample */
    }

    p->from_h = fh;
    p->from_v = fv;
    p->onset_t = p->t[idx_back(p, pre)];
    p->detect_t = t;
    p->peak = 0.0;
    for (int j = 0; j < pre; j++) {
      double s = p->speed[idx_back(p, j)];
      if (s > p->peak) p->peak = s;
    }
      {
      double dh = h - fh, dv = v - fv, m = sqrt(dh*dh + dv*dv);
      p->ux = m > 0.0 ? dh / m : 1.0;
      p->uy = m > 0.0 ? dv / m : 0.0;
    }
    p->landed = 0;
    p->prim_end_t = t;
    p->state = SAC_IN_FLIGHT;
    p->n_onsets++;
    return emit(p, pinfo, SAC_EV_ONSET, t, h, v, t - p->onset_t);
  }

  /* ---- in flight ---- */
  if (speed > p->peak) p->peak = speed;

  if (t - p->onset_t > p->max_dur)
    return abort_flight(p, pinfo, t, h, v);

  /* landed: signed velocity along the onset direction drops to <= 0. The
     eye was last moving forward at the previous sample, which is where the
     primary saccade ended (em.c phase 2b, done causally). */
  if (!p->landed) {
    /* over the same vel_win as speed, so the reversal test is no noisier
       at a high rate than at 250 Hz */
    int span = p->n_span < p->n_hist - 1 ? p->n_span : p->n_hist - 1;
    int ks = idx_back(p, span);
    double dtk = t - p->t[ks];
    double sv = dtk > 0.0 ?
      ((h - p->h[ks]) * p->ux + (v - p->v[ks]) * p->uy) / dtk : 0.0;
    int k = idx_back(p, 1);
    if (sv <= 0.0) {
      p->landed = 1;
      p->prim_end_t = p->t[k];
      p->n_landed++;
      /* report the landing at the last forward sample, pre-overshoot */
      return emit(p, pinfo, SAC_EV_LANDED, t, p->h[k], p->v[k],
		  p->prim_end_t - p->onset_t);
    }
    p->prim_end_t = t;
  }

  /* settled: the NET displacement over the last coast_dur is below
     vel_thresh_low. Not "every sample below vel_thresh_low for coast_dur":
     per-sample speed is noisy (0.03 deg position noise over a 4 ms window
     is ~10 deg/s, right at the default), and the chance that every sample
     comes in under it falls with each extra sample, so that test held at
     250 Hz and failed at 400 -- the saccade ran out to max_dur and
     aborted. Over 12 ms the same noise is ~3.5 deg/s, at any rate. The
     window has to start after onset, or a tiny saccade could settle
     before it has landed. */
  {
    double tol = 0.25 * p->dt_est;	/* sample times are not exact */
    int j = 1;
    while (j < p->n_hist - 1 && t - p->t[idx_back(p, j)] < p->coast_dur - tol)
      j++;
    int kj = idx_back(p, j);
    double dtw = t - p->t[kj];
    if (dtw >= p->coast_dur - tol && p->t[kj] >= p->onset_t) {
      double dh = h - p->h[kj], dv = v - p->v[kj];
      if (sqrt(dh*dh + dv*dv) / dtw < p->vel_thresh_low) {
	p->state = SAC_FIX;
	p->run = 0;
	p->last_end_t = t;
	p->have_end = 1;
	p->n_offsets++;
	/* no reversal seen (a slow, monotonic landing): the primary ended
	   where the settled window begins */
	if (!p->landed) p->prim_end_t = p->t[kj];
	return emit(p, pinfo, SAC_EV_OFFSET, t, h, v,
		    p->prim_end_t - p->onset_t);
      }
    }
  }
  return DPOINT_PROCESS_IGNORE;
}
