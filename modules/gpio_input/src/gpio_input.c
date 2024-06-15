/*
 * NAME
 *   gpio_input.c
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

#ifdef __linux__
#include <linux/gpio.h>
#define GPIO_CHIP "/dev/gpiochip4"
#include <sys/epoll.h>
#include <pthread.h>
#endif

#include <tcl.h>

#include "Datapoint.h"
#include "tclserver_api.h"

typedef struct gpio_input_s
{
  int line;
#ifdef __linux__  
  struct gpioevent_request req;
  struct epoll_event ev;
  pthread_t input_thread_id;
  int epfd;			/* epoll fd */
#endif
  tclserver_t *tclserver;
  char *dpoint_prefix;
} gpio_input_t;
  
typedef struct gpio_info_s
{
  int fd;			/* chip fd */
  int nlines;
  tclserver_t *tclserver;
  char *dpoint_prefix;
#ifdef __linux__
  gpio_input_t **input_requests;
#endif
} gpio_info_t;

/* global to this module */
static gpio_info_t g_gpioInfo;


#ifdef __linux__

void *input_thread(void *arg)
{
  gpio_input_t *info = (gpio_input_t *) arg;

  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = info->req.fd;
  info->epfd = epoll_create(1);
  int res = epoll_ctl(info->epfd, EPOLL_CTL_ADD, info->req.fd, &ev);
  int nfds, nread;

  char point_name[64];
  sprintf(point_name, "%s/%d", info->dpoint_prefix, info->line);
  int status;
    
  while (1) {
    nfds = epoll_wait(info->epfd, &ev, 1, 20000);
    if (nfds != 0) {
      struct gpioevent_data edata;
      nread = read(info->req.fd, &edata, sizeof(edata));

      status = (edata.id == GPIOEVENT_EVENT_RISING_EDGE) ? 1 : 0;
      
      ds_datapoint_t *dp = dpoint_new(point_name,
				      tclserver_now(info->tclserver),
				      DSERV_INT, sizeof(int),
				      (unsigned char *) &status);
      tclserver_set_point(info->tclserver, dp);
      
      //      printf("%s: %u,%llu [%llu]\n", point_name,
      //	     edata.id, edata.timestamp, tclserver_now(info->tclserver));
    }
  }
}

static int shutdown_input_thread(gpio_input_t *ireq)
{
  pthread_cancel(ireq->input_thread_id);
  close(ireq->epfd);
  close(ireq->req.fd);
  return 0;
}

static int gpio_line_request_input_command(ClientData data,
				    Tcl_Interp *interp,
				    int objc, Tcl_Obj *objv[])
{
  gpio_info_t *info = (gpio_info_t *) data;
  int offset = 0;

  if (info->fd < 0) {
    return TCL_OK;
  }
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "offset [RISING|FALLING|BOTH] ...");
    return TCL_ERROR;
  }

  /* check all args first */
  if (Tcl_GetIntFromObj(interp, objv[1], &offset) != TCL_OK) {
    return TCL_ERROR;
  }
  if (offset >= info->nlines) {
    Tcl_AppendResult(interp, "invalid line specified for input (",
		     Tcl_GetString(objv[1]), ")",
		     NULL);
    return TCL_ERROR;
  }

  if (objc > 2) {
    // allow specification of event type
  }
  
  gpio_input_t *ireq = info->input_requests[offset];
  if (ireq) {		/* already opened, so close */
    shutdown_input_thread(ireq);
    /* wait for thread to shutdown */
    pthread_join(ireq->input_thread_id, NULL);
  }
  else {
    ireq = info->input_requests[offset] = (gpio_input_t *)
      calloc(1, sizeof(gpio_input_t));
  }

  ireq->req.lineoffset = offset;
  ireq->req.handleflags = GPIOHANDLE_REQUEST_INPUT;
  ireq->req.eventflags = GPIOEVENT_REQUEST_BOTH_EDGES;
  strncpy(ireq->req.consumer_label, "dserv input",
	  sizeof(ireq->req.consumer_label));

  ireq->tclserver = info->tclserver;
  ireq->dpoint_prefix = info->dpoint_prefix; /* belongs to global */
  ireq->line = offset;
  
  int ret = ioctl(info->fd, GPIO_GET_LINEEVENT_IOCTL, &ireq->req);
  if (ret != -1) {
    if (pthread_create(&ireq->input_thread_id, NULL, input_thread,
		       (void *) ireq)) {
      return TCL_ERROR;
    }
  }
  
  Tcl_SetObjResult(interp, Tcl_NewIntObj(ret));
  return TCL_OK;
}


static int gpio_line_release_input_command(ClientData data,
				    Tcl_Interp *interp,
				    int objc, Tcl_Obj *objv[])
{
  gpio_info_t *info = (gpio_info_t *) data;
  int offset = 0;
  
  if (info->fd < 0) {
    return TCL_OK;
  }
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "offset");
    return TCL_ERROR;
  }

  /* check all args first */
  if (Tcl_GetIntFromObj(interp, objv[1], &offset) != TCL_OK) {
    return TCL_ERROR;
  }
  if (offset >= info->nlines) {
    Tcl_AppendResult(interp, "invalid line specified (",
		     Tcl_GetString(objv[1]), ")",
		     NULL);
    return TCL_ERROR;
  }

  gpio_input_t *ireq = info->input_requests[offset];
  if (ireq) {		/* already opened, so close */
    shutdown_input_thread(ireq);
    /* wait for thread to shutdown */
    pthread_join(ireq->input_thread_id, NULL);
    free(ireq);
    info->input_requests[offset] = NULL;
    Tcl_SetObjResult(interp, Tcl_NewIntObj(offset));
  }
  else {
    Tcl_SetObjResult(interp, Tcl_NewIntObj(-1));
  }
  return TCL_OK;
}

static int gpio_line_release_all_inputs_command(ClientData data,
						Tcl_Interp *interp,
						int objc, Tcl_Obj *objv[])
{
  gpio_info_t *info = (gpio_info_t *) data;
  int offset;
  gpio_input_t *ireq;
  int nreleased = 0;
  
  if (info->fd < 0) {
    return TCL_OK;
  }

  /* loop through all lines and release if had been requested */
  for (offset = 0; offset < info->nlines; offset++) {
    ireq = info->input_requests[offset];
    if (ireq) {		/* already opened, so close */
      shutdown_input_thread(ireq);
      /* wait for thread to shutdown */
      pthread_join(ireq->input_thread_id, NULL);
      free(ireq);
      info->input_requests[offset] = NULL;
      nreleased++;
    }
  }
  Tcl_SetObjResult(interp, Tcl_NewIntObj(nreleased));
  return TCL_OK;
}

#else
static int gpio_line_request_input_command(ClientData data,
				    Tcl_Interp *interp,
				    int objc, Tcl_Obj *objv[])
{
  int offset, value;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "offset [initial_value]");
    return TCL_ERROR;
  }  
  if (Tcl_GetIntFromObj(interp, objv[1], &offset) != TCL_OK) {
    return TCL_ERROR;
  }
  if (objc > 2) {
    if (Tcl_GetIntFromObj(interp, objv[2], &value) != TCL_OK) {
      return TCL_ERROR;
    }
  }
  return TCL_OK;
}

static int gpio_line_release_input_command(ClientData data,
				    Tcl_Interp *interp,
				    int objc, Tcl_Obj *objv[])
{
  int offset;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "offset");
    return TCL_ERROR;
  }  
  if (Tcl_GetIntFromObj(interp, objv[1], &offset) != TCL_OK) {
    return TCL_ERROR;
  }
  return TCL_OK;
}

static int gpio_line_release_all_inputs_command(ClientData data,
				    Tcl_Interp *interp,
				    int objc, Tcl_Obj *objv[])
{
  return TCL_OK;
}

#endif



/*****************************************************************************
 * EXPORT
 *****************************************************************************/

#ifdef WIN32
EXPORT(int,Dserv_gpio_input_Init) (Tcl_Interp *interp)
#else
int Dserv_gpio_input_Init(Tcl_Interp *interp)
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

  g_gpioInfo.tclserver = tclserver_get();
  g_gpioInfo.dpoint_prefix = "gpio/input";
    
#ifdef __linux__
  /*
   * could do this in another function, but for now, just default to
   * the GPIO_CHIP set above
   */
  int ret;
  struct gpiochip_info info;
  g_gpioInfo.fd = open(GPIO_CHIP, O_RDONLY);
  if (g_gpioInfo.fd >= 0) {
    ret = ioctl(g_gpioInfo.fd, GPIO_GET_CHIPINFO_IOCTL, &info);
    
    if (ret >= 0) {
      g_gpioInfo.nlines = info.lines;
      g_gpioInfo.input_requests =
	(gpio_input_t **) calloc(info.lines, sizeof(struct gpio_input_t *));
    }
    else {
      g_gpioInfo.nlines = 0;
      g_gpioInfo.input_requests = NULL;
    }
  }
#endif
  
  Tcl_CreateObjCommand(interp, "gpioLineRequestInput",
		       (Tcl_ObjCmdProc *) gpio_line_request_input_command,
		       &g_gpioInfo, NULL);
  Tcl_CreateObjCommand(interp, "gpioLineReleaseInput",
		       (Tcl_ObjCmdProc *) gpio_line_release_input_command,
		       &g_gpioInfo, NULL);
  Tcl_CreateObjCommand(interp, "gpioLineReleaseAllInputs",
		       (Tcl_ObjCmdProc *) gpio_line_release_all_inputs_command,
		       &g_gpioInfo, NULL);
  return TCL_OK;
}
