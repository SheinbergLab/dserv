#ifndef TCLSERVER_API_H
#define TCLSERVER_API_H

#include "Datapoint.h"

typedef void tclserver_t;

#ifdef __cplusplus
extern "C" {
#endif
  uint64_t tclserver_now(tclserver_t *tclserver);

  /* Fixed offset between CLOCK_MONOTONIC and the us-since-1970 scale every
     datapoint timestamp uses. Captured once (Dataserver::clock_epoch_offset_us)
     and never recomputed.

     This exists so a module holding a KERNEL timestamp can place it on dserv's
     timebase exactly, rather than restamping with tclserver_now() and thereby
     recording when the module noticed an event instead of when it happened.
     Valid because Dataserver::now() is steady_clock + this offset, and on Linux
     steady_clock IS CLOCK_MONOTONIC -- the same base the GPIO v2 uAPI stamps in
     by default. (It would NOT have been valid before the timebase moved off
     high_resolution_clock, which aliased to CLOCK_REALTIME here.)

         dserv_us = tclserver_clock_epoch_offset_us() + kernel_ns / 1000  */
  int64_t tclserver_clock_epoch_offset_us(void);

  void tclserver_set_point(tclserver_t *tclserver, ds_datapoint_t *dp);
  tclserver_t* tclserver_get_from_interp(Tcl_Interp *interp);
  
  // Add new function for queuing Tcl scripts from modules
  void tclserver_queue_script(tclserver_t *tclserver, const char *script, int no_reply);
  
#ifdef __cplusplus
}
#endif
  
#endif
