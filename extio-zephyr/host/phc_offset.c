/*
 * phc_offset.c -- measure the PHC <-> dserv-clock relationship on the host.
 *
 * WHY THIS EXISTS
 *
 * PTP disciplines a box's IEEE-1588 counter to the host NIC's PHC (measured at
 * ~+/-100 ns on the RW612, see PORTING.md 2026-07-26). That does NOT put box
 * events on dserv's timeline, because dserv stamps from a different oscillator:
 *
 *     Dataserver::now() = clock_epoch_offset_us() + steady_us()
 *
 * i.e. CLOCK_MONOTONIC plus a constant captured once at startup. The PHC and
 * CLOCK_MONOTONIC are separate oscillators, so they drift.
 *
 * The saving grace is that BOTH CLOCKS ARE LOCAL -- no wire, no one-way delay to
 * estimate. That is what makes this tractable where the obs-anchor mechanism was
 * not: there is no asymmetry to model, only two clocks on one board.
 *
 * TWO INDEPENDENT METHODS, because a single one cannot detect its own error
 * (the standing rule in PORTING.md):
 *
 *   A) SANDWICH: read MONO, read PHC, read MONO.  offset = phc - (mono0+mono1)/2
 *      Error is bounded by the read window, which is reported; min-filtering
 *      across samples keeps the least-interrupted one.  Measures against
 *      CLOCK_MONOTONIC directly -- exactly dserv's clock.
 *
 *   B) PTP_SYS_OFFSET_PRECISE: hardware cross-timestamp of PHC against
 *      CLOCK_REALTIME and CLOCK_MONOTONIC_RAW.  Far tighter when the driver
 *      implements it (many do not -- it needs getcrosststamp support), but it
 *      yields MONOTONIC_RAW, not MONOTONIC, so it needs a local RAW<->MONO
 *      correction.  Reported separately so the two can be compared.
 *
 * If A and B disagree by more than their stated windows, STOP -- the same rule
 * that caught the loopback harness disagreement.
 *
 * WHY DRIFT COMES FROM B WHEN B EXISTS (2026-07-27)
 *
 * A's per-sample error is half its read window, and that window is not small or
 * even stable: on office-stim (I226-V, igc) it measured min 3.3 us but MEDIAN
 * 170 us -- a 50x spread that is preemption and C-states, not the PCIe read.
 * Fitting a drift line through samples carrying +/-85 us of noise over a 20 s
 * run is fitting mostly noise: the same run's first/last A samples implied
 * 4.3 ppm while the regression said 0.267 ppm.
 *
 * B has no PHC read window at all -- the correlation is done in hardware -- so
 * its only uncertainty is the tiny RAW<->MONO pair (two vDSO reads, tens of ns).
 * That is three to four ORDERS of magnitude better per sample, which is the
 * difference between a drift number you can act on and one you cannot.
 *
 * So: both methods are still sampled every iteration and BOTH drifts are
 * reported -- the cross-check is the whole point of the file and losing it would
 * be a bad trade. B is merely preferred for the headline number and for --once.
 * Where B is absent (the Pi 5 has no getcrosststamp) behaviour is unchanged.
 *
 * Residuals are reported as mean |residual| so the fits can be compared
 * directly: a tight B residual next to a wide A residual is the evidence for
 * that preference, rather than an assertion about which is better.
 *
 *   cc -O2 -Wall -o phc_offset phc_offset.c
 *   ./phc_offset [/dev/ptpN] [seconds] [interval_ms]
 *   ./phc_offset --once [/dev/ptpN]      # one number on stdout, for scripting
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/ptp_clock.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static int64_t ts_ns(const struct timespec *t)
{
	return (int64_t) t->tv_sec * 1000000000LL + t->tv_nsec;
}

static int64_t pct_ns(const struct ptp_clock_time *t)
{
	return (int64_t) t->sec * 1000000000LL + t->nsec;
}

/* clockid for a PHC fd, per the kernel's FD_TO_CLOCKID convention */
#define FD_TO_CLOCKID(fd) ((~(clockid_t) (fd) << 3) | 3)

/* One iteration's worth of both methods, kept together so the A-vs-B
 * cross-check compares samples taken at the SAME moment rather than whichever
 * happened to be first. */
typedef struct {
	double  t;                  /* elapsed seconds                        */
	int     have_a, have_b;
	int64_t a_off, a_win;       /* A: PHC - MONO, and its read window     */
	int64_t b_off, b_win;       /* B: PHC - MONO, and its RAW<->MONO window */
	int64_t b_praw, b_rawmono;  /* B components, for reporting            */
} sample_t;

/* Method A: sandwich a PHC read between two CLOCK_MONOTONIC reads. */
static int sample_sandwich(clockid_t phc, int64_t *offset_ns, int64_t *window_ns)
{
	struct timespec m0, p, m1;

	if (clock_gettime(CLOCK_MONOTONIC, &m0)) return -1;
	if (clock_gettime(phc, &p))              return -1;
	if (clock_gettime(CLOCK_MONOTONIC, &m1)) return -1;

	int64_t mono_mid = (ts_ns(&m0) + ts_ns(&m1)) / 2;
	*offset_ns = ts_ns(&p) - mono_mid;      /* PHC - MONOTONIC */
	*window_ns = ts_ns(&m1) - ts_ns(&m0);   /* our own uncertainty */
	return 0;
}

/* Method B: hardware cross-timestamp. Returns -1 if unsupported.
 * `window_ns` is B's OWN uncertainty -- the RAW<->MONO read pair, not a PHC
 * read, since the PHC correlation happened in hardware. */
static int sample_precise(int fd, sample_t *s)
{
	struct ptp_sys_offset_precise sp;
	struct timespec r0, m, r1;

	memset(&sp, 0, sizeof sp);
	if (ioctl(fd, PTP_SYS_OFFSET_PRECISE, &sp)) return -1;

	s->b_praw = pct_ns(&sp.device) - pct_ns(&sp.sys_monoraw);

	/* MONOTONIC_RAW is not NTP-slewed, CLOCK_MONOTONIC is -- so relate them
	 * locally.  Two back-to-back reads; the pair drifts only by slew rate. */
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &r0)) return -1;
	if (clock_gettime(CLOCK_MONOTONIC, &m))      return -1;
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &r1)) return -1;

	s->b_rawmono = (ts_ns(&r0) + ts_ns(&r1)) / 2 - ts_ns(&m);
	s->b_off     = s->b_praw + s->b_rawmono;      /* PHC - MONOTONIC */
	s->b_win     = ts_ns(&r1) - ts_ns(&r0);
	return 0;
}

static int cmp_i64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *) a, y = *(const int64_t *) b;
	return (x > y) - (x < y);
}

/* Least-squares slope in ns/s, plus mean |residual| so a noisy fit is visible
 * rather than implied. No sqrt, so this still links without -lm.
 *
 * THE BASELINE SUBTRACTION IS LOAD-BEARING, not tidiness. These offsets are
 * ~1.78e18 ns, and a double holds ~9e15 exactly -- one ulp up there is ~512 ns,
 * and (double)y[i] - my is then catastrophic cancellation between two huge
 * values. Fitting directly in absolute ns made BOTH methods report ~5.3 us of
 * mean residual (2026-07-27), which is nonsense for method B: its own per-sample
 * window was 22-133 ns. Two methods 1000x apart in precision reporting the SAME
 * residual was the tell -- they were both measuring the floating-point floor.
 * Subtracting y[0] in INTEGER arithmetic first keeps every value within a few
 * hundred ns of zero, where doubles are exact. */
static double fit_slope(const double *t, const int64_t *y, int n, double *mad)
{
	if (mad) *mad = 0.0;
	if (n < 2) return 0.0;

	const int64_t y0 = y[0];
	double mx = 0, my = 0;
	for (int i = 0; i < n; i++) { mx += t[i]; my += (double) (y[i] - y0); }
	mx /= n; my /= n;

	double num = 0, den = 0;
	for (int i = 0; i < n; i++) {
		num += (t[i] - mx) * ((double) (y[i] - y0) - my);
		den += (t[i] - mx) * (t[i] - mx);
	}
	double m = den > 0 ? num / den : 0.0;
	double b = my - m * mx;

	if (mad) {
		double ss = 0;
		for (int i = 0; i < n; i++) {
			double r = (double) (y[i] - y0) - (m * t[i] + b);
			ss += r < 0 ? -r : r;
		}
		*mad = ss / n;
	}
	return m;
}

static void print_window_stats(const char *label, int64_t *w, int n)
{
	qsort(w, n, sizeof *w, cmp_i64);
	printf("  %-15s: min %lld  med %lld  max %lld ns\n", label,
	       (long long) w[0], (long long) w[n / 2], (long long) w[n - 1]);
}

int main(int argc, char **argv)
{
	/* --once: print ONLY the PHC-MONO offset in ns, for scripting (publish it
	 * as a datapoint, feed it to a box). Prefers B's hardware cross-timestamp
	 * when the driver has it, else the min-window A sample -- the least
	 * interrupted one, whose error is bounded by half that window.
	 * Everything else goes to stderr so stdout stays a single integer. */
	int once = 0;
	if (argc > 1 && strcmp(argv[1], "--once") == 0) { once = 1; argc--; argv++; }

	const char *dev  = argc > 1 ? argv[1] : "/dev/ptp0";
	double secs      = argc > 2 ? atof(argv[2]) : (once ? 2.0 : 30.0);
	int interval_ms  = argc > 3 ? atoi(argv[3]) : (once ? 5 : 200);

	int fd = open(dev, O_RDONLY);
	if (fd < 0) { perror(dev); return 1; }
	clockid_t phc = FD_TO_CLOCKID(fd);

	struct timespec probe;
	if (clock_gettime(phc, &probe)) { perror("clock_gettime(phc)"); return 1; }

	int n_max = (int) (secs * 1000.0 / interval_ms) + 2;
	sample_t *s = calloc(n_max, sizeof *s);
	int n = 0, na = 0, nb = 0;

	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	if (!once)
		printf("device %s   sampling %.0f s every %d ms\n", dev, secs, interval_ms);

	while (n < n_max) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		double el = (ts_ns(&now) - ts_ns(&t0)) / 1e9;
		if (el > secs) break;

		sample_t *cur = &s[n];
		cur->t = el;

		/* BOTH every iteration: B for the numbers, A so the cross-check
		 * still exists. Sampling B once (as this did before) made a drift
		 * fit impossible and left the comparison anchored to whichever A
		 * sample happened to be first. */
		if (sample_sandwich(phc, &cur->a_off, &cur->a_win) == 0) {
			cur->have_a = 1; na++;
		}
		if (sample_precise(fd, cur) == 0) {
			cur->have_b = 1; nb++;
		}
		n++;
		usleep(interval_ms * 1000);
	}

	if (na < 4 && nb < 4) { fprintf(stderr, "too few samples\n"); return 1; }

	if (once) {
		int best = -1;

		if (nb >= 1) {                       /* B: no PHC read window at all */
			for (int i = 0; i < n; i++)
				if (s[i].have_b && (best < 0 || s[i].b_win < s[best].b_win))
					best = i;
			printf("%lld\n", (long long) s[best].b_off);
			fprintf(stderr, "method=B (hw cross-timestamp)  n=%d  "
			        "window=%lld ns  (error bound +/-%lld ns)\n",
			        nb, (long long) s[best].b_win,
			        (long long) (s[best].b_win / 2));
		} else {
			for (int i = 0; i < n; i++)
				if (s[i].have_a && (best < 0 || s[i].a_win < s[best].a_win))
					best = i;
			printf("%lld\n", (long long) s[best].a_off);
			fprintf(stderr, "method=A (sandwich)  n=%d  "
			        "window=%lld ns  (error bound +/-%lld ns)\n",
			        na, (long long) s[best].a_win,
			        (long long) (s[best].a_win / 2));
		}
		close(fd);
		return 0;
	}

	/* ---- fits ---- */
	double  *ta = calloc(n, sizeof *ta), *tb = calloc(n, sizeof *tb);
	int64_t *ya = calloc(n, sizeof *ya), *yb = calloc(n, sizeof *yb);
	int64_t *wa = calloc(n, sizeof *wa), *wb = calloc(n, sizeof *wb);
	int ia = 0, ib = 0;

	for (int i = 0; i < n; i++) {
		if (s[i].have_a) { ta[ia] = s[i].t; ya[ia] = s[i].a_off; wa[ia] = s[i].a_win; ia++; }
		if (s[i].have_b) { tb[ib] = s[i].t; yb[ib] = s[i].b_off; wb[ib] = s[i].b_win; ib++; }
	}

	double mad_a = 0, mad_b = 0;
	double slope_a = fit_slope(ta, ya, ia, &mad_a);
	double slope_b = fit_slope(tb, yb, ib, &mad_b);

	printf("\nMETHOD A (sandwich vs CLOCK_MONOTONIC -- dserv's clock)\n");
	if (ia >= 2) {
		printf("  n              : %d over %.1f s\n", ia, ta[ia - 1]);
		printf("  PHC - MONO     : %lld ns  (first)  ->  %lld ns  (last)\n",
		       (long long) ya[0], (long long) ya[ia - 1]);
		printf("  NB first/last are SINGLE samples, each carrying +/-half the\n"
		       "     read window below -- do not read a drift off them.\n");
		print_window_stats("read window", wa, ia);
		printf("  DRIFT          : %.1f ns/s  = %.3f ppm   (mean |resid| %.0f ns)\n",
		       slope_a, slope_a / 1000.0, mad_a);
	} else {
		printf("  no samples\n");
	}

	printf("\nMETHOD B (PTP_SYS_OFFSET_PRECISE, hardware cross-timestamp)\n");
	if (ib < 2) {
		printf("  NOT SUPPORTED by this driver (no getcrosststamp).\n");
		printf("  Method A stands alone -- its error is the read window above.\n");
	} else {
		int f0 = 0;                       /* first sample that actually HAS B */
		while (f0 < n && !s[f0].have_b) f0++;

		printf("  n              : %d over %.1f s\n", ib, tb[ib - 1]);
		printf("  PHC - MONORAW  : %lld ns\n", (long long) s[f0].b_praw);
		printf("  MONORAW - MONO : %lld ns\n", (long long) s[f0].b_rawmono);
		printf("  PHC - MONO (B) : %lld ns  (first)  ->  %lld ns  (last)\n",
		       (long long) yb[0], (long long) yb[ib - 1]);
		print_window_stats("RAW<->MONO win", wb, ib);
		printf("  DRIFT          : %.1f ns/s  = %.3f ppm   (mean |resid| %.0f ns)\n",
		       slope_b, slope_b / 1000.0, mad_b);
	}

	/* ---- cross-check: same instant, best A sample ---- */
	printf("\nCROSS-CHECK\n");
	int best = -1;
	for (int i = 0; i < n; i++)
		if (s[i].have_a && s[i].have_b &&
		    (best < 0 || s[i].a_win < s[best].a_win))
			best = i;

	if (best < 0) {
		printf("  no iteration produced both methods -- cannot cross-check.\n");
	} else {
		int64_t d = s[best].a_off - s[best].b_off;
		printf("  A vs B         : %lld ns  (at t=%.2f s, A window %lld ns)\n",
		       (long long) d, s[best].t, (long long) s[best].a_win);
		printf("  %s\n",
		       (d < 0 ? -d : d) <= s[best].a_win
		           ? "within A's read window -- the two methods agree"
		           : "OUTSIDE A's read window -- STOP, do not trust either");
		if (ib >= 2 && ia >= 2)
			printf("  DRIFT A vs B   : %.1f vs %.1f ns/s\n", slope_a, slope_b);
	}

	/* ---- the number to act on ---- */
	double use = (ib >= 2) ? slope_b : slope_a;
	const char *which = (ib >= 2) ? "B (hardware cross-timestamp)"
	                              : "A (sandwich; B unavailable)";

	printf("\nDRIFT USED: method %s\n", which);
	printf("  %.1f ns/s = %.3f ppm\n", use, use / 1000.0);
	if (use != 0.0)
		printf("  => re-measure every %.3g s to stay within 1 us\n",
		       1000.0 / (use < 0 ? -use : use));

	close(fd);
	return 0;
}
