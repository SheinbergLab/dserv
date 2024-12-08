/*
 * NAME
 *   tcl_dserv.c
 *
 * DESCRIPTION
 *
 * AUTHOR
 *   DLS, 12/24
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <tcl.h>
#include <Datapoint.h>

Tcl_Obj *dpoint_to_tclobj(Tcl_Interp *interp,
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

