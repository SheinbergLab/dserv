/*
 * photo_replay.c -- dserv's `photo` processor (processors/photo.c), run
 * offline over recorded extio analog blocks.
 *
 *     photo_replay ?key=value ...? < blocks
 *
 * Each input line is one block as the data file holds it:
 *
 *     <varname> <timestamp_us> <hex bytes>
 *
 * The key=value arguments are the processor's parameters, set in order
 * before the first block (`active` 1 and `group` empty, every block, are
 * set first; a `group` given filters as it does in dserv). Each edge the processor reports is printed as
 *
 *     edge dir t50 t10 t90 old new settled seq t_arm t_done
 *
 * and at the end its counters, one `param NAME VALUE` line each. This is
 * the same code dserv runs, so a recorded validation session (its group
 * named in extio `record`) can be detected again with other settings, and
 * a session recorded before the processor existed can be detected at all.
 * tools/photopair/photopair.tcl drives it.
 */
#include "../../processors/photo.c"

#include <stdio.h>
#include <ctype.h>

static void set(process_params_t *p, const char *name, const char *val)
{
  char *v = (char *) val;
  dpoint_process_param_setting_t ps = { 0 };
  ps.pname = (char *) name;
  ps.pval = &v;
  ps.params = p;
  setProcessParams(&ps);           /* says nothing about a name it lacks */
  char *got = NULL;
  dpoint_process_param_setting_t g = { 0 };
  g.pname = (char *) name;
  g.pval = &got;
  g.params = p;
  if (strcmp(name, "reset") && strcmp(name, "defaults") && !getProcessParams(&g))
    fprintf(stderr, "photo_replay: no parameter %s\n", name);
}

static int hexval(int c)
{
  if (c >= '0' && c <= '9') return c - '0';
  c = tolower(c);
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

int main(int argc, char **argv)
{
  process_params_t *p = newProcessParams();
  set(p, "active", "1");
  set(p, "group", "");             /* every block given: the caller picked them */
  for (int i = 1; i < argc; i++) {
    char key[64];
    const char *eq = strchr(argv[i], '=');
    if (!eq || eq == argv[i] || eq - argv[i] >= (int) sizeof(key)) {
      fprintf(stderr, "usage: photo_replay ?key=value ...? < blocks\n");
      return 2;
    }
    memcpy(key, argv[i], eq - argv[i]);
    key[eq - argv[i]] = 0;
    set(p, key, eq + 1);
  }

  size_t cap = 1 << 16;
  char *line = malloc(cap);
  unsigned char *buf = malloc(cap);
  long nlines = 0, nbad = 0;
  for (;;) {
    /* a line of any length */
    size_t len = 0;
    int c;
    while ((c = getchar()) != EOF && c != '\n') {
      if (len + 1 >= cap) { cap *= 2; line = realloc(line, cap); buf = realloc(buf, cap); }
      line[len++] = (char) c;
    }
    if (len == 0 && c == EOF) break;
    line[len] = 0;
    if (len == 0) continue;
    nlines++;

    char name[256];
    long long ts;
    int off = 0;
    if (sscanf(line, "%255s %lld %n", name, &ts, &off) < 2) { nbad++; continue; }
    const char *h = line + off;
    size_t n = 0;
    while (h[0] && h[1]) {
      int a = hexval(h[0]), b = hexval(h[1]);
      if (a < 0 || b < 0) break;
      buf[n++] = (unsigned char) (a * 16 + b);
      h += 2;
    }

    ds_datapoint_t in = { 0 };
    in.timestamp = (uint64_t) ts;
    in.varname = name;
    in.data.type = DSERV_BYTE;
    in.data.len = (uint32_t) n;
    in.data.buf = buf;
    dpoint_process_info_t pi = { 0 };
    pi.input_dpoint = &in;
    if (onProcess(&pi, p) == DPOINT_PROCESS_DSERV) {
      int rows = pi.dpoint->data.len / (NOUT * sizeof(double));
      const double *d = (const double *) pi.dpoint->data.buf;
      for (int r = 0; r < rows; r++) {
        printf("edge");
        for (int k = 0; k < NOUT; k++) printf(" %.3f", d[r * NOUT + k]);
        printf("\n");
      }
    }
  }

  static const char *counters[] = {
    "n_blocks", "n_other", "n_samples", "n_edges", "n_rising", "n_falling",
    "n_unsettled", "n_gaps", "n_bad", "n_stale", "n_multi", "n_dropped",
    "n_pulses", "n_missing", "n_long", "level", "noise", "base", "dt_est",
    "item_dt", "period_est", "n_smooth", "n_base", "n_settle", NULL
  };
  for (int i = 0; counters[i]; i++) {
    char *val = NULL;
    dpoint_process_param_setting_t ps = { 0 };
    ps.pname = (char *) counters[i];
    ps.pval = &val;
    ps.params = p;
    if (getProcessParams(&ps) && val) printf("param %s %s\n", counters[i], val);
  }
  printf("param lines %ld\nparam unparsed %ld\n", nlines, nbad);
  return 0;
}
