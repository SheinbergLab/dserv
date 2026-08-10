#include <iostream>
#include <chrono>
#include <future>
#include <csignal>
#ifndef _MSC_VER
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#endif

#include "sharedqueue.h"
#include "Dataserver.h"
#include "TclServer.h"
#include "ObjectRegistry.h"
#include "cxxopts.hpp"
#include "dserv.h"
#include "tclserver_api.h"
#include "mdns_advertise.h"

#include "dservConfig.h"

#include "tclserver_api.h"

// A registry for main tclserver and subprocesses
ObjectRegistry<TclServer> TclServerRegistry;

// Provide hooks for loaded modules
Dataserver *dserver;
TclServer *tclserver;

// These should be part of an api 
extern "C" {

	Dataserver *get_ds(void) { return dserver; }
	TclServer *get_tclserver(void) { return tclserver; }
	
	tclserver_t* tclserver_get_from_interp(Tcl_Interp *interp) {
		TclServer* server = (TclServer*)Tcl_GetAssocData(interp, 
		       "tclserver_instance", NULL);
		return server ? (tclserver_t*)server : nullptr; // fallback to main
	}
	
	void tclserver_set_point(tclserver_t *tclserver, ds_datapoint_t *dp)
	{
	  ((TclServer *) tclserver)->set_point(dp);
	}
	
	uint64_t tclserver_now(tclserver_t *tclserver)
	{
	  return ((TclServer *) tclserver)->now();
	}

	/* Kernel timestamps (e.g. gpio_v2_line_event.timestamp_ns) are CLOCK_MONOTONIC;
	   dserv time is steady_clock + this fixed offset, and on Linux steady_clock IS
	   CLOCK_MONOTONIC. So a module can place a hardware timestamp on dserv's scale
	   exactly, instead of restamping and recording when it NOTICED the event. */
	int64_t tclserver_clock_epoch_offset_us(void)
	{
	  return Dataserver::clock_epoch_offset_us();
	}
	
	
  void tclserver_queue_script(tclserver_t *tclserver,
			      const char *script, int no_reply) 
	{
	  TclServer *ts = static_cast<TclServer*>(tclserver);
	  
	  client_request_t req;
	  req.type = (request_t) (no_reply ? REQ_SCRIPT_NOREPLY : REQ_SCRIPT);
	  req.script = std::string(script);
	  
	  ts->queue.push_back(req);
	}
}

static std::atomic<bool> shutdownRequested{false};

/*
 * Signal handler: async-signal-safe work only.  Everything the old
 * handler did here (Tcl evals, registry lookups, deletes, iostreams)
 * takes locks and allocates — undefined behavior in signal context
 * and the source of intermittent crashes on ctrl-c.  Now it just
 * sets a flag; main() runs the teardown in normal thread context.
 */
void signalHandler(int signum) {
  if (shutdownRequested.exchange(true)) {
    // second signal: force exit without destructors
    const char msg[] = "\nForced exit (second signal)\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    std::_Exit(1);
  }
}

static void graceful_shutdown(void) {
  std::cout << "\nShutting down gracefully..." << std::endl;

  /*
   * Shut subprocesses down from C++, NOT via eval("exit").
   *
   * eval() is an unbounded synchronous round-trip: it queues a request and
   * blocks on rqueue->front() until that subprocess's single worker thread
   * dequeues it.  A subprocess sitting in a blocking read (serial, socket)
   * or a long script never gets there, so shutdown hung until systemd's
   * TimeoutStopSec fired -- 90s of "Job dserv.service/stop running".
   *
   * The interp's own "exit" (subprocess_exit_cmd) does nothing but call
   * tserv->shutdown(), so the round-trip bought us nothing: call it
   * directly.  It sets m_bDone and pushes the wake message, so the worker
   * leaves its loop as soon as it finishes whatever it is doing, and we
   * never wait on a thread that may not be listening.
   */
  std::cout << "Shutting down subprocesses..." << std::endl;
  std::vector<std::string> names = TclServerRegistry.getNames();
  for (const auto& name : names) {
    if (name != "dserv" && !name.empty()) {
      TclServer* child = TclServerRegistry.getObject(name);
      if (child) {
        std::cout << "  Shutting down: " << name << std::endl;
        child->shutdown();
      }
    }
  }
  // Brief grace period for subprocesses to unwind. Durable state does not
  // depend on this: the datafiles are flushed by ~Dataserver below, which
  // waits (bounded) for the log writers.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  std::cout << "Deleting TclServer..." << std::endl;
  delete tclserver;
  std::cout << "Deleting Dataserver..." << std::endl;
  delete dserver;    // waits (bounded) for log writers: datafiles complete
  std::cout << "Clean shutdown complete." << std::endl;
  std::cout.flush();

  /*
   * _Exit rather than exit: detached threads (network accept loops,
   * websocket/uWS event loops, any lingering send clients) are still
   * running, and letting exit() tear down statics under them is a
   * crash lottery.  Everything that must be durable — the datafiles —
   * was already flushed and closed above.
   */
  std::_Exit(0);
}

/*
 * Is dserv's timeline still anchored to real time?
 *
 * WHY THIS IS IN THE CORE and not in a subprocess. The fact being checked is a
 * property of clock_epoch_offset_us() -- a dserv fact, not a PTP one. It lived
 * briefly in config/ptpconf.tcl because that is where both clocks were already
 * being read, but dsconf.tcl gates that subprocess on /sys/class/ptp/ptp*
 * existing, so the check ran ONLY on PTP-capable hosts. That is backwards
 * relative to the risk: a host with no PHC (a Pi 4, a Realtek NIC, a VM) is
 * typically the one with no RTC either, and so the one most likely to boot with
 * a wrong clock. Here it cannot be gated off.
 *
 * WHAT IT CATCHES (rig Pi, 2026-08-09). The host was put into the PTP client
 * role on a segment with no real grandmaster; phc2sys steered CLOCK_REALTIME
 * down to a from-zero PHC and the machine ran in 1970 with chrony stopped. dserv
 * was immune -- it stamps from CLOCK_MONOTONIC and never steps -- but it had
 * captured its epoch constant 13 s after boot from the stale clock systemd
 * restored, so every datapoint carried a timestamp ~29 h in the past. The extio
 * config page, which ages datapoints against the browser's clock, then showed
 * EVERY box as "offline 29h" while the rig was healthy. Nothing reported it.
 *
 * NOTE THE SKEW IS ZERO AT STARTUP, BY CONSTRUCTION: the offset is captured as
 * system_us() - steady_us(), so now() == system_us() at that instant. This can
 * only ever detect a LATER divergence -- which is exactly the failure. Both
 * halves of 2026-08-09 are later divergences: the clock being dragged to 1970
 * after dserv started, and then chrony correcting it while dserv went on holding
 * the stale anchor. So it must be periodic, not a startup check.
 *
 * TWO FAULTS, REPORTED DIFFERENTLY, because their fixes are opposite and
 * confusing them costs an evening.
 */
static const int64_t CLOCK_SKEW_WARN_US   = 60LL * 1000000;           /* 60 s */
static const int64_t CLOCK_EPOCH_FLOOR_US = 1735689600LL * 1000000;   /* 2025-01-01Z */

static void publishClockHealth(Dataserver *ds)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return;
  int64_t wall_us = (int64_t) ts.tv_sec * 1000000 + (int64_t) ts.tv_nsec / 1000;
  int64_t skew    = Dataserver::now() - wall_us;

  /* 60 s cannot false-positive. The two clocks behind the constant are slewed
     TOGETHER by NTP and diverge only across a STEP, so ordinary skew over a long
     session is milliseconds. */
  char buf[32];
  snprintf(buf, sizeof buf, "%lld", (long long) skew);

  std::string err;
  if (wall_us < CLOCK_EPOCH_FLOOR_US) {
    /* Not merely wrong -- never SET. A clock counting from zero is what a host
       with no RTC and no time source looks like, and what phc2sys copies out of
       a free-running PHC. */
    err = "host wall clock is not set; dserv timestamps are " +
          std::to_string(std::llabs(skew) / 1000000) + " s " +
          (skew < 0 ? "behind" : "ahead of") + " it. Fix the host clock "
          "(check its NTP daemon and dserv-ptp-setup status), then RESTART dserv.";
  } else if (std::llabs(skew) > CLOCK_SKEW_WARN_US) {
    /* The clock has been corrected since startup and dserv is still on the old
       anchor. clock_epoch_offset_us() is a static const, captured once and never
       recomputed -- correct for an acquisition timebase, and precisely why this
       has to be said out loud rather than inferred. */
    err = "dserv timestamps are " + std::to_string(std::llabs(skew) / 1000000) +
          " s " + (skew < 0 ? "behind" : "ahead of") + " the wall clock. The epoch "
          "anchor was captured at startup from a clock that was wrong; it is never "
          "recomputed, so RESTART dserv to pick up the corrected time.";
  }

  /* dpoint_new() strdup()s the name and memcpy()s the value, so these casts hand
     over nothing the datapoint keeps a pointer into. */
  ds->set((char *) "system/clock_skew_us", buf);
  ds->set((char *) "system/clock_error", const_cast<char *>(err.c_str()));

  /* Log a NEW fault once, not every minute -- and log the RECOVERY too: a fault
     that simply goes quiet is indistinguishable from a fault nobody noticed.
     Only ever touched from the park loop (the main thread), so the static needs
     no synchronisation. */
  static std::string last_err;
  if (err != last_err) {
    if (!err.empty()) std::cerr << "dserv: CLOCK " << err << std::endl;
    else              std::cerr << "dserv: clock skew back within "
                                << (CLOCK_SKEW_WARN_US / 1000000)
                                << " s of the wall clock" << std::endl;
    last_err = err;
  }
}

void setVersionInfo(TclServer* tclserver) {
    std::string version_str = dserv_VERSION;
    
    // Set as a datapoint
    tclserver->eval("dservSet system/version \"" + version_str + "\"");
    
    // Also set as a Tcl variable for direct access
    tclserver->eval("set ::dserv_version \"" + version_str + "\"");
    
    std::cout << "dserv version " << version_str << " initialized" << std::endl;
}


std::string getHostname() {
  char hostname[256];
  if (gethostname(hostname, sizeof(hostname)) == 0) {
    return std::string(hostname);
  }
  return "unknown";
}

/*
 * mainline
 */
int main(int argc, char *argv[])
{
  bool version = false;
  bool help = false;

  std::string trigger_script;
  std::string configuration_script;
  std::string www_path;
  
  cxxopts::Options options("dserv", "Data server");
  options.add_options()
    ("h,help", "Print help", cxxopts::value<bool>(help))
    ("t,tscript", "Trigger script path",
     cxxopts::value<std::string>(trigger_script))
    ("c,cscript", "Configuration script path",
     cxxopts::value<std::string>(configuration_script))
    ("v,version", "Version", cxxopts::value<bool>(version))
    ("w,www", "Static file serving directory",
     cxxopts::value<std::string>(www_path));    

  try {
    auto result = options.parse(argc, argv);
  } catch (const cxxopts::exceptions::parsing& e) {
    std::cerr << "Error parsing options: " << e.what() << std::endl;
    // Explicit exit, rather than abort, for testing with ctest.
    exit(-1);
  }

  if (help) {
    std::cout << options.help() << std::endl;
    exit(0);
  }
  
  if (version) {
    std::cout << dserv_VERSION << std::endl;
    exit(0);
  }

  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);	/* systemd stop: same clean path */

  if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
    std::cerr << "mlockall failed: " << strerror(errno) 
              << " (continuing without memory locking)" << std::endl;
  }
  
  // Create core dserv components
  dserver = new Dataserver(argc, argv);

  TclServerConfig tclserver_config("dserv", 2570, 2560, 2565);

  // Include default www path if not specified
  if (www_path.empty()) {
    // Check if default location exists
    struct stat st;
    if (stat("/usr/local/dserv/www", &st) == 0 && S_ISDIR(st.st_mode)) {
      www_path = "/usr/local/dserv/www";
    }
  }
  
  tclserver_config.www_path = www_path;
  tclserver = new TclServer(argc, argv, dserver, tclserver_config);
  
  TclServerRegistry.registerObject("dserv", tclserver);

  setVersionInfo(tclserver);

  // Run initialization scripts
  if (!trigger_script.empty()) {
    auto result = dserver->eval(std::string("source ")+trigger_script);
    if (result.starts_with("!TCL_ERROR ")) std::cerr << result;
  }
  
  if (!configuration_script.empty()) {
    auto result = tclserver->eval(std::string("source ")+configuration_script);
    if (result.starts_with("!TCL_ERROR ")) std::cerr << result << std::endl;
  }

  /* park until a signal requests shutdown, then tear down from the
     main thread (not from signal context) */
  //
  // The park loop doubles as the clock-health timer: no extra thread, and so
  // nothing extra for graceful_shutdown() to have to reason about. 600 ticks =
  // 60 s, which is far tighter than it needs to be -- the fault it reports
  // persists until dserv is restarted, so detection latency is irrelevant. The
  // counter starts at the trigger so the datapoints exist from the first tick
  // rather than a minute in, where a consumer would see them as missing.
  int clock_check_ticks = 600;
  while (!shutdownRequested.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (++clock_check_ticks >= 600) {
      clock_check_ticks = 0;
      publishClockHealth(dserver);
    }
  }
  graceful_shutdown();
}
