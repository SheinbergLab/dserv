#ifndef TCLSERVER_H
#define TCLSERVER_H

#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <queue>

#include <stdlib.h>

#ifndef _WIN32
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#endif

#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/select.h>
#include <fcntl.h>

#ifdef __QNX__
#include <sys/socket.h>
#endif

#include <jim.h>
#include "jiminterp.hpp"

class Tclserver
{
  int socket_fd;
  int tcpport;
  std::thread net_thread;
  std::thread process_thread;

  JimInterp interp;
  Dataserver *ds;
  
  std::atomic<bool> m_bDone;

  // for client requests
  SharedQueue<client_request_t *> queue;

public:
    
  static void
  tcp_client_process(int sock,
		     SharedQueue<client_request_t *> *queue);
  
  Tclserver(char *name, Dataserver *dserv, int port = 2570)
  {
    m_bDone = false;
    tcpport = port;
    ds = dserv;

    net_thread = std::thread(&Tclserver::start_tcp_server, this);
    process_thread = std::thread(&Tclserver::process_requests, this);
  }

  ~Tclserver()
  {
    shutdown();
    net_thread.detach();
    process_thread.detach();
  }

  void shutdown(void)
  {
    m_bDone = true;
  }

  bool isDone()
  {
    return m_bDone;
  }

  void start_tcp_server(void)
  {
    struct sockaddr_in address;
    struct sockaddr client_address;
    socklen_t client_address_len = sizeof(client_address);
    int new_socket_fd;		// client socket
    int on = 1;
    
    //    std::cout << "opening server on port " << std::to_string(tcpport) << std::endl;

    /* Initialise IPv4 address. */
    memset(&address, 0, sizeof(struct sockaddr_in));
    address.sin_family = AF_INET;
    address.sin_port = htons(tcpport);
    address.sin_addr.s_addr = INADDR_ANY;

        
    /* Create TCP socket. */
    if ((socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
      perror("socket");
      return;
    }

    /* Allow this server to reuse the port immediately */
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  
    /* Bind address to socket. */
    if (bind(socket_fd, (const struct sockaddr *) &address,
	     sizeof (struct sockaddr)) == -1) {
      perror("bind");
      return;
    }

    /* Listen on socket. */
    if (listen(socket_fd, 20) == -1) {
      perror("listen");
      return;
    }

    while (1) {
      /* Accept connection to client. */
      new_socket_fd = accept(socket_fd, &client_address, &client_address_len);
      if (new_socket_fd == -1) {
	perror("accept");
	continue;
      }
      
      setsockopt(new_socket_fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

      // Create a thread and transfer the new stream to it.
      std::thread thr(tcp_client_process, new_socket_fd, &queue);
      thr.detach();
    }
  }
  
  int process_requests(void) {
    int retcode;
    client_request_t *req;

    /* process until receive a message saying we are done */
    while (!m_bDone) {
      
      req = queue.front();
      queue.pop_front();
      const char *script = req->script.c_str();
      
      std::string rcstring;
      retcode = interp.eval(script, rcstring);
      if (retcode == JIM_OK) {
	req->rqueue->push_back(rcstring);
      }
      else {
	req->rqueue->push_back("!TCL_ERROR "+rcstring);
	//      std::cout << "Error: " + std::string(rcstr) << std::endl;
	
      }
    }
    return 0;
  }
  
  int queue_size(void)
  {
    return queue.size();
  }
  
  std::string eval(std::string script)
  {
    static SharedQueue<std::string> rqueue;
    client_request_t client_request;
    client_request.rqueue = &rqueue;
    client_request.script = script;
    
    // std::cout << "TCL Request: " << std::string(buf, n) << std::endl;
    
    queue.push_back(&client_request);
    
    //      queue->push_back(std::string(buf, n));

    /* rqueue will be available after command has been processed */
    std::string s(client_request.rqueue->front());
    client_request.rqueue->pop_front();

    return s;
  }
    
};

void Tclserver::tcp_client_process(int sockfd,
				   SharedQueue<client_request_t *> *queue)
{
  // fix this...
  char buf[16384];
  double start;
  int rval;
  
  // each client has its own request structure and reply queue
  SharedQueue<std::string> rqueue;
  client_request_t client_request;
  client_request.rqueue = &rqueue;
  
  while ((rval = read(sockfd, buf, sizeof(buf))) > 0) {
    client_request.script = std::string(buf, rval);
    
    //std::cout << "TCL Request: " << std::string(buf, res.value()) << std::endl;
    
    queue->push_back(&client_request);
    
    //      queue->push_back(std::string(buf, n));
    
    /* rqueue will be available after command has been processed */
    std::string s(client_request.rqueue->front());
    client_request.rqueue->pop_front();
    
    //std::cout << "TCL Result: " << s << std::endl;
    
    // Add a newline, and send the buffer including the null termination
    s = s+"\n";
    write(sockfd, s.c_str(), s.size());
  }
  // std::cout << "Connection closed from " << sock.peer_address() << std::endl;
  close(sockfd);
}

#endif  // TCLSERVER_H
