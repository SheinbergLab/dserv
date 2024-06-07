/*
 * setitimer.c - simple use of the interval timer
 */

#include <iostream>
#include <chrono>
#include <functional>

#include <sys/time.h>		/* for setitimer */
#include <unistd.h>		/* for pause */
#include <signal.h>		/* for signal */
#include <stdlib.h>
#include <stdio.h>

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif

class Timer {

#ifdef __linux__
private:
  int timerfd;
  uint64_t expirations = 0;
  int iterations = 0;
  bool limited_repeats = false;
  uint64_t expiration_count = 0;
  std::function<void(int)> callback = nullptr;
  int nrepeats = 0, expirations;

  timer_t            timerid;
  sigset_t           mask;
  long long          freq_nanosecs;
  struct sigevent    sev;
  struct sigaction   sa;
  struct itimerspec  its;
  	   
  static void
    handler(int sig, siginfo_t *si, void *uc)
  {
    /* Note: calling printf() from a signal handler is not safe
       (and should not be done in production programs), since
       printf() is not async-signal-safe; see signal-safety(7).
       Nevertheless, we use printf() here as a simple way of
       showing that the handler was called. */
    
    printf("Caught signal %d\n", sig);
    //  print_siginfo(si);
    signal(sig, SIG_IGN);
  }
  
public:
  Timer()
  {
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = Timer::handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGRTMIN, &sa, NULL);

    /* Block timer signal temporarily. */
    printf("Blocking signal %d\n", SIGRTMIN);
    sigemptyset(&mask);
    sigaddset(&mask, SIGRTMIN);
    sigprocmask(SIG_SETMASK, &mask, NULL);

    /* Create the timer. */
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGRTMIN;
    sev.sigev_value.sival_ptr = &timerid;
    timer_create(CLOCK_REALTIME, &sev, &timerid);

    printf("timer ID is %#jx\n", (uintmax_t) timerid);
  }

  ~Timer()
  {
    timer_delete(timerid);
  }


  void arm_ms(int start_ms, int interval_ms, int loop = -1)
  {
    int ms, sec;
    ms = start_ms%1000;
    sec = start_ms/1000;
    its.it_value.tv_sec = sec;
    its.it_value.tv_nsec = ms*1000000;
    
    ms = start_ms%1000;
    sec = start_ms/1000;
    its.it_interval.tv_sec = sec;
    its.it_interval.tv_nsec = ms*1000000;
    
  }

  void fire()
  {
    timer_settime(timerid, 0, &its, NULL);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
  }

  void reset()
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
  void add_callback(std::function<void(int)> cb)
  {
    callback = cb;
  }
  
  static void timer_handler(Timer *t, dispatch_source_t timer)
  {
    t->expired = true;
    if (t->callback)
      t->callback(t->timer_id);
    if (t->nrepeats != -1 && t->expirations >= t->nrepeats) {
      dispatch_suspend(t->timer);
      t->suspend_count++;
    }
    t->expirations++;
  }
  
 Timer(int id = 0): timer_id(id)
    {
      queue = dispatch_queue_create("timerQueue", 0);
      
      // Create dispatch timer source
      timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
      
      // Set block for dispatch source when catched events
      dispatch_source_set_event_handler(timer, ^{timer_handler(this, timer);});

      // Set block for dispatch source when cancel source
      dispatch_source_set_cancel_handler(timer, ^{
	  dispatch_release(timer);
	  dispatch_release(queue);
	});
      expirations = 0;
      nrepeats = 0;
      expired = true;
      suspend_count = 1;
    }
  
  ~Timer()
    {
      dispatch_source_cancel(timer);
    }

  void arm_ms(int start_ms, int interval_ms = 0, int loop = -1)
  {
    if (!suspend_count) {
      dispatch_suspend(timer);
      suspend_count++;
    }
    dispatch_time_t start = dispatch_time(DISPATCH_TIME_NOW, start_ms*1000000);
    dispatch_source_set_timer(timer, start, interval_ms*1000000, 0);
    if (!interval_ms) nrepeats = 0;
    else nrepeats = loop;
    expired = true;
    expirations = 0;
  }

  void reset()
  {
    if (!suspend_count) {
      dispatch_suspend(timer);
      suspend_count++;
    }
  }
  

  void fire()
  {
    expired = false;
    dispatch_resume(timer);
    suspend_count--;
  }
  
#else
private:
  struct itimerval it_val;	/* for setting itimer */

  int iterations = 0;
  bool limited_repeats = false;
  uint64_t expirations = 0, expiration_count = 0;

 public:
  Timer() {
  }


  int arm_ms(int start_ms, int interval_ms, int nrepeats = -1): nrepeats(nrepeats)
  {
    int ms;
    int sec;

    // first fire
    ms = start_ms%1000;
    sec = start_ms/1000;
    it_val.it_value.tv_sec = sec;
    it_val.it_value.tv_usec = ms*1000;

    // repeat interval
    ms = interval_ms%1000;
    sec = interval_ms/1000;
    it_val.it_interval.tv_sec = sec;
    it_val.it_interval.tv_usec = ms*1000;

    iterations = expirations = 0;

    return 1;
  }

  int fire(void)
  {
    return setitimer(ITIMER_REAL, &it_val, NULL);
  }
#endif
};

