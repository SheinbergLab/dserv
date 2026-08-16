#ifndef LISTENERSOCKET_H
#define LISTENERSOCKET_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

/*
 * Create + bind + listen for dserv's hand-rolled TCP listeners (newline 2570,
 * message 2560, dataserver 4620), with three properties the raw pattern they
 * replaced did not have:
 *
 * FD_CLOEXEC ON THE LISTENER. Tcl `exec` children fork from the worker thread
 * and inherit every fd that is not close-on-exec -- including these listening
 * sockets. The failure that motivated this file (raspberrypi, 2026-08-15): an
 * install run via dserv's own interp left dpkg/systemctl children alive when
 * systemd SIGKILLed dserv (KillMode=process kills only the main pid), and
 * those orphans HELD THE LISTENING PORTS. The successor dserv then failed
 * every text-port bind despite SO_REUSEADDR -- REUSEADDR helps against
 * TIME_WAIT remnants, not against a socket that is still genuinely open in
 * someone's fd table. uWS creates its 2565 socket close-on-exec, which is why
 * the WebSocket side alone survived and the server came up half-alive.
 *
 * BOUNDED RETRY. A predecessor's sockets can outlive it by a few seconds
 * (slow orphan exit, FIN_WAIT drains). Retrying covers every transient hold
 * without masking a real conflict.
 *
 * FAILURE IS FATAL. The old code perror'd and returned, silently ending the
 * accept thread inside a process that keeps running -- a server that answers
 * on some ports and refuses others, which systemd reports as healthy. If the
 * port cannot be bound after the retry window, something else owns it for
 * real (typically a second instance); exit nonzero so systemd's
 * Restart=on-failure relaunches into a clean bind -- or stops trying, loudly,
 * if the conflict persists. Nothing durable is open this early: these
 * listeners bind at construction time, before dsconf runs or any datafile
 * opens, so _exit here cannot lose data.
 */
static inline int listener_socket_or_die(int port, const char *what)
{
  struct sockaddr_in address;
  int socket_fd;
  int on = 1;
  const int attempts = 30;      /* x 1s: outlasts any orphan/TIME_WAIT hold */

  memset(&address, 0, sizeof(struct sockaddr_in));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = INADDR_ANY;

  if ((socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    fprintf(stderr, "%s: socket(port %d): %s\n", what, port, strerror(errno));
    _exit(1);
  }

  fcntl(socket_fd, F_SETFD, FD_CLOEXEC);
  setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  for (int i = 1;; i++) {
    if (bind(socket_fd, (const struct sockaddr *) &address,
             sizeof(struct sockaddr)) == 0)
      break;
    if (i == 1 || i % 5 == 0)
      fprintf(stderr, "%s: bind port %d: %s (attempt %d/%d)\n",
              what, port, strerror(errno), i, attempts);
    if (i >= attempts) {
      fprintf(stderr,
              "%s: port %d still unavailable after %ds -- another instance? "
              "exiting so systemd can relaunch clean\n",
              what, port, attempts);
      _exit(1);
    }
    sleep(1);
  }

  if (listen(socket_fd, 20) == -1) {
    fprintf(stderr, "%s: listen(port %d): %s\n", what, port, strerror(errno));
    _exit(1);
  }

  return socket_fd;
}

/*
 * Accepted client sockets must not leak into exec'd children either: a
 * long-lived child would pin the connection open after dserv dies, leaving
 * the peer talking to a corpse.
 */
static inline void accepted_socket_cloexec(int fd)
{
  fcntl(fd, F_SETFD, FD_CLOEXEC);
}

#endif /* LISTENERSOCKET_H */
