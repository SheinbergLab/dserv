/*
 * NAME
 *   eventlog.c
 *
 * DESCRIPTION
 *
 * AUTHOR
 *   DLS, 06/24
 */

#include <stdlib.h>
#include <string.h>
#include <tcl.h>

#include "Datapoint.h"
#include "eventlog.h"
#include "tclserver_api.h"

static  char names[256][64];	      /* Name of each event type            */
static  char timetype[256];	      /* Time format for each event         */
static  char puttype[256];	      /* Type of params put for each event  */

static  nametype_t nametypes[256];	    /* Store current event names    */

static const char *dpoint_name = "eventlog/events";

static  ds_datapoint_t dpoint;
static  unsigned char dpoint_buf[256];

static  void initialize_names()
  {
    /* Clear out the table */
    for (int j = 0; j < 256; j++) nametypes[j].name[0] = 0;
    
    /*
     * This is a "tricky" technique for incorporating tables into
     * the source code.  The same line is used to set the name,
     * the timetype, and the puttype by using the #define macro
     */
#undef name
#define group(a,b,c) 	
#define name(i, e, str, t, p) strncpy(nametypes[i].name, #str, 63);	\
    nametypes[i].types[0] = t;						\
    nametypes[i].types[1] = p;
#include "evt_name.h"
#undef group
#undef name	
  }

static ds_datapoint_t *to_dpoint(char type, char subtype,
				 uint64_t tstamp, char ndata,char *data)
{
  ds_datatype_t datatype;
  
  if (type == E_NAME) {
    int slot =  subtype;
    
    /* slot == 0 is not allowed -- MagicEvent */ 
    if (slot == 1) initialize_names();
    else if (slot > 1) {
      /* Set the event name */
      strncpy(nametypes[slot].name, data, 63);
      
      /* Set the "timetype" */
      nametypes[slot].types[0] = ((unsigned char *) (&tstamp))[0];
      
      /* Set the "puttype" (for data field) */
      nametypes[slot].types[1] = ((unsigned char *) (&tstamp))[1];
    }
  }
  
  PUT_TYPE puttype = nametypes[(int) type].types[1];
  
  // events have their own types, so w need to format these
  switch (puttype) {
  case PUT_unknown:
  case PUT_null:
    datatype = DSERV_BYTE;
    break;
  case PUT_string:
    datatype = DSERV_STRING;
    break;
  case PUT_short:
    datatype = DSERV_SHORT;
    break;
  case PUT_long:
    datatype = DSERV_INT;
    break;
  case PUT_float:
    datatype = DSERV_FLOAT;
    break;
  case PUT_double:
    datatype = DSERV_DOUBLE;
    break;
  default:
    datatype = DSERV_UNKNOWN;
    break;
  }
  
  dpoint.timestamp = tstamp;
  dpoint.data.e.type = type;
  dpoint.data.e.subtype =  subtype;
  dpoint.data.e.puttype = datatype;
  dpoint.data.len = ndata;
  memcpy(dpoint.data.buf, data, ndata);
  
  return dpoint_copy(&dpoint);
}

static int add_params(Tcl_Interp *interp,
		      int type, int ptype,
		      int objc, Tcl_Obj *objv[], char *buf)
{
  int len = 0, l;
  char *strarg;
  short sarg;
  int iarg;
  int larg;
  float farg;
  double darg;
  char *work = buf;
  int ndx = objc;
  
  switch (ptype)
    {
    case PUT_null:
      len = 0;
      break;
    case PUT_string:
      while (ndx--) {
	strarg = Tcl_GetString(objv[ndx]);
	l = strlen(strarg);
	len += l;
	if (len > 256) return -1;
	memcpy(work, strarg, l);
	work = buf+len;
      }
      break;
    case PUT_short:
      while (ndx--) {
	if (Tcl_GetIntFromObj(interp, objv[ndx], &iarg) != TCL_OK)
	  return -1;
	sarg = (short) iarg;
	if (( len += sizeof(short) ) > 256) return -1;
	memcpy(work, &sarg, sizeof(short));
	work += sizeof(short);
      }
      break;
    case PUT_long:
      while (ndx--) {
	if (Tcl_GetIntFromObj(interp, objv[ndx], &larg) != TCL_OK)
	  return -1;
	if (( len += sizeof(int) ) > 256) return -1;
	memcpy(work, &larg, sizeof(int));
	work += sizeof(int);
      }
      break;
    case PUT_float:
      while (ndx--) {
	if (Tcl_GetDoubleFromObj(interp, objv[ndx], &darg) != TCL_OK)
	  return -1;
	farg = (float) darg;
	if (( len += sizeof(float) ) > 256) return -1;
	memcpy(work, &farg, sizeof(float));
	work += sizeof(float);
      }
      break;
    case PUT_double:
      while (ndx--)
	{
	  if (Tcl_GetDoubleFromObj(interp, objv[ndx], &darg) != TCL_OK)
	    return -1;
	  if (( len += sizeof(double) ) > 256) return -1;
	  memcpy(work, &darg, sizeof(double));
	  work += sizeof(double);
	}			
      break;
    case PUT_unknown:
    default:
      return -1;
      break;
    }
  return len;
}

int evt_name_set_command(ClientData data, Tcl_Interp *interp,
			 int objc, Tcl_Obj *objv[])
{
  tclserver_t *tclserver = (tclserver_t *) data;
  int type;
  int rc;
  int ttype = 'c';
  int ptype;
  char *name;
  int namelen;
  
  if (objc < 3)
    {
      Tcl_WrongNumArgs(interp, 1, objv, "type name ptype");
      return TCL_ERROR;
    }
  
  if (Tcl_GetIntFromObj(interp, objv[1], &type) != TCL_OK)
    return TCL_ERROR;
  if (type < 0 || type > 255)
    {
      Tcl_AppendResult(interp, "evtNameSet: bad type", NULL);
      return TCL_ERROR;
    }
  
  name = Tcl_GetString(objv[2]);
  namelen = strlen(name);
  if (namelen > 255) {
    Tcl_AppendResult(interp, "evtNameSet: invalid name", NULL);
    return TCL_ERROR;
  }
  
  if (Tcl_GetIntFromObj(interp, objv[3], &ptype) != TCL_OK)
    return TCL_ERROR;
  
  if (ptype < 0 || ptype >= PUT_TYPES ) {
    Tcl_AppendResult(interp, "evtNameSet: bad ptype", NULL);
    return TCL_ERROR;
  }
  ds_datapoint_t *dp = to_dpoint(E_NAME, type, (ptype << 8) + ttype,
				 namelen, name);
  tclserver_set_point(tclserver, dp);
  return TCL_OK;
}

static int evt_put_command(ClientData data, Tcl_Interp * interp,
		    int objc, Tcl_Obj *objv[])
{
  tclserver_t *tclserver = (tclserver_t *) data;
  int type, subtype;
  Tcl_WideInt ts;
  int rc;
  
  int ptype = 0;
  char buf[256];
  int buflen = 0;
  
  if (objc < 4)
    {
      Tcl_WrongNumArgs(interp, 1, objv,
		       "type subtype timestamp ?ptype? ?params?");
      return TCL_ERROR;
    }
  
  if (Tcl_GetIntFromObj(interp, objv[1], &type) != TCL_OK)
    return TCL_ERROR;
  if (type < 0 || type > 255) {
    Tcl_AppendResult(interp, "evtPut: type out of range", NULL);
    return TCL_ERROR;
  }
  if (Tcl_GetIntFromObj(interp, objv[2], &subtype) != TCL_OK)
    return TCL_ERROR;
  if (subtype < 0 || subtype > 255) {
    Tcl_AppendResult(interp, "evtPut: subtype out of range", NULL);
    return TCL_ERROR;
  }
  if (Tcl_GetWideIntFromObj(interp, objv[3], &ts) != TCL_OK)
    return TCL_ERROR;
  
  if (objc > 5) {
    if (Tcl_GetIntFromObj(interp, objv[4], &ptype) != TCL_OK)
      return TCL_ERROR;
    if (ptype < 0 || ptype >= PUT_TYPES) {
      Tcl_AppendResult(interp, "evtPut: bad ptype", NULL);
      return TCL_ERROR;
    }
    buflen = add_params(interp, type, ptype, objc-5, &objv[5], buf);
    if (buflen < 0) {
      Tcl_AppendResult(interp, "evtPut: parameter error", NULL);
      return TCL_ERROR;
    }
  }

  ds_datapoint_t *dp = to_dpoint(type, subtype, ts,
				 buflen, buf);
  tclserver_set_point(tclserver, dp);
  return TCL_OK;
}


/*****************************************************************************
 * EXPORT
 *****************************************************************************/

#ifdef WIN32
EXPORT(int,Dserv_eventlog_Init) (Tcl_Interp *interp)
#else
int Dserv_eventlog_Init(Tcl_Interp *interp)
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

  // initialize names
  initialize_names();
  
  // initialize generic datapoint to use for events
  dpoint.varname = (char *) dpoint_name;
  dpoint.varlen = strlen(dpoint_name)+1;
  dpoint.data.e.dtype = DSERV_EVT;
  dpoint.data.buf = &dpoint_buf[0];
  
  Tcl_CreateObjCommand(interp, "evtPut",
		       (Tcl_ObjCmdProc *) evt_put_command, tclserver, NULL);
  Tcl_CreateObjCommand(interp, "evtNameSet",
		       (Tcl_ObjCmdProc *) evt_name_set_command, tclserver, NULL);
  return TCL_OK;
}
