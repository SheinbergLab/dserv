/*
 * NAME
 *   juicer.c
 *
 * DESCRIPTION
 *
 * AUTHOR
 *   DLS, 06/24
 */

#include <iostream>
#include <chrono>
#include <future>
#include <vector>
#include <unordered_map>

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include "windows.h" 
#undef WIN32_LEAN_AND_MEAN
#pragma warning (disable:4244)
#pragma warning (disable:4305)
#define DllEntryPoint DllMain
#define EXPORT(a,b) __declspec(dllexport) a b
#else
#define DllEntryPoint
#define EXPORT a b
#include <unistd.h>
#endif
#include <tcl.h>

#include "sharedqueue.h"
#include "Dataserver.h"
#include "Datapoint.h"
#include "TclServer.h"
#include "Timer.h"
#include "dserv.h"

// for GPIO control
#ifdef HAVE_GPIO
#include <gpiod.h>
#endif

extern "C" {
  int Dserv_juicer_Init(Tcl_Interp *interp);
}

class ModInfo
{
public:
  Dataserver *ds;
  TclServer *tclserver;

  // timers for controlling juice lines
  const int ntimers = 2;
  std::vector<Timer *> timers;
  std::vector<int> timer_pins;

public:
  int timer_callback(int timerid)
  {
#ifdef HAVE_GPIO
    auto iter = tclserver->gpio_output_lines.find(timer_pins[timerid]);
    if (iter != tclserver->gpio_output_lines.end()) {
      gpiod_line_set_value(iter->second, 0);
    }
#endif	
    return 0;
  }
  
  ModInfo(Dataserver *ds, TclServer *ts): ds(ds), tclserver(ts) {
    using namespace std::placeholders;

    for (auto i = 0; i < ntimers; i++) {
      Timer *timer = new Timer(i);
      timers.push_back(timer);
      timer_pins.push_back(-1);
      timer->add_callback(std::bind(&ModInfo::timer_callback,
				    this, _1));
    }
  }
  ~ModInfo() {
    
    for (auto t: timers) {
      delete t;
    }
  }
};

static int juicer_juice_command (ClientData data, Tcl_Interp *interp,
				 int objc, Tcl_Obj *objv[])
{
  ModInfo *minfo = (ModInfo *) data;
  Dataserver *ds = minfo->ds;
  TclServer *tclserver = minfo->tclserver;
  
  int id, ms;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "?juicerid? start");
    return TCL_ERROR;
  }
  
  if (objc < 3) {		/* default to timer 0 */
    id = 0;
    if (Tcl_GetIntFromObj(interp, objv[1], &ms) != TCL_OK)
      return TCL_ERROR;
  }
  
  else {
    if (Tcl_GetIntFromObj(interp, objv[1], &id) != TCL_OK)
      return TCL_ERROR;
    if (Tcl_GetIntFromObj(interp, objv[2], &ms) != TCL_OK)
      return TCL_ERROR;
  }
  
  // if pin set for juicer juice and set alarm to turn off
  if (minfo->timer_pins[id] >= 0) {
#ifdef HAVE_GPIO
    auto iter =
      tclserver->gpio_output_lines.find(minfo->timer_pins[id]);
    if (iter != tclserver->gpio_output_lines.end()) {
      gpiod_line_set_value(iter->second, 1);
    }
#endif
    minfo->timers[id]->arm_ms(ms);
    minfo->timers[id]->fire();
  }  
  Tcl_SetObjResult(interp, Tcl_NewIntObj(id));
  return TCL_OK;
}

static int juicer_set_pin_command (ClientData data, Tcl_Interp *interp,
				   int objc, Tcl_Obj *objv[])
{
  ModInfo *minfo = (ModInfo *) data;
  Dataserver *ds = minfo->ds;
  TclServer *tclserver = minfo->tclserver;

  int id, pin;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "?juicerid? pin");
    return TCL_ERROR;
  }
  
  if (objc < 3) {		/* default to timer 0 */
    id = 0;
    if (Tcl_GetIntFromObj(interp, objv[1], &pin) != TCL_OK)
      return TCL_ERROR;
  }
  
  else {
    if (Tcl_GetIntFromObj(interp, objv[1], &id) != TCL_OK)
      return TCL_ERROR;
    
    if (id >= minfo->ntimers) {
      const char *msg = "invalid timer";
      Tcl_SetResult(interp, (char *) msg, TCL_STATIC);
      return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[2], &pin) != TCL_OK)
      return TCL_ERROR;
  }
  
  minfo->timer_pins[id] = pin;
  Tcl_SetObjResult(interp, Tcl_NewIntObj(id));
  return TCL_OK;
}

/*****************************************************************************
 *
 * EXPORT
 *
 *****************************************************************************/

#ifdef WIN32
EXPORT(int,Dserv_juicer_Init) (Tcl_Interp *interp)
#else
  int Dserv_juicer_Init(Tcl_Interp *interp)
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

  Dataserver *ds = get_ds();
  TclServer *tclserver = get_tclserver();
  ModInfo *info = new ModInfo(ds, tclserver);
  
  Tcl_CreateObjCommand(interp, "juicerJuice",
		       (Tcl_ObjCmdProc *) juicer_juice_command,
		       (ClientData) info,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "juicerSetPin",
		       (Tcl_ObjCmdProc *) juicer_set_pin_command,
		       (ClientData) info,
		       (Tcl_CmdDeleteProc *) NULL);
  return TCL_OK;
}
