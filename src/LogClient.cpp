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

#include "Datapoint.h"
#include "sharedqueue.h"
#include "LogMatchDict.h"
#include "LogClient.h"

  
LogClient::LogClient(std::string filename, int fd): fd(fd), filename(filename)
{
  active = 1;
  
  // use to track if any matches are obs limited (to track begin/end obs events)
  obs_limited_matches = 0;
  
  // special begin/end obs datapoints for obslimited logging
  beginobs_dpoint.varname = (char *) beginobs_varname;
  beginobs_dpoint.varlen = strlen(beginobs_dpoint.varname)+1;
  beginobs_dpoint.data.type = DSERV_NONE;
  beginobs_dpoint.data.len = 0;
  
  endobs_dpoint.varname = (char *) endobs_varname;
  endobs_dpoint.varlen = strlen(endobs_dpoint.varname)+1;
  endobs_dpoint.data.type = DSERV_NONE;
  endobs_dpoint.data.len = 0;
  
  shutdown_dpoint.flags =
    DSERV_DPOINT_SHUTDOWN_FLAG | DSERV_DPOINT_DONTFREE_FLAG;
  pause_dpoint.flags =
    DSERV_DPOINT_LOGPAUSE_FLAG | DSERV_DPOINT_DONTFREE_FLAG;
  start_dpoint.flags =
    DSERV_DPOINT_LOGSTART_FLAG | DSERV_DPOINT_DONTFREE_FLAG;
  flush_dpoint.flags =
    DSERV_DPOINT_LOGFLUSH_FLAG | DSERV_DPOINT_DONTFREE_FLAG;
  
  state = LOGGER_CLIENT_PAUSED;
  
  if (fd >= 0) write_header(now());
}

LogClient::~LogClient()
{
  if (fd >= 0) close(fd);
}

uint64_t LogClient::now(void) {
  std::chrono::time_point<std::chrono::high_resolution_clock> now = 
    std::chrono::high_resolution_clock::now();    
  auto duration = now.time_since_epoch();
  return
    ((int64_t) std::chrono::duration_cast<std::chrono::microseconds>(duration).
     count());
}

int LogClient::write_header(uint64_t timestamp)
{
  unsigned char buf[DSERV_LOG_HEADER_SIZE];
  
  if (fd < 0) return 0;
  
  buf[0] = 'd';
  buf[1] = 's';
  buf[2] = 'l';
  buf[3] = 'o';
  buf[4] = 'g';
  
  buf[5] = DSERV_LOG_CURRENT_VERSION;
    
  memcpy((unsigned char *) &buf[8], &timestamp, sizeof(uint64_t));
  
  if (write(fd, buf, DSERV_LOG_HEADER_SIZE) != DSERV_LOG_HEADER_SIZE) {
    return 0;
  }
  return 1;
}

void LogClient::flush_dpoints(void)
{
  for (auto it : matches.get_matches()) {
    LogMatchSpec *match = it.second;
    if (!match->active) continue;
    log_flush(match->logbuf);
  }
  return;
}

void LogClient::log_pause(void)
{
  write_dpoint(&pause_dpoint);
}

void LogClient::log_resume(void)
{
  write_dpoint(&start_dpoint);
}
  
void LogClient::log_flush(ds_logger_buf_t *logbuf)
{
  ds_datapoint_t *dpoint;
  if (logbuf && logbuf->bufcount) {
    dpoint = &logbuf->dpoint;
    dpoint->data.len = logbuf->bufcount;
    dpoint->data.buf = (unsigned char *) logbuf->buf;
    dpoint->flags = 0;
    if (write_dpoint(dpoint) < 0) {
      //	printf("dpoint write for %s returned -1\n", dpoint->varname);
    }
    
#if 0
    printf("%s(%d) %d %d %d\n", dpoint->varname, dpoint->varlen,
	   dpoint->data.e.dtype, dpoint->flags, dpoint->data.len);
#endif    
    
    logbuf->bufcount = 0;
    dpoint->flags |= DSERV_DPOINT_NOT_INITIALIZED;
  }
}

		    
int LogClient::log_point(ds_datapoint_t *dpoint, ds_logger_buf_t *logbuf)
{
  int would_overflow = 0;
  
  if (fd < 0) return 0;
  
  //    std::cout << "varname: " << dpoint->varname << " buf: " << logbuf << std::endl;
  
  // write every point to log individually
  if (!logbuf) {
    if (write_dpoint(dpoint) < 0) {
      //printf("dpoint write for %s returned -1\n", dpoint->varname);
      state = LOGGER_CLIENT_SHUTDOWN;
      return 0;
    }
    return 1;
  }
  
  // first dpoint to be added to empty buffer so initialize
  if (logbuf->dpoint.flags & DSERV_DPOINT_NOT_INITIALIZED) {
    if (!logbuf->dpoint.varname) {
      logbuf->dpoint.varname = strdup(dpoint->varname);
      logbuf->dpoint.varlen = dpoint->varlen;
    }
    logbuf->dpoint.data.type = dpoint->data.type;
    logbuf->dpoint.timestamp = dpoint->timestamp;
    logbuf->dpoint.flags &= ~DSERV_DPOINT_NOT_INITIALIZED;
  }
  
  // if the data point type has changed (it shouldn't) just continue
  if (dpoint->data.type != logbuf->dpoint.data.type) {
    return 0;
  }
  
  // if the log buffer can't hold the current point just write out point
  if (logbuf->bufsize <= dpoint->data.len) {
    if (write_dpoint(dpoint) < 0) {
      state = LOGGER_CLIENT_SHUTDOWN;
      return 0;
    }
    return 1;
  }
  
  // if data fits, add it
  if (dpoint->data.len+logbuf->bufcount <= logbuf->bufsize) {
    // buffer the new point's data and bump the bufcount
    memcpy(&((unsigned char *) logbuf->buf)[logbuf->bufcount],
	   dpoint->data.buf, dpoint->data.len);
    logbuf->bufcount += dpoint->data.len;
  }
  else {
    would_overflow = 1;
  }
  
  if ((logbuf->bufcount == logbuf->bufsize) || would_overflow) {
    logbuf->dpoint.data.len = logbuf->bufcount;
    logbuf->dpoint.data.buf = (unsigned char *) logbuf->buf;
    
    if (write_dpoint(&logbuf->dpoint) < 0) {
      state = LOGGER_CLIENT_SHUTDOWN;
      return 0;
    }
    else {
      logbuf->bufcount = 0;
    }
    
    if (would_overflow) {
      memcpy(&((unsigned char *) logbuf->buf)[0],
	     dpoint->data.buf, dpoint->data.len);
      logbuf->bufcount = dpoint->data.len;
      logbuf->dpoint.timestamp = dpoint->timestamp;
    }
    else {
      logbuf->dpoint.flags |= DSERV_DPOINT_NOT_INITIALIZED;
    }
  }
  return state != LOGGER_CLIENT_SHUTDOWN;
}

int LogClient::write_dpoint(ds_datapoint_t *dpoint)
{
#if 0
  printf("%s(%d) %d %d %d\n", dpoint->varname, dpoint->varlen,
	 dpoint->data.e.dtype, dpoint->flags, dpoint->data.len);
#endif
  
  if (write(fd, &dpoint->varlen, sizeof(uint16_t)) != sizeof(uint16_t))
    goto write_error;
  
  if (write(fd, dpoint->varname, (int) dpoint->varlen) !=
      (int) dpoint->varlen)
    goto write_error;
  
  if (write(fd, &dpoint->timestamp, sizeof(uint64_t)) != sizeof(uint64_t))
    goto write_error;
  
  if (write(fd, &dpoint->flags, sizeof(uint32_t)) != sizeof(uint32_t))
    goto write_error;
  
  if (write(fd, &dpoint->data.type, sizeof (ds_datatype_t)) !=
      sizeof (ds_datatype_t))
    goto write_error;
  
  if (write(fd, &dpoint->data.len, sizeof(uint32_t)) != sizeof(uint32_t))
    goto write_error;
  
  if (dpoint->data.len) {
    if (write(fd, dpoint->data.buf, dpoint->data.len) != dpoint->data.len)
      goto write_error;
  }
  return 0;
  
 write_error:
  return -1;
}

void LogClient::log_client_process(LogClient *logclient)
{
  ds_datapoint_t *dpoint;
  bool done = false;

  //  std::cout << "waiting to receive dpoints to log" << std::endl;

  /* process until receive a message saying we are done */
  while (!done) {
    dpoint = logclient->dpoint_queue.front();
    logclient->dpoint_queue.pop_front();

    /* check for shutdown */
    if (dpoint->flags & DSERV_DPOINT_SHUTDOWN_FLAG) {
      logclient->flush_dpoints();
      logclient->state = LOGGER_CLIENT_SHUTDOWN;
      done = true;
    }
    else if (dpoint->flags & DSERV_DPOINT_LOGPAUSE_FLAG) {
      logclient->flush_dpoints();
      logclient->state = LOGGER_CLIENT_PAUSED;
    }
    else if (dpoint->flags & DSERV_DPOINT_LOGSTART_FLAG) {
      logclient->state = LOGGER_CLIENT_RUNNING;
    }
    else if (dpoint->flags & DSERV_DPOINT_LOGFLUSH_FLAG) {
      logclient->flush_dpoints();
    }
    else {
      auto result = logclient->write_dpoint(dpoint);
      dpoint_free(dpoint);
    }
  }

  //  std::cout << "shutting down" << std::endl;

  delete logclient;
}

