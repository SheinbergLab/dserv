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
#define GPIO_CHIP "/dev/gpiochip4"
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
  timer_t timerid;
  sigset_t           mask;
  long long          freq_nanosecs;
  struct sigevent    sev;
  struct sigaction   sa;
  struct itimerspec  its;
#endif
} juicer_info_t;

/* global to this module */
static juicer_info_t g_juicerInfo;

#ifdef __linux__
void timer_arm_ms(juicer_info_t *info, int start_ms, int interval_ms)
{
  int ms, sec;
  int loop = 0;
  ms = start_ms%1000;
  sec = start_ms/1000;
  info->its.it_value.tv_sec = sec;
  info->its.it_value.tv_nsec = ms*1000000;
  
  ms = interval_ms%1000;
  sec = interval_ms/1000;
  info->its.it_interval.tv_sec = sec;
  info->its.it_interval.tv_nsec = ms*1000000;
  info->expired = 0;
}

void timer_fire(juicer_info_t *info)
{
  timer_settime(info->timerid, 0, &info->its, NULL);
  sigprocmask(SIG_UNBLOCK, &info->mask, NULL);
  info->expired = 0;
}

static void juicer_handler(int sig, siginfo_t *si, void *uc)
{
  juicer_info_t *info;
  info = (juicer_info_t *) si->si_value.sival_ptr;
  
  info->expired = 1;
  bzero(&info->its, sizeof(info->its));
  timer_settime(info->timerid, 0, &info->its, NULL);

  
  /* set juice low */
  if (info->fd != -1 &&
      info->juice_pin >= 0 && info->juice_pin < info->nlines &&
      info->line_requests[info->juice_pin]) {
    struct gpiohandle_data datavals;
    datavals.values[0] = 0;
    int ret = ioctl(info->line_requests[info->juice_pin]->fd,
		    GPIOHANDLE_SET_LINE_VALUES_IOCTL, &datavals);
  }
}
#endif

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
  
#ifdef __linux__
  if (info->fd != -1 &&
      info->juice_pin >= 0 && info->juice_pin < info->nlines &&
      info->line_requests[info->juice_pin]) {
    struct gpiohandle_data datavals;
    datavals.values[0] = 1;
    int ret = ioctl(info->line_requests[info->juice_pin]->fd,
		    GPIOHANDLE_SET_LINE_VALUES_IOCTL, &datavals);
  }
  timer_arm_ms(info, ms, 0);
  timer_fire(info);
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
      Tcl_InitStubs(interp, "8.6", 0)
#else
      Tcl_PkgRequire(interp, "Tcl", "8.6", 0)
#endif
      == NULL) {
    return TCL_ERROR;
  }

  g_juicerInfo.njuicers = 1;
  g_juicerInfo.juice_pin = -1;	/* not set */
    
#ifdef __linux__
  int ret;
  struct gpiochip_info info;
  g_juicerInfo.fd = open(GPIO_CHIP, O_RDONLY);
  if (g_juicerInfo.fd >= 0) {
    ret = ioctl(g_juicerInfo.fd, GPIO_GET_CHIPINFO_IOCTL, &info);
    
    if (ret >= 0) {
      g_juicerInfo.nlines = info.lines;
      g_juicerInfo.line_requests = 
	(struct gpiohandle_request **)
	calloc(info.lines,
	       sizeof(struct gpiohandle_request *));
    }
    else {
      g_juicerInfo.nlines = 0;
      g_juicerInfo.line_requests = NULL;
    }
  }
#endif

  
#ifdef __linux__
  g_juicerInfo.sa.sa_flags = SA_SIGINFO;
  g_juicerInfo.sa.sa_sigaction = juicer_handler;
  sigemptyset(&g_juicerInfo.sa.sa_mask);
  sigaction(SIGRTMIN, &g_juicerInfo.sa, NULL);
  
  /* Block timer signal temporarily. */
  //    printf("Blocking signal %d\n", SIGRTMIN);
  sigemptyset(&g_juicerInfo.mask);
  sigaddset(&g_juicerInfo.mask, SIGRTMIN);
  sigprocmask(SIG_SETMASK, &g_juicerInfo.mask, NULL);
  
  /* Create the timer. */
  g_juicerInfo.sev.sigev_notify = SIGEV_SIGNAL;
  g_juicerInfo.sev.sigev_signo = SIGRTMIN;
  g_juicerInfo.sev.sigev_value.sival_ptr =  &g_juicerInfo;
  timer_create(CLOCK_REALTIME, &g_juicerInfo.sev, &g_juicerInfo.timerid);
#endif
  
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
