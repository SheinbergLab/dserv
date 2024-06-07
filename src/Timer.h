#ifndef TIMER_H
#define TIMER_H

#include <iostream>
#include <chrono>
#include <functional>

#ifndef _WIN32
#include <sys/time.h>		/* for setitimer */
#include <unistd.h>		/* for pause */
#endif

#include <signal.h>		/* for signal */
#include <stdlib.h>
#include <stdio.h>

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif

class Timer {

#ifdef __linux__
private:
  int iterations = 0;
  uint64_t expiration_count = 0;
  std::function<void(int)> callback = nullptr;
  int nrepeats = 0, expirations;

  int                timer_id;
  timer_t            timerid;
  sigset_t           mask;
  long long          freq_nanosecs;
  struct sigevent    sev;
  struct sigaction   sa;
  struct itimerspec  its;

  static void handler(int sig, siginfo_t *si, void *uc);

public:
  bool expired;

  Timer(int id);
  ~Timer();
  void add_callback(std::function<void(int)> cb);
  void arm_ms(int start_ms, int interval_ms = 0, int loop = -1);
  void fire();
  void reset();
#elif defined(__APPLE__)
 private:
  int timer_id;
  dispatch_queue_t queue;
  dispatch_source_t timer;
  std::function<void(int)> callback = nullptr;
  int nrepeats = 0, expirations;
  std::atomic<int> suspend_count;
  
 public:
  bool expired;
  void add_callback(std::function<void(int)> cb);
  static void timer_handler(Timer *t, dispatch_source_t timer);
  Timer(int id = 0);
  ~Timer();
  void arm_ms(int start_ms, int interval_ms = 0, int loop = -1);
  void reset();
  void fire();
#else
private:
  struct itimerval it_val;	/* for setting itimer */

  int iterations = 0;
  bool limited_repeats = false;
  uint64_t expirations = 0, expiration_count = 0;

 public:
  Timer();
  int arm_ms(int start_ms, int interval_ms, int nrepeats = -1);
  int fire(void);
#endif
};

#endif
