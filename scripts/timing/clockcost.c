/* cost of the clock reads the instrumentation adds per request */
#include <stdio.h>
#include <stdint.h>
#include <time.h>

static inline uint64_t ns_of(struct timespec *ts)
{ return (uint64_t) ts->tv_sec * 1000000000ull + ts->tv_nsec; }

static double bench(clockid_t id, const char *name, long n)
{
  struct timespec a, b, t;
  volatile uint64_t sink = 0;
  clock_gettime(CLOCK_MONOTONIC, &a);
  for (long i = 0; i < n; i++) { clock_gettime(id, &t); sink += ns_of(&t); }
  clock_gettime(CLOCK_MONOTONIC, &b);
  double per = (double)(ns_of(&b) - ns_of(&a)) / (double) n;
  printf("  %-24s %7.2f ns/call\n", name, per);
  return per;
}

int main(void)
{
  long n = 5000000;
  printf("clock read cost (%ld iterations):\n", n);
  double raw = bench(CLOCK_MONOTONIC_RAW, "CLOCK_MONOTONIC_RAW", n);
  double mono = bench(CLOCK_MONOTONIC, "CLOCK_MONOTONIC", n);
  printf("\n  3 reads/request (RAW):  %.1f ns\n", 3 * raw);
  printf("  3 reads/request (MONO): %.1f ns\n", 3 * mono);
  return 0;
}
