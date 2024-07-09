/*
 * timer.cpp - simple use of the interval timer
 */

#include <iostream>
#include <chrono>
#include <functional>
#include <cstring>

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

#include "Timer.h"

#ifdef __linux__
void Timer::handler(int sig, siginfo_t *si, void *uc)
{
  /* Note: calling printf() from a signal handler is not safe
     (and should not be done in production programs), since
     printf() is not async-signal-safe; see signal-safety(7).
     Nevertheless, we use printf() here as a simple way of
     showing that the handler was called. */
  
  Timer *t;
  t = (Timer *) si->si_value.sival_ptr;
  
  t->expired = true;
  if (t->callback)
    t->callback(t->timer_id);
  if (t->nrepeats != -1 && t->expirations >= t->nrepeats) {
    bzero(&t->its, sizeof(t->its));
    timer_settime(t->timerid, 0, &t->its, NULL);
  }
  t->expirations++;
  
  //    printf("Caught signal %d from timer %d\n", sig, t->timer_id);
  //  print_siginfo(si);
  //    signal(sig, SIG_IGN);
}

Timer::Timer(int id): timer_id(id)
{
  sa.sa_flags = SA_SIGINFO;
  sa.sa_sigaction = Timer::handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGRTMIN, &sa, NULL);
  
  /* Block timer signal temporarily. */
  //    printf("Blocking signal %d\n", SIGRTMIN);
  sigemptyset(&mask);
  sigaddset(&mask, SIGRTMIN);
  sigprocmask(SIG_SETMASK, &mask, NULL);
  
  /* Create the timer. */
  sev.sigev_notify = SIGEV_SIGNAL;
  sev.sigev_signo = SIGRTMIN;
  sev.sigev_value.sival_ptr = this;
  timer_create(CLOCK_REALTIME, &sev, &timerid);
  
  expirations = 0;
  nrepeats = 0;
  expired = true;
}

Timer::~Timer()
{
  timer_delete(timerid);
}

void Timer::add_callback(std::function<void(int)> cb)
{
  callback = cb;
}

void Timer::arm_ms(int start_ms, int interval_ms, int loop)
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
  
  if (!interval_ms) nrepeats = 0;
  else nrepeats = loop;
  expired = true;
  expirations = 0;
}

void Timer::fire()
{
  timer_settime(timerid, 0, &its, NULL);
  sigprocmask(SIG_UNBLOCK, &mask, NULL);
  if (!its.it_value.tv_sec && !its.it_value.tv_nsec &&
      !its.it_interval.tv_sec && !its.it_interval.tv_nsec) {
    expired = true;
  }
  else
    expired = false;
}

void Timer::reset()
{
  
}
#elif defined(__APPLE__)
void Timer::add_callback(std::function<void(int)> cb)
{
  callback = cb;
}

void Timer::timer_handler(Timer *t, dispatch_source_t timer)
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

Timer::Timer(int id): timer_id(id)
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

Timer::~Timer()
{
  dispatch_source_cancel(timer);
}

void Timer::arm_ms(int start_ms, int interval_ms, int loop)
{
  if (!suspend_count) {
    dispatch_suspend(timer);
    suspend_count++;
  }
  dispatch_time_t start = dispatch_time(DISPATCH_TIME_NOW, (uint64_t) start_ms*1000000);
  dispatch_source_set_timer(timer, start, (uint64_t) interval_ms*1000000, 0);
  if (!interval_ms) nrepeats = 0;
  else nrepeats = loop;
  expired = true;
  expirations = 0;
}

void Timer::reset()
{
  if (!suspend_count) {
    dispatch_suspend(timer);
    suspend_count++;
  }
}
  

void Timer::fire()
{
  expired = false;
  dispatch_resume(timer);
  suspend_count--;
}

#else
struct itimerval it_val;

Timer::Timer(int id): timerid(id) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = &timer_handler;
  sigaction(SIGALRM, &sa, 
}


int Timer::arm_ms(int start_ms, int interval_ms, int nrepeats): nrepeats(nrepeats)
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

Timer::fire(void)
{
  return setitimer(ITIMER_REAL, &it_val, NULL);
}
#endif


