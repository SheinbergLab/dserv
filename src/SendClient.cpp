#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <queue>
#include <cstring>

#ifdef __linux__
#include <sstream>
#endif

#include <stdlib.h>

#ifndef _WIN32
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/select.h>
#else
include <io.h>
#endif

#include <sys/types.h>
#include <fcntl.h>

#include "SendClient.h"

SendClient::SendClient(int socket, char *hoststr, int port, uint8_t flags):
    port(port), fd(socket)
  {
    uint8_t binary = (flags & 0x01) != 0;
    uint8_t json = (flags & 0x02) != 0;    
    active = 1;
    type = SOCKET_CLIENT;
    host = strdup(hoststr);
    port = port;
    send_binary = binary;
    send_json = json;
    shutdown_dpoint.flags = DSERV_DPOINT_SHUTDOWN_FLAG;
  }

SendClient::SendClient(SharedQueue<client_request_t> *client_queue):
  client_queue(client_queue)
{
  type = QUEUE_CLIENT;
  active = 1;
  shutdown_dpoint.flags = DSERV_DPOINT_SHUTDOWN_FLAG;
}

SendClient::~SendClient()
{
  if (type == SOCKET_CLIENT) {
    if (fd >= 0) close(fd);
    free(host);
  }
  //    std::cout << "SendClient shutdown" << std::endl;
}

int SendClient::send_dpoint(ds_datapoint_t *dpoint)
{
  int nwritten;
  
  if (send_binary) {
    int bufsize = DPOINT_BINARY_FIXED_LENGTH - 1;
    static unsigned char buf[DPOINT_BINARY_FIXED_LENGTH];
    buf[0] = DPOINT_BINARY_MSG_CHAR;
    if (dpoint_to_binary(dpoint, &buf[1], &bufsize)) {
      nwritten = write(fd, buf, DPOINT_BINARY_FIXED_LENGTH);
      if (nwritten != DPOINT_BINARY_FIXED_LENGTH) {
	if (nwritten == -1)
	  {
	  }
	active = 0;
      }
    }
  }
  else {
    iovec iov[2];
    char newline_buf[2] = "\n";
    char buf[128], *dstring_buf;
    int dstring_bufsize, dstring_size, dstring_alloc;
    
    /* original send format */
    if (!send_json) {
      dstring_bufsize = dpoint_string_size(dpoint);
      if (dstring_bufsize < sizeof(buf)) {
	dstring_size = dpoint_to_string(dpoint, buf, sizeof(buf));
	iov[0].iov_base = buf;
	iov[0].iov_len = dstring_size;
	iov[1].iov_base = newline_buf;
	iov[1].iov_len = 1;
	dstring_alloc = 0;
      }
      /* Still some concern about below... */
      else {
	dstring_buf = (char *) malloc(dstring_bufsize);
	dstring_size = dpoint_to_string(dpoint, dstring_buf, dstring_bufsize);
	iov[0].iov_base = dstring_buf;
	iov[0].iov_len = dstring_size;
	iov[1].iov_base = newline_buf;
	iov[1].iov_len = 1;
	dstring_alloc = 1;
      }
    }
    /* send dpoint as JSON */
    else {
      if ((dstring_buf = dpoint_to_json(dpoint))) {
	dstring_size = strlen(dstring_buf);
	iov[0].iov_base = dstring_buf;
	iov[0].iov_len = dstring_size;
	iov[1].iov_base = newline_buf;
	iov[1].iov_len = 1;
	dstring_alloc = 1;
      }
      else {
	/* should never happen? */
	const char s[] = "json";
	iov[0].iov_base = (void *) s;
	iov[0].iov_len = strlen(s);
	iov[1].iov_base = newline_buf;
	iov[1].iov_len = 1;
	dstring_alloc = 0;
      }
    }
    
    nwritten = writev(fd, (const struct iovec *) iov, 2);
    
    /* if we allocated the buffer, free here */
    if (dstring_alloc) free(dstring_buf);
    
    /* if write failed, dactivate this thread */
    if (nwritten != dstring_size+1) {
      if (nwritten == -1) {
      }
      
      active = 0;
    }
  }
  return active;
}

void SendClient::send_client_process(SendClient *sendclient)
{
  ds_datapoint_t *dpoint;
  bool done = false;

  //  std::cout << "waiting to receive dpoints to send" << std::endl;

  /* process until receive a message saying we are done */
  while (!done) {
    dpoint = sendclient->dpoint_queue.front();
    sendclient->dpoint_queue.pop_front();

    /* check for shutdown */
    if (dpoint->flags & DSERV_DPOINT_SHUTDOWN_FLAG) {
      done = true;
    }
    else {
      if (sendclient->type == SOCKET_CLIENT) {
	auto result = sendclient->send_dpoint(dpoint);
	dpoint_free(dpoint);
      }
      else if (sendclient->type == QUEUE_CLIENT) {
	if (sendclient->client_queue) {
	  /* the client is reponsible for freeing the dpoint */
	  client_request_t client_request;
	  client_request.type = REQ_DPOINT_SCRIPT;
	  client_request.dpoint = dpoint;
	  sendclient->client_queue->push_back(client_request);
	}
      }
    }
  }
  //  std::cout << "shutting down" << std::endl;

  delete sendclient;
}
