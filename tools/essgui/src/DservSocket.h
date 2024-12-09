#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <functional>
#include <condition_variable>
#include <chrono>
#include <queue>

#include <stdlib.h>

#ifndef _WIN32
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#endif

#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/select.h>
#include <fcntl.h>

void process_dpoint_cb(void *cbdata);

class DservSocket
{
  int tcpport;

  std::atomic<bool> done;
  std::atomic<bool> m_bDone;

public:
  int dsport;
  char myIP[16];

  std::string dsaddr;
  
  std::mutex mutex;
  std::condition_variable cond;
    
  static void
  ds_client_process(int sock);
  
  DservSocket()
  {
    m_bDone = false;
    dsport = 0;
  }

  ~DservSocket()
  {

  }

  void shutdown(void)
  {
    m_bDone = true;
  }

  bool isDone()
  {
    return m_bDone;
  }

  std::thread start_server(void)
  {
    std::unique_lock<std::mutex> mlock(mutex);
    std::thread dsnet_thread(&DservSocket::start_ds_tcp_server, this);

    cond.wait(mlock);
    mlock.unlock();
    
    return dsnet_thread;
  }

  void
  start_ds_tcp_server(void)
  {
    struct sockaddr_in address;
    struct sockaddr client_address;
    socklen_t client_address_len = sizeof(client_address);
    int socket_fd, new_socket_fd;
    int on = 1;
    
    /* Initialise IPv4 address. */
    memset(&address, 0, sizeof(struct sockaddr_in));
    address.sin_family = AF_INET;
    address.sin_port = htons(dsport);
    address.sin_addr.s_addr = INADDR_ANY;
    
    /* Create TCP socket. */
    if ((socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
      perror("socket");
      return;
    }
    
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

    /* Find assigned port. */
    socklen_t len = sizeof(address);
    if (getsockname(socket_fd, (struct sockaddr *)&address, &len) != -1) {
      dsport = ntohs(address.sin_port);
    }

    /* let main process know we are ready to receive */
    cond.notify_one();    
  
    //    std::cout << "listening on port " << std::to_string(dsport) << std::endl;
    
    while (!m_bDone) {

      /* Accept connection to client. */
      new_socket_fd = accept(socket_fd, &client_address, &client_address_len);
      
      if (new_socket_fd == -1) {
	perror("accept");
	continue;
      }
      
      setsockopt(new_socket_fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
      
      // Create a thread and transfer the new stream to it.
      std::thread thr(ds_client_process, std::move(new_socket_fd));
      thr.detach();
    }
    
    close(socket_fd);
  }

  int
  processDSCommands(void) {
    int n = 0;
    int retcode;
    return n;
  }
  
  int
  processReplies(void)
  {
    int n = 0;
    return n;
  }

  int client_socket(const char *host, int port)
  {
    int client_fd;
    int status;
    struct sockaddr_in serv_addr;
    if ((client_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("\n Socket creation error \n");
        return -1;
    }
 
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
 
    if (inet_pton(AF_INET, host, &serv_addr.sin_addr) <= 0) {
      return -1;
    }
 
    if ((status = connect(client_fd, (struct sockaddr*) &serv_addr,
			  sizeof(serv_addr))) < 0) {
      return -1;
    }
    return client_fd;
  }

  int ds_command(int sock, std::string cmd, std::string &retstr)
  {
    int len = cmd.length();
    int nwritten = send(sock, cmd.c_str(), cmd.length(), 0);
    if (len != nwritten) return 0;

    retstr.resize(4096);
    ssize_t n = read(sock, &retstr[0], retstr.length());
    if (!n) {
      return 0;
    }
    return 1;
  }

  int get_keys(std::string host, std::string &keys, int port=4620)
  {
    int sock = client_socket(host.c_str(), port);
    if (sock <= 0) return sock;

    std::string s("%getkeys");
    ds_command(sock, s, keys);
    close(sock);

    int result = std::stoi(keys.substr(0,2));
    keys = keys.substr(2,keys.length());

    return result;
  }
  

  int get(std::string host, std::string key, std::string &value, int port=4620)
  {
    std::string s("%get ");
    //    std::cout << s+key << std::endl;
    
    int sock = client_socket(host.c_str(), port);
    if (sock <= 0) return sock;

    ds_command(sock, s+key, value);

    close(sock);
    return 1;
  }
  

  int reg(std::string host, int port=4620)
  {
    unsigned int myPort;    
    struct sockaddr_in local_sin;
    socklen_t local_sinlen = sizeof(local_sin);
    
    int sock = client_socket(host.c_str(), port);
    if (sock <= 0) return sock;

    /* get the local address of the connected socket */
    getsockname(sock, (struct sockaddr*) &local_sin, &local_sinlen);
    inet_ntop(AF_INET, &local_sin.sin_addr, myIP, sizeof(myIP));
    myPort = ntohs(local_sin.sin_port);
 
    std::string s = std::string("%reg ") + myIP + " " + std::to_string(dsport) + " 2";

    std::string retstr;
    ds_command(sock, s, retstr);

    //    std::cout << "reg: " << s << " -> " << retstr;

    close(sock);
    
    return (stoi(retstr));
  }
  
  int unreg(std::string host, int port=4620)
  {
    unsigned int myPort;    
    struct sockaddr_in local_sin;
    socklen_t local_sinlen = sizeof(local_sin);
    
    int sock = client_socket(host.c_str(), port);
    if (sock <= 0) return sock;

    /* get the local address of the connected socket */
    getsockname(sock, (struct sockaddr*) &local_sin, &local_sinlen);
    inet_ntop(AF_INET, &local_sin.sin_addr, myIP, sizeof(myIP));
    myPort = ntohs(local_sin.sin_port);
 
    std::string s = std::string("%unreg ") +
      myIP + " " +
      std::to_string(dsport);

    std::string retstr;
    ds_command(sock, s, retstr);
    
    close(sock);
    
    return (stoi(retstr));
  }
  

  int add_match(std::string host, std::string matchstr, int every=1, int port=4620)
  {
    int sock = client_socket(host.c_str(), port);
    if (sock <= 0) return sock;

    std::string s("%match ");
    s += myIP;
    s += " ";
    s += std::to_string(dsport);
    s += " ";
    s += matchstr;
    s += " ";
    s += std::to_string(every);

    std::string retstr;
    ds_command(sock, s, retstr);
    //    std::cout << "addmatch: " << s << " -> " << retstr;
    
    close(sock);
    return 1;
  }
  
  int remove_match(std::string host, std::string matchstr, int port=4620)
  {
    int sock = client_socket(host.c_str(), port);
    if (sock <= 0) return sock;

    std::string s("%unmatch ");
    s += myIP;
    s += " ";
    s += std::to_string(dsport);
    s += " ";
    s += matchstr;

    std::string retstr;
    ds_command(sock, s, retstr);
    //    std::cout << "unmatch: " << s << " -> " << retstr;
    
    close(sock);
    return 1;
  }

  int touch(std::string host, std::string var, int port=4620)
  {
    int sock = client_socket(host.c_str(), port);
    if (sock <= 0) return sock;

    std::string s("%touch ");
    s += var;

    std::string retstr;
    ds_command(sock, s, retstr);
    //std::cout << "touch: " << s << " -> " << retstr;
    
    close(sock);
    return 1;
  }

  int dscmd(std::string host, std::string cmd, std::string &rstr, int port=2570)
  {
    int sock = client_socket(host.c_str(), port);
    if (sock <= 0) return sock;

    std::string retstr;
    int result = ds_command(sock, cmd, rstr);

    rstr.erase(std::remove(rstr.begin(), rstr.end(), '\n'), rstr.cend());
    
    close(sock);
    return result;
  }

  int esscmd(std::string host, std::string cmd, std::string &rstr) {
    return dscmd(host, cmd, rstr, 2570);
  }
  int dbcmd(std::string host, std::string cmd, std::string &rstr) {
    return dscmd(host, cmd, rstr, 2571);
  }
  int dservcmd(std::string host, std::string cmd, std::string &rstr) {
    return dscmd(host, cmd, rstr, 4620);
  }
  

  
};

void
DservSocket::ds_client_process(int sockfd)
{
  // fix this...
  char buf[16384];
  int rval;

  //  std::cout << "starting tcp_client_process: " << std::to_string(sockfd) << std::endl;

  std::string dpoint_str;  

  while ((rval = read(sockfd, buf, sizeof(buf))) > 0) {
    for (int i = 0; i < rval; i++) {
      char c = buf[i];
      if (c == '\n') {

	if (dpoint_str.length() > 0) {

	  Fl::lock();      // acquire the lock
	  Fl::unlock();    // release the lock; allow other threads to access FLTK again
	  //	  Fl::awake((void *) strdup(dpoint_str.c_str()));
	  Fl::awake(process_dpoint_cb, (void *) strdup(dpoint_str.c_str())); 
	}
	dpoint_str = "";
      }
      else {
	dpoint_str += c;
      }
    }
  }
  // std::cout << "Connection closed from " << sock.peer_address() << std::endl;
  close(sockfd);
}
