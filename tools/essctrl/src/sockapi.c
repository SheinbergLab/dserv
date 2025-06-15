#if 0
*******************************************************************************
Project		MPI
Program 	sockapi.c
Date		1998Feb22
Author		David Sheinberg
Description	API functions for sending commands using TCP/IP sockets
*******************************************************************************
#endif


#ifndef _WIN32
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <unistd.h>
#else

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <io.h>

#pragma comment(lib, "Ws2_32.lib")
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>


#include "sockapi.h"

/*****************************************************************************/
/********************************* SOCKET_OPEN *******************************/
/*****************************************************************************/
/*
 * Purpose:	To open a communication socket
 * Input:	name of server, port number
 * Process:	open socket and connect to server
 * Output:	0 or greater: successful socket
 *                    -1: failed socket call
 *                    -2: bad host
 *                    -3: failed connect call
 */
/*****************************************************************************/
int socket_open(char *name, int port)
{
	int sock, param;
	struct sockaddr_in server;
	struct hostent *hp;
	
	/* Create socket */
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		return -1;
	}
	server.sin_family = AF_INET;
	hp = gethostbyname(name);
	if (hp == 0) {
		return -2;
	}
	memcpy(&server.sin_addr, hp->h_addr, hp->h_length);
	server.sin_port = htons(port);
	
	if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
		return -3;
	}
	param = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *) &param, sizeof(param));
	return sock;
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
int socket_close(int sock)
{
#ifdef _MSC_VER
	closesocket(sock);
#else	
	close(sock);
#endif	
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
int socket_write(int sock, char *message, int nbytes)
{
	if (send(sock, message, nbytes, 0) < 0) {
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
int socket_read(int sock, char **message, int *nbytes)
{
  static char buf[16384];
  memset(buf, 0, sizeof(buf));
  int n;
  if ((n = recv(sock, buf, sizeof(buf), 0)) < 0) {
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
int socket_send(int sock, char *sbuf, int sbytes, char **rbuf, int *rbytes)
{
	int status;
	status = socket_write(sock, sbuf, sbytes);
	if (!status) return 0;
	status = socket_read(sock, rbuf, rbytes);
	return status;
}

char *sock_send(char *server, int port, char *buf, int len)
{
  int i, ntries = 32;
  int sock;
  char *rbuf, *eol;
  int rbytes, status, l;
  static char sbuf[SOCK_BUF_SIZE];

  sock = socket_open(server, port);
  if (sock < 0) return NULL;

  /* add a new line if necessary */
  if (len > SOCK_BUF_SIZE-1) return NULL;
  memcpy(sbuf, buf, len);
  if (buf[len-1] != '\n') {
    buf[len] = '\n';
    len++;
  }
    
  status = socket_send(sock, buf, len, &rbuf, &rbytes);
  if (status == 0) return NULL;
  if ((eol = strrchr(rbuf, '\n'))) *eol = 0;
  if ((eol = strrchr(rbuf, '\r'))) *eol = 0;
  
  socket_close(sock);
  return rbuf;
}

#ifdef _MSC_VER
void init_w32_socket(void)
{
    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (iResult != 0) {
        printf("WSAStartup failed with error: %d\n", iResult);
        return;
    }
}

void cleanup_w32_socket(void)
{
	WSACleanup();
}

#endif

int sendMessage(int socket, char *message, int nbytes)
{
  unsigned int msgSize = htonl(nbytes);
  if (send(socket, &msgSize, sizeof(msgSize), 0) != sizeof(msgSize)) return 0;
  if (send(socket, message, nbytes, 0) != nbytes) return 0;
  return 1;
}

int receiveMessage(int socket, char **rbuf)
{
  unsigned int msgSize;
  
  // Receive the size of the message
  ssize_t bytesReceived = recv(socket, &msgSize, sizeof(msgSize), 0);
  if (bytesReceived <= 0) {
    *rbuf = NULL;
    return 0;
  }
  
  msgSize = ntohl(msgSize);
  
  // Allocate buffer for the message
  char* buffer = (char *) malloc(msgSize);
  size_t totalBytesReceived = 0;
  while (totalBytesReceived < msgSize) {
    bytesReceived = recv(socket, buffer + totalBytesReceived, msgSize - totalBytesReceived, 0);
    if (bytesReceived <= 0) {
      free(buffer);
      *rbuf = NULL;
      return 0;
    }
    totalBytesReceived += bytesReceived;
  }
  *rbuf = buffer;
  return msgSize;
}

