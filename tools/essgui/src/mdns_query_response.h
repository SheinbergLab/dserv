#ifndef MDNS_QUERY_RESPONSE_H
#define MDNS_QUERY_RESPONSE_H

typedef struct query_response_s
{
  char host_address[128];
  int  host_port;
  int  has_txt;
  char txt_dict[256];
} query_response_t;
#endif
  
