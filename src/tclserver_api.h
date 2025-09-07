#ifndef TCLSERVER_API_H
#define TCLSERVER_API_H

typedef void tclserver_t;

#ifdef __cplusplus
extern "C" {
#endif
  uint64_t tclserver_now(tclserver_t *tclserver);
  void tclserver_set_point(tclserver_t *tclserver, ds_datapoint_t *dp);
  tclserver_t* tclserver_get_from_interp(Tcl_Interp *interp);
#ifdef __cplusplus
}
#endif
  
#endif
