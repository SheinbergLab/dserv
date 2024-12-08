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



static Tcl_Obj *dpoint_to_tclobj(Tcl_Interp *interp,
				 ds_datapoint_t *dpoint)
{
  Tcl_Obj *obj = NULL;
  Tcl_Obj *elt;
  int i, n;
    
  if (!dpoint->data.len) return Tcl_NewObj();
    
  switch (dpoint->data.type) {
  case DSERV_BYTE:
    if (dpoint->data.len == sizeof(unsigned char)) {
      obj = Tcl_NewIntObj(*((int *) dpoint->data.buf));
    }
    else {
      obj = Tcl_NewByteArrayObj(dpoint->data.buf, dpoint->data.len);
    }
    break;
  case DSERV_STRING:
  case DSERV_JSON:
    obj = Tcl_NewStringObj((char *) dpoint->data.buf, dpoint->data.len);
    break;
  case DSERV_FLOAT:
    if (dpoint->data.len == sizeof(float)) {
      obj = Tcl_NewDoubleObj(*((float *) dpoint->data.buf));
    }
    else {
      float *p = (float *) dpoint->data.buf;
      n = dpoint->data.len/sizeof(float);
      elt = Tcl_NewDoubleObj(*p++);
      obj = Tcl_NewListObj(1, &elt);
      for (i = 1; i < n; i++) {
	elt = Tcl_NewDoubleObj(*p++);
	Tcl_ListObjAppendElement(interp, obj, elt);
      }
    }
    break;
  case DSERV_DOUBLE:
    if (dpoint->data.len == sizeof(double)) {
      obj = Tcl_NewDoubleObj(*((double *) dpoint->data.buf));
    }
    else {
      double *p = (double *) dpoint->data.buf;
      n = dpoint->data.len/sizeof(double);
      elt = Tcl_NewDoubleObj(*p++);
      obj = Tcl_NewListObj(1, &elt);
      for (i = 1; i < n; i++) {
	elt = Tcl_NewIntObj(*p++);
	Tcl_ListObjAppendElement(interp, obj, elt);
      }
    }
    break;
  case DSERV_SHORT:
    if (dpoint->data.len == sizeof(short)) {
      obj = Tcl_NewIntObj(*((short *) dpoint->data.buf));
    }
    else {
      short *p = (short *) dpoint->data.buf;
      n = dpoint->data.len/sizeof(short);
      elt = Tcl_NewIntObj(*p++);
      obj = Tcl_NewListObj(1, &elt);
      for (i = 1; i < n; i++) {
	elt = Tcl_NewIntObj(*p++);
	Tcl_ListObjAppendElement(interp, obj, elt);
      }
    }
    break;
  case DSERV_INT:
    if (dpoint->data.len == sizeof(int)) {
      obj = Tcl_NewIntObj(*((int *) dpoint->data.buf));
    }
    else {
      int *p = (int *) dpoint->data.buf;
      n = dpoint->data.len/sizeof(int);
      elt = Tcl_NewIntObj(*p++);
      obj = Tcl_NewListObj(1, &elt);
      for (i = 1; i < n; i++) {
	elt = Tcl_NewIntObj(*p++);
	Tcl_ListObjAppendElement(interp, obj, elt);
      }
    }
    break;
  case DSERV_DG:
    obj = Tcl_NewByteArrayObj(dpoint->data.buf, dpoint->data.len);
    break;
  case DSERV_SCRIPT:
  case DSERV_TRIGGER_SCRIPT:
    obj = Tcl_NewStringObj((char *) dpoint->data.buf, dpoint->data.len);
    break;
  case DSERV_EVT:
  case DSERV_NONE:
  case DSERV_UNKNOWN:
    obj = NULL;
    break;
  }
  return obj;
}

int dserv_get_command(ClientData cd, Tcl_Interp *interp,
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
