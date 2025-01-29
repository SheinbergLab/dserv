#ifndef TRIGGERDICT_H
#define TRIGGERDICT_H

#include <unordered_map>
#include <mutex>
#include "MatchDict.h"

class TriggerDict
{
 private:
  std::unordered_map<std::string, std::string> map_;
  std::mutex mutex_;

 public:
  void insert(std::string key, std::string script)
    {
      std::lock_guard<std::mutex> mlock(mutex_);
      map_[key] = script;
    }

  void remove(std::string key)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    map_.erase (key);
  }
  
  void clear()
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    map_.clear ();
  }
  
  bool find(std::string key, std::string &script)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    auto iter = map_.find(key);
    
    if (iter != map_.end()) {
      script = iter->second;
      return true;
    }
    return false;
  }


  /*
   * find_match()
   *
   *   If datapoint is a match return true and set script
   */
  
  bool find_match(std::string varname, std::string &script)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    for (auto const& [ key, value ] : map_) {
      if (MatchDict::FastWildCompare((char *) key.c_str(), (char *) varname.c_str())) {
	script = value;
	return true;
      }
    }
    return false;
  }
  
};

#endif
