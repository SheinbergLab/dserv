/*
 * NAME
 *   ain.cpp
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
#include <cmath>
#include <thread>
#include <unistd.h>
#include <tcl.h>
#include <sys/timerfd.h>

#include "sharedqueue.h"
#include "Dataserver.h"
#include "Datapoint.h"
#include "TclServer.h"
#include "dserv.h"

// for GPIO control
#include "Mcp3204.h"

extern "C" {
  int Dserv_ain_Init(Tcl_Interp *interp);
}


class PeriodicTimer
{
private:
  int fd;
  struct itimerspec new_value;
  struct timespec now;
  
public:
  const char *ADC_DPOINT_NAME = "ain/vals";
  Mcp3204 mcp3204;
  Dataserver *ds;
  TclServer *tclserver;
  int nchan = 2;

  void start_timer_thread(void)
  {
    uint64_t exp;
    ssize_t s;

    ds_datapoint_t adc_dpoint;
    uint16_t vals[4];

    while (1) {
      s = read(fd, &exp, sizeof(uint64_t));
      if (s == sizeof(uint64_t)) {
	mcp3204.read(nchan, vals);

	/* fill the data point */
	ds_datapoint_t *dp = dpoint_new((char *) ADC_DPOINT_NAME,
					ds->now(), DSERV_SHORT,
					sizeof(uint16_t)*nchan,
					(unsigned char *) vals);
	tclserver->set_point(dp);
      }
    }
  }

  PeriodicTimer(Dataserver *ds, TclServer *ts): ds(ds), tclserver(ts)
  {
    fd = timerfd_create(CLOCK_REALTIME, 0);
    if (fd == -1) return;
    
    std::thread thr(&PeriodicTimer::start_timer_thread, this);
    thr.detach();
  }
  
  void start(int start_ms, int interval_ms)
  {
    struct itimerspec new_value;
    struct timespec now;

    if (clock_gettime(CLOCK_REALTIME, &now) == -1)
      return;

    /* Create a CLOCK_REALTIME absolute timer with initial
       expiration and interval as specified in command line */
    
    new_value.it_value.tv_sec = now.tv_sec;
    new_value.it_value.tv_nsec = now.tv_nsec + start_ms*1000000;
    new_value.it_interval.tv_sec = 0;
    new_value.it_interval.tv_nsec = interval_ms*1000000;

    timerfd_settime(fd, TFD_TIMER_ABSTIME, &new_value, NULL);
  }

  void stop(void)
  {
    struct itimerspec new_value;
    
    new_value.it_value.tv_sec = 0;
    new_value.it_value.tv_nsec = 0;
    new_value.it_interval.tv_sec = 0;
    new_value.it_interval.tv_nsec = 0;
    timerfd_settime(fd, TFD_TIMER_ABSTIME, &new_value, NULL);
  }
};

static int ain_start_command (ClientData data, Tcl_Interp *interp,
			      int objc, Tcl_Obj *objv[])
{
  PeriodicTimer *timer = (PeriodicTimer *) data;
  Dataserver *ds = timer->ds;
  TclServer *tclserver = timer->tclserver;
  int ms = 10;
  
  if (objc > 1) {
    if (Tcl_GetIntFromObj(interp, objv[1], &ms) != TCL_OK)
      return TCL_ERROR;
  }
  
  timer->start(ms, ms);

  return TCL_OK;
}

static int ain_stop_command (ClientData data, Tcl_Interp *interp,
			       int objc, Tcl_Obj *objv[])
{
  PeriodicTimer *timer = (PeriodicTimer *) data;
  Dataserver *ds = timer->ds;
  TclServer *tclserver = timer->tclserver;
  
  timer->stop();

  return TCL_OK;
}

/*****************************************************************************
 * EXPORT
 *****************************************************************************/

#ifdef WIN32
EXPORT(int,Dserv_ain_Init) (Tcl_Interp *interp)
#else
  int Dserv_ain_Init(Tcl_Interp *interp)
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
  PeriodicTimer *timer = new PeriodicTimer(ds, tclserver);
  
  Tcl_CreateObjCommand(interp, "ainStart",
		       (Tcl_ObjCmdProc *) ain_start_command,
		       (ClientData) timer,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "ainStop",
		       (Tcl_ObjCmdProc *) ain_stop_command,
		       (ClientData) timer,
		       (Tcl_CmdDeleteProc *) NULL);

  return TCL_OK;
}
