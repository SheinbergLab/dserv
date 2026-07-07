#ifndef LOGTABLE_H
#define LOGTABLE_H

#include <unordered_map>
#include <mutex>
#include <cstdint>

class LogClient;

class LogTable
{
 private:
  std::unordered_map<std::string, LogClient *> map_;
  std::mutex mutex_;

public:
  LogTable() {};
  ~LogTable() { shutdown_clients(); }

  void insert(std::string key, LogClient *s);
  void remove(std::string key);
  void clear(std::string key);
  bool find(std::string key, LogClient **s);
  std::string clients(void);
  void forward_dpoint(ds_datapoint_t *dpoint);
  void control_client(std::string key, uint32_t flags);
  int add_match(std::string key, const char *match,
		int every, int obs, int bufsize);
  void shutdown_clients(void);

};

#endif
