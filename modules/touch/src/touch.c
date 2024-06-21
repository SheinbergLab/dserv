/*
 * NAME
 *   touch.c
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
#include <pthread.h>

#ifdef __linux__
#include <libevdev/libevdev.h>
#include <linux/input.h>
#endif

#include <tcl.h>

#include "Datapoint.h"
#include "tclserver_api.h"

typedef struct touch_info_s
{
  pthread_t input_thread_id;
  int input_thread_running;		/* should be atomic... */
  int fd;
  tclserver_t *tclserver;
  char *dpoint_prefix;
#ifdef __linux__
  struct libevdev *dev;
#endif
  int screen_width;
  int screen_height;
  int maxx, maxy, minx, miny;
  float rangex, rangey;
} touch_info_t;

/* global to this module */
static touch_info_t g_touchInfo;

#ifdef __linux__

void *input_thread(void *arg)
{
  touch_info_t *info = (touch_info_t *) arg;

  char point_name[64];
  sprintf(point_name, "%s/touch", info->dpoint_prefix);
  int status;

  struct input_event ev;
  int x, y;
  int begin_touch = 0;
  int rc;
  
  static char buf[128];
  

  do {
    rc = libevdev_next_event(info->dev, LIBEVDEV_READ_FLAG_BLOCKING, &ev);
    if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
      switch (ev.type) {
      case EV_KEY:
	switch (ev.code) {
	case 330:
	  if (ev.value == 1) { begin_touch = 1; }
	  else if (ev.value == 0) { /* touch release event */ }
	  break;
	}
      case EV_ABS:
	switch (ev.code) {
	case 0:
	  if (ev.value > 0) {
	    x =
	      (int) (info->screen_width*((ev.value-info->minx)/info->rangex));
	  }
	  break;
	case 1:
	  if (ev.value > 0) {
	    y =
	      (int) (info->screen_height*((ev.value-info->miny)/info->rangey));
	    if (begin_touch) {
	      begin_touch = 0;
	      snprintf(buf, sizeof(buf), "0 0 %d %d", x, y);
	      ds_datapoint_t *dp = dpoint_new(point_name,
					      tclserver_now(info->tclserver),
					      DSERV_STRING, strlen(buf), buf);
	      tclserver_set_point(info->tclserver, dp);
	    }
	    else {
	      /* could log touch move here */
	    }
	  }
	  break;
	default:
	  break;
	}
	break;
      default:
	break;
      }
    }
  } while (rc >= 0);
  
}

static int touch_open_command(ClientData data,
			       Tcl_Interp *interp,
			       int objc, Tcl_Obj *objv[])
{
  touch_info_t *info = (touch_info_t *) data;
  int width, height;

  struct libevdev *dev;

  int fd;
  int rc;
    
  if (objc < 4) {
    Tcl_WrongNumArgs(interp, 1, objv, "path width height");
    return TCL_ERROR;
  }
  if (Tcl_GetIntFromObj(interp, objv[2], &width) != TCL_OK) {
    return TCL_ERROR;
  }
  if (Tcl_GetIntFromObj(interp, objv[3], &height) != TCL_OK) {
    return TCL_ERROR;
  }  
  
  fd = open(Tcl_GetString(objv[1]), O_RDONLY);
  if (fd < 0) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		     ": error opening ", Tcl_GetString(objv[1]),
		     NULL);
    return TCL_ERROR;
  }

  rc = libevdev_new_from_fd(fd, &dev);
  if (rc < 0) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		     ": error creating libev device",
		     NULL);
    close(fd);
    return TCL_ERROR;
  }
  
  //  libevdev_get_id_vendor(dev);
  //  libevdev_get_id_product(dev);

  info->minx = libevdev_get_abs_minimum(dev, ABS_X);
  info->maxx = libevdev_get_abs_maximum(dev, ABS_X);

  info->miny = libevdev_get_abs_minimum(dev, ABS_Y);
  info->maxy = libevdev_get_abs_maximum(dev, ABS_Y);

  info->rangex = info->maxx-info->minx;
  info->rangey = info->maxy-info->miny;

  info->dev = dev;
  info->fd = fd;
  info->screen_width = width;
  info->screen_height = height;

  return TCL_OK;
}

static int touch_close_command(ClientData data,
			       Tcl_Interp *interp,
			       int objc, Tcl_Obj *objv[])
{
  touch_info_t *info = (touch_info_t *) data;
  
  if (info->input_thread_running) {
    /* cancel input thread */
    pthread_cancel(info->input_thread_id);
    
    /* wait for thread to shutdown */
    pthread_join(info->input_thread_id, NULL);
    
    info->input_thread_running = 0;
  }

  if (info->dev) libevdev_free(info->dev);
  info->dev = NULL;
  if (info->fd >= 0) close(info->fd);
  info->fd = -1;

  return TCL_OK;
}
 
static int touch_start_command(ClientData data,
			       Tcl_Interp *interp,
			       int objc, Tcl_Obj *objv[])
{
  touch_info_t *info = (touch_info_t *) data;

  /* if thread is already running, just return */
  if (info->input_thread_running) return TCL_OK;

  if (pthread_create(&info->input_thread_id, NULL, input_thread,
		     (void *) info)) {
    return TCL_ERROR;
  }

  info->input_thread_running = 1;
  
  return TCL_OK;
}

static int touch_stop_command(ClientData data,
			      Tcl_Interp *interp,
			      int objc, Tcl_Obj *objv[])
{
  touch_info_t *info = (touch_info_t *) data;

  /* if thread is not running, just return */
  if (!info->input_thread_running) return TCL_OK;

  /* cancel input thread */
  pthread_cancel(info->input_thread_id);
  
  /* wait for thread to shutdown */
  pthread_join(info->input_thread_id, NULL);
  
  info->input_thread_running = 0;

  return TCL_OK;
}
#else

static int touch_open_command(ClientData data,
			       Tcl_Interp *interp,
			       int objc, Tcl_Obj *objv[])
{
  int width, height;

  if (objc < 4) {
    Tcl_WrongNumArgs(interp, 1, objv, "path width height");
    return TCL_ERROR;
  }
  if (Tcl_GetIntFromObj(interp, objv[2], &width) != TCL_OK) {
    return TCL_ERROR;
  }
  if (Tcl_GetIntFromObj(interp, objv[3], &height) != TCL_OK) {
    return TCL_ERROR;
  }  

  return TCL_OK;
}

static int touch_close_command(ClientData data,
			       Tcl_Interp *interp,
			       int objc, Tcl_Obj *objv[])
{
  return TCL_OK;
}
 
static int touch_start_command(ClientData data,
			       Tcl_Interp *interp,
			       int objc, Tcl_Obj *objv[])
{
  return TCL_OK;
}

static int touch_stop_command(ClientData data,
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
EXPORT(int,Dserv_touch_Init) (Tcl_Interp *interp)
#else
int Dserv_touch_Init(Tcl_Interp *interp)
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

  g_touchInfo.tclserver = tclserver_get();
  g_touchInfo.dpoint_prefix = "mtouch";
  g_touchInfo.fd = -1;
  g_touchInfo.dev = NULL;
  g_touchInfo.input_thread_running = 0;
  
  Tcl_CreateObjCommand(interp, "touchOpen",
		       (Tcl_ObjCmdProc *) touch_open_command,
		       &g_touchInfo, NULL);
  Tcl_CreateObjCommand(interp, "touchClose",
		       (Tcl_ObjCmdProc *) touch_close_command,
		       &g_touchInfo, NULL);
  Tcl_CreateObjCommand(interp, "touchStart",
		       (Tcl_ObjCmdProc *) touch_start_command,
		       &g_touchInfo, NULL);
  Tcl_CreateObjCommand(interp, "touchStop",
		       (Tcl_ObjCmdProc *) touch_stop_command,
		       &g_touchInfo, NULL);

  return TCL_OK;
}
