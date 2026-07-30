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
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <io.h>

#pragma comment(lib, "Ws2_32.lib")
#endif

#include <sys/types.h>
#include <fcntl.h>

#include "SendClient.h"
#include <errno.h>

SendClient::SendClient(int socket, char *hoststr, int port, uint8_t flags):
    port(port), fd(socket)
  {
    uint8_t binary = (flags & 0x01) != 0;
    uint8_t json = (flags & 0x02) != 0;
    type = SOCKET_CLIENT;
    host = strdup(hoststr);
    send_binary = binary;
    send_json = json;
    shutdown_dpoint.flags = DSERV_DPOINT_SHUTDOWN_FLAG;
  }

SendClient::SendClient(SharedQueue<client_request_t> *client_queue):
  client_queue(client_queue)
{
  type = QUEUE_CLIENT;
  shutdown_dpoint.flags = DSERV_DPOINT_SHUTDOWN_FLAG;
}

SendClient::~SendClient()
{
  if (type == SOCKET_CLIENT) {
#ifndef _MSC_VER
    if (fd >= 0) close(fd);
#else
    if (fd >= 0) closesocket(fd);
#endif
    free(host);
  }
  //    std::cout << "SendClient shutdown" << std::endl;
}

int SendClient::send_dpoint(ds_datapoint_t *dpoint)
{
  int nwritten;
  
  if (send_binary) {
    int bufsize = DPOINT_BINARY_FIXED_LENGTH - 1;

    unsigned char buf[DPOINT_BINARY_FIXED_LENGTH];
    buf[0] = DPOINT_BINARY_MSG_CHAR;
    if (dpoint_to_binary(dpoint, &buf[1], &bufsize)) {
#ifndef _MSC_VER
      /* A SEND TIMEOUT IS NOT A DEAD PEER, and conflating them broke OTA.
       *
       * This used to be an unconditional `active = 0` on any short write, with an
       * EMPTY `if (nwritten == -1) {}` block where the errno test belonged. That
       * was harmless while the socket had no SO_SNDTIMEO: write() blocked as long
       * as the consumer needed and eventually succeeded. Adding a 5 s send timeout
       * (Dataserver.cpp, to reap vanished boxes) turned every SLOW consumer into a
       * reaped one -- and an OTA target is slow BY DEFINITION, because it writes
       * flash between chunks. Symptom: the box's session dies seconds into
       * staging, the next host write fails, and the OTA reports host_io with
       * progress=0 while the box itself is perfectly healthy. It then cannot
       * re-register (dserv's connect-back is one-shot) so it looks dead until
       * power-cycled. Diagnosed 2026-07-30 after it killed two boxes in a row.
       *
       * EAGAIN/EWOULDBLOCK after SO_SNDTIMEO means "the buffer is full and the
       * peer has not drained it yet" -- transient. Retry, bounded, so a genuinely
       * wedged peer cannot park this thread forever. Detecting a truly dead peer
       * is TCP_USER_TIMEOUT's job (socket_keepalive.h): it fires on data that goes
       * unacknowledged, which is the actual signal for "gone", and it is what the
       * leak fix should have relied on for this case all along.
       *
       * Only a HARD error (EPIPE, ECONNRESET, ...) or a persistent stall reaps the
       * client now. Each SendClient has its own thread, so the retry delays only
       * this consumer and never dserv as a whole.
       */
      int tries = 0;
      for (;;) {
	nwritten = write(fd, buf, DPOINT_BINARY_FIXED_LENGTH);
	if (nwritten == DPOINT_BINARY_FIXED_LENGTH) {
	  break;
	}
	if (nwritten == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
	  if (++tries < 6) {         /* ~30 s at the 5 s SO_SNDTIMEO */
	    continue;
	  }
	}
	active = 0;                  /* hard error, short write, or a real stall */
	break;
      }
#else
      nwritten = send(fd, (const char *) buf, DPOINT_BINARY_FIXED_LENGTH, 0);
      if (nwritten != DPOINT_BINARY_FIXED_LENGTH) {
	active = 0;
      }
#endif
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
    
    /* Same slow-consumer-is-not-a-dead-consumer fix as the binary path above --
     * see that comment for why. This path had the identical unconditional
     * `active = 0` with the identical empty errno block, and it feeds the string
     * and JSON clients (essgui, plain socket subscribers), which have exactly the
     * same right not to be reaped for being briefly slow. Fixed together on
     * purpose: an hour earlier the same day, fixing one of two identical build
     * matchers and leaving its twin alone cost a debugging round. */
    {
      int tries = 0;
      for (;;) {
	nwritten = writev(fd, (const struct iovec *) iov, 2);
	if (nwritten == dstring_size + 1) {
	  break;
	}
	if (nwritten == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
	  if (++tries < 6) {
	    continue;
	  }
	}
	active = 0;
	break;
      }
    }

    /* if we allocated the buffer, free here */
    if (dstring_alloc) free(dstring_buf);
  }
  return active;
}

void SendClient::send_client_process(std::shared_ptr<SendClient> sendclient)
{
  ds_datapoint_t *dpoint;
  bool done = false;

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

  /*
   * end-of-stream contract: this thread is the only producer into
   * client_queue, and this push is its final touch of that queue.
   * The queue's owner may free it only after receiving REQ_QUEUE_EOS
   * (consumers that never tear their queue down just ignore it).
   */
  if (sendclient->type == QUEUE_CLIENT && sendclient->client_queue) {
    client_request_t eos_request;
    eos_request.type = REQ_QUEUE_EOS;
    sendclient->client_queue->push_back(eos_request);
  }

  /* the object is freed when the last shared_ptr (table's or ours)
     goes away — no delete here */
}
