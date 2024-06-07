//
// Created by David Sheinberg on 2019-04-12.
//

#ifndef DSERV_TIMERFD_HPP
#define DSERV_TIMERFD_HPP

#ifdef __linux__
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>        /* Definition of uint64_t */


class TimerFD {

private:
  int timerfd;
  uint64_t expirations = 0;
  int iterations = 0;
  bool limited_repeats = false;
  uint64_t expiration_count = 0;
  struct itimerspec new_value;
  void * userData = nullptr;
  
public:
  TimerFD()
  {
    timerfd = timerfd_create(CLOCK_REALTIME, 0);
  }

  TimerFD(int milliseconds)
  {
    struct timespec now;
    ssize_t s;
    
    bzero(&new_value, sizeof(new_value));
    
    if (clock_gettime(CLOCK_REALTIME, &now) == -1) {
      std::cout << "clock_gettime" << std::endl;
      return;
    }
    
    new_value.it_value.tv_sec = now.tv_sec;
    new_value.it_value.tv_nsec = now.tv_nsec;
    
    int ms = milliseconds%1000;
    int sec = milliseconds/1000;
    
    new_value.it_interval.tv_sec = sec;
    new_value.it_interval.tv_nsec = ms*1000000;
    
    timerfd = timerfd_create(CLOCK_REALTIME, 0);
    if (timerfd == -1) {
      return;
    }
    
    timerfd_settime(timerfd, TFD_TIMER_ABSTIME, &new_value, NULL);
  }
  
  ~TimerFD()
  {
    close(timerfd);
    timerfd = -1;
    // need to remove item from poll list
  }
  

  int arm_ms(int start_ms, int interval_ms, int loop_count = -1)
  {
    int ms;
    int sec;
    
    bzero(&new_value, sizeof(new_value));
    
    // first fire
    ms = start_ms%1000;
    sec = start_ms/1000;
    new_value.it_value.tv_sec = sec;
    new_value.it_value.tv_nsec = ms*1000000;

    // repeat interval
    ms = interval_ms%1000;
    sec = interval_ms/1000;
    new_value.it_interval.tv_sec = sec;
    new_value.it_interval.tv_nsec = ms*1000000;

    iterations = expirations = 0;
    
    if (loop_count > 0) {
      limited_repeats = true;
      expiration_count = loop_count;
    }
    return 1;
  }

  int fire(void)
  {
    return (timerfd_settime(timerfd, 0, &new_value, NULL));
  }
  
  int getfd(void)
  {
    return timerfd;
  }
  
  int getItemId(void)
  {
    return timer_item_id;
  }

  int disarm(void)
  {
    // inactivate timer by setting counts to 0
    new_value.it_value.tv_sec = 0;
    new_value.it_value.tv_nsec = 0;
    new_value.it_interval.tv_sec = 0;
    new_value.it_interval.tv_nsec = 0;
    return (timerfd_settime(timerfd, TFD_TIMER_ABSTIME, &new_value, NULL));
  }
  
  bool process(void)
  {
    int res = read(timerfd, &expirations, sizeof(expirations));
    if (res < 0) return false;
    iterations++;
    if (limited_repeats && iterations > expiration_count) {
      disarm();
      return false;
    }
    return true;
  }
  
  void shutdown(void)
  {
    return;
  }
};

#else

#include <chrono>
#include <numeric>
#include <string.h>

/// Tick Thread
struct period_info {
    struct timespec next_period;
    long period_ns;
};

class TickThread
{
private:
    struct timespec request;
    int usec_offset = 100;
    int nsec_offset = usec_offset*1000;
    struct period_info pinfo;

public:
    bool m_bDone;


    TickThread(zmq::context_t *context, const char *endpoint, int ms)
    {
        m_bDone = false;
        request.tv_sec  = 0;
        request.tv_nsec = 1000000*ms-nsec_offset;

        // Check for errors here
        tick_socket = new zmq::socket_t(*context, ZMQ_PAIR);
        tick_socket->connect (endpoint);
    }

    ~TickThread()
    {
        delete tick_socket;
    }

    void close() { m_bDone = true; }

    void periodic_init()
    {
#ifndef _WIN32
      pinfo.period_ns = 1000000;
      clock_gettime(CLOCK_MONOTONIC, &(pinfo.next_period));
#endif
    }

    void inc_period()
    {
        pinfo.next_period.tv_nsec += pinfo.period_ns;

        while (pinfo.next_period.tv_nsec >= 1000000000) {
            /* timespec nsec overflow */
            pinfo.next_period.tv_sec++;
            pinfo.next_period.tv_nsec -= 1000000000;
        }
    }

    void wait_rest_of_period()
    {
        inc_period();

        /* for simplicity, ignoring possibilities of signal wakes */
#ifdef __APPLE__
        nanosleep(&request, NULL);
#elif _WIN32
	Sleep(100);
#else
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &pinfo.next_period, NULL);
#endif
    }

    void run() {
        periodic_init();
        Msg closemsg(Msg::DS_CLOSE_MESSAGE);
        Msg tickmsg(Msg::DS_SCHEDULER_TICK);
        while (!m_bDone) {
            zmq::message_t req(sizeof(tickmsg));
            memcpy(req.data(), &tickmsg, sizeof(tickmsg));
            tick_socket->send(req);
            wait_rest_of_period();
        }
    }
};


class TimerFD {

private:
    int timerfd;
    int timer_item_id;
    int iterations = 0;
    TickThread *tickThreadPtr;
    std::thread *tick_thread;

public:
    TimerFD(int milliseconds)
    {
      // The tickThread fires a tick message every wake_every ms
      tickThreadPtr = new TickThread(server->getContext(), "inproc://tick", milliseconds);
      tick_thread = new std::thread(&TickThread::run, tickThreadPtr);
    }

    ~TimerFD()
    {
        delete tickThreadPtr;
        delete tick_thread;
        delete tick_socket;
        //  remove from zmq poll item list here
    }

    int getfd(void)
    {
        return -1;
    }

    int getItemId(void)
    {
        return timer_item_id;
    }

    int process(void)
    {
        tick_socket->recv(&request);
        iterations++;
        return iterations;
    }

    void shutdown(void)
    {
        tickThreadPtr->m_bDone = true;
        tick_thread->join();
    }
};


#endif



#endif //DSERV_TIMERFD_HPP
