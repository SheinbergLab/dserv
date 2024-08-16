/*
 * NAME
 *   sound.c
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
#include <termios.h>
#include <pthread.h>

#include <tcl.h>
#include "Datapoint.h"
#include "tclserver_api.h"

typedef struct usb_input_info_s
{
  int usb_input_fd;
  tclserver_t *tclserver;
} usb_input_info_t;

/* global to this module */
static usb_input_info_t g_usbInputInfo;

static void process_request(usb_input_info_t *info, char *buf, int nbytes)
{
  if (nbytes > 9 &&
      buf[1] == 's' && buf[2] == 'e' &&
      buf[3] == 't' && buf[4] == 'd' &&
      buf[5] == 'a' && buf[6] == 't' &&
      buf[7] == 'a' && buf[8] == ' ') {
    char *p2 = &buf[9];

    ds_datapoint_t *dpoint = dpoint_from_string(p2, nbytes-9);
	  
    if (dpoint) {
      if (!dpoint->timestamp) dpoint->timestamp = tclserver_now(info->tclserver);

      tclserver_set_point(info->tclserver, dpoint);
    }
  }
}

void* workerThread(void *arg) {
  usb_input_info_t *info = (usb_input_info_t *) arg;

  char buf[16384];
  int port = info->usb_input_fd;
  long n;
  char output_buf[1024];
  int bufsize = sizeof(output_buf);
  int nchar, write_index = 0;
  char *newline;

  while (1) {
    n = read(port, buf, sizeof(buf));
    if (n < 1) {
      break;
    }
    else {
      if ((newline = strchr(buf, '\n'))) {
	nchar =  newline-buf;
	if ((nchar+write_index) < bufsize) {
	  memcpy(&output_buf[write_index], buf, nchar);
	  output_buf[write_index+nchar] = '\0';

	  process_request(info, buf, write_index+nchar);

	  write_index = 0;
	  
	  /* now put rest of line in output buffer */
	  if ((nchar = n-(nchar+1)) > 0) {
	    memcpy(&output_buf[write_index], buf, nchar);
	    write_index = nchar;
	  }
	}
	else {
	  /* overflow */
	  write_index = 0;
	}
      }
      else {
	if (write_index+n < bufsize)
	  memcpy(&output_buf[write_index], buf, n);
	else
	  write_index = 0;
      }
    }
  }

  close(port);
  return 0;
}

static int configure_serial_port(int fd)
{  
  struct termios ser;
  tcflush(fd,TCIFLUSH);
  tcflush(fd,TCOFLUSH);
  int res = tcgetattr(fd, &ser);
  if (res < 0) {
    return -1;
  }
  cfmakeraw(&ser);
  if ((res = tcsetattr(fd, TCSANOW, &ser)) < 0){
    return -2;
  }
  return 0;
}

static int usb_input_open_command (ClientData data, Tcl_Interp *interp,
				   int objc, Tcl_Obj *objv[])
{
  usb_input_info_t *info = (usb_input_info_t *) data;
  if (info->usb_input_fd >= 0) close(info->usb_input_fd);

  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "port");
    return TCL_ERROR;
  }

  info->usb_input_fd = open(Tcl_GetString(objv[1]), O_RDONLY);
  
  if (info->usb_input_fd < 0) {
    Tcl_AppendResult(interp,
		     Tcl_GetString(objv[0]), ": error opening port \"",
		     Tcl_GetString(objv[1]), "\"", NULL);
    return TCL_ERROR;
  }
  int ret = configure_serial_port(info->usb_input_fd);

  pthread_t id;
  pthread_create(&id, NULL, workerThread, &g_usbInputInfo);
  
  Tcl_SetObjResult(interp, Tcl_NewIntObj(ret));
  
  return TCL_OK;
}

/*****************************************************************************
 *
 * EXPORT
 *
 *****************************************************************************/

#ifdef WIN32
EXPORT(int,Dserv_usb_input_Init) (Tcl_Interp *interp)
#else
  int Dserv_usb_input_Init(Tcl_Interp *interp)
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
  g_usbInputInfo.usb_input_fd = -1;
  g_usbInputInfo.tclserver = tclserver_get();
  
  Tcl_CreateObjCommand(interp, "usbInputOpen",
		       (Tcl_ObjCmdProc *) usb_input_open_command,
		       (ClientData) &g_usbInputInfo,
		       (Tcl_CmdDeleteProc *) NULL);
  return TCL_OK;
}



