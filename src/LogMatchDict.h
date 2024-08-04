#ifndef LOGMATCHDICT_H
#define LOGMATCHDICT_H

#include <unordered_map>
#include <mutex>
#include "MatchDict.h"

/* storage for buffered datapoints for a given match */
typedef struct ds_logger_buf_s {
  ds_datapoint_t dpoint;
  int bufsize;
  int bufcount;
  void *buf;
} ds_logger_buf_t;


class LogMatchSpec: public MatchSpec {
public:
  ds_logger_buf_t *logbuf;
  int obs_limited;

public:

  LogMatchSpec(const char *str, int every, int obs_limited, int bufsize):
    MatchSpec(str, every), obs_limited(obs_limited) {

    if (bufsize > 0) {
      logbuf = (ds_logger_buf_t *) calloc(1, sizeof (ds_logger_buf_t));
      logbuf->bufsize = bufsize;
      logbuf->buf = malloc(bufsize);
      logbuf->bufcount = 0;
      logbuf->dpoint.data.buf = NULL;
      logbuf->dpoint.varname = NULL;
      logbuf->dpoint.flags |= DSERV_DPOINT_NOT_INITIALIZED_FLAG;
    }
    else {
      logbuf = nullptr;
    }
  }
  ~LogMatchSpec() {
    if (logbuf) {
      if (logbuf->buf) free(logbuf->buf);
      free(logbuf);
    }
  }
};

class LogMatchDict
{
 private:
  std::unordered_map<std::string, LogMatchSpec *> map_;
  std::mutex mutex_;

 public:
  
  std::unordered_map<std::string, LogMatchSpec *>get_matches(void)
  {
    return map_;
  }
  
  void insert(std::string key, LogMatchSpec *m)
  {
    LogMatchSpec *old;
    if (find(key, &old)) {
      delete old;
    }
    std::lock_guard<std::mutex> mlock(mutex_);
      map_[key] = m;
  }
  
  void remove(std::string key)
  {
    LogMatchSpec *old;
    std::lock_guard<std::mutex> mlock(mutex_);
    if (find(key, &old)) {
      delete old;
      map_.erase (key);
    }
  }
  
  void clear(void)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    for (auto it : map_) {
      LogMatchSpec *match = it.second;
      delete match;
    }
    map_.clear ();
  }
  
  bool find(std::string key, LogMatchSpec **m)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    auto iter = map_.find(key);
    
    if (iter != map_.end()) {
      if (m) *m = iter->second;
      return true;
    }
    return false;
  }

  /*
   * is_match()
   *
   *   Is this logger subscribed to this datapoint?
   *
   * Returns:
   *   1 - yes
   *   0 - no
   *   if buffer_size != NULL, set to buffer count for this dpoint
   *   if obs_limited != NULL, indicates if storing only during obs periods
   */
  
  bool is_match(char *var, ds_logger_buf_t **logbuf, bool in_obs)
  {
    bool ret = false;
    for (auto it : map_) {
      LogMatchSpec *match = it.second;
      if (!match->active) continue;
      switch (match->type) {
      case MatchSpec::MATCH_EXACT:
	if (!strcmp(var, match->matchstr.c_str()) &&
	    !(match->count++ % match->alert_every) &&
	    !(match->obs_limited && !in_obs)) {
	  if (logbuf) *logbuf = match->logbuf;
	  ret = true;
	}
	break;
      case MatchSpec::MATCH_KRAUSS:
	if (MatchDict::FastWildCompare((char *) match->matchstr.c_str(), var) &&
	    !(match->count++ % match->alert_every) &&
	    !(match->obs_limited && !in_obs)) {
	  if (logbuf) *logbuf = match->logbuf;
	  ret = true;
	}
	break;
      case MatchSpec::MATCH_BEGIN:
      case MatchSpec::MATCH_END:
      case MatchSpec::MATCH_ANYWHERE:
	break;
      }
    }

    return ret;
  }
};


#endif	// LOGMATCHDICT_H

