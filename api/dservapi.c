#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/select.h>
#include <fcntl.h>

#include "dservapi.h"
#include <Base64.h>

int dservapi_open_socket(const char *host, int port)
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

void dservapi_close_socket(int fd) {
  if (fd >= 0) close(fd);
}



static int doRead(int fd, char *buf, int size)
{
  int rbytes = 0;
  int n = 0;

  while(rbytes < size) {
    if((n = read(fd, buf + rbytes, size - rbytes)) < 0) {
      return -1;
    }

    rbytes += n;
  }
  return 0;
}

/*
 * get dpoint from server and return binary stream
 */

int
dservapi_get_from_dataserver(int fd,
			     const char *varname,
			     char **outbuf)
{
  char cmd[] = "<";
  int bufsize;
  int rval;
  uint16_t varlen = strlen(varname);
  
  struct iovec iov[3];
  iov[0].iov_base = cmd;
  iov[0].iov_len = strlen(cmd);
  iov[1].iov_base = (void *) &varlen;
  iov[1].iov_len = sizeof(uint16_t);
  iov[2].iov_base = (void *) varname;
  iov[2].iov_len = strlen(varname);
  
  ssize_t bytes_written = writev(fd, iov, 3);
  if (bytes_written == -1) {
        return -1;
  }

  rval = read(fd, &bufsize, sizeof(int));
  if (rval != sizeof(int)) return -1;
  
  /* no dpoint exists so return 0 */
  if (!bufsize) return 0;
  
  char *buf = (char *) malloc(bufsize);
  if (!buf) return -1;

  if (doRead(fd, buf, bufsize)) {
    free(buf);
    return -1;
  }

  *outbuf = buf;
  
  return bufsize;
}


#define DPOINT_BINARY_MSG_CHAR '>'
#define DPOINT_BINARY_FIXED_LENGTH (128)

int dservapi_write_to_dataserver(int fd,
			const char *varname,
			int dtype,
			int len, void *data) 
{
  uint8_t cmd = DPOINT_BINARY_MSG_CHAR;
  static char buf[DPOINT_BINARY_FIXED_LENGTH];
 
  uint16_t varlen;
  uint64_t timestamp = 0;
  uint32_t datatype = dtype,  datalen = len;

  uint16_t bufidx = 0;
  uint16_t total_bytes = 0;

  varlen = strlen(varname);

  // Start by seeing how much space we need
  total_bytes += sizeof(uint16_t); // varlen
  total_bytes += varlen;           // strlen(varname)
  total_bytes += sizeof(uint64_t); // timestamp
  total_bytes += sizeof(uint32_t); // datatype
  total_bytes += sizeof(uint32_t); // datalen
  total_bytes += len;              // data

  // data don't fit
  if (total_bytes > sizeof(buf)-1) {
    return 0;
  }

  memcpy(&buf[bufidx], &cmd, sizeof(uint8_t));
  bufidx += sizeof(uint8_t);

  memcpy(&buf[bufidx], &varlen, sizeof(uint16_t));
  bufidx += sizeof(uint16_t);

  memcpy(&buf[bufidx], varname, varlen);
  bufidx += varlen;

  memcpy(&buf[bufidx], &timestamp, sizeof(uint64_t));
  bufidx += sizeof(uint64_t);

  memcpy(&buf[bufidx], &datatype, sizeof(uint32_t));
  bufidx += sizeof(uint32_t);

  memcpy(&buf[bufidx], &datalen, sizeof(uint32_t));
  bufidx += sizeof(uint32_t);

  memcpy(&buf[bufidx], data, datalen);
  bufidx += datalen;

  if (write(fd, buf, sizeof(buf)) < 0)
  {
    printf("writing socket");
    return 0;
  }
  return 1;
}

int dservapi_send_to_dataserver(int fd, const char *var,
				int dtype, int n, void *data)
{
  static char buf[1024];
  static char sendbuf[2048];
  int datalen;
  int eltsizes[] = { 1, 0, 4, 0, 2, 4 };
  if (dtype == DSERV_STRING) {
    sprintf(sendbuf, "%%setdata %s %d %u %d {%s}\r\n", var, dtype,
	    0, n, (char *) data);
  }
  else if (dtype != 0 && dtype != 2 && dtype != 4 && dtype != 5) {
    datalen = n*eltsizes[dtype];
    if ((unsigned int) base64size(datalen) > sizeof(buf)) return 0;
    base64encode((char *) data, datalen, buf, sizeof(buf));
    sprintf(sendbuf, "%%setdata %s %d %u %d {%s}\r\n", var, dtype,
	    0, datalen, buf);
  }
  else { return -1; }
  
  int writen = strlen(sendbuf);
  if (write(fd, sendbuf, writen) != writen) {
    return -1;
  }
  return 0;
}


#ifdef STAND_ALONE
int main(int argc, char *argv[])
{
  int count = 0;
  
  int sockfd = dservapi_open_socket("127.0.0.1", 4620);
  if (sockfd < 0) {
    fprintf(stderr, "error opening socket\n");
    return -1;
  }

  char buf[64];

  printf("opened socket %d\n", sockfd);
  
  const char *pointname = "test/counter";
  for (;;) {
    snprintf(buf, sizeof(buf), "%d", count++);

    dservapi_write_to_dataserver(sockfd, pointname,
				 DSERV_STRING,
				 strlen(buf), buf);
    dservapi_send_to_dataserver(sockfd, pointname,
				DSERV_STRING,
				strlen(buf), buf);
    sleep(1);
  }
}
#endif
