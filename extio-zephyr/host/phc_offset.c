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
 *   cc -O2 -Wall -o phc_offset phc_offset.c
 *   ./phc_offset [/dev/ptpN] [seconds] [interval_ms]
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

/* Method B: hardware cross-timestamp. Returns -1 if unsupported. */
static int sample_precise(int fd, int64_t *phc_minus_monoraw_ns,
                          int64_t *raw_minus_mono_ns)
{
	struct ptp_sys_offset_precise sp;
	struct timespec r0, m, r1;

	memset(&sp, 0, sizeof sp);
	if (ioctl(fd, PTP_SYS_OFFSET_PRECISE, &sp)) return -1;

	*phc_minus_monoraw_ns = pct_ns(&sp.device) - pct_ns(&sp.sys_monoraw);

	/* MONOTONIC_RAW is not NTP-slewed, CLOCK_MONOTONIC is -- so relate them
	 * locally.  Two back-to-back reads; the pair drifts only by slew rate. */
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &r0)) return -1;
	if (clock_gettime(CLOCK_MONOTONIC, &m))      return -1;
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &r1)) return -1;
	*raw_minus_mono_ns = (ts_ns(&r0) + ts_ns(&r1)) / 2 - ts_ns(&m);
	return 0;
}

static int cmp_i64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *) a, y = *(const int64_t *) b;
	return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
	/* --once: print ONLY the min-filtered PHC-MONO offset in ns, for scripting
	 * (publish it as a datapoint, feed it to a box). The min-window sample is
	 * the least-interrupted one, so its error is bounded by half that window --
	 * sub-us in practice.  Everything else goes to stderr so stdout stays clean. */
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
	int64_t *off = calloc(n_max, sizeof *off);
	int64_t *win = calloc(n_max, sizeof *win);
	double  *tsec = calloc(n_max, sizeof *tsec);
	int n = 0, precise_ok = 0;
	int64_t pb_off = 0, pb_raw = 0;

	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	if (!once)
		printf("device %s   sampling %.0f s every %d ms\n", dev, secs, interval_ms);

	while (n < n_max) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		double el = (ts_ns(&now) - ts_ns(&t0)) / 1e9;
		if (el > secs) break;

		int64_t o, w;
		if (sample_sandwich(phc, &o, &w) == 0) {
			off[n] = o; win[n] = w; tsec[n] = el; n++;
		}
		if (!precise_ok) {
			int64_t a, b;
			if (sample_precise(fd, &a, &b) == 0) {
				precise_ok = 1; pb_off = a; pb_raw = b;
			}
		}
		usleep(interval_ms * 1000);
	}

	if (n < 4) { fprintf(stderr, "too few samples\n"); return 1; }

	if (once) {
		/* least-interrupted sample wins */
		int best = 0;
		for (int i = 1; i < n; i++) if (win[i] < win[best]) best = i;
		printf("%lld\n", (long long) off[best]);
		fprintf(stderr, "n=%d  window=%lld ns  (error bound +/-%lld ns)\n",
		        n, (long long) win[best], (long long) (win[best] / 2));
		close(fd);
		return 0;
	}

	/* Drift: least-squares slope of offset vs elapsed, in ppm. */
	double mx = 0, my = 0;
	for (int i = 0; i < n; i++) { mx += tsec[i]; my += (double) off[i]; }
	mx /= n; my /= n;
	double num = 0, den = 0;
	for (int i = 0; i < n; i++) {
		num += (tsec[i] - mx) * ((double) off[i] - my);
		den += (tsec[i] - mx) * (tsec[i] - mx);
	}
	double slope_ns_per_s = den > 0 ? num / den : 0.0;

	int64_t *sw = malloc(n * sizeof *sw);
	memcpy(sw, win, n * sizeof *sw);
	qsort(sw, n, sizeof *sw, cmp_i64);

	printf("\nMETHOD A (sandwich vs CLOCK_MONOTONIC -- dserv's clock)\n");
	printf("  n              : %d over %.1f s\n", n, tsec[n - 1]);
	printf("  PHC - MONO     : %lld ns  (first)  ->  %lld ns  (last)\n",
	       (long long) off[0], (long long) off[n - 1]);
	printf("  read window    : min %lld  med %lld  max %lld ns\n",
	       (long long) sw[0], (long long) sw[n / 2], (long long) sw[n - 1]);
	printf("  DRIFT          : %.1f ns/s  = %.3f ppm\n",
	       slope_ns_per_s, slope_ns_per_s / 1000.0);
	printf("  => re-measure every %.3g s to stay within 1 us\n",
	       slope_ns_per_s != 0.0 ? 1000.0 / (slope_ns_per_s < 0 ? -slope_ns_per_s
	                                                            : slope_ns_per_s)
	                             : 0.0);

	printf("\nMETHOD B (PTP_SYS_OFFSET_PRECISE, hardware cross-timestamp)\n");
	if (!precise_ok) {
		printf("  NOT SUPPORTED by this driver (no getcrosststamp).\n");
		printf("  Method A stands alone -- its error is the read window above.\n");
	} else {
		int64_t b_equiv = pb_off + pb_raw;   /* PHC - MONOTONIC */
		printf("  PHC - MONORAW  : %lld ns\n", (long long) pb_off);
		printf("  MONORAW - MONO : %lld ns\n", (long long) pb_raw);
		printf("  PHC - MONO (B) : %lld ns\n", (long long) b_equiv);
		printf("  A vs B         : %lld ns  <- must be within the read window\n",
		       (long long) (off[0] - b_equiv));
	}
	close(fd);
	return 0;
}
