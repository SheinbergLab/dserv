/*
 * NAME
 *   usbio.c
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
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <termios.h>
#include <pthread.h>

#include <tcl.h>
#include "Datapoint.h"
#include "tclserver_api.h"

typedef struct usbio_info_s
{
  int usbio_fd;
  tclserver_t *tclserver;
} usbio_info_t;

/* global to this module */
static usbio_info_t g_usbioInfo;

static void process_request(usbio_info_t *info, char *buf, int nbytes)
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
  usbio_info_t *info = (usbio_info_t *) arg;

  char buf[16384];
  int port = info->usbio_fd;
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


static void iovSet(struct iovec *iov, char *buf, int n)
{
  iov->iov_base = buf;
  iov->iov_len = n;
}

static int usbio_send_command (ClientData data, Tcl_Interp *interp,
				   int objc, Tcl_Obj *objv[])
{
  usbio_info_t *info = (usbio_info_t *) data;
  static char newline_buf[2] = "\n";

  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "command");
    return TCL_ERROR;
  }
  if (info->usbio_fd < 0) return TCL_OK;

  char *cmd = Tcl_GetString(objv[1]);

  struct iovec iovs[2];
  int cmdsize = strlen(cmd);
  iovSet(&iovs[0], cmd, cmdsize);
  iovSet(&iovs[1], newline_buf, 1);
  int bytes_to_send = cmdsize + 1;
	  
  int rval = writev(info->usbio_fd, iovs, 2);
  if (rval != bytes_to_send) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]), ": send error", NULL);
    return TCL_ERROR;
  }
  else {
    Tcl_SetObjResult(interp, Tcl_NewIntObj(rval));
    return TCL_OK;
  }
}

static int usbio_open_command (ClientData data, Tcl_Interp *interp,
				   int objc, Tcl_Obj *objv[])
{
  usbio_info_t *info = (usbio_info_t *) data;
  if (info->usbio_fd >= 0) close(info->usbio_fd);

  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "port");
    return TCL_ERROR;
  }

  /* open read only but don't become "controlling terminal" */
  info->usbio_fd = open(Tcl_GetString(objv[1]), O_NOCTTY | O_RDWR);
  
  if (info->usbio_fd < 0) {
    Tcl_AppendResult(interp,
		     Tcl_GetString(objv[0]), ": error opening port \"",
		     Tcl_GetString(objv[1]), "\"", NULL);
    return TCL_ERROR;
  }
  int ret = configure_serial_port(info->usbio_fd);

  pthread_t id;
  pthread_create(&id, NULL, workerThread, &g_usbioInfo);
  
  Tcl_SetObjResult(interp, Tcl_NewIntObj(ret));
  
  return TCL_OK;
}

static int usbio_close_command (ClientData data, Tcl_Interp *interp,
				   int objc, Tcl_Obj *objv[])
{
  usbio_info_t *info = (usbio_info_t *) data;
  if (info->usbio_fd >= 0) close(info->usbio_fd);

  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "port");
    return TCL_ERROR;
  }

  /* open read only but don't become "controlling terminal" */
  if (info->usbio_fd < 0) return TCL_OK;
  else {
    close(info->usbio_fd);
    info->usbio_fd = -1;
  }

  return TCL_OK;
}

/*****************************************************************************
 *
 * EXPORT
 *
 *****************************************************************************/

#ifdef WIN32
EXPORT(int,Dserv_usbio_Init) (Tcl_Interp *interp)
#else
  int Dserv_usbio_Init(Tcl_Interp *interp)
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
  g_usbioInfo.usbio_fd = -1;
  g_usbioInfo.tclserver = tclserver_get();
  
  Tcl_CreateObjCommand(interp, "usbioOpen",
		       (Tcl_ObjCmdProc *) usbio_open_command,
		       (ClientData) &g_usbioInfo,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "usbioClose",
		       (Tcl_ObjCmdProc *) usbio_close_command,
		       (ClientData) &g_usbioInfo,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "usbioSend",
		       (Tcl_ObjCmdProc *) usbio_send_command,
		       (ClientData) &g_usbioInfo,
		       (Tcl_CmdDeleteProc *) NULL);
  return TCL_OK;
}



