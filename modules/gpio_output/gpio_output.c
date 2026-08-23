/*
 * NAME
 *   gpio_output.c
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
#endif

#include <tcl.h>

#include "Datapoint.h"
#include "tclserver_api.h"

typedef struct gpio_info_s
{
  int fd;			/* chip fd */
  int nlines;
  tclserver_t *tclserver;
  char *dpoint_prefix;		/* "gpio/output" */
#ifdef __linux__
  struct gpiohandle_request **line_requests;
#endif
} gpio_info_t;

/* global to this module */
static gpio_info_t g_gpioInfo;




#ifdef __linux__

/* Publish a drive of line <offset> to <value> as gpio/output/<offset>.
 *
 * The output leaves a timestamped trail like every other actuator: the eventlog
 * records host GPIO activity during an experiment, and `dservTimestamp
 * gpio/output/<n>` gives callers a t0 for the edge without bracketing the call
 * in Tcl (which folds interpreter latency into the measurement).
 *
 * Stamped AFTER the driving ioctl returns, so the timestamp is a lower bound on
 * when the line moved rather than an upper one -- the ioctl is what generates
 * the edge, so stamping before it would report a time at which the pin
 * demonstrably had not changed yet.
 *
 * Callers publish only on ioctl success: a failed drive moved no pin, and a
 * datapoint for it would be a lie in the eventlog. */
static void gpio_output_publish(gpio_info_t *info, int offset, int value)
{
  char point_name[64];
  snprintf(point_name, sizeof(point_name), "%s/%d",
	   info->dpoint_prefix, offset);
  ds_datapoint_t *dp = dpoint_new(point_name,
				  tclserver_now(info->tclserver),
				  DSERV_INT, sizeof(int),
				  (unsigned char *) &value);
  tclserver_set_point(info->tclserver, dp);
}

static int gpio_output_init_command(ClientData data,
				   Tcl_Interp *interp,
				   int objc, Tcl_Obj *objv[])
{
  gpio_info_t *info = (gpio_info_t *) data;
  int ret;

  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "chipname");
    return TCL_ERROR;
  }

  /* clean up if we already initialized, so a re-init can switch chips */
  if (info->fd >= 0) {
    if (info->line_requests) {
      for (int i = 0; i < info->nlines; i++) {
	struct gpiohandle_request *req = info->line_requests[i];
	if (req) {		/* line handle was opened, so close it */
	  close(req->fd);
	  free(req);
	}
      }
      free(info->line_requests);
      info->line_requests = NULL;
    }
    close(info->fd);
    info->fd = -1;
    info->nlines = 0;
  }
  
  info->fd = open(Tcl_GetString(objv[1]), O_RDONLY);
  if (info->fd < 0) {
    Tcl_AppendResult(interp, "error opening gpio chip",
		     Tcl_GetString(objv[1]), NULL);
    return TCL_ERROR;
  }
  
  struct gpiochip_info gpioinfo;  
  ret = ioctl(info->fd, GPIO_GET_CHIPINFO_IOCTL, &gpioinfo);
  
  if (ret >= 0) {
    info->nlines = gpioinfo.lines;
    info->line_requests = 
      (struct gpiohandle_request **)
      calloc(info->nlines,
	     sizeof(struct gpiohandle_request *));
  }
  else {
    info->nlines = 0;
    info->line_requests = NULL;
  }

  Tcl_SetObjResult(interp, Tcl_NewIntObj(ret));
  return TCL_OK; 
}

int gpio_line_request_output_command(ClientData data,
				     Tcl_Interp *interp,
				     int objc, Tcl_Obj *objv[])
{
  gpio_info_t *info = (gpio_info_t *) data;
  int offset, value = 0;

  if (info->fd < 0) {
    return TCL_OK;
  }
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "offset [initial_value] ...");
    return TCL_ERROR;
  }

  /* check all args first */
  if (Tcl_GetIntFromObj(interp, objv[1], &offset) != TCL_OK) {
    return TCL_ERROR;
  }
  if (offset < 0 || offset >= info->nlines) {
    Tcl_AppendResult(interp, "invalid line specified for output (",
		     Tcl_GetString(objv[1]), ")",
		     NULL);
    return TCL_ERROR;
  }

  if (objc > 2) {
    if (Tcl_GetIntFromObj(interp, objv[2], &value) != TCL_OK) {
      return TCL_ERROR;
    }
  }
  
  struct gpiohandle_request *req = info->line_requests[offset];
  if (req) {		/* already opened, so close */
    close(req->fd);
  }
  else {
    req = info->line_requests[offset] = (struct gpiohandle_request *)
      calloc(1, sizeof(struct gpiohandle_request));
  }

  req->lineoffsets[0] = offset;
  req->flags = GPIOHANDLE_REQUEST_OUTPUT;
  req->default_values[0] = value;
  strncpy(req->consumer_label, "dserv output", sizeof(req->consumer_label));
  req->lines = 1;
    
  int ret = ioctl(info->fd, GPIO_GET_LINEHANDLE_IOCTL, req);

  /* the request itself drives the line to `value` (default_values[0]), so this
   * is a real edge -- and it seeds gpio/output/<n> so the datapoint reads back
   * correctly before the first gpioLineSetValue. */
  if (ret != -1) gpio_output_publish(info, offset, value);

  Tcl_SetObjResult(interp, Tcl_NewIntObj(ret));
  return TCL_OK;
}

int gpio_line_set_value_command(ClientData data,
				Tcl_Interp *interp,
				int objc, Tcl_Obj *objv[])
{
  gpio_info_t *info = (gpio_info_t *) data;
  int offset, value;
  
  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "offset value");
    return TCL_ERROR;
  }

  /* just return if no gpio set */
  if (info->fd < 0) return TCL_OK;
  
  if (Tcl_GetIntFromObj(interp, objv[1], &offset) != TCL_OK) {
    return TCL_ERROR;
  }
  
  if (Tcl_GetIntFromObj(interp, objv[2], &value) != TCL_OK) {
    return TCL_ERROR;
  }

  /* A negative offset is the declared "no pin" sentinel (`ess obs_pin -1`
   * on rigs whose extio box owns the obs line): an explicit no-op, never
   * an error -- this command sits on the BEGINOBS/ENDOBS hot path, and it
   * used to index line_requests[-1] instead. Out-of-range HIGH stays an
   * error: that is a real misconfiguration, and config-time requests keep
   * rejecting negatives too. */
  if (offset < 0) return TCL_OK;
  if (offset >= info->nlines) {
    Tcl_AppendResult(interp, "invalid line specified for output (",
		     Tcl_GetString(objv[1]), ")",
		     NULL);
    return TCL_ERROR;
  }

  if (!info->line_requests[offset] ||
      (info->line_requests[offset]->fd < 0)) {
    Tcl_AppendResult(interp, "line not set for output (",
		     Tcl_GetString(objv[1]), ")",
		     NULL);
    return TCL_ERROR;
  }
  struct gpiohandle_data datavals;
  datavals.values[0] = value;
  int ret = ioctl(info->line_requests[offset]->fd,
		  GPIOHANDLE_SET_LINE_VALUES_IOCTL, &datavals);

  if (ret != -1) gpio_output_publish(info, offset, value);

  Tcl_SetObjResult(interp, Tcl_NewIntObj(ret));
  return TCL_OK;
}

#else
static int gpio_output_init_command(ClientData data,
				    Tcl_Interp *interp,
				    int objc, Tcl_Obj *objv[])
{
  return TCL_OK;
}

int gpio_line_request_output_command(ClientData data,
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

int gpio_line_set_value_command(ClientData data,
				Tcl_Interp *interp,
				int objc, Tcl_Obj *objv[])
{
  int offset, value;
  
  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "offset value");
    return TCL_ERROR;
  }
  if (Tcl_GetIntFromObj(interp, objv[1], &offset) != TCL_OK) {
    return TCL_ERROR;
  }
  if (Tcl_GetIntFromObj(interp, objv[2], &value) != TCL_OK) {
    return TCL_ERROR;
  }
  return TCL_OK;
}
#endif



/*****************************************************************************
 * EXPORT
 *****************************************************************************/

#ifdef WIN32
EXPORT(int,Dserv_gpio_output_Init) (Tcl_Interp *interp)
#else
int Dserv_gpio_output_Init(Tcl_Interp *interp)
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

  tclserver_t *tclserver = tclserver_get_from_interp(interp);

  g_gpioInfo.fd = -1;
  g_gpioInfo.tclserver = tclserver;
  g_gpioInfo.dpoint_prefix = "gpio/output";

  Tcl_CreateObjCommand(interp, "gpioOutputInit",
		       (Tcl_ObjCmdProc *) gpio_output_init_command,
		       &g_gpioInfo, NULL);
  Tcl_CreateObjCommand(interp, "gpioLineRequestOutput",
		       (Tcl_ObjCmdProc *) gpio_line_request_output_command,
		       &g_gpioInfo, NULL);
  Tcl_CreateObjCommand(interp, "gpioLineSetValue",
		       (Tcl_ObjCmdProc *) gpio_line_set_value_command,
		       &g_gpioInfo, NULL);
  return TCL_OK;
}
