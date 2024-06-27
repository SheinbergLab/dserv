#if 0
*******************************************************************************
Project		MPI
Program 	sockapi.h
Date		1998Feb22
Author		David Sheinberg
Description	Header for API functions for sending commands using TCP/IP sockets
*******************************************************************************
#endif

#define SOCK_BUF_SIZE 16384

int socket_open(char *server, int port);
int socket_close(int);
int socket_write(int sock, char *message, int nbytes);
int socket_read(int sock, char **message, int *nbytes);
int socket_send(int sock, char *sbuf, int sbytes, char **rbuf, int *rbytes);
char *sock_send(char *server, int port, char *buf, int bufsize);

