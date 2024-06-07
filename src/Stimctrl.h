#ifndef STIMCTRL_H
#define STIMCTRL_H

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/ioctl.h>

class Stimctrl {
 public:
  std::string host;
  int port;
  static const int STIM_PORT = 4610;
  static const int SOCK_BUF_SIZE = 65536 ;
private:
  int rmt_socket = -1;

public:
  Stimctrl()  {}
  
  int socket_open(void)
  {
    const char *name = host.c_str();
    int param;
    struct sockaddr_in server;
    struct hostent *hp;
    
    /* Create socket */
    rmt_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (rmt_socket < 0) {
      return -1;
    }
    server.sin_family = AF_INET;
    hp = gethostbyname(name);
    if (hp == 0) {
      return -2;
    }
    memcpy(&server.sin_addr, hp->h_addr, hp->h_length);
    server.sin_port = htons(port);
    
    if (connect(rmt_socket, (struct sockaddr *)&server, sizeof(server)) < 0) {
      return -3;
    }
    param = 1;
    setsockopt(rmt_socket, SOL_SOCKET, SO_REUSEADDR, &param, sizeof(param));
    socket_flush();

    return rmt_socket;
  }
  
  void socket_flush()
  {
    static char buf[64];
    int flag = 1;
    int n;

    if (rmt_socket < 0) return;
    
    if (ioctl(rmt_socket, FIONBIO, &flag) < 0) {
      return;
    }
    do {
      n = read(rmt_socket, buf, sizeof(buf));
    } while (n >= 0);
    
    flag = 0;
    if (ioctl(rmt_socket, FIONBIO, &flag) < 0) {
      return;
    }
  }
  
  /*****************************************************************************/
  /****************************** SOCKET_CLOSE**********************************/
  /*****************************************************************************/
  /*
   * Purpose:	Close socket
   * Input:	socket id
   * Process:	close socket
   * Output:	none
   */
  /*****************************************************************************/
  int socket_close(void)
  {
    if (rmt_socket >= 0) {
      close(rmt_socket);
      rmt_socket = -1;
    }
    return 1;
  }
  
  /*****************************************************************************/
  /***************************** SOCKET_WRITE **********************************/
  /*****************************************************************************/
  /*
   * Purpose:	Write data over the socket
   * Input:	socket id, message, length of message
   * Process:	write message to socket
   * Output:	returns 1 on success, 0 on failure
   */
  /*****************************************************************************/
  int socket_write(char *message, int nbytes)
  {
    if (rmt_socket < 0) return 0;
    if (write(rmt_socket, message, nbytes) < 0) {
      perror("writing socket");
      return 0;
    }
    return 1;
  }
  
  /*****************************************************************************/
  /***************************** SOCKET_READ ***********************************/
  /*****************************************************************************/
  /*
   * Purpose:	Read data from the socket
   * Input:	socket id, char **message, message length *
   * Process:	read message from socket
   * Output:	returns 1 on success, 0 on failure
   * Limitations: buf is currently limited to 4096 characters
   */
  /*****************************************************************************/
  int socket_read(char **message, int *nbytes)
  {
    static char buf[SOCK_BUF_SIZE];
    int n;
    if ((n = read(rmt_socket, buf, sizeof(buf))) < 0) {
      perror("reading stream socket");
      if (message) *message = NULL;
      if (nbytes) *nbytes = 0;
      return 0;
    }
    
    if (message) *message = buf;
    if (nbytes) *nbytes = n;
    return 1;
  }
  
  /*****************************************************************************/
  /****************************** SOCKET_SEND **********************************/
  /*****************************************************************************/
  /*
   * Purpose:	Write data over the socket and wait for reply
   * Input:	socket id, message, length of message
   * Process:	write message to socket and read reply
   * Output:	returns 1 on success, 0 on failure
   */
  /*****************************************************************************/
  int socket_send(char *sbuf, int sbytes, char **rbuf, int *rbytes)
  {
    int status;
    status = socket_write(sbuf, sbytes);
    if (!status) return 0;
    status = socket_read(rbuf, rbytes);
    return status;
  }
  
  
  /*----------------------------------------------------------------------*/
  /*                        "Remote" Functions                            */
  /*----------------------------------------------------------------------*/
  
  int rmt_sync(void)
  {
    static int i = 0;
    char *res;
    char expected[32];
    if (rmt_socket == -1) return 0;
    
    ++i;
    static const char *ping_fmt = "ping %d";
    static const char *pong_fmt = "pong %d";
    
    res = rmt_send((char *) ping_fmt, i);
    snprintf(expected, sizeof(expected), (char *) pong_fmt, i);
    if (strcmp(res, expected)) return 0;
    else return 1;
  }

  int rmt_init(std::string stim_host, int stim_port=STIM_PORT)
  {
    int i, ntries = 32;
    host = stim_host;
    port = stim_port;
    const char *server = host.c_str();
    int tcpport = port;
    
    if (rmt_socket >= 0) {
      rmt_close();
    }
    
    /* 
     * This loop is an attempt to fix a read/write sync problem
     * encountered with Socket 4.24, which seems to not properly
     * flush its buffers properly (although I can't say for sure
     * this is the problem).  This is a brute force solution --
     * try onces, check the sync, if it's ok, break...
     */
     
    for (i = 0; i < ntries; i++) {
      rmt_socket = socket_open();
      if (rmt_socket < 0) break;
      if (rmt_sync()) break;	/* success! */
      socket_close();
    }
    if (i != ntries) return rmt_socket >= 0;
    else return 0;
  }

  int rmt_close(void)
  {
    if (rmt_socket == -1) return 0;	/* Never opened */
    else socket_close();
    rmt_socket = -1;
    return 1;
  }
    
  char *rmt_send(char *format, ...)
  {
    char *rbuf, *eol;
    int rbytes, status, l;
    static char sbuf[SOCK_BUF_SIZE];
    
    va_list arglist;
    va_start(arglist, format);
    vsnprintf(sbuf, sizeof(sbuf)-2, format, arglist);
    va_end(arglist);
    
    if (rmt_socket == -1) return NULL;	/* Not connected */

    
    l = strlen(sbuf)-1;
    if (sbuf[l] != '\n') {
      sbuf[l+1] = '\n';
      sbuf[l+2] = '\0';
    }

    status = socket_send(sbuf, strlen(sbuf), &rbuf, &rbytes);
    if (status == 0) return NULL;
    if ((eol = strrchr(rbuf, '\n'))) *eol = 0;
    if ((eol = strrchr(rbuf, '\r'))) *eol = 0;
    return rbuf;
  }
};


#endif
