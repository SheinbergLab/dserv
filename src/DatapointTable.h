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
  void clear()
  {
    std::lock_guard<std::mutex> mlock(mutex_);

    // free all points in the map
    for (const auto & [ key, dpoint ] : map_) {
      if (dpoint) dpoint_free(dpoint);
    }

    // clear the map
    map_.clear ();
  }

  int replace(std::string key, ds_datapoint_t *d)
  {
    int result = 0;
    std::lock_guard<std::mutex> mlock(mutex_);
    auto iter = map_.find(key);
    if (iter != map_.end()) {
      dpoint_free(iter->second);
      result = 1;
    }
    map_[key] = d;
    return result;
  }

  /*
    update existing point in table
    return 1 if point updated, 0 if new point added
  */
  int update(ds_datapoint_t *d)
  {
    ds_datapoint_t *old;
    std::string key{d->varname};
    
    std::lock_guard<std::mutex> mlock(mutex_);
    auto iter = map_.find(key);
    if (iter != map_.end()) {	// point is in table
      ds_datapoint_t *old = iter->second;
      if (old->data.type == d->data.type &&
	  old->data.len == d->data.len) {
	old->timestamp = d->timestamp;
	memcpy(old->data.buf, d->data.buf, old->data.len);

	/* done so can return without updating map_*/
	return 1;
      }
      else {			// points didn't match
	dpoint_free(old);	// so free old one
      }
    }
    map_[key] = d;
    return 0;
  }
  
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

  /*
   * getcopy
   *
   * Ensure that a valid copy of a point is returned, as the point
   * in the table can change quickly, so cannot return the pointer
   * and expect it to still be valid.
   * 
   */
  ds_datapoint_t *getcopy(std::string key)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    auto iter = map_.find(key);
    
    if (iter != map_.end()) {
      return dpoint_copy(iter->second);
    }
    return nullptr;

  }

  int deletepoint(std::string key)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    auto iter = map_.find(key);
    
    if (iter != map_.end()) {
      dpoint_free(iter->second);
      map_.erase (key);
      return 1;
    }

    return 0;
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

  ds_datapoint_t *get_dpoint(std::string key)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    auto iter = map_.find(key);
    if (iter != map_.end()) {
      return dpoint_copy(iter->second);
    }
    else {
      return nullptr;
    }    
  }
  
  int delete_dpoint(std::string key)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    auto iter = map_.find(key);
    if (iter != map_.end()) {
      dpoint_free(iter->second);
      map_.erase (key);
      return 1;
    }
    else {
      return 0;
    }
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
