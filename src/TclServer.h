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

class TclServerConfig
{
public:
  int newline_listener_port = -1;
  int message_listener_port = -1;
  std::string name;
  TclServerConfig(std::string name, int newline_port, int message_port):
    name(name), newline_listener_port(newline_port), message_listener_port(message_port) {};
  TclServerConfig(std::string name):
    name(name), newline_listener_port(-1), message_listener_port(-1) {};
  TclServerConfig(std::string name, int port):
    name(name), newline_listener_port(port), message_listener_port(-1) {};
};
  
class TclServer
{
  std::thread newline_net_thread;
  std::thread message_net_thread;
  std::thread process_thread;
  
  std::mutex mutex;	      // ensure only one thread accesses table
  std::condition_variable cond;	// condition variable for sync

public:
  int argc;
  char **argv;

  enum socket_t { SOCKET_LINE, SOCKET_MESSAGE };
  
  std::atomic<bool> m_bDone;	// flag to close process loop

  std::string name;		// name of this TclServer
  
  // identify connection to send process
  std::string client_name;

  // our dataserver
  Dataserver *ds;
  
  // scripts attached to dpoints
  TriggerDict dpoint_scripts;
  
  // for client requests
  SharedQueue<client_request_t> queue;

  const char *PRINT_DPOINT_NAME = "print";

  int _newline_port;		// for CR/LF oriented communication
  int _message_port;		// for message oriented comm  

  int newline_port(void) { return _newline_port; }
  int message_port(void) { return _message_port; }
  
  // socket type can be SOCKET_LINE (newline oriented) or SOCKET_MESSAGE
  socket_t socket_type;
  
  static void
  tcp_client_process(TclServer *tserv,
		     int sock,
		     SharedQueue<client_request_t> *queue);
  
  static void
  message_client_process(TclServer *tserv,
			 int sock,
			 SharedQueue<client_request_t> *queue);
  
  TclServer(int argc, char **argv, Dataserver *dserv, TclServerConfig cfg);
  TclServer(int argc, char **argv, Dataserver *dserv, std::string name);
  TclServer(int argc, char **argv, Dataserver *dserv,
	    std::string name, int port);
  TclServer(int argc, char **argv, Dataserver *dserv,
	    std::string name, int newline_port, int message_port);
  ~TclServer();
  void shutdown(void);
  bool isDone();
  void start_tcp_server(void);
  void start_message_server(void);
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

