#ifndef DATAPOINTTABLE_H
#define DATAPOINTTABLE_H

#include <unordered_map>
#include <mutex>

class DatapointTable
{
 private:
  std::unordered_map<std::string, ds_datapoint_t *> map_;
  std::mutex mutex_;
  
 public:
  void insert(std::string key, ds_datapoint_t *d)
    {
      std::lock_guard<std::mutex> mlock(mutex_);
      map_[key] = d;
    }

  void remove(std::string key)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    map_.erase (key);
  }
  
  bool find(std::string key, ds_datapoint_t **d)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    auto iter = map_.find(key);
    
    if (iter != map_.end()) {
      if (d) *d = iter->second;
      return true;
    }
    return false;
  }

  
  std::string get_keys(void)
  {
    std::vector<std::string> keys;
    keys.reserve(map_.size());
    std::string s;
    s.clear();
    
    std::unique_lock<std::mutex> mlock(mutex_);
    for (auto kv : map_) { keys.push_back(kv.first); }
    mlock.unlock();
    
    for (std::vector<std::string>::const_iterator p = keys.begin();
	 p != keys.end(); ++p) {
      s += *p;
      if (p != keys.end() - 1)
        s += ' ';
    }
    return s;
  }
  
  std::string get_dg_dir(void)
  {
    std::vector<std::string> entries;

    ds_datapoint_t *dpoint;
    std::string s, entry;
    
    std::unique_lock<std::mutex> mlock(mutex_);
    for (auto kv : map_) {
      dpoint = kv.second;
      if (dpoint->data.e.dtype == DSERV_DG) {
	entry.clear();
	entry = std::string("{") + kv.first + " 0 " +
	  std::to_string(dpoint->data.len) + "}";
	entries.push_back(entry);
      }
    }
    mlock.unlock();

    for (std::vector<std::string>::const_iterator p = entries.begin();
	 p != entries.end(); ++p) {
      s += *p;
      if (p != entries.end() - 1)
        s += ' ';
    }
    return s;
  }
};

#endif
