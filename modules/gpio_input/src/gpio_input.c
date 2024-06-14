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
#ifdef __linux__  
  struct gpioevent_request req;
  struct epoll_event ev;
  pthread_t input_thread_id;
#endif
  char *dpoint_name;
} gpio_input_t;
  
typedef struct gpio_info_s
{
  int fd;			/* chip fd */
  int nlines;
#ifdef __linux__
  struct gpio_input_t **input_requests;
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
  ev.data.fd = info->req->fd;
  int epfd = epoll_create(1);
  int res = epoll_ctrl(epfd, EPOLL_CTL_ADD, info->req->fd, &ev);
  int nfds, res;
  
  while (1) {
    nfds = epoll_wait(epfd, &ev, 1, 20000);
    if (nfds != 0) {
      struct gpioevent_data edata;
      res = read(req.fd, &edata, sizeof(edata));
      printf("%u,%llu\n", edata.id, edata.timestamp);
    }
  }
}


int gpio_line_request_input_command(ClientData data,
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
    Tcl_AppendResult(interp, "invalid line specified for output (",
		     Tcl_GetString(objv[1]), ")",
		     NULL);
    return TCL_ERROR;
  }

  if (objc > 2) {
    // allow specification of event type
  }
  
  struct gpio_input_t *ireq = info->event_requests[offset];
  if (ireq) {		/* already opened, so close */
    /* clean up! */
    close(ireq->req.fd);
    /* shutdown receive thread */
  }
  else {
    ireq = info->input_requests[offset] = (struct gpioinput_request *)
      calloc(1, sizeof(struct gpioinput_request));
  }

  ireq->req.lineoffsets[0] = offset;
  ireq->req.handleflags = GPIOHANDLE_REQUEST_INPUT;
  ireq->req.eventflags = GPIOEVENT_REQUEST_RISING_EDGE;
  strncpy(ireq->req.consumer_label, "dserv input",
	  sizeof(ireq->req.consumer_label));
    
  int ret = ioctl(info->fd, GPIO_GET_LINEEVENT_IOCTL, &ireq->req);
  if (ret != -1) {
    if (pthread_create(&ireq->input_thread_id, NULL, input_thread,
		       (void *) req)) {
      return TCL_ERROR;
    }
  }
  
  Tcl_SetObjResult(interp, Tcl_NewIntObj(ret));
  return TCL_OK;
}

#else
int gpio_line_request_input_command(ClientData data,
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

  tclserver_t *tclserver = tclserver_get();

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
      g_gpioInfo.line_requests = 
	(struct gpiohandle_request **) calloc(info.lines,
					      sizeof(struct gpiohandle_request *));
    }
    else {
      g_gpioInfo.nlines = 0;
      g_gpioInfo.line_requests = NULL;
    }
  }
#endif
  
  Tcl_CreateObjCommand(interp, "gpioLineRequestInput",
		       (Tcl_ObjCmdProc *) gpio_line_request_input_command,
		       &g_gpioInfo, NULL);
  return TCL_OK;
}
