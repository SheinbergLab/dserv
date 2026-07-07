#include <unordered_map>
#include <mutex>
#include <cstring>
#include <thread>
#include <chrono>
#include <iostream>

#include "Datapoint.h"
#include "sharedqueue.h"
#include "LogMatchDict.h"
#include "LogTable.h"
#include "LogClient.h"

void LogTable::insert(std::string key, LogClient *s)
{
  std::lock_guard<std::mutex> mlock(mutex_);
  map_[key] = s;
}

void LogTable::remove(std::string key)
{
  std::lock_guard<std::mutex> mlock(mutex_);
  map_.erase (key);
}

void LogTable::clear(std::string key)
{
  std::lock_guard<std::mutex> mlock(mutex_);
  map_.clear ();
}
  
bool LogTable::find(std::string key, LogClient **s)
{
  std::lock_guard<std::mutex> mlock(mutex_);
  auto iter = map_.find(key);
  
  if (iter != map_.end()) {
    if (s) *s = iter->second;
    return true;
  }
  return false;
}

std::string LogTable::clients(void)
{
  std::string clients;
  std::lock_guard<std::mutex> mlock(mutex_);
  for (auto it : map_) {
    LogClient *log_client = it.second;
    if (log_client) {
      if (clients.length() != 0) clients += " ";
      clients += log_client->filename;
    }
  }
  return clients;
}


/*
 * control_client
 *
 *  Handle a pause/start/flush/close request for one log client.
 * Called only from the dataserver's logger thread, which processes
 * these in arrival order with the data stream (so a close/pause
 * cannot overtake datapoints set before it) and is the sole owner of
 * all logbuf state (so the flushes here cannot race the buffering in
 * forward_dpoint).
 *
 *  Holding mutex_ excludes the client's writer thread from removing
 * itself (its self-removal takes this lock), so the client cannot be
 * deleted while we work on it.
 */
void LogTable::control_client(std::string key, uint32_t flags)
{
  std::lock_guard<std::mutex> mlock(mutex_);
  auto iter = map_.find(key);
  if (iter == map_.end()) return;

  LogClient *log_client = iter->second;
  if (!log_client || !log_client->active) return;

  if (flags & DSERV_DPOINT_LOGSTART_FLAG) {
    log_client->state = LogClient::LOGGER_CLIENT_RUNNING;
  }
  else if (flags & DSERV_DPOINT_LOGPAUSE_FLAG) {
    log_client->flush_dpoints();
    log_client->state = LogClient::LOGGER_CLIENT_PAUSED;
  }
  else if (flags & DSERV_DPOINT_LOGFLUSH_FLAG) {
    log_client->flush_dpoints();
  }
  else if (flags & DSERV_DPOINT_LOGCLOSE_FLAG) {
    /* flush buffered matches, then queue the writer-thread shutdown
       BEHIND the flushed buffers and everything forwarded earlier:
       the file ends exactly at the close point, nothing is dropped.
       active = 0 stops any further forwarding to this client. */
    log_client->flush_dpoints();
    log_client->state = LogClient::LOGGER_CLIENT_SHUTDOWN;
    log_client->active = 0;
    log_client->dpoint_queue.push_back(&log_client->shutdown_dpoint);
  }
}

/*
 * add_match
 *
 *  Attach a match to an open log client under the table lock.  A
 * LogClient always removes itself from this table (taking mutex_)
 * before deleting itself, so holding the lock guarantees the client
 * stays alive for the duration of the call.
 */
int LogTable::add_match(std::string key, const char *match,
			int every, int obs, int bufsize)
{
  std::lock_guard<std::mutex> mlock(mutex_);
  auto iter = map_.find(key);
  if (iter == map_.end()) return 0;

  LogClient *log_client = iter->second;
  if (!log_client || !log_client->active) return 0;

  LogMatchSpec *m = new LogMatchSpec(match, every, obs, bufsize);
  log_client->matches.insert(m->matchstr, m);
  log_client->obs_limited_matches += m->obs_limited;
  return 1;
}

/*
 * shutdown_clients
 *
 *  Runs from ~LogTable (via ~Dataserver), after the logger thread has
 * been joined — so flushing logbufs from here cannot race it.  This is
 * the one sanctioned exception to "only the logger thread touches
 * logbufs".
 *
 *  After queueing the shutdowns, wait (bounded) for the writer
 * threads to finish: each removes itself from this table after its
 * final write, so an empty map means every datafile is complete and
 * no detached writer will touch this table after it is destroyed.
 */
void LogTable::shutdown_clients(void)
{
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    for (auto it : map_) {
      LogClient *log_client = it.second;
      if (log_client) {
	if (log_client->active) log_client->flush_dpoints();
	log_client->dpoint_queue.push_back(&log_client->shutdown_dpoint);
      }
    }
  }

  for (int i = 0; i < 5000; i++) {	/* bounded: up to ~5s */
    {
      std::lock_guard<std::mutex> mlock(mutex_);
      if (map_.empty()) return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::cerr << "LogTable: timed out waiting for log writers to finish"
	    << std::endl;
}

void LogTable::forward_dpoint(ds_datapoint_t *dpoint)
{
  std::lock_guard<std::mutex> mlock(mutex_);
  for (auto it : map_) {
    LogClient *log_client = it.second;
    ds_logger_buf_t *logbuf;
    
    if (!log_client->active) continue;
    
    if (log_client->state == LogClient::LOGGER_CLIENT_RUNNING ||
	log_client->obs_limited_matches) {
      
      // if the client has obs limited matches, always log obs events
      if (log_client->obs_limited_matches &&
	  dpoint->data.e.dtype == DSERV_EVT &&
	  (dpoint->data.e.type == LogClient::DSERV_EVT_OBS_BEGIN ||
	   dpoint->data.e.type == LogClient::DSERV_EVT_OBS_END)) {
	
	if (dpoint->data.e.type == LogClient::DSERV_EVT_OBS_END) {
	  // flush any buffered matches before closing obs period
	  log_client->flush_dpoints();
	  
	  // if the client wants the actual ENDOBS event logged
	  if (log_client->matches.is_match(dpoint->varname,
					   &logbuf, log_client->in_obs)) {
	    log_client->log_point(dpoint, logbuf);
	  }

	  log_client->endobs_dpoint.timestamp = dpoint->timestamp;
	  log_client->log_point(&log_client->endobs_dpoint, NULL);
	  log_client->in_obs = false;
	}
	else {
	  log_client->in_obs = true;
	  log_client->beginobs_dpoint.timestamp = dpoint->timestamp;
	  log_client->log_point(&log_client->beginobs_dpoint, NULL);
	  
	  // if the client wants the actual BEGINOBS event logged
	  if (log_client->matches.is_match(dpoint->varname, &logbuf,
					   log_client->in_obs)) {
	    log_client->log_point(dpoint, logbuf);
	  }
	}
      }
      else if (log_client->matches.is_match(dpoint->varname, &logbuf,
					    log_client->in_obs)) {
	log_client->log_point(dpoint, logbuf);
      }
    }
  }
}
