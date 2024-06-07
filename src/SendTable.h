#ifndef SENDTABLE_H
#define SENDTABLE_H

#include <unordered_map>
#include <mutex>

#include "Datapoint.h"
#include "DatapointTable.h"
#include "MatchDict.h"
#include "LogMatchDict.h"
#include "TriggerDict.h"
#include "SendClient.h"

class SendTable
{
 private:
  std::unordered_map<std::string, SendClient *> map_;
  std::mutex mutex_;
  
 public:
  void insert(std::string key, SendClient *s)
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
  
  bool find(std::string key, SendClient **s)
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
    std::vector<SendClient *> close_vec;
    
    std::lock_guard<std::mutex> mlock(mutex_);
    for (auto it : map_) {
      SendClient *send_client = it.second;
      if (!send_client->active) {
	close_vec.push_back(send_client);
      }
      else if (send_client->matches.is_match(dpoint->varname)) {
	ds_datapoint_t *dp = dpoint_copy(dpoint);
	send_client->dpoint_queue.push_back(dp);
      }
    }

    char key[128];
    for (auto send_client : close_vec) {
      snprintf(key, sizeof(key), "%s:%d",
	       send_client->host, send_client->port);
      
      send_client->dpoint_queue.push_back(&send_client->shutdown_dpoint);
      map_.erase (key);

      //  std::cout << "inactive: " << key << std::endl;
    }
  }
};

#endif
