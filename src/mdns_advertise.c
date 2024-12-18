/*
 * mDNS advertisement code (https://github.com/mjansson/mdns)
 */

#ifndef _MSC_VER
#include <pthread.h>
#endif

#include <stdio.h>
#include "mdns.h"

typedef struct thread_args_s {
  int ds_port;
  int ess_port;
} thread_args_t;

void *advertise_service_thread(void *data)
{
  thread_args_t *args = (thread_args_t *) data;
  
  char hostname_buffer[256];
  size_t hostname_size = sizeof(hostname_buffer);
  const char *hostname = &hostname_buffer[0];
  if (gethostname(hostname_buffer, hostname_size) == 0)
    hostname = hostname_buffer;

  service_mdns(hostname, "_dserv._tcp",
	       args->ds_port,
	       args->ess_port);

  return NULL;
}

int advertise_services(int ds_port, int ess_port, const char *id_str)
{
#ifndef _MSC_VER
  pthread_t mdns_thread_id;
#endif
  thread_args_t *args;

  args = (thread_args_t *) malloc(sizeof(thread_args_t));
  args->ds_port = ds_port;
  args->ess_port = ess_port;

#ifndef _MSC_VER
  if (pthread_create(&mdns_thread_id, NULL, advertise_service_thread,
		     (void *) args)) {
    return -1;
  }
#endif
  return 0;
}

