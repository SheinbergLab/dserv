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
  std::string client_name;
  int socket_fd;
  int tcpport;
  std::thread net_thread;
  std::thread process_thread;
  
  Tcl_Interp *interp;
  Dataserver *ds;
  
  std::atomic<bool> m_bDone;
  
  // scripts attached to dpoints
  TriggerDict dpoint_scripts;
  
  std::mutex mutex;		        // ensure only one thread accesses table
  std::condition_variable cond;		// conditino variable for sync
  
public:
  // for client requests
  SharedQueue<client_request_t> queue;

  const char *PRINT_DPOINT_NAME = "print";

  int port(void) { return tcpport; }

  static void
  tcp_client_process(int sock,
		     SharedQueue<client_request_t> *queue);
  
  TclServer(int argc, char **argv,
	    Dataserver *dserv, int port = 2570);
  ~TclServer();
  void shutdown(void);
  bool isDone();
  void start_tcp_server(void);
  int sourceFile(const char *filename);
  uint64_t now(void) { return ds->now(); }
  
/********************************* now *********************************/

  static int now_command (ClientData data, Tcl_Interp *interp,
			  int objc, Tcl_Obj *objv[]);
  static int dserv_add_match_command(ClientData data, Tcl_Interp * interp,
				     int objc,
				     Tcl_Obj * const objv[]);
  static int dserv_add_exact_match_command(ClientData data, Tcl_Interp * interp,
					   int objc,
					   Tcl_Obj * const objv[]);
  static int dserv_remove_match_command(ClientData data, Tcl_Interp * interp,
					int objc,
					Tcl_Obj * const objv[]);
  static int dserv_remove_all_matches_command(ClientData data,
					      Tcl_Interp * interp,
					      int objc,
					      Tcl_Obj * const objv[]);
  static
  int dserv_logger_clients_command(ClientData data, Tcl_Interp *interp,
				   int objc, Tcl_Obj * const objv[]);

  static
  int dserv_log_open_command(ClientData data, Tcl_Interp *interp,
			     int objc, Tcl_Obj * const objv[]);
  static
  int dserv_log_close_command(ClientData data, Tcl_Interp *interp,
			      int objc, Tcl_Obj * const objv[]);
  static
  int dserv_log_pause_command(ClientData data, Tcl_Interp *interp,
			      int objc, Tcl_Obj * const objv[]);
  static
  int dserv_log_start_command(ClientData data, Tcl_Interp *interp,
			      int objc, Tcl_Obj * const objv[]);
  static
  int dserv_log_add_match_command(ClientData data, Tcl_Interp *interp,
				  int objc, Tcl_Obj * const objv[]);
  static int dpoint_set_script_command (ClientData data, Tcl_Interp *interp,
					int objc, Tcl_Obj *objv[]);
  static int dpoint_remove_script_command (ClientData data, Tcl_Interp *interp,
					   int objc, Tcl_Obj *objv[]);
  static int dpoint_remove_all_scripts_command (ClientData data,
						Tcl_Interp *interp,
						int objc, Tcl_Obj *objv[]);
  static int gpio_line_request_output_command(ClientData data,
					      Tcl_Interp *interp,
					      int objc, Tcl_Obj *objv[]);
  static int gpio_line_set_value_command(ClientData data,
					 Tcl_Interp *interp,
					 int objc, Tcl_Obj *objv[]);
  static int print_command (ClientData data, Tcl_Interp *interp,
			    int objc, Tcl_Obj *objv[]);
  void add_tcl_commands(Tcl_Interp *interp);
  int Tcl_StimAppInit(Tcl_Interp *interp);
  int setup_tcl(int argc, char **argv);
  void set_point(ds_datapoint_t *dp);
  int process_requests(void);
  int queue_size(void);
  std::string eval(char *s);
  std::string eval(std::string script);
  void eval_noreply(char *s);
  void eval_noreply(std::string script);
};
#endif  // TCLSERVER_H
