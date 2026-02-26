/*
 * NAME
 *   touch.c - Updated with unified events and per-interpreter allocation
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
  int rotation;  /* 0, 90, 180, 270 */  
  int maxx, maxy, minx, miny;
  float rangex, rangey;
  int track_drag;  /* flag to enable/disable drag tracking */
} touch_info_t;

#ifdef __linux__

void *input_thread(void *arg)
{
  touch_info_t *info = (touch_info_t *) arg;
  char point_name[64];
  sprintf(point_name, "%s/event", info->dpoint_prefix);

  struct input_event ev;
  int raw_x = 0, raw_y = 0;
  int x = 0, y = 0;
  int touch_active = 0;
  int touch_changed = 0;       /* BTN_TOUCH changed this frame */
  int coords_changed = 0;      /* coordinates updated this frame */
  int first_coordinate_after_press = 0;
  int rc;

  do {
    rc = libevdev_next_event(info->dev, LIBEVDEV_READ_FLAG_BLOCKING, &ev);
    if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
      switch (ev.type) {
      case EV_KEY:
	switch (ev.code) {
	case BTN_TOUCH:
	  if (ev.value == 1) {
	    touch_active = 1;
	    touch_changed = 1;
	    first_coordinate_after_press = 0;
	  }
	  else if (ev.value == 0) {
	    touch_active = 0;
	    touch_changed = 1;
	  }
	  break;
	}
	break;
      case EV_ABS:
	switch (ev.code) {
	case ABS_X:
	  if (ev.value > 0) {
	    raw_x = (int)(info->screen_width * ((ev.value - info->minx) / info->rangex));
	    coords_changed = 1;
	  }
	  break;
	case ABS_Y:
	  if (ev.value > 0) {
	    raw_y = (int)(info->screen_height * ((ev.value - info->miny) / info->rangey));
	    coords_changed = 1;
	  }
	  break;
	}
	break;
      case EV_SYN:
	if (ev.code == SYN_REPORT) {
	  /* Apply rotation transform when coordinates changed */
	  if (coords_changed) {
	    switch (info->rotation) {
	    case 90:
	      x = raw_y;
	      y = info->screen_width - 1 - raw_x;
	      break;
	    case 180:
	      x = info->screen_width - 1 - raw_x;
	      y = info->screen_height - 1 - raw_y;
	      break;
	    case 270:
	      x = info->screen_height - 1 - raw_y;
	      y = raw_x;
	      break;
	    default:
	      x = raw_x;
	      y = raw_y;
	      break;
	    }
	  }

	  if (touch_active && (coords_changed || touch_changed)) {
	    uint16_t vals[3];
	    vals[0] = x;
	    vals[1] = y;

	    if (!first_coordinate_after_press) {
	      vals[2] = 0;  /* PRESS */
	      first_coordinate_after_press = 1;
	    }
	    else if (info->track_drag && coords_changed) {
	      vals[2] = 1;  /* DRAG */
	    }
	    else {
	      goto sync_done;
	    }

	    ds_datapoint_t *dp = dpoint_new(point_name,
					    tclserver_now(info->tclserver),
					    DSERV_SHORT,
					    sizeof(vals),
					    (unsigned char *) vals);
	    tclserver_set_point(info->tclserver, dp);
	  }
	  else if (!touch_active && touch_changed) {
	    /* Release */
	    uint16_t vals[3];
	    vals[0] = x;
	    vals[1] = y;
	    vals[2] = 2;  /* RELEASE */
	    ds_datapoint_t *dp = dpoint_new(point_name,
					    tclserver_now(info->tclserver),
					    DSERV_SHORT,
					    sizeof(vals),
					    (unsigned char *) vals);
	    tclserver_set_point(info->tclserver, dp);
	  }

	sync_done:
	  touch_changed = 0;
	  coords_changed = 0;
	}
	break;
      }
    }
  } while (rc >= 0);

  return NULL;
}

static int get_hdmi_rotation(const char *output_name) {
    FILE *f = fopen("/proc/cmdline", "r");
    if (!f) return 0;
    
    char buf[4096];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    fclose(f);
    
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "video=%s:", output_name);
    
    char *p = strstr(buf, prefix);
    if (!p) return 0;
    
    char *rot = strstr(p, "rotate=");
    if (!rot) return 0;
    
    char *space = strchr(p, ' ');
    if (space && rot > space) return 0;
    
    return atoi(rot + 7);
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
  const char *hdmi_output = "HDMI-A-1";  /* default */
    
  if (objc < 4) {
    Tcl_WrongNumArgs(interp, 1, objv,
		     "path width height [track_drag] [hdmi_output]");
    return TCL_ERROR;
  }
  if (Tcl_GetIntFromObj(interp, objv[2], &width) != TCL_OK) {
    return TCL_ERROR;
  }
  if (Tcl_GetIntFromObj(interp, objv[3], &height) != TCL_OK) {
    return TCL_ERROR;
  }  
  
  /* Optional parameter for drag tracking */
  if (objc >= 5) {
    int track_drag;
    if (Tcl_GetIntFromObj(interp, objv[4], &track_drag) != TCL_OK) {
      return TCL_ERROR;
    }
    info->track_drag = track_drag;
  } else {
    info->track_drag = 0;
  }

  /* Optional HDMI output name for rotation detection */
  if (objc >= 6) {
    hdmi_output = Tcl_GetString(objv[5]);
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
  
  info->minx = libevdev_get_abs_minimum(dev, ABS_X);
  info->maxx = libevdev_get_abs_maximum(dev, ABS_X);
  info->miny = libevdev_get_abs_minimum(dev, ABS_Y);
  info->maxy = libevdev_get_abs_maximum(dev, ABS_Y);
  info->rangex = info->maxx - info->minx;
  info->rangey = info->maxy - info->miny;
  info->dev = dev;
  info->fd = fd;
  info->screen_width = width;
  info->screen_height = height;
  info->rotation = get_hdmi_rotation(hdmi_output);
    
  return TCL_OK;
}

static int touch_set_drag_tracking_command(ClientData data,
					   Tcl_Interp *interp,
					   int objc, Tcl_Obj *objv[])
{
  touch_info_t *info = (touch_info_t *) data;
  int track_drag;

  if (objc != 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "enable");
    return TCL_ERROR;
  }
  
  if (Tcl_GetIntFromObj(interp, objv[1], &track_drag) != TCL_OK) {
    return TCL_ERROR;
  }
  
  info->track_drag = track_drag;
  
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

/* NEW: Cleanup function called when interpreter is deleted */
static void touch_cleanup(ClientData data, Tcl_Interp *interp)
{
  touch_info_t *info = (touch_info_t *) data;
  
  /* Stop any running thread - use the proper stop command for cleaner shutdown */
  if (info->input_thread_running) {
    /* Try the clean approach first by calling our own stop function */
    touch_stop_command(info, interp, 0, NULL);
    
    /* If thread is still running (shouldn't happen), force cancel as fallback */
    if (info->input_thread_running) {
      pthread_cancel(info->input_thread_id);
      pthread_join(info->input_thread_id, NULL);
      info->input_thread_running = 0;
    }
  }
  
  /* Clean up libevdev resources */
  if (info->dev) libevdev_free(info->dev);
  if (info->fd >= 0) close(info->fd);
  
  /* Free the info structure */
  free(info);
}

#else

static int touch_open_command(ClientData data,
			       Tcl_Interp *interp,
			       int objc, Tcl_Obj *objv[])
{
  int width, height;

  if (objc < 4) {
    Tcl_WrongNumArgs(interp, 1, objv, "path width height [track_drag]");
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

static int touch_set_drag_tracking_command(ClientData data,
					   Tcl_Interp *interp,
					   int objc, Tcl_Obj *objv[])
{
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

static void touch_cleanup(ClientData data, Tcl_Interp *interp)
{
  touch_info_t *info = (touch_info_t *) data;
  free(info);
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
  touch_info_t *touchInfo;
  
  if (
#ifdef USE_TCL_STUBS
      Tcl_InitStubs(interp, "8.6-", 0)
#else
      Tcl_PkgRequire(interp, "Tcl", "8.6-", 0)
#endif
      == NULL) {
    return TCL_ERROR;
  }

  /* Allocate per-interpreter touch info structure */
  touchInfo = (touch_info_t *) calloc(1, sizeof(touch_info_t));
  if (!touchInfo) {
    return TCL_ERROR;
  }
  
  /* Initialize the structure */
  touchInfo->tclserver = tclserver_get_from_interp(interp);
  touchInfo->dpoint_prefix = "mtouch";
  touchInfo->fd = -1;
#ifdef __linux__
  touchInfo->dev = NULL;
#endif  
  touchInfo->input_thread_running = 0;
  touchInfo->track_drag = 0;
  
  /* Create commands with the per-interpreter data */
  Tcl_CreateObjCommand(interp, "touchOpen",
		       (Tcl_ObjCmdProc *) touch_open_command,
		       touchInfo, NULL);
  Tcl_CreateObjCommand(interp, "touchClose",
		       (Tcl_ObjCmdProc *) touch_close_command,
		       touchInfo, NULL);
  Tcl_CreateObjCommand(interp, "touchStart",
		       (Tcl_ObjCmdProc *) touch_start_command,
		       touchInfo, NULL);
  Tcl_CreateObjCommand(interp, "touchStop",
		       (Tcl_ObjCmdProc *) touch_stop_command,
		       touchInfo, NULL);
  Tcl_CreateObjCommand(interp, "touchSetDragTracking",
		       (Tcl_ObjCmdProc *) touch_set_drag_tracking_command,
		       touchInfo, NULL);

  /* Register cleanup callback for when interpreter is deleted */
  Tcl_CallWhenDeleted(interp, touch_cleanup, touchInfo);

  return TCL_OK;
}
