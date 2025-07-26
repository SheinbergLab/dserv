#ifndef MDNS_QUERY_RESPONSE_H
#define MDNS_QUERY_RESPONSE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char host_address[64];
    int host_port;
    int has_txt;
    char txt_dict[512];
} query_response_t;

#ifdef __cplusplus
}
#endif

#endif // MDNS_QUERY_RESPONSE_H
