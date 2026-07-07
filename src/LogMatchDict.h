#ifndef LOGMATCHDICT_H
#define LOGMATCHDICT_H

#include <unordered_map>
#include <vector>
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

  /* specs replaced/removed while the logger thread may still hold
     their logbuf pointer from is_match(); freeing them here would be
     a use-after-free, so they are parked and reaped in clear() */
  std::vector<LogMatchSpec *> graveyard_;

  std::mutex mutex_;

 public:
  ~LogMatchDict() { clear(); }

  /* snapshot of the logbufs of active matches, taken under the lock
     (iterating map_ directly from another thread races insert/remove) */
  std::vector<ds_logger_buf_t *> active_logbufs(void)
  {
    std::vector<ds_logger_buf_t *> bufs;
    std::lock_guard<std::mutex> mlock(mutex_);
    for (auto it : map_) {
      LogMatchSpec *match = it.second;
      if (match->active && match->logbuf)
	bufs.push_back(match->logbuf);
    }
    return bufs;
  }
  
  void insert(std::string key, LogMatchSpec *m)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    auto iter = map_.find(key);
    if (iter != map_.end()) {
      graveyard_.push_back(iter->second);
    }
    map_[key] = m;
  }

  void remove(std::string key)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    auto iter = map_.find(key);
    if (iter != map_.end()) {
      graveyard_.push_back(iter->second);
      map_.erase (iter);
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
    for (auto match : graveyard_) {
      delete match;
    }
    graveyard_.clear ();
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
    /* lock held for the full iteration: insert()/remove() run on other
       threads and would otherwise invalidate iterators mid-scan */
    std::lock_guard<std::mutex> mlock(mutex_);
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

