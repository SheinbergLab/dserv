#ifndef LOGCLIENT_H
#define LOGCLIENT_H

class LogClient {
 public:
  // These are special events that dserv can process
  enum { DSERV_EVT_OBS_BEGIN = 19, DSERV_EVT_OBS_END = 20 };

  enum {
    LOGGER_CLIENT_PAUSED, LOGGER_CLIENT_RUNNING, LOGGER_CLIENT_SHUTDOWN
  };

  const int DSERV_LOG_CURRENT_VERSION = 3;
  const int DSERV_LOG_HEADER_SIZE = 16;
  
  std::string filename;
  std::atomic<int> active;			/* set to 0 if connection bad */
  int fd;			/* file to write to           */

  SharedQueue<ds_datapoint_t *> dpoint_queue;
    
  ds_datapoint_t pause_dpoint;    /* dpoint signal pause    */
  ds_datapoint_t start_dpoint;    /* dpoint signal start    */
  ds_datapoint_t flush_dpoint;    /* dpoint signal flush    */
  ds_datapoint_t shutdown_dpoint; /* dpoint signal shutdown */

  ds_datapoint_t beginobs_dpoint; /* dpoint signal beginobs */
  ds_datapoint_t endobs_dpoint;   /* dpoint signal endobs   */
  
  LogMatchDict matches;
  std::atomic<int> obs_limited_matches;

  int state;
  bool in_obs;
  
  const char *beginobs_varname = "logger:beginobs";
  const char *endobs_varname = "logger:endobs";

  static void log_client_process(LogClient *);
  
  LogClient(std::string filename, int fd);
  ~LogClient();
  uint64_t now(void);
  int write_header(uint64_t timestamp);
  void flush_dpoints(void);
  void log_pause(void);
  void log_resume(void);
  void log_flush(ds_logger_buf_t *logbuf);
  int log_point(ds_datapoint_t *dpoint, ds_logger_buf_t *logbuf);
  int write_dpoint(ds_datapoint_t *dpoint);
};

#endif
