#ifndef SHAREDQUEUE_H
#define SHAREDQUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>

/*
 * SharedQueue
 *
 *  Multi-producer, SINGLE-consumer blocking queue.  The consumer
 * idiom is front() (blocks until an item is available, returns a
 * copy) followed by pop_front(); that two-call sequence is only
 * atomic because exactly one thread ever consumes a given queue.
 */
template <typename T>
class SharedQueue
{
public:
  SharedQueue();
  ~SharedQueue();

  T front();
  void pop_front();

  void push_back(const T& item);
  void push_back(T&& item);

  int size();

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
T SharedQueue<T>::front()
{
  std::unique_lock<std::mutex> mlock(mutex_);
  while (queue_.empty())
    {
      cond_.wait(mlock);
    }
  // Return a COPY made while the lock is held.
  return queue_.front();
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

/*
 * push_back notifies while still holding the lock.  With
 * unlock-then-notify, a consumer could wake on the unlock, pop the
 * final item, and destroy the queue before our notify_one() runs —
 * leaving the notify touching a freed condition variable.  Notifying
 * under the lock makes the unlock the producer's last touch, so a
 * consumer that frees the queue after popping an end-of-stream item
 * is safe.
 */
template <typename T>
void SharedQueue<T>::push_back(const T& item)
{
  std::unique_lock<std::mutex> mlock(mutex_);
  queue_.push_back(item);
  cond_.notify_one(); // notify one waiting thread
}

template <typename T>
void SharedQueue<T>::push_back(T&& item)
{
  std::unique_lock<std::mutex> mlock(mutex_);
  queue_.push_back(std::move(item));
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
