#ifndef TCLSERVER_API_H
#define TCLSERVER_API_H

typedef void tclserver_t;

#ifdef __cplusplus
extern "C" {
#endif

  uint64_t tclserver_now(tclserver_t *tclserver);
  void tclserver_set_point(tclserver_t *tclserver, ds_datapoint_t *dp);
  void *tclserver_get(void);

#ifdef __cplusplus
}
#endif
  
#endif
