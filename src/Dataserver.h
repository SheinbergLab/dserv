#ifndef DATASERVER_H
#define DATASERVER_H

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

class Dataserver;		// defined below

#include "Base64.h"
#include "Datapoint.h"
#include "DatapointTable.h"
#include "MatchDict.h"
#include "LogMatchDict.h"
#include "TriggerDict.h"
#include "ClientRequest.h"
#include "SendClient.h"
#include "SendTable.h"
#include "LogTable.h"
#include "LogClient.h"

#include <tcl.h>


class Dataserver
{
  const char *KEYS_POINT_NAME = "@keys";
  
  enum dserv_rc { DSERV_OK, DSERV_BadArgument };

  int socket_fd;

  std::thread process_thread;	// main process loop
  std::thread net_thread;	// tcpip communication
  std::thread send_thread;	// client subscriptions
  std::thread logger_thread;	// log to file

  std::mutex mutex;		        // ensure only one thread accesses table
  std::condition_variable cond;		// condition variable for sync
  
  DatapointTable datapoint_table;
  SendTable send_table;
  LogTable log_table;

  // point queue for notifications
  SharedQueue<ds_datapoint_t *> notify_queue;

  // point queue for loggers
  SharedQueue<ds_datapoint_t *> logger_queue;

public:
  int argc;
  char **argv;
  
  std::atomic<bool> m_bDone;

  // process requests
  SharedQueue<client_request_t> queue;

  // matches used to forward trigger scripts
  MatchDict trigger_matches;
  TriggerDict trigger_scripts;
  
  static void
  tcp_client_process(Dataserver *ds, int sock);


  static int tcp_process_request(Dataserver *ds, char buf[],
				 int nbytes, int msgsock,
				 char **repbuf, int *repsize, int *repalloc);
    
  
  Dataserver(int argc, char **argv, int port=4620);
  ~Dataserver();

  static int64_t now(void);
  int tcpport;
  int port(void) { return tcpport; }

  int add_datapoint_to_table(char *varname,
			     ds_datapoint_t *dpoint);
  int update_datapoint(ds_datapoint_t *dpoint);
  int find_datapoint(char *varname);
  ds_datapoint_t *get_datapoint(char *varname);
  int delete_datapoint(char *varname);
  ds_datapoint_t *new_trigger_point(ds_datapoint_t *dpoint);
  void trigger(ds_datapoint_t *dpoint);
  ds_datapoint_t *process(ds_datapoint_t *dpoint);
  void set(ds_datapoint_t &dpoint);
  void set(ds_datapoint_t *dpoint);
  void set(char *varname, char *value);

  void update(ds_datapoint_t *dpoint);
  int touch(char *varname);
  int get(char *varname, ds_datapoint_t **dpoint);
  int exists(char *varname);
  int clear(char *varname);
  void clear(void);
  char *get_table_keys(void);
  char *get_dg_dir(void);
  void set_key_dpoint(void);
  void add_trigger(char *match, int every, char *script) ;
  void remove_trigger(char *match);
  void remove_all_triggers(void);
  int tcpip_register(char *host, int port, int flags);
  int tcpip_unregister(char *host, int port);
  int tcpip_add_match(char *host, int port, char *match, int every);
  int tcpip_remove_match(char *host, int port, char *match);
  int client_add_match(std::string key, char *match, int every=1);
  int client_add_exact_match(std::string key, char *match, int every=1);
  int client_remove_match(std::string key, char *match);
  int client_remove_all_matches(std::string key);
  std::string get_matches(char *host, int port);
  std::string get_logger_clients(void);
  int logger_client_open(std::string filename, bool overwrite);
  int logger_client_close(std::string filename);
  int logger_client_pause(std::string filename);
  int logger_client_start(std::string filename);
  int logger_add_match(char *path, char *match,
		       int every, int obs, int bufsize);
  void shutdown(void);
  void shutdown_message(SharedQueue<client_request_t> *q);

  bool isDone();
  
  std::string eval(std::string script);
  void eval_noreply(std::string script);
  void start_tcp_server(void);

  int add_to_notify_queue(ds_datapoint_t *dpoint);
  int move_to_notify_queue(ds_datapoint_t *dpoint);
  int process_send_requests(void);
  int add_new_send_client(char *host, int port, uint8_t flags);
  std::string send_client_id(void);
  int remove_send_client_by_id(std::string client_id);  
  std::string add_new_send_client(SharedQueue<client_request_t> *queue);
  int remove_send_client(char *host, int port);
  int open_send_sock(char *host, int port);

  int add_to_logger_queue(ds_datapoint_t *dpoint);
  int move_to_logger_queue(ds_datapoint_t *dpoint);
  int process_log_requests(void);
  int add_new_log_client(std::string filename, bool overwrite = false);
  int remove_log_client(std::string filename);
  int pause_log_client(std::string filename);
  int start_log_client(std::string filename);
  int log_add_match(std::string filename, std::string varname,
		    int every, int obs, int buflen);
};

#endif  // DATASERVER_H
