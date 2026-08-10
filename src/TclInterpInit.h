#ifndef TCL_INTERP_INIT_H
#define TCL_INTERP_INIT_H

/*
 * Serialised construction of Tcl interpreters.
 *
 * WHY THIS EXISTS
 *
 * dserv builds Tcl interpreters on several threads: the Dataserver request
 * thread, every TclServer request thread (the main one plus one per dsconf
 * subprocess), and every tpool_map worker. Left alone they call
 * Tcl_FindExecutable()/Tcl_CreateInterp()/Tcl_Init() at the same instant, and
 * that is not safe -- not because of anything in dserv, but because of how Tcl
 * allocates its own global mutexes.
 *
 * Tcl_Mutex is a POINTER, allocated on first use (unix/tclUnixThrd.c,
 * Tcl_MutexLock):
 *
 *     if (*mutexPtr == NULL) {          <-- read with NO lock and NO barrier
 *         pthread_mutex_lock(&globalLock);
 *         if (*mutexPtr == NULL) {
 *             pmutexPtr = Tcl_Alloc(sizeof(PMutex));
 *             PMutexInit(pmutexPtr);    <-- initialises the mutex body
 *             *mutexPtr = pmutexPtr;    <-- plain store publishes the pointer
 *         }
 *         pthread_mutex_unlock(&globalLock);
 *     }
 *     pmutexPtr = *((PMutex **) mutexPtr);
 *     PMutexLock(pmutexPtr);
 *
 * That is textbook broken double-checked locking. The unsynchronised read can
 * observe the published pointer while the PMutexInit() writes behind it are not
 * yet visible, so the second thread locks a mutex body it sees as pre-init
 * garbage. On x86 the store buffer is FIFO and this is benign in practice --
 * which is why it never showed up on the Macs. aarch64 reorders, so on the Pis
 * it eventually bites: the loser is left parked on a futex nobody will ever
 * wake, holding up whichever startup step was waiting on that interpreter.
 *
 * Measured on a Pi with stock libtcl9.0: two threads doing nothing but
 * Tcl_FindExecutable + Tcl_CreateInterp + Tcl_Init wedged 9 times in 400 runs;
 * four threads, 19 in 300. With the two rules below, 0 in 300.
 *
 * This is NOT fixed upstream -- Tcl_MutexLock is byte-for-byte identical in
 * 9.0.1 (what we ship against), 9.0.2, 9.0.3, 9.0.4 and current main. It has to
 * be handled here.
 *
 * THE TWO RULES
 *
 *   1. tcl_interp_global_init() runs once from main(), before any thread that
 *      touches Tcl exists, so the process-global mutexes are first allocated
 *      single-threaded and the racy branch above is never reached again for
 *      them. This is also what Tcl documents: Tcl_InitSubsystems and
 *      Tcl_FindExecutable are "typically invoked as the very first thing in the
 *      application's main program", not from worker threads.
 *
 *   2. Every remaining interpreter is built under tcl_interp_init_lock(), which
 *      covers the mutexes rule 1 cannot reach -- the ones a package first
 *      touches when it is loaded. Held only across construction, never across
 *      script execution, so it costs nothing at run time: dsconf starts its
 *      subprocesses in sequence anyway.
 */

#include <mutex>
#include <tcl.h>

/* The lock every interpreter construction is serialised behind. */
inline std::mutex &tcl_interp_init_lock(void)
{
  static std::mutex m;
  return m;
}

/*
 * Allocate Tcl's process-global state while still single-threaded.
 *
 * The throwaway interpreter is the point: Tcl_Init() and the script below walk
 * the encoding, library-path, executable-name and filesystem globals, which is
 * what forces their Tcl_Mutexes to be allocated here rather than in a race
 * later. Call once from main() before starting any thread.
 */
inline void tcl_interp_global_init(const char *argv0)
{
  Tcl_FindExecutable(argv0);

  Tcl_Interp *warm = Tcl_CreateInterp();
  if (!warm) return;

  if (Tcl_Init(warm) == TCL_OK) {
    /* Touch the process-global values that Tcl allocates lazily. Errors are
       deliberately ignored -- this is a warm-up, not a check. */
    Tcl_Eval(warm,
             "catch {encoding system}\n"
             "catch {encoding dirs}\n"
             "catch {zipfs root}\n"
             "catch {info nameofexecutable}\n"
             "catch {file exists [info nameofexecutable]}\n");
  }
  Tcl_DeleteInterp(warm);
}

#endif /* TCL_INTERP_INIT_H */
