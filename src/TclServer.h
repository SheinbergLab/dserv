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

#include "Datapoint.h"
#include <tcl.h>
#include "EventLog.h"
#include "Stimctrl.h"

class TTimer: public Timer {
private:
  ds_datapoint_t timer_datapoint;
  
public:
  std::string timername;
  TTimer(int id): Timer(id)
  {
    timername = "timer/" + std::to_string(id);
    dpoint_set(&timer_datapoint, (char *) timername.c_str(), 0,
	       DSERV_SCRIPT, 0, NULL);
  }
};

#include <unordered_map>
#include <mutex>

class TimerDict
{
 private:
  std::unordered_map<int, std::string> map_;
  std::mutex mutex_;

 public:
  void insert(int key, std::string script)
    {
      std::lock_guard<std::mutex> mlock(mutex_);
      map_[key] = script;
    }

  void remove(int key)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    map_.erase (key);
  }
  
  bool find(int key, std::string &script)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    auto iter = map_.find(key);
    
    if (iter != map_.end()) {
      script = iter->second;
      return true;
    }
    return false;
  }
};

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
  
  // number of timers
  const int ntimers = 8;
  std::vector<TTimer *> timers;

  // scripts attached to dpoints
  TriggerDict dpoint_scripts;
  
  // scripts attached to dpoints
  TimerDict timer_scripts;

  std::mutex mutex;		        // ensure only one thread accesses table
  std::condition_variable cond;		// conditino variable for sync
  
public:
  // for client requests
  SharedQueue<client_request_t> queue;

  const char *PRINT_DPOINT_NAME = "print";

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
  static int timer_tick_command (ClientData data, Tcl_Interp *interp,
				 int objc, Tcl_Obj *objv[]);
  static int timer_reset_command (ClientData data, Tcl_Interp *interp,
				  int objc, Tcl_Obj *objv[]);
  static int timer_tick_interval_command (ClientData data, Tcl_Interp *interp,
					  int objc, Tcl_Obj *objv[]);
  static int timer_expired_command (ClientData data, Tcl_Interp *interp,
				    int objc, Tcl_Obj *objv[]);
  static int timer_set_script_command (ClientData data, Tcl_Interp *interp,
				       int objc, Tcl_Obj *objv[]);
  static int timer_remove_script_command (ClientData data, Tcl_Interp *interp,
					  int objc, Tcl_Obj *objv[]);
  static int timer_status_command (ClientData data, Tcl_Interp *interp,
				   int objc, Tcl_Obj *objv[]);
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
  int timer_callback(int timerid);
  void set_point(ds_datapoint_t *dp);
  int process_requests(void);
  int queue_size(void);
  std::string eval(char *s);
  std::string eval(std::string script);
  void eval_noreply(char *s);
  void eval_noreply(std::string script);
};
#endif  // TCLSERVER_H
