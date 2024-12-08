/*
 * NAME
 *   rmt.c
 *
 * DESCRIPTION
 *
 * AUTHOR
 *   DLS, 06/24
 */

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/ioctl.h>

#include <tcl.h>

#include "Datapoint.h"
#include "tclserver_api.h"

static char host[128];
static int port;
static const int STIM_PORT = 4610;
static int rmt_socket = -1;

/* these buffers are allocated once */
static char *rmt_socket_recv_buf = NULL;
static char *rmt_socket_send_buf = NULL;
static const int SOCK_BUF_SIZE = 65536;

static void socket_flush()
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
  

static int socket_open(void)
{
  const char *name = host;
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
static int socket_close(void)
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
static int socket_write(char *message, int nbytes)
{
  if (rmt_socket < 0) return 0;
  if (write(rmt_socket, message, nbytes) < 0) {
    close(rmt_socket);
    rmt_socket = -1;
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
static int socket_read(char **message, int *nbytes)
{
  char *buf = rmt_socket_recv_buf;
  if (!buf) return 0;
  
  int n;
  if ((n = read(rmt_socket, buf, SOCK_BUF_SIZE)) < 0) {
    if (message) *message = NULL;
    if (nbytes) *nbytes = 0;
    close(rmt_socket);
    rmt_socket = -1;
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
static int socket_send(char *sbuf, int sbytes, char **rbuf, int *rbytes)
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

static  int rmt_close(void)
{
  if (rmt_socket == -1) return 0;	/* Never opened */
  else socket_close();
  rmt_socket = -1;
  return 1;
}

static  char *rmt_send(char *format, ...)
{
  char *rbuf, *eol;
  int rbytes, status, l;
  char *sbuf = rmt_socket_send_buf;
  
  va_list arglist;
  va_start(arglist, format);
  vsnprintf(sbuf, SOCK_BUF_SIZE-2, format, arglist);
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

static int rmt_init(char *stim_host, int stim_port)
{
  strncpy(host, stim_host, sizeof(host));
  port = stim_port;
  const char *server = host;
  int tcpport = port;
  
  if (rmt_socket >= 0) {
    rmt_close();
  }
  
  rmt_socket = socket_open();
  return rmt_socket >= 0;
}


static int rmt_open_command(ClientData data, Tcl_Interp *interp,
		     int objc, Tcl_Obj *objv[])
{
  int port = STIM_PORT;
  int rc;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "host [port]");
    return TCL_ERROR;
  }
  
  if (objc > 2) {
    if (Tcl_GetIntFromObj(interp, objv[2], &port) != TCL_OK)
      return TCL_ERROR;
  }
  
  strncpy(host, Tcl_GetString(objv[1]), sizeof(host));
  
  rc = rmt_init(host, port);
  
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return TCL_OK;
}

static int rmt_close_command(ClientData data, Tcl_Interp *interp,
			       int objc, Tcl_Obj *objv[])
{
  int rc = rmt_close();
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return TCL_OK;
}

int rmt_send_command(ClientData data, Tcl_Interp *interp,
				int objc, Tcl_Obj *objv[])
{
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "rmt_cmd");
    return TCL_ERROR;
  }
  
  char *cmd = Tcl_GetString(objv[1]);
  char *result = rmt_send(cmd);
  if (result)
    Tcl_SetObjResult(interp, Tcl_NewStringObj(result, strlen(result)));
  return TCL_OK;
}

/*****************************************************************************
 * EXPORT
 *****************************************************************************/

#ifdef WIN32
EXPORT(int,Dserv_rmt_Init) (Tcl_Interp *interp)
#else
int Dserv_rmt_Init(Tcl_Interp *interp)
#endif
{
  if (
#ifdef USE_TCL_STUBS
      Tcl_InitStubs(interp, "8.6-", 0)
#else
      Tcl_PkgRequire(interp, "Tcl", "8.6-", 0)
#endif
      == NULL) {
    return TCL_ERROR;
  }

  tclserver_t *tclserver = tclserver_get();
  rmt_socket_recv_buf = calloc(1, SOCK_BUF_SIZE);
  rmt_socket_send_buf = calloc(1, SOCK_BUF_SIZE);
  
  Tcl_CreateObjCommand(interp, "rmtOpen",
		       (Tcl_ObjCmdProc *) rmt_open_command, tclserver, NULL);
  Tcl_CreateObjCommand(interp, "rmtClose",
		       (Tcl_ObjCmdProc *) rmt_close_command, tclserver, NULL);
  Tcl_CreateObjCommand(interp, "rmtSend",
		       (Tcl_ObjCmdProc *) rmt_send_command, tclserver, NULL);

  return TCL_OK;
}
