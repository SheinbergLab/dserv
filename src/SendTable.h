#ifndef SENDTABLE_H
#define SENDTABLE_H

#include <unordered_map>
#include <memory>
#include <mutex>

#include "Datapoint.h"
#include "DatapointTable.h"
#include "MatchDict.h"
#include "LogMatchDict.h"
#include "TriggerDict.h"
#include "SendClient.h"

/*
 * SendTable
 *
 *  Registry of active send clients, keyed by "host:port" (socket
 * clients) or a generated id (queue clients).  Clients are held by
 * shared_ptr — the client's own thread holds another reference — so
 * a pointer handed out by get() can never dangle; at worst it names
 * a client that has already been shut down, on which operations are
 * harmless no-ops.
 */
class SendTable
{
 private:
  std::unordered_map<std::string, std::shared_ptr<SendClient>> map_;
  std::mutex mutex_;

 public:
  SendTable() {};
  ~SendTable() { shutdown_clients(); }

  void insert(std::string key, std::shared_ptr<SendClient> s)
    {
      std::lock_guard<std::mutex> mlock(mutex_);
      map_[key] = s;
    }

  std::shared_ptr<SendClient> get(std::string key)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    auto iter = map_.find(key);
    if (iter != map_.end()) return iter->second;
    return nullptr;
  }

  /*
   * atomically remove a client from the table and queue its
   * shutdown.  Doing both under one lock means two racing removals
   * (or a removal racing the inactive-client reaping in
   * forward_dpoint) cannot both queue a shutdown for the same client.
   */
  int remove_and_shutdown(std::string key)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    auto iter = map_.find(key);
    if (iter == map_.end()) return 0;

    std::shared_ptr<SendClient> send_client = iter->second;
    map_.erase(iter);
    if (send_client)
      send_client->dpoint_queue.push_back(&send_client->shutdown_dpoint);
    return 1;
  }

  void shutdown_clients(void)
  {
    std::lock_guard<std::mutex> mlock(mutex_);
    for (auto const& [key, send_client] : map_) {
      if (send_client)
	send_client->dpoint_queue.push_back(&send_client->shutdown_dpoint);
    }
    map_.clear();
    /* each client thread still holds its own shared_ptr, so the
       objects stay alive until those threads exit */
  }

  // Return diagnostic info: list of {key active queue_size matches}
  std::vector<std::tuple<std::string, int, size_t, std::string>> get_client_info()
  {
    std::vector<std::tuple<std::string, int, size_t, std::string>> result;
    std::lock_guard<std::mutex> mlock(mutex_);
    for (auto const& [key, send_client] : map_) {
      if (send_client) {
	result.emplace_back(
	  key,
	  send_client->active,
	  send_client->dpoint_queue.size(),
	  send_client->matches.to_string()
	);
      }
    }
    return result;
  }

  void forward_dpoint(ds_datapoint_t *dpoint)
  {
    std::vector<std::shared_ptr<SendClient>> close_vec;

    std::lock_guard<std::mutex> mlock(mutex_);
    for (auto const& it : map_) {
      std::shared_ptr<SendClient> send_client = it.second;
      if (!send_client->active) {
	close_vec.push_back(send_client);
      }
      else if (send_client->matches.is_match(dpoint->varname)) {
	ds_datapoint_t *dp = dpoint_copy(dpoint);
	send_client->dpoint_queue.push_back(dp);
      }
    }

    /* reap inactive clients by their registration key (still under
       the same lock, so this cannot race another removal) */
    for (auto const& send_client : close_vec) {
      map_.erase(send_client->key);
      send_client->dpoint_queue.push_back(&send_client->shutdown_dpoint);
    }
  }
};

#endif
