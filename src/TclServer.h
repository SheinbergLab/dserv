#ifndef TCLSERVER_H
#define TCLSERVER_H

#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <queue>

#include <stdlib.h>

#ifndef _WIN32
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#endif

#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/select.h>
#include <fcntl.h>

#ifdef __QNX__
#include <sys/socket.h>
#endif

#include <tcl.h>

#include "Datapoint.h"
#include "Dataserver.h"

#include <unordered_map>
#include <mutex>

class TclServer
{
  int socket_fd;
  std::thread net_thread;
  std::thread process_thread;
  
  std::mutex mutex;	      // ensure only one thread accesses table
  std::condition_variable cond;	// condition variable for sync
  
public:
  int argc;
  char **argv;

  std::atomic<bool> m_bDone;	// flag to close process loop
  
  // identify connection to send process
  std::string client_name;

  // our dataserver
  Dataserver *ds;
  
  // scripts attached to dpoints
  TriggerDict dpoint_scripts;
  
  // for client requests
  SharedQueue<client_request_t> queue;

  const char *PRINT_DPOINT_NAME = "print";

  // communication port setting (2570)
  int tcpport;
  int port(void) { return tcpport; }

  static void
  tcp_client_process(TclServer *tserv,
		     int sock,
		     SharedQueue<client_request_t> *queue);
  
  TclServer(int argc, char **argv,
	    Dataserver *dserv, int port = 2570);
  ~TclServer();
  void shutdown(void);
  bool isDone();
  void start_tcp_server(void);
  int sourceFile(const char *filename);
  uint64_t now(void) { return ds->now(); }
  
  void set_point(ds_datapoint_t *dp);
  int queue_size(void);
  void shutdown_message(SharedQueue<client_request_t> *queue);
  std::string eval(char *s);
  std::string eval(std::string script);
  void eval_noreply(char *s);
  void eval_noreply(std::string script);
};
#endif  // TCLSERVER_H
