#ifndef DSERV_H
#define DSERV_H

#ifdef __cplusplus
extern "C" {
#endif
  Dataserver *get_ds(void);
  TclServer *get_tclserver(void);
  int service_mdns(const char* hostname, const char* service_name, int service_port);
  extern ObjectRegistry<TclServer> TclServerRegistry;
#ifdef __cplusplus
}
#endif

#endif
