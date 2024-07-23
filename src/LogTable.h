#ifndef LOGTABLE_H
#define LOGTABLE_H

#include <unordered_map>
#include <mutex>

class LogTable
{
 private:
  std::unordered_map<std::string, LogClient *> map_;
  std::mutex mutex_;
  
public:
  void insert(std::string key, LogClient *s)
    {
      std::lock_guard<std::mutex> mlock(mutex_);
      map_[key] = s;
    }

  void remove(std::string key)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    map_.erase (key);
  }

  void clear(std::string key)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    map_.clear ();
  }
  
  bool find(std::string key, LogClient **s)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    auto iter = map_.find(key);
    
    if (iter != map_.end()) {
      if (s) *s = iter->second;
      return true;
    }
    return false;
  }

  void forward_dpoint(ds_datapoint_t *dpoint)
  {
    std::vector<LogClient *> close_vec;
    
    std::lock_guard<std::mutex> mlock(mutex_);
    for (auto it : map_) {
      LogClient *log_client = it.second;
      ds_logger_buf_t *logbuf;

      if (!log_client->active) {
	//	std::cout << "log client " << log_client->filename << " inactive" << std::endl;
	close_vec.push_back(log_client);
      }
      
      else if (log_client->state == LogClient::LOGGER_CLIENT_RUNNING ||
	       log_client->obs_limited_matches) {

	// if the client has obs limited matches, always log obs events
	if (log_client->obs_limited_matches &&
	    dpoint->data.e.dtype == DSERV_EVT &&
	    (dpoint->data.e.type == LogClient::DSERV_EVT_OBS_BEGIN ||
	     dpoint->data.e.type == LogClient::DSERV_EVT_OBS_END)) {
	  
	  if (dpoint->data.e.type == LogClient::DSERV_EVT_OBS_END) {
	    log_client->flush_dpoint.timestamp = dpoint->timestamp;
	    log_client->log_point(&log_client->flush_dpoint, NULL);
	    
	    // if the client wants the actual ENDOBS event logged
	    if (log_client->matches.is_match(dpoint->varname,
					     &logbuf, log_client->in_obs)) {
	      log_client->log_point(dpoint, logbuf);
	    }
	    
	    log_client->endobs_dpoint.timestamp = dpoint->timestamp;
	    log_client->log_point(&log_client->endobs_dpoint, NULL);
	  }
	  else {
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
    
    // for clients no longer active, shut them down and erase from table
    for (auto log_client : close_vec) {
      map_.erase (log_client->filename);
      delete log_client;
      //  std::cout << "inactive: " << key << std::endl;
    }
  }
};

#endif
