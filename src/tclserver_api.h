#ifndef TCLSERVER_API_H
#define TCLSERVER_API_H

#include "Datapoint.h"

typedef void tclserver_t;

#ifdef __cplusplus
extern "C" {
#endif
  uint64_t tclserver_now(tclserver_t *tclserver);
  void tclserver_set_point(tclserver_t *tclserver, ds_datapoint_t *dp);
  tclserver_t* tclserver_get_from_interp(Tcl_Interp *interp);
  
  // Add new function for queuing Tcl scripts from modules
  void tclserver_queue_script(tclserver_t *tclserver, const char *script, int no_reply);
  
#ifdef __cplusplus
}
#endif
  
#endif
