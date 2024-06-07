#ifndef SHAREDQUEUE_H
#define SHAREDQUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>

template <typename T>
class SharedQueue
{
public:
  SharedQueue();
  ~SharedQueue();
  
  T& front();
  void pop_front();
  
  void push_back(const T& item);
  void push_back(T&& item);
  
  int size();
  bool empty();
  
private:
  std::deque<T> queue_;
  std::mutex mutex_;
  std::condition_variable cond_;
}; 

template <typename T>
SharedQueue<T>::SharedQueue(){}

template <typename T>
SharedQueue<T>::~SharedQueue(){}

template <typename T>
T& SharedQueue<T>::front()
{
  std::unique_lock<std::mutex> mlock(mutex_);
  while (queue_.empty())
    {
      cond_.wait(mlock);
    }
  T& val = queue_.front();
  mlock.unlock();     // unlock before notificiation to minimize mutex con
  return val;
}

template <typename T>
void SharedQueue<T>::pop_front()
{
  std::unique_lock<std::mutex> mlock(mutex_);
  while (queue_.empty())
    {
      cond_.wait(mlock);
    }
  queue_.pop_front();
  mlock.unlock();     // unlock before notificiation to minimize mutex con
}     

template <typename T>
void SharedQueue<T>::push_back(const T& item)
{
  std::unique_lock<std::mutex> mlock(mutex_);
  queue_.push_back(item);
  mlock.unlock();     // unlock before notificiation to minimize mutex con
  cond_.notify_one(); // notify one waiting thread
}

template <typename T>
void SharedQueue<T>::push_back(T&& item)
{
  std::unique_lock<std::mutex> mlock(mutex_);
  queue_.push_back(std::move(item));
  mlock.unlock();     // unlock before notificiation to minimize mutex con
  cond_.notify_one(); // notify one waiting thread  
}

template <typename T>
int SharedQueue<T>::size()
{
  std::unique_lock<std::mutex> mlock(mutex_);
  int size = queue_.size();
  mlock.unlock();
  return size;
}

#endif
