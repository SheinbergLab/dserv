/*
 * NAME
 *   tcl_dserv.c
 *
 * DESCRIPTION
 *
 * AUTHOR
 *   DLS, 12/24
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

#include <tcl.h>

#include "dservapi.h"
#include "dpoint_tclobj.h"	/* get data from dpoint into Tcl_Obj */

static int dserv_open_command(ClientData data, Tcl_Interp *interp,
			      int objc, Tcl_Obj *objv[])
{
  const char *host;
  int port = 4620;
  int fd;
  int rc;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "host [port]");
    return TCL_ERROR;
  }

  host = Tcl_GetString(objv[1]);
  
  if (objc > 2) {
    if (Tcl_GetIntFromObj(interp, objv[2], &port) != TCL_OK)
      return TCL_ERROR;
  }
  
  
  rc = dservapi_open_socket(host, port);
  
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return TCL_OK;
}

static int dserv_close_command(ClientData data, Tcl_Interp *interp,
			       int objc, Tcl_Obj *objv[])
{
  int fd;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "socketfd");
    return TCL_ERROR;
  }
  
  if (Tcl_GetIntFromObj(interp, objv[1], &fd) != TCL_OK)
    return TCL_ERROR;
  
  dservapi_close_socket(fd);
  return TCL_OK;
}


static int dserv_get_command(ClientData cd, Tcl_Interp *interp,
		      int objc, Tcl_Obj *objv[])
{
  int fd;
  const char *varname;
  Tcl_Obj *obj;
  int bufsize;
  char *buf;
  ds_datapoint_t *dpoint;
  
  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "socketfd varname");
    return TCL_ERROR;
  }
  
  if (Tcl_GetIntFromObj(interp, objv[1], &fd) != TCL_OK)
    return TCL_ERROR;
  varname = Tcl_GetString(objv[2]);

  bufsize = dservapi_get_from_dataserver(fd, varname, &buf);

  /* if bufize == -1 we encountered an error */
  if (bufsize == -1) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]), ": error getting dpoint",
		     NULL);
    return TCL_ERROR;
  }

  /* if bufize == 0 it means we got no point back, which is fine */
  if (!bufsize) {
    return TCL_OK;
  }

  /* convert payload to a Tcl_Obj */
  dpoint = dpoint_from_binary(buf, bufsize);
  if (dpoint) {
    obj = dpoint_to_tclobj(interp, dpoint);
    if (obj)
      Tcl_SetObjResult(interp, obj);
  }
  
  /* free new dpoint and original buffer from dserv */
  dpoint_free(dpoint);
  free(buf);
  
  return TCL_OK;
}

int dserv_send_command(ClientData cd, Tcl_Interp *interp,
					int objc, Tcl_Obj *objv[])
{
  int fd;
  int dtype;
  const char *varname;
  Tcl_Size len;
  void *data;
  
  if (objc < 5) {
    Tcl_WrongNumArgs(interp, 1, objv, "socketfd varname dtype data");
    return TCL_ERROR;
  }
  
  if (Tcl_GetIntFromObj(interp, objv[1], &fd) != TCL_OK)
    return TCL_ERROR;
  varname = Tcl_GetString(objv[2]);
  if (Tcl_GetIntFromObj(interp, objv[3], &dtype) != TCL_OK)
    return TCL_ERROR;

  /* string types */
  if (dtype == 1 || dtype == 7 || dtype == 11) {
    data = (void *) Tcl_GetString(objv[4]);
    len = strlen(data);
  }
  /* binary types */
  else {
    if (Tcl_GetByteArrayFromObj(objv[2], &len) != TCL_OK)
      return TCL_ERROR;
  }

  if (dservapi_send_to_dataserver(fd, varname, dtype, len, data) < 0) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]), ": error sending dpoint",
		     NULL);
  }
  
  return TCL_OK;
}

int dserv_write_command(ClientData cd, Tcl_Interp *interp,
			int objc, Tcl_Obj *objv[])
{
  int fd;
  int dtype;
  const char *varname;
  Tcl_Size len;
  void *data;
  
  if (objc < 5) {
    Tcl_WrongNumArgs(interp, 1, objv, "socketfd varname dtype data");
    return TCL_ERROR;
  }
  
  if (Tcl_GetIntFromObj(interp, objv[1], &fd) != TCL_OK)
    return TCL_ERROR;
  varname = Tcl_GetString(objv[2]);
  if (Tcl_GetIntFromObj(interp, objv[3], &dtype) != TCL_OK)
    return TCL_ERROR;

  /* string types */
  if (dtype == 1 || dtype == 7 || dtype == 11) {
    data = (void *) Tcl_GetString(objv[4]);
    len = strlen(data);
  }
  /* binary types */
  else {
    if (Tcl_GetByteArrayFromObj(objv[2], &len) != TCL_OK)
      return TCL_ERROR;
  }

  int payload = len+strlen(varname)+4;
  if (payload > 128) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		     ": dpoint too large for fixed length binary send",
		     NULL);
    return TCL_ERROR;
  }
  if (dservapi_write_to_dataserver(fd, varname, dtype, len, data) < 0) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]), ": error writing dpoint",
		     NULL);
  }
  
  return TCL_OK;
}

/*****************************************************************************
 * EXPORT
 *****************************************************************************/

#ifdef WIN32
EXPORT(int,Dserv_Init) (Tcl_Interp *interp)
#else
int Dserv_Init(Tcl_Interp *interp)
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

  Tcl_PkgProvide(interp, "dserv", "1.0");
		 
  Tcl_CreateObjCommand(interp, "dserv::open",
		       (Tcl_ObjCmdProc *) dserv_open_command,
		       (ClientData) NULL, NULL);
  Tcl_CreateObjCommand(interp, "dserv::close",
		       (Tcl_ObjCmdProc *) dserv_close_command,
		       (ClientData) NULL, NULL);
  Tcl_CreateObjCommand(interp, "dserv::send",
		       (Tcl_ObjCmdProc *) dserv_send_command,
		       (ClientData) NULL, NULL);
  Tcl_CreateObjCommand(interp, "dserv::write",
		       (Tcl_ObjCmdProc *) dserv_write_command,
		       (ClientData) NULL, NULL);
  Tcl_CreateObjCommand(interp, "dserv::get",
		       (Tcl_ObjCmdProc *) dserv_get_command,
		       (ClientData) NULL, NULL);

  return TCL_OK;
}
