#ifndef DSERV_H
#define DSERV_H

#ifdef __cplusplus
extern "C" {
#endif
  Dataserver *get_ds(void);
  TclServer *get_tclserver(void);
#ifdef __cplusplus
}
#endif

#endif
