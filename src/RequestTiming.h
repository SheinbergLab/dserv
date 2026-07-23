#ifndef REQUEST_TIMING_H
#define REQUEST_TIMING_H

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include <map>
#include <utility>
#include <algorithm>

/*
 * RequestTiming
 *   Instrumentation for the serialized request path.
 *
 *   Every client_request_t stamps itself at construction (t_enqueue);
 *   process_requests stamps again after dequeue and after the request
 *   is evaluated, which yields three quantities per request:
 *
 *     queue residency = t_dequeue - t_enqueue   how long it waited
 *     execution       = t_done    - t_dequeue   how long the work took
 *     utilization rho = sum(execution) / wall   how close to saturation
 *
 *   rho is the one that predicts trouble.  The queue has a single
 *   consumer, so wait time scales as rho/(1-rho): once the process
 *   thread approaches saturation a modest increase in per-request work
 *   produces a disproportionate increase in latency.  Per-request
 *   execution time alone will not show that coming.
 *
 *   Both recording and reporting happen on the process thread, so no
 *   synchronization is needed here.  t_enqueue is stamped on whichever
 *   thread built the request, but that is a bare clock read into a
 *   field the producer owns outright.
 */

static inline uint64_t request_timing_now_ns(void)
{
  struct timespec ts;
#if defined(CLOCK_MONOTONIC_RAW)
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
  clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
  return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

/*
 * Condense a script into a stable label: collapse whitespace and keep the
 * leading command, so repeated invocations of the same call aggregate
 * together instead of fragmenting on their arguments.
 */
static inline std::string request_timing_script_label(const std::string &s)
{
  std::string out;
  bool space = false;
  for (size_t i = 0; i < s.size() && out.size() < 64; i++) {
    char c = s[i];
    if (c == '\n' || c == '\t' || c == '\r' || c == ' ') {
      if (!out.empty()) space = true;
      continue;
    }
    if (space) { out += ' '; space = false; }
    out += c;
  }
  if (out.empty()) out = "(empty)";
  return out;
}

class RequestTiming
{
public:
  static const size_t NTYPES = 16;

  /*
   * Retained samples for percentile estimation.  8192 puts ~80 samples
   * above p99, which is ample, and keeps the buffer under 100 KB.  The
   * ring is allocated only while enabled: with 20 interpreters a fixed
   * buffer is real resident memory for a feature that is usually off.
   */
  static const size_t RING = 8192;

  RequestTiming(void) { reset(); }

  bool enabled(void) const { return m_enabled; }

  void set_enabled(bool e)
  {
    m_enabled = e;
    if (e) {
      m_ring.assign(RING, sample_t{0, 0, 0});
      reset();
    }
    else {
      std::vector<sample_t>().swap(m_ring);   // release, don't just clear
      reset();
    }
  }

  void reset(void)
  {
    m_head = 0;
    m_filled = 0;
    m_total = 0;
    m_busy_ns = 0;
    m_agg.clear();
    m_slow.clear();
    m_window_start = request_timing_now_ns();
    for (size_t i = 0; i < NTYPES; i++) {
      m_type_count[i] = 0;
      m_type_exec_ns[i] = 0;
    }
  }

  void record(int type, const std::string &label,
	      uint64_t t_enqueue, uint64_t t_dequeue, uint64_t t_done)
  {
    if (!m_enabled) return;

    {
      uint64_t x = (t_done > t_dequeue) ? (t_done - t_dequeue) : 0;

      /*
       * Per-label attribution.  Long execution times mean very different
       * things depending on the label: real interpreter work scales with
       * core speed, while a blocking round-trip (a "!" call into stim,
       * a synchronous device read) is bounded by the peer and does not.
       * Only the former is a reason to worry about a slower CPU.
       */
      if (m_agg.size() < MAX_LABELS || m_agg.count(label)) {
	agg_t &a = m_agg[label];
	a.count++;
	a.total_ns += x;
	if (x > a.max_ns) a.max_ns = x;
      }

      /* keep the K slowest individual requests, with their labels */
      if (m_slow.size() < MAX_SLOW || x > m_slow.back().exec_ns) {
	slow_t s;
	s.exec_ns = x;
	s.queue_ns = (t_dequeue > t_enqueue) ? (t_dequeue - t_enqueue) : 0;
	s.label = label;
	m_slow.insert(std::lower_bound(m_slow.begin(), m_slow.end(), s),
		      s);
	if (m_slow.size() > MAX_SLOW) m_slow.pop_back();
      }
    }

    uint64_t q = (t_dequeue > t_enqueue) ? (t_dequeue - t_enqueue) : 0;
    uint64_t x = (t_done > t_dequeue) ? (t_done - t_dequeue) : 0;

    m_busy_ns += x;
    m_total++;

    if (type >= 0 && (size_t) type < NTYPES) {
      m_type_count[type]++;
      m_type_exec_ns[type] += x;
    }

    if (m_ring.empty()) return;          // enabled but not yet allocated

    sample_t *s = &m_ring[m_head];
    s->queue_ns = clamp32(q);
    s->exec_ns = clamp32(x);
    s->type = (uint8_t) (type & 0xff);

    m_head = (m_head + 1) % m_ring.size();
    if (m_filled < m_ring.size()) m_filled++;
  }

  /*
   * Report as a Tcl dict.  Percentiles come from the retained ring
   * (the most recent RING requests); rho and the totals cover the
   * whole window since the last reset.
   */
  std::string report(void)
  {
    uint64_t now = request_timing_now_ns();
    uint64_t wall = (now > m_window_start) ? (now - m_window_start) : 1;

    std::vector<uint32_t> q, x;
    q.reserve(m_filled);
    x.reserve(m_filled);
    for (size_t i = 0; i < m_filled; i++) {
      q.push_back(m_ring[i].queue_ns);
      x.push_back(m_ring[i].exec_ns);
    }
    std::sort(q.begin(), q.end());
    std::sort(x.begin(), x.end());

    double rho = (double) m_busy_ns / (double) wall;
    double window_s = (double) wall / 1e9;
    double rate = window_s > 0 ? (double) m_total / window_s : 0;

    char buf[1024];
    snprintf(buf, sizeof(buf),
             "total %llu window_s %.3f rate_hz %.1f rho %.4f "
             "queue_us {p50 %.1f p95 %.1f p99 %.1f max %.1f} "
             "exec_us {p50 %.1f p95 %.1f p99 %.1f max %.1f}",
             (unsigned long long) m_total, window_s, rate, rho,
             pct(q, 0.50), pct(q, 0.95), pct(q, 0.99), pct(q, 1.0),
             pct(x, 0.50), pct(x, 0.95), pct(x, 0.99), pct(x, 1.0));

    std::string out(buf);

    /* per-type breakdown: which request kinds own the busy time */
    out += " by_type {";
    for (size_t i = 0; i < NTYPES; i++) {
      if (!m_type_count[i]) continue;
      snprintf(buf, sizeof(buf), "%zu {n %llu exec_ms %.3f} ", i,
               (unsigned long long) m_type_count[i],
               (double) m_type_exec_ns[i] / 1e6);
      out += buf;
    }
    out += "}";

    return out;
  }

  /* the K slowest individual requests seen, slowest first */
  std::string slowest(void)
  {
    std::string out;
    char buf[512];
    for (size_t i = 0; i < m_slow.size(); i++) {
      snprintf(buf, sizeof(buf), "{exec_us %.1f queue_us %.1f what {%s}} ",
	       (double) m_slow[i].exec_ns / 1000.0,
	       (double) m_slow[i].queue_ns / 1000.0,
	       m_slow[i].label.c_str());
      out += buf;
    }
    return out;
  }

  /* per-label totals, ordered by total time spent */
  std::string labels(void)
  {
    std::vector<std::pair<uint64_t, std::string> > v;
    for (std::map<std::string, agg_t>::iterator it = m_agg.begin();
	 it != m_agg.end(); ++it)
      v.push_back(std::make_pair(it->second.total_ns, it->first));
    std::sort(v.rbegin(), v.rend());

    std::string out;
    char buf[512];
    for (size_t i = 0; i < v.size() && i < 25; i++) {
      const agg_t &a = m_agg[v[i].second];
      snprintf(buf, sizeof(buf),
	       "{n %llu total_ms %.2f mean_us %.1f max_us %.1f what {%s}} ",
	       (unsigned long long) a.count, (double) a.total_ns / 1e6,
	       (double) a.total_ns / (double) a.count / 1000.0,
	       (double) a.max_ns / 1000.0, v[i].second.c_str());
      out += buf;
    }
    return out;
  }

private:
  static const size_t MAX_SLOW = 12;
  static const size_t MAX_LABELS = 250;

  typedef struct {
    uint32_t queue_ns;
    uint32_t exec_ns;
    uint8_t type;
  } sample_t;

  typedef struct agg_s {
    uint64_t count = 0;
    uint64_t total_ns = 0;
    uint64_t max_ns = 0;
  } agg_t;

  typedef struct slow_s {
    uint64_t exec_ns = 0;
    uint64_t queue_ns = 0;
    std::string label;
    /* sort slowest-first */
    bool operator<(const struct slow_s &o) const { return exec_ns > o.exec_ns; }
  } slow_t;

  static uint32_t clamp32(uint64_t v)
  {
    return (v > 0xffffffffull) ? 0xffffffffu : (uint32_t) v;
  }

  /* percentile in microseconds; v must be sorted */
  static double pct(const std::vector<uint32_t> &v, double p)
  {
    if (v.empty()) return 0.0;
    size_t i = (size_t) (p * (double) (v.size() - 1) + 0.5);
    if (i >= v.size()) i = v.size() - 1;
    return (double) v[i] / 1000.0;
  }

  bool m_enabled = false;
  std::vector<sample_t> m_ring;          // allocated only while enabled
  std::map<std::string, agg_t> m_agg;    // per-label totals
  std::vector<slow_t> m_slow;            // K slowest, slowest first
  size_t m_head = 0;
  size_t m_filled = 0;
  uint64_t m_total = 0;
  uint64_t m_busy_ns = 0;
  uint64_t m_window_start = 0;
  uint64_t m_type_count[NTYPES];
  uint64_t m_type_exec_ns[NTYPES];
};

#endif
