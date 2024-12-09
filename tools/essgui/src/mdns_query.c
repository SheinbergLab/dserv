#include <stdio.h>
#include <errno.h>

#include "mdns.h"
#include "mdns_query_response.h"

#define MAX_RESPONSES 32

// Callback handling parsing answers to queries sent
int
query_callback(int sock, const struct sockaddr* from, size_t addrlen, mdns_entry_type_t entry,
               uint16_t query_id, uint16_t rtype, uint16_t rclass, uint32_t ttl, const void* data,
               size_t size, size_t name_offset, size_t name_length, size_t record_offset,
               size_t record_length, void* user_data);

int open_client_sockets(int* sockets, int max_sockets, int port);

// Send a mDNS query
int
send_mdns_query_service(const char *service_name,
			char *result_buf, int result_len,
			int timeout_ms)
{
  query_response_t query_responses[MAX_RESPONSES];
  
  mdns_query_t* query;
  mdns_query_t query_buf;
  query = &query_buf;

  query->name = service_name;
  query->type = MDNS_RECORDTYPE_PTR;
  query->length = strlen(query->name);
  size_t count = 1;

  result_buf[0] = '\0';
  
  int sockets[32];
  int query_id[32];
  int num_sockets =
    open_client_sockets(sockets, sizeof(sockets) / sizeof(sockets[0]), 0);
  if (num_sockets <= 0) {
    printf("Failed to open any client sockets\n");
    return -1;
  }

  size_t capacity = 2048;
  void* buffer = malloc(capacity);

  if (timeout_ms < 0) timeout_ms = 10;
   
  for (size_t iq = 0; iq < count; ++iq) {
    const char* record_name = "PTR";
    if (query[iq].type == MDNS_RECORDTYPE_SRV)
      record_name = "SRV";
    else if (query[iq].type == MDNS_RECORDTYPE_A)
      record_name = "A";
    else if (query[iq].type == MDNS_RECORDTYPE_AAAA)
      record_name = "AAAA";
    else
      query[iq].type = MDNS_RECORDTYPE_PTR;
  }
  for (int isock = 0; isock < num_sockets; ++isock) {
    query_id[isock] =
      mdns_multiquery_send(sockets[isock], query, count, buffer, capacity, 0);
    if (query_id[isock] < 0)
      printf("Failed to send mDNS query: %s\n", strerror(errno));
  }
  
  // Loop as long as we get replies with 1s timeout
  int res;

  int records = 0;

  int resp_count = 0;

  do {
    struct timeval timeout;
    timeout.tv_sec = timeout_ms/1000;
    timeout.tv_usec = (timeout_ms%1000)*1000;
    
    int nfds = 0;
    fd_set readfs;
    FD_ZERO(&readfs);
    for (int isock = 0; isock < num_sockets; ++isock) {
      if (sockets[isock] >= nfds)
	nfds = sockets[isock] + 1;
      FD_SET(sockets[isock], &readfs);
    }

    res = select(nfds, &readfs, 0, 0, &timeout);
    if (res > 0) {
      for (int isock = 0; isock < num_sockets; ++isock) {
	if (FD_ISSET(sockets[isock], &readfs)) {
	  size_t rec = mdns_query_recv(sockets[isock], buffer, capacity,
				       query_callback,
				       &query_responses[resp_count],
				       query_id[isock]);
	  if (rec > 0) {
	    int start_at = strlen(result_buf);
	    int left = result_len-start_at;
	    if (left < 0) left = 0;
	    
	    snprintf(&result_buf[start_at], left,
		     "%.*s{ %s { %s } }",
		     resp_count, " ",
		     query_responses[resp_count].host_address,
		     query_responses[resp_count].txt_dict);
	    resp_count++;
	    records += rec;
	    if (resp_count >= MAX_RESPONSES) break;
	  }
	}
	FD_SET(sockets[isock], &readfs);
      }
    }
  } while (res > 0);
  
  free(buffer);
  
  for (int isock = 0; isock < num_sockets; ++isock)
    mdns_socket_close(sockets[isock]);
  
  return 0;
}

#ifdef STAND_ALONE
int main(int argc, char *argv[])
{
  if (argc < 2) {
    printf("usage: %s service\n", argv[0]);
    return 0;
  }
  char buf[1024];
  
  send_mdns_query_service(argv[1], buf, sizeof(buf));

  if (strlen(buf)) printf("%s\n", buf); 

  return 1;
}
#endif
