/*
 * NAME
 *   juicer.c
 *
 * DESCRIPTION
 *
 * AUTHOR
 *   DLS, 06/24
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

#ifdef __linux__
#include <linux/gpio.h>
#include <pthread.h>
#include <sys/timerfd.h>
#endif

#include <tcl.h>
#include "Datapoint.h"
#include "tclserver_api.h"

typedef struct juicer_info_s
{
  int fd;			/* chip fd */
  int nlines;
#ifdef __linux__
  struct gpiohandle_request **line_requests;
#endif
  int njuicers;
  int juice_pin;
  int expired;
#ifdef __linux__
  pthread_t timer_thread_id;
  struct timespec juice_delay;
  pthread_cond_t cond;
  pthread_mutex_t mutex;
#endif
} juicer_info_t;

/* global to this module */
static juicer_info_t g_juicerInfo;

#ifdef __linux__
void *timer_thread(void *arg)
{
  juicer_info_t *info = (juicer_info_t *) arg;
  while (1) {
    pthread_mutex_lock(&info->mutex);
    pthread_cond_wait(&info->cond, &info->mutex);
    
    /* sleep until time to turn juice off */
    nanosleep(&info->juice_delay, NULL);

    /* set juice low */
    if (info->fd != -1 &&
	info->juice_pin >= 0 && info->juice_pin < info->nlines &&
	info->line_requests[info->juice_pin]) {
      struct gpiohandle_data datavals;
      datavals.values[0] = 0;
      int ret = ioctl(info->line_requests[info->juice_pin]->fd,
		      GPIOHANDLE_SET_LINE_VALUES_IOCTL, &datavals);
    }
    pthread_mutex_unlock(&info->mutex);
    //printf("juice off\n");
  }
}
#endif

static int juicer_init_command (ClientData data, Tcl_Interp *interp,
				int objc, Tcl_Obj *objv[])
{
  juicer_info_t *jinfo = (juicer_info_t *) data;

  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "chipname");
    return TCL_ERROR;
  }
  
#ifdef __linux__
  if (jinfo->fd > 0) return TCL_OK; /* could reinitialize */
  
  int ret;
  struct gpiochip_info info;
  jinfo->fd = open(Tcl_GetString(objv[1]), O_RDONLY);
  if (jinfo->fd >= 0) {
    ret = ioctl(jinfo->fd, GPIO_GET_CHIPINFO_IOCTL, &info);
    
    if (ret >= 0) {
      jinfo->nlines = info.lines;
      jinfo->line_requests = 
	(struct gpiohandle_request **)
	calloc(info.lines,
	       sizeof(struct gpiohandle_request *));
    }
    else {
      jinfo->nlines = 0;
      jinfo->line_requests = NULL;
    }
  }
#endif
  
  
#ifdef __linux__
  if (pthread_mutex_init(&jinfo->mutex, NULL) != 0) {
    perror("pthread_mutex_init() error");                                       
    return TCL_ERROR;
  }                                                                             
  
  if (pthread_cond_init(&jinfo->cond, NULL) != 0) {
    perror("pthread_cond_init() error");                                        
    return TCL_ERROR;
  }
  
  if (pthread_create(&jinfo->timer_thread_id, NULL, timer_thread,
		     (void *) jinfo)) {
    return TCL_ERROR;
  }
  pthread_detach(jinfo->timer_thread_id);
  
#endif
  return TCL_OK;
}

static int juicer_juice_command (ClientData data, Tcl_Interp *interp,
				 int objc, Tcl_Obj *objv[])
{
  juicer_info_t *info = (juicer_info_t *) data;

  int id, ms;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "?juicerid? start");
    return TCL_ERROR;
  }
  
  if (objc < 3) {		/* default to timer 0 */
    id = 0;
    if (Tcl_GetIntFromObj(interp, objv[1], &ms) != TCL_OK)
      return TCL_ERROR;
  }
  
  else {
    if (Tcl_GetIntFromObj(interp, objv[1], &id) != TCL_OK)
      return TCL_ERROR;
    if (Tcl_GetIntFromObj(interp, objv[2], &ms) != TCL_OK)
      return TCL_ERROR;
  }

  if (ms <= 0) return TCL_OK;

  //printf("juice on\n");
#ifdef __linux__
  info->juice_delay.tv_sec = ms/1000;
  info->juice_delay.tv_nsec = (ms%1000)*1000000;

  if (info->fd != -1 &&
      info->juice_pin >= 0 && info->juice_pin < info->nlines &&
      info->line_requests[info->juice_pin]) {
    struct gpiohandle_data datavals;
    datavals.values[0] = 1;
    int ret = ioctl(info->line_requests[info->juice_pin]->fd,
		    GPIOHANDLE_SET_LINE_VALUES_IOCTL, &datavals);
  }

  if (pthread_cond_signal(&info->cond) != 0) {
    perror("pthread_cond_signal() error");                                      
    return TCL_ERROR;
  }   
#endif
  Tcl_SetObjResult(interp, Tcl_NewIntObj(id));
  return TCL_OK;
}

static int juicer_set_pin_command (ClientData data, Tcl_Interp *interp,
				   int objc, Tcl_Obj *objv[])
{
  juicer_info_t *info = (juicer_info_t *) data;

  int id, pin;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "?juicerid? pin");
    return TCL_ERROR;
  }
  
  if (objc < 3) {		/* default to timer 0 */
    id = 0;
    if (Tcl_GetIntFromObj(interp, objv[1], &pin) != TCL_OK)
      return TCL_ERROR;
  }
  
  else {
    if (Tcl_GetIntFromObj(interp, objv[1], &id) != TCL_OK)
      return TCL_ERROR;
    
    if (id >= info->njuicers) {
      const char *msg = "invalid juicer";
      Tcl_SetResult(interp, (char *) msg, TCL_STATIC);
      return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[2], &pin) != TCL_OK)
      return TCL_ERROR;
  }

#ifdef __linux__
  if (info->fd == -1) return TCL_OK;
  
  if (info->juice_pin >= 0 && info->line_requests[info->juice_pin]) {
    close(info->line_requests[info->juice_pin]->fd);
    free(info->line_requests[info->juice_pin]);
  }
  if (pin >= info->nlines) {
    Tcl_AppendResult(interp, "invalid pin selected", NULL);
    return TCL_ERROR;
  }
  struct gpiohandle_request *req;  
  req = info->line_requests[pin] = (struct gpiohandle_request *)
    calloc(1, sizeof(struct gpiohandle_request));

  req->lineoffsets[0] = pin;
  req->flags = GPIOHANDLE_REQUEST_OUTPUT;
  req->default_values[0] = 0;
  strncpy(req->consumer_label, "juicer output", sizeof(req->consumer_label));
  req->lines = 1;

  int ret = ioctl(info->fd, GPIO_GET_LINEHANDLE_IOCTL, req);  
#endif
  info->juice_pin = pin;
  Tcl_SetObjResult(interp, Tcl_NewIntObj(id));
  return TCL_OK;
}

/*****************************************************************************
 *
 * EXPORT
 *
 *****************************************************************************/

#ifdef WIN32
EXPORT(int,Dserv_juicer_Init) (Tcl_Interp *interp)
#else
  int Dserv_juicer_Init(Tcl_Interp *interp)
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

  g_juicerInfo.njuicers = 1;
  g_juicerInfo.juice_pin = -1;	/* not set */
  
  
  Tcl_CreateObjCommand(interp, "juicerInit",
		       (Tcl_ObjCmdProc *) juicer_init_command,
		       (ClientData) &g_juicerInfo,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "juicerJuice",
		       (Tcl_ObjCmdProc *) juicer_juice_command,
		       (ClientData) &g_juicerInfo,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "juicerSetPin",
		       (Tcl_ObjCmdProc *) juicer_set_pin_command,
		       (ClientData) &g_juicerInfo,
		       (Tcl_CmdDeleteProc *) NULL);
  return TCL_OK;
}
