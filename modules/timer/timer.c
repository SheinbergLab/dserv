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

typedef struct timer_info_s
{
  tclserver_t *tclserver;
  queue* q;
  int ntimers;
  int *expired;
  tick_info_t **requests;
} timer_info_t;

/* global to this module */
static timer_info_t g_timerInfo;


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
  
  if (ms > 0) {
    tick_info_t *request = new_tickinfo(info, timerid, ms);
    enqueue(info->q, request);
    info->expired[timerid] = 0;
    info->requests[timerid] = request;
  }
  else info->expired[timerid] = 1;
  
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

  Tcl_SetObjResult(interp, Tcl_NewIntObj(info->expired[timerid]));
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

  Tcl_CreateObjCommand(interp, "timerTick",
		       (Tcl_ObjCmdProc *) timer_tick_command,
		       (ClientData) &g_timerInfo,
		       (Tcl_CmdDeleteProc *) NULL); 
  Tcl_CreateObjCommand(interp, "timerExpired",
		       (Tcl_ObjCmdProc *) timer_expired_command,
		       (ClientData) &g_timerInfo,
		       (Tcl_CmdDeleteProc *) NULL);
 return TCL_OK;
}



