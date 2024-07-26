#ifndef LOGTABLE_H
#define LOGTABLE_H

#include <unordered_map>
#include <mutex>

class LogClient;

class LogTable
{
 private:
  std::unordered_map<std::string, LogClient *> map_;
  std::mutex mutex_;
  
public:  
  void insert(std::string key, LogClient *s);
  void remove(std::string key);
  void clear(std::string key);
  bool find(std::string key, LogClient **s);
  std::string clients(void);
  void forward_dpoint(ds_datapoint_t *dpoint);
};

#endif
