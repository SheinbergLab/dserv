#ifndef SHAREDMAP_H
#define SHAREDMAP_H

#include <unordered_map>
#include <mutex>

template <typename T>
class SharedMap
{
public:
  SharedMap();
  ~SharedMap();

  void insert(std::string, const T& item);
  T find(std::string);
  void free_contents();
  
private:
  
  std::unordered_map<std::string, T> map_;
  std::mutex mutex_;
}; 

template <typename T>
SharedMap<T>::SharedMap(){}

template <typename T>
SharedMap<T>::~SharedMap(){}

template <typename T>
void SharedMap<T>::insert(std::string key, const T& item)
{
  std::lock_guard<std::mutex> mlock(mutex_);
  map_[key] = item;
}

template <typename T>
T SharedMap<T>::find(std::string key)
{
  std::lock_guard<std::mutex> mlock(mutex_);
  auto iter = map_.find(key);

  if(iter != map_.end()) {
    return iter->second;
  }
  return nullptr;
}

template <typename T>
T SharedMap<T>::free_contents(void)
{
  std::lock_guard<std::mutex> mlock(mutex_);
  for (const auto & [ key, value ] : map_) {
    free(value);
  }
}


#endif

