/*
 * NAME
 *   timer.c
 *
 * DESCRIPTION
 *
 * AUTHOR
 *   DLS, 07/24
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

#ifdef __linux__
#include <sys/timerfd.h>
#include <strings.h>
#endif

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif

#include <tcl.h>
#include "Datapoint.h"
#include "tclserver_api.h"

const char *TIMER_DPOINT_PREFIX = "timer";

typedef struct tick_info_s {
  struct timer_info_s *info;
  int timerid;			/* timer id */
  int ms;
} tick_info_t;

typedef struct dserv_timer_s {
  tclserver_t *tclserver;
  int timer_id;
  volatile sig_atomic_t expired;
#ifdef __APPLE__
  dispatch_queue_t queue;
  dispatch_source_t timer;
  volatile sig_atomic_t suspend_count;
#else
  int timer_fd;
  struct itimerspec  its;
#endif
  int nrepeats;
  int expirations;
  int timeout_ms;
} dserv_timer_t;

typedef struct timer_info_s
{
  tclserver_t *tclserver;
  int ntimers;
  dserv_timer_t *timers;
} timer_info_t;

/* global to this module */
static timer_info_t g_timerInfo;

void timer_notify_dserv(dserv_timer_t *t)
{
  /* notify dserv */
  char dpoint_name[64];
  snprintf(dpoint_name, sizeof(dpoint_name),
	   "%s/%d", TIMER_DPOINT_PREFIX, t->timer_id);
	   
  ds_datapoint_t *dp = dpoint_new(dpoint_name,
				  tclserver_now(t->tclserver),
				  DSERV_NONE, 0, NULL);
  tclserver_set_point(t->tclserver, dp);
}

#ifdef __APPLE__
/*
 * For MacOS we use the grand central dispatch system to manage timers
 */

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
  t->queue = dispatch_queue_create("timerQueue", 0);
  
  // Create dispatch timer source
  t->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, t->queue);
  
  // Set block for dispatch source when catched events
  dispatch_source_set_event_handler(t->timer, ^{timer_handler(t, t->timer);});
  
  // Set block for dispatch source when cancel source
  dispatch_source_set_cancel_handler(t->timer, ^{
      dispatch_release(t->timer);
      dispatch_release(t->queue);
    });
  t->expirations = 0;
  t->nrepeats = -1;
  t->expired = true;
  t->suspend_count = 1;

  t->tclserver = info->tclserver;
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
  t->nrepeats = -1;		/* loop not yet implemented */
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

#else

int dserv_timer_init(dserv_timer_t *t, timer_info_t *info, int id)
{
  t->timer_id = id;
  t->timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
  t->expirations = 0;
  t->nrepeats = 0;
  t->expired = true;
  t->tclserver = info->tclserver;
  return 0;
}

void dserv_timer_destroy(dserv_timer_t *t)
{
  close(t->timer_fd);
}


void dserv_timer_reset(dserv_timer_t *t)
{
  bzero(&t->its, sizeof(t->its));
  timerfd_settime(t->timer_fd, TFD_TIMER_ABSTIME, &t->its, NULL);
}

void dserv_timer_arm_ms(dserv_timer_t *t, int start_ms,
			int interval_ms, int loop)
{
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
  
void dserv_timer_fire(dserv_timer_t *t)
{
  if (t->timeout_ms <= 0) return;
  t->expired = false;
  timerfd_settime(t->timer_fd, 0, &t->its, NULL);
}

void* timerWorkerThread(void *arg) {
  dserv_timer_t *t = (dserv_timer_t *) arg;
  
  uint64_t exp;
  ssize_t s;

  /* if we read a 64bit int from t->timer_fd our timer expired */
  while (1) {
    s = read(t->timer_fd, &exp, sizeof(uint64_t));
    if (s == sizeof(uint64_t)) {
      t->expired = true;
      timer_notify_dserv(t);
    }
  }
  
  return NULL;
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

/*****************************************************************************
 *
 * EXPORT
 *
 *****************************************************************************/

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
  g_timerInfo.tclserver = tclserver_get();
  g_timerInfo.ntimers = ntimers;
  g_timerInfo.timers =
    (dserv_timer_t *) calloc(ntimers, sizeof(dserv_timer_t));
  for (int i = 0; i < ntimers; i++) {
    dserv_timer_init(&g_timerInfo.timers[i], &g_timerInfo, i);
  }
  
#ifdef __linux__
  /* setup a worker thread to listen for expirations on each timer */
  pthread_t w;
  for (int i = 0; i < ntimers; i++) {
    pthread_create(&w, NULL, timerWorkerThread, &g_timerInfo.timers[i]);
  }
#endif

  Tcl_CreateObjCommand(interp, "timerTick",
		       (Tcl_ObjCmdProc *) timer_tick_command,
		       (ClientData) &g_timerInfo,
		       (Tcl_CmdDeleteProc *) NULL); 
  Tcl_CreateObjCommand(interp, "timerTickInterval",
		       (Tcl_ObjCmdProc *) timer_tick_interval_command,
		       (ClientData) &g_timerInfo,
		       (Tcl_CmdDeleteProc *) NULL); 
  Tcl_CreateObjCommand(interp, "timerExpired",
		       (Tcl_ObjCmdProc *) timer_expired_command,
		       (ClientData) &g_timerInfo,
		       (Tcl_CmdDeleteProc *) NULL);

  Tcl_LinkVar(interp, "nTimers", (char *) &g_timerInfo.ntimers,
              TCL_LINK_INT | TCL_LINK_READ_ONLY);
  
  return TCL_OK;
}



