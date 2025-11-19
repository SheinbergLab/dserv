/*
 * NAME
 *   timer.c
 *
 * DESCRIPTION
 *   Cross-platform timer implementation 
 *
 * AUTHOR
 *   DLS, 07/24, 06/25 (added support for WSL using signals)
 */

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>

#ifdef __linux__
#include <sys/timerfd.h>
#include <strings.h>
#include <time.h>
#endif

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif

#include <tcl.h>
#include "Datapoint.h"
#include "tclserver_api.h"

const char *DEFAULT_TIMER_DPOINT_PREFIX = "timer";
static int module_count = 0;

// Forward declarations
typedef struct timer_info_s timer_info_t;

typedef struct dserv_timer_s {
  tclserver_t *tclserver;
  timer_info_t *info;		/* each timer has access to main info */
  int timer_id;
  volatile sig_atomic_t expired;
  
#ifdef __APPLE__
  dispatch_queue_t queue;
  dispatch_source_t timer;
  volatile sig_atomic_t suspend_count;
#elif defined(__linux__)
  // Try timerfd first, fall back to signal-based if it fails
  int use_signal_fallback;
  
  // timerfd implementation
  int timer_fd;
  struct itimerspec its;
  
  // signal-based fallback implementation
  timer_t posix_timer;
  int is_armed;
#endif
  
  int nrepeats;
  int expirations;
  int timeout_ms;
  int interval_ms;
} dserv_timer_t;

typedef struct timer_info_s
{
  tclserver_t *tclserver;
  int ntimers;
  dserv_timer_t *timers;
  char *dpoint_prefix;
#ifdef __linux__
  int use_signal_fallback;  // Global flag for all timers (signal-based fallback)
#endif
} timer_info_t;

// Forward declaration for Linux timer functions
#ifdef __linux__
void dserv_timer_reset(dserv_timer_t *t);
#endif

void timer_notify_dserv(dserv_timer_t *t)
{
  /* notify dserv */
  char dpoint_name[64];
  snprintf(dpoint_name, sizeof(dpoint_name),
           "%s/%d", t->info->dpoint_prefix, t->timer_id);
           
  ds_datapoint_t *dp = dpoint_new(dpoint_name,
                                  tclserver_now(t->tclserver),
                                  DSERV_NONE, 0, NULL);
  tclserver_set_point(t->tclserver, dp);
}

#ifdef __APPLE__
void timer_handler(dserv_timer_t *t, dispatch_source_t timer)
{
  t->expired = true;
  
  timer_notify_dserv(t);

  if (t->nrepeats != -1 && t->expirations >= t->nrepeats) {
    dispatch_suspend(t->timer);
    t->suspend_count++;
  }
  t->expirations++;
}

int dserv_timer_init(dserv_timer_t *t, timer_info_t *info, int id)
{
  t->timer_id = id;
  char queue_name[64];
  snprintf(queue_name, sizeof(queue_name), "timerQueue%d", module_count);
  t->queue = dispatch_queue_create(queue_name, 0);
  
  t->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, t->queue);
  
  dispatch_source_set_event_handler(t->timer, ^{timer_handler(t, t->timer);});
  
  dispatch_source_set_cancel_handler(t->timer, ^{
      dispatch_release(t->timer);
      dispatch_release(t->queue);
    });
  t->expirations = 0;
  t->nrepeats = -1;
  t->expired = true;
  t->suspend_count = 1;

  t->tclserver = info->tclserver;
  t->info = info;
  return 0;
}

void dserv_timer_destroy(dserv_timer_t *t)
{
  dispatch_source_cancel(t->timer);
}

void dserv_timer_arm_ms(dserv_timer_t *t, int start_ms, int interval_ms, int loop)
{
  if (!t->suspend_count) {
    dispatch_suspend(t->timer);
    t->suspend_count++;
  }
  dispatch_time_t start = dispatch_time(DISPATCH_TIME_NOW, (uint64_t) start_ms*1000000);
  dispatch_source_set_timer(t->timer, start, (uint64_t) interval_ms*1000000, 0);
  if (!interval_ms) t->nrepeats = 0;
  else t->nrepeats = -1;
  t->expired = true;
  t->expirations = 0;
  t->timeout_ms = start_ms;
}

void dserv_timer_reset(dserv_timer_t *t)
{
  if (!t->suspend_count) {
    dispatch_suspend(t->timer);
    t->suspend_count++;
  }
}
  
void dserv_timer_fire(dserv_timer_t *t)
{
  t->expired = false;
  dispatch_resume(t->timer);
  t->suspend_count--;
}

#else // Linux implementation with signal-based fallback

// Global array to map timer signals to timer objects
static dserv_timer_t *g_signal_timers[8] = {NULL};

// Signal handler for timer expiration (fallback implementation)
void timer_signal_handler(int sig, siginfo_t *info, void *context) {
  if (info && info->si_value.sival_ptr) {
    dserv_timer_t *t = (dserv_timer_t *)info->si_value.sival_ptr;
    
    t->expired = true;
    t->expirations++;
    
    // Call notification
    timer_notify_dserv(t);
    
    // Handle repeating timers
    if (t->nrepeats > 0 && t->expirations >= t->nrepeats) {
      // Timer finished
      t->is_armed = 0;
    } else if (t->interval_ms == 0) {
      // Single-shot timer finished
      t->is_armed = 0;
    }
    // For repeating timers (interval_ms > 0), the POSIX timer automatically repeats
  }
}

// Test if timerfd works properly (detects WSL issues)
int test_timerfd_reliability() {
  int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  if (fd == -1) return 0;  // timerfd not available
  
  // Try a simple read to see if we get correct error behavior
  uint64_t exp;
  ssize_t result = read(fd, &exp, sizeof(exp));
  int saved_errno = errno;
  close(fd);
  
  // On WSL, this often returns EINVAL instead of EAGAIN
  if (result == -1 && saved_errno == EINVAL) {
    printf("Warning: Detected WSL timerfd issues, using signal-based fallback\n");
    return 0;  // Use signal-based fallback
  }
  
  return 1;  // timerfd seems to work
}

// Original timerfd worker thread (for native Linux)
void* timerWorkerThread(void *arg) {
  dserv_timer_t *t = (dserv_timer_t *) arg;
  
  uint64_t exp;
  ssize_t s;

  while (1) {
    s = read(t->timer_fd, &exp, sizeof(uint64_t));
    if (s == sizeof(uint64_t)) {
      t->expired = true;
      timer_notify_dserv(t);
      
      t->expirations++;
      if (t->nrepeats > 0 && t->expirations >= t->nrepeats) {
        dserv_timer_reset(t);
      }
    } else if (s == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        usleep(1000);
      } else {
        perror("timerfd read error");
        break;
      }
    }
  }
  
  return NULL;
}

int dserv_timer_init(dserv_timer_t *t, timer_info_t *info, int id)
{
  t->timer_id = id;
  t->expirations = 0;
  t->nrepeats = 0;
  t->expired = true;  // Match the timerfd behavior
  t->tclserver = info->tclserver;
  t->info = info;
  
  // Check if we should use signal-based fallback
  t->use_signal_fallback = info->use_signal_fallback;
  
  if (t->use_signal_fallback) {
    
    // Set up signal handler (only once)
    static int signal_handler_installed = 0;
    if (!signal_handler_installed) {
      struct sigaction sa;
      sa.sa_sigaction = timer_signal_handler;
      sigemptyset(&sa.sa_mask);
      sa.sa_flags = SA_SIGINFO | SA_RESTART;
      
      // Install handler for SIGRTMIN (real-time signal)
      if (sigaction(SIGRTMIN, &sa, NULL) == -1) {
        perror("sigaction");
        return -1;
      }
      signal_handler_installed = 1;
    }
    
    // Create POSIX timer
    struct sigevent sev;
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGRTMIN;
    sev.sigev_value.sival_ptr = t;
    
    if (timer_create(CLOCK_MONOTONIC, &sev, &t->posix_timer) == -1) {
      perror("timer_create");
      return -1;
    }
    
    // Store in global array for signal handler access
    g_signal_timers[id] = t;
    t->is_armed = 0;
  } else {
    // Initialize timerfd-based timer
    t->timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (t->timer_fd == -1) {
      return -1;
    }
  }
  
  return 0;
}

void dserv_timer_destroy(dserv_timer_t *t)
{
  if (t->use_signal_fallback) {
    timer_delete(t->posix_timer);
    g_signal_timers[t->timer_id] = NULL;
  }
  else {
    close(t->timer_fd);
  }
}

void dserv_timer_reset(dserv_timer_t *t)
{
  if (t->use_signal_fallback) {
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    timer_settime(t->posix_timer, 0, &its, NULL);
    t->is_armed = 0;
  }
  else {
    bzero(&t->its, sizeof(t->its));
    timerfd_settime(t->timer_fd, TFD_TIMER_ABSTIME, &t->its, NULL);
  }
}

void dserv_timer_arm_ms(dserv_timer_t *t, int start_ms, int interval_ms, int loop)
{
  if (t->use_signal_fallback) {
    t->timeout_ms = start_ms;
    t->interval_ms = interval_ms;
    if (!interval_ms) t->nrepeats = 0;
    else t->nrepeats = loop;
    t->expired = false;  // Reset expired flag when arming
    t->expirations = 0;
  }
  else {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    t->its.it_value.tv_sec = start_ms/1000;
    t->its.it_value.tv_nsec = (start_ms%1000)*1000000;
    
    t->its.it_interval.tv_sec = interval_ms/1000;
    t->its.it_interval.tv_nsec = (interval_ms%1000)*1000000;

    if (!interval_ms) t->nrepeats = 0;
    else t->nrepeats = loop;
    t->expired = true;
    t->expirations = 0;
    t->timeout_ms = start_ms;
  }
}

void dserv_timer_fire(dserv_timer_t *t)
{
  if (t->use_signal_fallback) {
    if (t->timeout_ms <= 0) {
      // Immediate timer - fire right away
      t->expired = true;
      timer_notify_dserv(t);
      return;
    }
    
    // Set up POSIX timer
    struct itimerspec its;
    its.it_value.tv_sec = t->timeout_ms / 1000;
    its.it_value.tv_nsec = (t->timeout_ms % 1000) * 1000000;
    
    if (t->interval_ms > 0) {
      its.it_interval.tv_sec = t->interval_ms / 1000;
      its.it_interval.tv_nsec = (t->interval_ms % 1000) * 1000000;
    } else {
      its.it_interval.tv_sec = 0;
      its.it_interval.tv_nsec = 0;
    }
    
    t->expired = false;
    t->is_armed = 1;
    
    if (timer_settime(t->posix_timer, 0, &its, NULL) == -1) {
      perror("timer_settime");
      return;
    }
  }
  else {
    if (t->timeout_ms <= 0) return;
    t->expired = false;
    timerfd_settime(t->timer_fd, 0, &t->its, NULL);
  }
}

#endif

static int timer_tick_command (ClientData data, Tcl_Interp *interp,
                               int objc, Tcl_Obj *objv[])
{
  timer_info_t *info = (timer_info_t *) data;
  int timerid = 0;
  int ms;

  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "?id? ms");
    return TCL_ERROR;
  }

  if (objc == 2) {
    if (Tcl_GetIntFromObj(interp, objv[1], &ms) != TCL_OK)
      return TCL_ERROR;
  }
  else {
    if (Tcl_GetIntFromObj(interp, objv[1], &timerid) != TCL_OK)
      return TCL_ERROR;
    if (Tcl_GetIntFromObj(interp, objv[2], &ms) != TCL_OK)
      return TCL_ERROR;
  }

  if (timerid > info->ntimers) {
    Tcl_AppendResult(interp,
                     Tcl_GetString(objv[0]), ": invalid timer", NULL);
    return TCL_ERROR;
  }

  dserv_timer_arm_ms(&info->timers[timerid], ms, 0, 0);
  dserv_timer_fire(&info->timers[timerid]);
  
  return TCL_OK;
}

static int timer_tick_interval_command (ClientData data, Tcl_Interp *interp,
                                        int objc, Tcl_Obj *objv[])
{
  timer_info_t *info = (timer_info_t *) data;
  int timerid = 0;
  int start_ms, interval_ms;
  
  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "?id? ms interval");
    return TCL_ERROR;
  }

  if (objc == 3) {
    if (Tcl_GetIntFromObj(interp, objv[1], &start_ms) != TCL_OK)
      return TCL_ERROR;
    if (Tcl_GetIntFromObj(interp, objv[2], &interval_ms) != TCL_OK)
      return TCL_ERROR;
  }
  else {
    if (Tcl_GetIntFromObj(interp, objv[1], &timerid) != TCL_OK)
      return TCL_ERROR;
    if (Tcl_GetIntFromObj(interp, objv[2], &start_ms) != TCL_OK)
      return TCL_ERROR;
    if (Tcl_GetIntFromObj(interp, objv[3], &interval_ms) != TCL_OK)
      return TCL_ERROR;
  }
  
  if (timerid > info->ntimers) {
    Tcl_AppendResult(interp,
                     Tcl_GetString(objv[0]), ": invalid timer", NULL);
    return TCL_ERROR;
  }

  dserv_timer_arm_ms(&info->timers[timerid], start_ms, interval_ms, 0);
  dserv_timer_fire(&info->timers[timerid]);
  
  return TCL_OK;
}

static int timer_expired_command (ClientData data, Tcl_Interp *interp,
                                  int objc, Tcl_Obj *objv[])
{
  timer_info_t *info = (timer_info_t *) data;
  int timerid = 0;
  
  if (objc > 1) {
    if (Tcl_GetIntFromObj(interp, objv[1], &timerid) != TCL_OK)
      return TCL_ERROR;
  }
  if (timerid > info->ntimers) {
    Tcl_AppendResult(interp,
                     Tcl_GetString(objv[0]), ": invalid timer", NULL);
    return TCL_ERROR;
  }
  
  Tcl_SetObjResult(interp, Tcl_NewIntObj(info->timers[timerid].expired));

  return TCL_OK;
}

static int timer_set_dpoint_prefix_command (ClientData data, Tcl_Interp *interp,
					    int objc, Tcl_Obj *objv[])
{
  timer_info_t *info = (timer_info_t *) data;
  int timerid = 0;
  int ms;

  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "prefix");
    return TCL_ERROR;
  }

  if (info->dpoint_prefix) free(info->dpoint_prefix);
  info->dpoint_prefix = strdup(Tcl_GetString(objv[1]));

  Tcl_SetObjResult(interp, Tcl_NewStringObj(Tcl_GetString(objv[1]), -1));
  return TCL_OK;
}


#ifdef WIN32
EXPORT(int,Dserv_timer_Init) (Tcl_Interp *interp)
#else
  int Dserv_timer_Init(Tcl_Interp *interp)
#endif
{
  
  if (
#ifdef USE_TCL_STUBS
      Tcl_InitStubs(interp, "8.6-", 0)
#else
      Tcl_PkgRequire(interp, "Tcl", "8.6-", 0)
#endif
      == NULL) {
    return TCL_ERROR;
  }
  int ntimers = 8;

  timer_info_t *g_timerInfo = (timer_info_t *) calloc(1, sizeof(timer_info_t));
  g_timerInfo->tclserver = tclserver_get_from_interp(interp);
  g_timerInfo->ntimers = ntimers;
  g_timerInfo->dpoint_prefix = strdup(DEFAULT_TIMER_DPOINT_PREFIX);
  
#ifdef __linux__
  // Test timerfd reliability and decide on implementation FIRST
  g_timerInfo->use_signal_fallback = !test_timerfd_reliability();
#endif
  
  g_timerInfo->timers =
    (dserv_timer_t *) calloc(ntimers, sizeof(dserv_timer_t));
  for (int i = 0; i < ntimers; i++) {
    if (dserv_timer_init(&g_timerInfo->timers[i], g_timerInfo, i) != 0) {
      printf("ERROR: Failed to initialize timer %d\n", i);
      return TCL_ERROR;
    }
  }
  
#ifdef __linux__
  // Create worker threads if using timerfd (not signal-based fallback)
  if (!g_timerInfo->use_signal_fallback) {
    pthread_t w;
    for (int i = 0; i < ntimers; i++) {
      pthread_create(&w, NULL, timerWorkerThread, g_timerInfo.timers[i]);
    }
  } 
#endif

  Tcl_CreateObjCommand(interp, "timerTick",
                       (Tcl_ObjCmdProc *) timer_tick_command,
                       (ClientData) g_timerInfo,
                       (Tcl_CmdDeleteProc *) NULL); 
  Tcl_CreateObjCommand(interp, "timerTickInterval",
                       (Tcl_ObjCmdProc *) timer_tick_interval_command,
                       (ClientData) g_timerInfo,
                       (Tcl_CmdDeleteProc *) NULL); 
  Tcl_CreateObjCommand(interp, "timerExpired",
                       (Tcl_ObjCmdProc *) timer_expired_command,
                       (ClientData) g_timerInfo,
                       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "timerPrefix",
                       (Tcl_ObjCmdProc *) timer_set_dpoint_prefix_command,
                       (ClientData) g_timerInfo,
                       (Tcl_CmdDeleteProc *) NULL); 
  
  Tcl_LinkVar(interp, "nTimers", (char *) &g_timerInfo->ntimers,
              TCL_LINK_INT | TCL_LINK_READ_ONLY);

  module_count++;
  
  return TCL_OK;
}
