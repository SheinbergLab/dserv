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
#include <termios.h>
#include <pthread.h>
#include <semaphore.h>

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif


#include <tcl.h>
#include "Datapoint.h"
#include "tclserver_api.h"

const char *TIMER_DPOINT_PREFIX = "timer";

/*************************************************************************/
/***                   queues for timer tick requests                  ***/
/*************************************************************************/

/*
  semaphore example:
  http://www2.lawrence.edu/fast/GREGGJ/CMSC480/net/workerThreads.html
*/
#define QUEUE_SIZE 16

struct tick_info_s;
struct timer_info_s;

typedef struct {
  struct tick_info_s *d[QUEUE_SIZE];
  int front;
  int back;
  sem_t *mutex;
  sem_t *slots;
  sem_t *items;
#ifndef __APPLE__
  sem_t unnamed_mutex;
  sem_t unnamed_slots;
  sem_t unnamed_items;
#endif
} queue;

queue* queueCreate();
void enqueue(queue* q, struct tick_info_s *tickinfo);
struct tick_info_s *dequeue(queue* q);

queue* queueCreate() {
    queue *q = (queue*) malloc(sizeof(queue));
    q->front = 0;
    q->back = 0;

#ifdef __APPLE__
    q->mutex = sem_open ("qMutex", O_CREAT | O_EXCL, 0644, 1); 
    sem_unlink ("qMutex");      

    q->slots = sem_open ("qSlots", O_CREAT | O_EXCL, 0644, QUEUE_SIZE); 
    sem_unlink ("qSlots");      

    q->items = sem_open ("qItems", O_CREAT | O_EXCL, 0644, 0); 
    sem_unlink ("qItems");      
#else
    q->mutex = &q->unnamed_mutex;
    sem_init(q->mutex, 0, 1);

    q->slots = &q->unnamed_slots;
    sem_init(q->slots, 0, QUEUE_SIZE);

    q->items = &q->unnamed_items;
    sem_init(q->items, 0, 0);
#endif
    return q;
}

void enqueue(queue* q, struct tick_info_s *tickinfo) {
    sem_wait(q->slots);
    sem_wait(q->mutex);
    q->d[q->back] = tickinfo;
    q->back = (q->back+1)%QUEUE_SIZE;
    sem_post(q->mutex);
    sem_post(q->items);
}

struct tick_info_s *dequeue(queue* q) {
  struct tick_info_s *tick_info;
  sem_wait(q->items);
  sem_wait(q->mutex);
  tick_info = q->d[q->front];
  q->front = (q->front+1)%QUEUE_SIZE;
  sem_post(q->mutex);
  sem_post(q->slots);
  return tick_info;
}

typedef struct tick_info_s {
  struct timer_info_s *info;
  int timerid;			/* timer id */
  int ms;
} tick_info_t;

#ifdef __APPLE__
typedef struct dserv_timer_s {
  tclserver_t *tclserver;
  int timer_id;
  dispatch_queue_t queue;
  dispatch_source_t timer;
  int nrepeats;
  int expirations;
  volatile sig_atomic_t suspend_count;
  int expired;
} dserv_timer_t;
#endif


typedef struct timer_info_s
{
  tclserver_t *tclserver;
  int ntimers;
#ifdef __APPLE__
  dserv_timer_t *timers;
#else
  queue* q;
  int *expired;
  tick_info_t **requests;
#endif
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

int dserv_timer_init(dserv_timer_t *t, timer_info_t *info)
{
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
  t->nrepeats = 0;
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
  else t->nrepeats = loop;
  t->expired = true;
  t->expirations = 0;
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

#endif

#ifndef __APPLE__
int tickRequest(tick_info_t *tickinfo)
{
  struct timespec rqtp = { tickinfo->ms/1000, (tickinfo->ms%1000)*1000000 };
  nanosleep(&rqtp, NULL);

  /* ignore expiration if another timer request has been made */
  if (tickinfo != g_timerInfo.requests[tickinfo->timerid]) return 0;
  
  tickinfo->info->expired[tickinfo->timerid] = 1;

  /* notify system */
  char dpoint_name[64];
  snprintf(dpoint_name, sizeof(dpoint_name),
	   "%s/%d", TIMER_DPOINT_PREFIX, tickinfo->timerid);
	   
  ds_datapoint_t *dp = dpoint_new(dpoint_name,
				  tclserver_now(tickinfo->info->tclserver),
				  DSERV_NONE, 0, NULL);
  tclserver_set_point(tickinfo->info->tclserver, dp);
  
  /* free the request */
  free(tickinfo);
  return 0;
}

void* workerThread(void *arg) {
  timer_info_t *info = (timer_info_t *) arg;

  ds_datapoint_t timer_dpoint;
  
  while(1) {
    tick_info_t *req = dequeue(info->q);
    tickRequest(req);
  }
  return NULL;
}

static tick_info_t *new_tickinfo(timer_info_t *info, int timerid, int ms)
{
  tick_info_t *request = (tick_info_t *) malloc(sizeof(tick_info_t));
  request->info = info;
  request->timerid = timerid;
  request->ms = ms;		/* when to turn off    */
  return request;
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
#ifdef __APPLE__
  dserv_timer_arm_ms(&info->timers[timerid], ms, 0, 0);
  dserv_timer_fire(&info->timers[timerid]);
#else
  if (ms > 0) {
    tick_info_t *request = new_tickinfo(info, timerid, ms);
    enqueue(info->q, request);
    info->expired[timerid] = 0;
    info->requests[timerid] = request;
  }
  else info->expired[timerid] = 1;
#endif
  
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
#ifdef __APPLE__
  Tcl_SetObjResult(interp, Tcl_NewIntObj(info->timers[timerid].expired));
#else
  Tcl_SetObjResult(interp, Tcl_NewIntObj(info->expired[timerid]));
#endif
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
      Tcl_InitStubs(interp, "8.6", 0)
#else
      Tcl_PkgRequire(interp, "Tcl", "8.6", 0)
#endif
      == NULL) {
    return TCL_ERROR;
  }
  const int nworkers = 6;

#ifdef __APPLE__
  int ntimers = 8;
  g_timerInfo.tclserver = tclserver_get();
  g_timerInfo.ntimers = ntimers;
  g_timerInfo.timers =
    (dserv_timer_t *) calloc(ntimers, sizeof(dserv_timer_t));
  for (int i = 0; i < ntimers; i++) {
    dserv_timer_init(&g_timerInfo.timers[i], &g_timerInfo);
  }
    
#else
  g_timerInfo.tclserver = tclserver_get();
  g_timerInfo.q = queueCreate();
  g_timerInfo.ntimers = 8;
  g_timerInfo.expired =
    (int *) calloc(g_timerInfo.ntimers, sizeof(int));
  g_timerInfo.requests =
    (tick_info_t **) calloc(g_timerInfo.ntimers, sizeof(tick_info_t *));

  for (int i = 0; i < g_timerInfo.ntimers; i++) {
    g_timerInfo.expired[i] = 1;
  }
  
  /* setup workers */
  pthread_t w;
  for (int i = 0; i < nworkers; i++) {
    pthread_create(&w, NULL, workerThread, &g_timerInfo);
  }
#endif

  Tcl_CreateObjCommand(interp, "timerTick",
		       (Tcl_ObjCmdProc *) timer_tick_command,
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



