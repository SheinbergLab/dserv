#ifndef DSERV_H
#define DSERV_H

#ifdef __cplusplus
extern "C" {
#endif
  const int msgport = 2560;
  
  Dataserver *get_ds(void);
  int service_mdns(const char* hostname, const char* service_name, int service_port);
  extern ObjectRegistry<TclServer> TclServerRegistry;
#ifdef __cplusplus
}
#endif

#endif
