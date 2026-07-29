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
#include <netinet/tcp.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>

#include <tcl.h>

#include "Datapoint.h"
#include "tclserver_api.h"
#include "socket_keepalive.h"

static const int STIM_PORT = 4612;

/*
 * EVERY BLOCKING OPERATION HERE IS BOUNDED.
 *
 * These calls run on the ess subprocess interp -- the experiment control loop.
 * An unbounded one does not merely delay a stimulus, it wedges the rig with no
 * recovery short of restarting dserv, which is the same hazard ess-2.0.tm
 * already documents for the stim->dserv direction ("A stimulus must never block
 * on the network"). This direction had no guard at all:
 *
 *   - connect() to a host that is unreachable rather than refusing gets no RST,
 *     so it sat for the full TCP SYN timeout -- ~75 s, measured -- on EVERY
 *     system load.
 *   - recv() waiting on a stim2 that is alive but wedged (stuck in a render, a
 *     loop in a stim script) never returned AT ALL.
 *
 * A timeout mid-message leaves the stream desynced -- we have consumed part of a
 * reply and cannot frame the next one -- so every expiry closes the socket
 * rather than retrying. A reported disconnect is recoverable; a hang is not.
 *
 * DNS is the remaining gap: getaddrinfo() has no portable timeout, so a
 * hostname that does not resolve still blocks on the resolver. It only bites
 * on a name (not an IP or localhost), and bounding it wants a resolver thread.
 */
static const int RMT_CONNECT_TIMEOUT_MS = 5000;
static const int RMT_DEFAULT_IO_TIMEOUT_MS = 30000;  /* generous: a stim script
                                                        upload legitimately
                                                        takes seconds */

/* A reply length is read off the wire, so a corrupt or desynced one would
   otherwise malloc up to 4 GB and then block trying to fill it. */
static const unsigned int RMT_MAX_MSG = 64u * 1024u * 1024u;

/*
 * PER-INTERPRETER STATE, NOT FILE STATICS.
 *
 * Only the ess subprocess loads this module today, which is the only reason
 * file statics worked: they made rmt_socket process-global, so a SECOND
 * subprocess loading dserv_rmt would have silently shared one socket with the
 * first and interleaved their request/response exchanges -- a desync with no
 * visible cause. Init() also re-allocated its scratch buffers over the previous
 * loader's pointers. Following the house pattern (see sound.c, gpio_input.c):
 * calloc one of these per interp and hand it to every command as ClientData.
 *
 * CONNECTION STATE IS PUBLISHED, NOT REMEMBERED.
 *
 * Every entry point that can change whether we are talking to stim2 routes
 * through rmt_set_connected(), which mirrors the state to status_point. This
 * module is the SINGLE OWNER of that datapoint -- the Tcl layer must not
 * dservSet it, or the two will disagree the moment stim2 dies between system
 * loads. (ess/rmt_host stays Tcl's: it is configuration, not state.)
 *
 * This matters because programs run happily with no stim attached: rmtSend on a
 * dead socket returns an empty string and TCL_OK, so a rig with a dark screen
 * looks identical to a working one unless something reports the truth.
 *
 * status_point is per-interp for the same reason the socket is: two loaders
 * publishing their independent link states to one datapoint would put the
 * ambiguity back. It defaults to ess/rmt_connected; a second loader should
 * claim its own name with rmtStatusPoint.
 */
typedef struct rmt_info_s {
  tclserver_t *tclserver;
  int socket;                   /* -1 when not connected */
  char host[128];
  int port;
  int io_timeout_ms;
  int published;                /* -1 = nothing published yet */
  char status_point[128];
} rmt_info_t;

static void rmt_publish(rmt_info_t *info, char *name, char *value)
{
  if (!info->tclserver) return;
  ds_datapoint_t *dp = dpoint_new(name,
				  tclserver_now(info->tclserver),
				  DSERV_STRING,
				  strlen(value), (unsigned char *) value);
  tclserver_set_point(info->tclserver, dp);
}

/*
 * Record the connection state and mirror it out. Publishes only on transition
 * unless force is set (rmtOpen forces, so a reload of the same system
 * republishes for any GUI that came up in between).
 */
static void rmt_set_connected(rmt_info_t *info, int connected, int force)
{
  if (!force && connected == info->published) return;
  info->published = connected;
  rmt_publish(info, info->status_point, connected ? "1" : "0");
}

/* Tear down after an I/O failure and tell everyone. */
static void rmt_disconnected(rmt_info_t *info)
{
  if (info->socket >= 0) {
    close(info->socket);
    info->socket = -1;
  }
  rmt_set_connected(info, 0, 0);
}

/*
 * Is the link actually alive, as opposed to merely opened at some point?
 *
 * A peek probe costs nothing and sends no protocol traffic: recv() returning 0
 * means the peer sent FIN, which is exactly what happens when stim2 exits. So
 * a screen that went away between rmtSends is noticed by the next status query
 * rather than by the next silently-dropped stimulus command.
 *
 * It cannot see a peer that vanished WITHOUT closing (power cut, cable pulled);
 * SO_KEEPALIVE on the socket is the backstop for that, at ~60 s.
 */
static int rmt_check_alive(rmt_info_t *info)
{
  char probe;
  ssize_t n;

  if (info->socket < 0) return 0;

  n = recv(info->socket, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
  if (n == 0) {                 /* orderly shutdown by stim2 */
    rmt_disconnected(info);
    return 0;
  }
  if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
    rmt_disconnected(info);
    return 0;
  }
  return 1;                     /* n > 0 (buffered reply) or simply nothing to read */
}

/* send() on a blocking socket usually takes everything, but "usually" is not
   a guarantee -- a short write on a multi-KB stim script would leave stim2
   waiting for bytes that never come, desyncing every later exchange on this
   socket. Loop until it is all gone. */
static int send_all(int socket, const char *buf, size_t nbytes)
{
  size_t sent = 0;
  while (sent < nbytes) {
    ssize_t n = send(socket, buf + sent, nbytes - sent, 0);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return 0;
    }
    sent += (size_t) n;
  }
  return 1;
}

static int sendMessage(int socket, char *message, int nbytes)
{
  unsigned int msgSize = htonl(nbytes);
  if (!send_all(socket, (const char *) &msgSize, sizeof(msgSize))) return 0;
  if (!send_all(socket, message, nbytes)) return 0;
  return 1;
}

/*
 * Returns -1 on I/O FAILURE, or the reply length (which may legitimately be 0).
 *
 * That distinction is the whole point of the signature. A stim2 command whose
 * Tcl result is the empty string -- every `proc`, `rename`, and `set ... {}` in
 * the block configure_stim uploads -- replies with a zero-length message. Folding
 * that into the same return value as a dead socket makes a completely normal
 * exchange look like a failure, and anything that reacts to failure (closing the
 * socket, say) then fires on healthy links during every system load.
 *
 * On success *rbuf owns a malloc'd buffer the caller frees; on 0 or -1 it is NULL.
 */
static int receiveMessage(int socket, char **rbuf)
{
  unsigned int msgSize;

  *rbuf = NULL;

  /* a short read of the header leaves us unable to frame anything that
     follows, so treat it as failure rather than proceeding on garbage */
  ssize_t bytesReceived = recv(socket, &msgSize, sizeof(msgSize), 0);
  if (bytesReceived != (ssize_t) sizeof(msgSize)) return -1;

  msgSize = ntohl(msgSize);
  if (msgSize == 0) return 0;           /* empty result, not an error */
  if (msgSize > RMT_MAX_MSG) return -1; /* corrupt or desynced: fail fast */

  char* buffer = (char *) malloc(msgSize);
  if (!buffer) return -1;

  size_t totalBytesReceived = 0;
  while (totalBytesReceived < msgSize) {
    bytesReceived = recv(socket, buffer + totalBytesReceived,
			 msgSize - totalBytesReceived, 0);
    if (bytesReceived <= 0) {
      free(buffer);
      return -1;
    }
    totalBytesReceived += bytesReceived;
  }
  *rbuf = buffer;
  return (int) msgSize;
}


/* Bound every subsequent send()/recv() on this socket. On expiry they fail with
   EAGAIN/EWOULDBLOCK, which the I/O paths treat as a dead link. */
static void socket_set_io_timeouts(int fd, int ms)
{
  struct timeval tv;
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  (void) setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  (void) setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

/*
 * connect() with a deadline: go non-blocking, start the handshake, wait for
 * writability with a bounded poll, then restore the original flags. SO_SNDTIMEO
 * does NOT bound connect(), which is why this has to be done by hand.
 */
static int socket_connect_timeout(int fd, struct sockaddr *addr,
				  socklen_t alen, int ms)
{
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;

  int rc = connect(fd, addr, alen);

  if (rc < 0 && errno == EINPROGRESS) {
    struct pollfd pfd;
    int pr;

    pfd.fd = fd;
    pfd.events = POLLOUT;
    do {
      pr = poll(&pfd, 1, ms);
    } while (pr < 0 && errno == EINTR);

    if (pr <= 0) {                      /* timed out, or poll failed */
      fcntl(fd, F_SETFL, flags);
      return -1;
    }

    /* poll says writable, but that is also how a REFUSED connection reports;
       the real verdict is in SO_ERROR */
    int err = 0;
    socklen_t elen = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
      fcntl(fd, F_SETFL, flags);
      return -1;
    }
  }
  else if (rc < 0) {                    /* failed outright */
    fcntl(fd, F_SETFL, flags);
    return -1;
  }

  if (fcntl(fd, F_SETFL, flags) < 0) return -1;   /* back to blocking */
  return 0;
}

/*
 * getaddrinfo() rather than gethostbyname(): the latter returns a pointer to a
 * static hostent, so two interps resolving at once raced on it -- and it is
 * IPv4-only. Each candidate address gets its own bounded connect, so a name
 * that resolves to several addresses can cost up to N * RMT_CONNECT_TIMEOUT_MS
 * before giving up. In practice a stim host is one address.
 *
 * Returns the fd, or -1 socket(), -2 resolve failed, -3 nothing connected.
 */
static int socket_open(rmt_info_t *info)
{
  struct addrinfo hints, *res = NULL, *ai;
  char portstr[16];
  int param = 1;
  int fd = -1;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;          /* v4 or v6 */
  hints.ai_socktype = SOCK_STREAM;

  snprintf(portstr, sizeof(portstr), "%d", info->port);

  info->socket = -1;

  if (getaddrinfo(info->host, portstr, &hints, &res) != 0 || !res) {
    return -2;
  }

  for (ai = res; ai; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;

    if (socket_connect_timeout(fd, ai->ai_addr, ai->ai_addrlen,
			       RMT_CONNECT_TIMEOUT_MS) == 0) break;

    /* close before trying the next: this path runs on every load of a system
       whose stim host is absent, and a leaked fd per attempt eventually wins */
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);

  if (fd < 0) return -3;

  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &param, sizeof(param));
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &param, sizeof(param));

  socket_set_io_timeouts(fd, info->io_timeout_ms);

  /* so a stim host that vanishes without closing is reaped in ~60 s rather
     than at the retransmit default */
  dserv_set_keepalive(fd);

  /* (the old socket_flush() lived here: it drained a freshly connected socket,
     where by construction there is nothing to drain, and its `while (n >= 0)`
     loop spun forever if the peer had already sent FIN -- read() returns 0 at
     EOF, which satisfied the condition. Removed rather than fixed.) */

  info->socket = fd;
  return fd;
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
static int socket_close(rmt_info_t *info)
{
  if (info->socket >= 0) {
    close(info->socket);
    info->socket = -1;
  }
  rmt_set_connected(info, 0, 0);
  return 1;
}

/*----------------------------------------------------------------------*/
/*                        "Remote" Functions                            */
/*----------------------------------------------------------------------*/

/*
 * (Removed here: socket_write/socket_read/socket_send and the #if 0 varargs
 * rmt_send they served. All four were unreachable -- the live path is
 * sendMessage/receiveMessage below -- but socket_write and socket_read each
 * carried their own close-on-error handling, so the file READ as though I/O
 * failures were handled while the code that actually ran ignored them. That
 * decoy is what made the missing failure handling hard to see, and it cost a
 * wrong patch before it was noticed. socket_read also handed back a pointer
 * into a shared static buffer.)
 */

static  int rmt_close(rmt_info_t *info)
{
  if (info->socket == -1) return 0;	/* Never opened */
  else socket_close(info);
  return 1;
}

/* allocates it's return buffer, so caller needs to free */
/* -1 = not connected / exchange failed, else reply length (0 is a valid reply).
   Allocates *buffer on a nonzero-length reply; caller frees. */
static int rmt_send(rmt_info_t *info, char *msg, int size, char **buffer)
{
  int rsize;

  *buffer = NULL;
  if (info->socket == -1) return -1;	/* Not connected */
  if (size < 0) size = strlen(msg);

  /* A genuine I/O failure means the exchange did not complete -- a short send,
     a timeout, or a recv that hit EOF because stim2 exited. Either way the
     socket is no longer usable for this request/response protocol (a partial
     message would desync every subsequent one), so drop it and report the state
     change instead of leaving a socket that claims a link no longer there. */
  if (!sendMessage(info->socket, msg, size)) {
    rmt_disconnected(info);
    return -1;
  }
  rsize = receiveMessage(info->socket, buffer);
  if (rsize < 0) {
    rmt_disconnected(info);
    return -1;
  }
  return rsize;
}

static int rmt_init(rmt_info_t *info, const char *stim_host, int stim_port)
{
  strncpy(info->host, stim_host, sizeof(info->host)-1);
  info->host[sizeof(info->host)-1] = '\0';
  info->port = stim_port;

  if (info->socket >= 0) {
    rmt_close(info);
  }

  socket_open(info);

  /* forced: republish even when the state is unchanged, so reloading a system
     re-announces the host and refreshes any GUI that connected since */
  rmt_set_connected(info, info->socket >= 0, 1);

  return info->socket >= 0;
}


static int rmt_open_command(ClientData data, Tcl_Interp *interp,
		     int objc, Tcl_Obj *objv[])
{
  rmt_info_t *info = (rmt_info_t *) data;
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

  rc = rmt_init(info, Tcl_GetString(objv[1]), port);

  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return TCL_OK;
}

static int rmt_close_command(ClientData data, Tcl_Interp *interp,
			       int objc, Tcl_Obj *objv[])
{
  rmt_info_t *info = (rmt_info_t *) data;
  int rc = rmt_close(info);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return TCL_OK;
}

int rmt_send_command(ClientData data, Tcl_Interp *interp,
				int objc, Tcl_Obj *objv[])
{
  rmt_info_t *info = (rmt_info_t *) data;

  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "rmt_cmd");
    return TCL_ERROR;
  }

  Tcl_Size len;
  char *cmd = Tcl_GetStringFromObj(objv[1], &len);
  char *result = NULL;
  int result_len = rmt_send(info, cmd, len, &result);

  /* An empty result and a failed send both leave the Tcl result empty -- that
     is deliberate, so a system can run with no display attached. What tells
     them apart is ess/rmt_connected, which rmt_disconnected() has already
     updated by the time we get here. */
  if (result_len > 0) {
    Tcl_SetObjResult(interp, Tcl_NewStringObj(result, result_len));
  }
  if (result) free(result);
  return TCL_OK;
}

int rmt_host_command(ClientData data, Tcl_Interp *interp,
		       int objc, Tcl_Obj *objv[])
{
  rmt_info_t *info = (rmt_info_t *) data;
  Tcl_SetObjResult(interp, Tcl_NewStringObj(info->host, strlen(info->host)));
  return TCL_OK;
}

/* rmtTimeout ?ms? -- get/set the per-operation send+recv deadline.
   Applies to the CURRENT socket immediately and to any opened later. */
int rmt_timeout_command(ClientData data, Tcl_Interp *interp,
			int objc, Tcl_Obj *objv[])
{
  rmt_info_t *info = (rmt_info_t *) data;

  if (objc > 1) {
    int ms;
    if (Tcl_GetIntFromObj(interp, objv[1], &ms) != TCL_OK) return TCL_ERROR;
    if (ms < 1) {
      Tcl_AppendResult(interp, "timeout must be >= 1 ms", NULL);
      return TCL_ERROR;
    }
    info->io_timeout_ms = ms;
    if (info->socket >= 0) socket_set_io_timeouts(info->socket, info->io_timeout_ms);
  }
  Tcl_SetObjResult(interp, Tcl_NewIntObj(info->io_timeout_ms));
  return TCL_OK;
}

/* rmtStatusPoint ?name? -- get/set the datapoint this interp mirrors its
   connection state to. Exists so a second loader can avoid colliding with
   ess/rmt_connected; the ess subprocess should leave it at the default. */
int rmt_status_point_command(ClientData data, Tcl_Interp *interp,
			     int objc, Tcl_Obj *objv[])
{
  rmt_info_t *info = (rmt_info_t *) data;

  if (objc > 1) {
    const char *name = Tcl_GetString(objv[1]);
    if (!name[0]) {
      Tcl_AppendResult(interp, "status point name must not be empty", NULL);
      return TCL_ERROR;
    }
    strncpy(info->status_point, name, sizeof(info->status_point)-1);
    info->status_point[sizeof(info->status_point)-1] = '\0';
    rmt_set_connected(info, info->socket >= 0, 1);   /* claim the new name */
  }
  Tcl_SetObjResult(interp, Tcl_NewStringObj(info->status_point,
					    strlen(info->status_point)));
  return TCL_OK;
}

int rmt_connected_command(ClientData data, Tcl_Interp *interp,
			  int objc, Tcl_Obj *objv[])
{
  rmt_info_t *info = (rmt_info_t *) data;

  /* probes the link rather than reporting whether we once opened it; a
     transition discovered here republishes the status point as a side effect */
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rmt_check_alive(info)));
  return TCL_OK;
}


/* Per-interp state needs a per-interp teardown, or an interp that goes away
   takes its open socket to stim2 with it. Deliberately does NOT publish: the
   tclserver may already be shutting down. */
static void rmt_interp_deleted(ClientData data, Tcl_Interp *interp)
{
  rmt_info_t *info = (rmt_info_t *) data;
  if (info->socket >= 0) close(info->socket);
  free(info);
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

  /* Allocate per-interpreter rmt info (see the rmt_info_t comment: file
     statics here meant a second loader would have shared this one's socket) */
  rmt_info_t *info = (rmt_info_t *) calloc(1, sizeof(rmt_info_t));
  if (!info) {
    Tcl_SetResult(interp, "Failed to allocate rmt_info_t", TCL_STATIC);
    return TCL_ERROR;
  }

  info->tclserver = tclserver_get_from_interp(interp);
  info->socket = -1;
  info->port = STIM_PORT;
  info->io_timeout_ms = RMT_DEFAULT_IO_TIMEOUT_MS;
  info->published = -1;
  strcpy(info->status_point, "ess/rmt_connected");

  Tcl_CallWhenDeleted(interp, rmt_interp_deleted, (ClientData) info);

  /* fail closed: until something opens a link, the answer is "no stim" */
  rmt_set_connected(info, 0, 1);

  Tcl_CreateObjCommand(interp, "rmtOpen",
		       (Tcl_ObjCmdProc *) rmt_open_command,
		       (ClientData) info, NULL);
  Tcl_CreateObjCommand(interp, "rmtClose",
		       (Tcl_ObjCmdProc *) rmt_close_command,
		       (ClientData) info, NULL);
  Tcl_CreateObjCommand(interp, "rmtSend",
		       (Tcl_ObjCmdProc *) rmt_send_command,
		       (ClientData) info, NULL);
  Tcl_CreateObjCommand(interp, "rmtHost",
		       (Tcl_ObjCmdProc *) rmt_host_command,
		       (ClientData) info, NULL);
  Tcl_CreateObjCommand(interp, "rmtConnected",
		       (Tcl_ObjCmdProc *) rmt_connected_command,
		       (ClientData) info, NULL);
  Tcl_CreateObjCommand(interp, "rmtTimeout",
		       (Tcl_ObjCmdProc *) rmt_timeout_command,
		       (ClientData) info, NULL);
  Tcl_CreateObjCommand(interp, "rmtStatusPoint",
		       (Tcl_ObjCmdProc *) rmt_status_point_command,
		       (ClientData) info, NULL);

  return TCL_OK;
}
