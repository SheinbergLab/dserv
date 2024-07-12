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
#ifdef __linux__
  struct gpiohandle_request **line_requests;
#endif
} gpio_info_t;

/* global to this module */
static gpio_info_t g_gpioInfo;




#ifdef __linux__

static int gpio_input_init_command(ClientData data,
				   Tcl_Interp *interp,
				   int objc, Tcl_Obj *objv[])
{
  int chipnum;
  char *chipstr;
  char chipstr_buf[128];
  
  gpio_info_t *info = (gpio_info_t *) data;
  int ret;

  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "chipnum|chipname");
    return TCL_ERROR;
  }

  if (Tcl_GetIntFromObj(interp, objv[1], &chipnum) == TCL_OK) {
    snprintf(chipstr_buf, sizeof(chipstr_buf), "/dev/gpiochip%d", chipnum);
  }
  else {
    chipstr = Tcl_GetString(objv[1]);
  }
		 
  /* try /dev/gpiochip4, which is is the 40-pin header on rpi5 */
  g_gpioInfo.fd = open(chipstr, O_RDONLY);
  if (g_gpioInfo.fd < 0) {
    Tcl_AppendResult(interp, "error opening gpio chip", chipstr, NULL);
    return TCL_ERROR;
  }
  
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
  if (offset >= info->nlines) {
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
      Tcl_InitStubs(interp, "8.6", 0)
#else
      Tcl_PkgRequire(interp, "Tcl", "8.6", 0)
#endif
      == NULL) {
    return TCL_ERROR;
  }

  tclserver_t *tclserver = tclserver_get();
  
  Tcl_CreateObjCommand(interp, "gpioOutputtInit",
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
