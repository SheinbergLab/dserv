/*
 * NAME
 *   usbio.c
 *
 * DESCRIPTION
 *   dserv module bridging a serial/USB-CDC device to the datapoint table.
 *
 *   Inbound (device -> dserv): a dual-mode framer. Bytes starting with '>' are
 *   read as fixed 128-byte binary dserv frames (the SAME format the networked
 *   wiznet-io box and dserv's '>' TCP handler use) -> dpoint -> tclserver_set_point.
 *   Other bytes are read as legacy newline-terminated "setdata <string>" text.
 *
 *   Outbound (dserv -> device): usbioSendFrame <name> <timestamp> <value> builds a
 *   128-byte binary frame ('>' + payload, zero-padded) and writes it. Wire it from
 *   Tcl with dpointSetScript so ess/in_obs and the config and cmd keys reach a USB
 *   box -- and pass [dservTimestamp $name] so the box's clock-sync anchor is exact:
 *       proc usbio_forward {dp data} { usbioSendFrame $dp [dservTimestamp $dp] $data }
 *       dservAddExactMatch ess/in_obs ; dpointSetScript ess/in_obs usbio_forward
 *
 *   The reader thread polls with a timeout and honours a stop flag, so a silent
 *   device never blocks it and usbioClose can stop+join it cleanly (no close of an
 *   fd with a read in flight -- that deadlocked the interp on macOS).
 *
 * AUTHOR
 *   DLS, 06/24  (binary framing + outbound frames 07/26; interruptible reader 07/26)
 */

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/uio.h>
#include <termios.h>
#include <time.h>
#include <pthread.h>

#include <tcl.h>
#include "Datapoint.h"
#include "tclserver_api.h"

#define USBIO_MAXWIRE 12             /* de-dup table for auto-registered forward patterns */

typedef struct usbio_info_s
{
  int usbio_fd;
  tclserver_t *tclserver;
  /* reader thread */
  volatile int running;
  pthread_t worker;
  /* inbound dual-mode framer state */
  int fr_mode;                 /* 0 idle, 1 binary(128B), 2 text(newline) */
  int fr_have;
  uint8_t fr[256];
  /* auto-registration: patterns the box has asked us to forward (from its %match) */
  int  nwired;
  char wired[USBIO_MAXWIRE][96];
} usbio_info_t;

/* global to this module */
static usbio_info_t g_usbioInfo;

/* One 128-byte binary frame ('>' + payload, zero-padded) -> datapoint. Mirrors the
 * parse in Dataserver.cpp's '>' handler; bounds every length so a malformed frame
 * can never read past the 128-byte buffer. */
static void process_binary_frame(usbio_info_t *info, const uint8_t *frame)
{
  const uint8_t *p = frame + 1;                 /* skip '>' */
  uint16_t varlen; memcpy(&varlen, p, sizeof varlen); p += sizeof varlen;
  if (varlen == 0 || varlen > 100) return;
  char name[128];
  memcpy(name, p, varlen); name[varlen] = '\0'; p += varlen;
  uint64_t ts;    memcpy(&ts,    p, sizeof ts);    p += sizeof ts;
  uint32_t dtype; memcpy(&dtype, p, sizeof dtype); p += sizeof dtype;
  uint32_t dlen;  memcpy(&dlen,  p, sizeof dlen);  p += sizeof dlen;
  if ((int) varlen + (int) dlen > 109) return;  /* keeps p+dlen inside the 128B frame */

  if (!ts) ts = tclserver_now(info->tclserver);
  ds_datapoint_t *dp = dpoint_new(name, ts, (ds_datatype_t) dtype, dlen, (unsigned char *) p);
  if (dp) tclserver_set_point(info->tclserver, dp);
}

/* Auto-registration: the box (built with -DBOX_USB_FORWARD_REGISTER) periodically
 * emits its "%match <ip> <port> <pattern> <onoff>" lines down the CDC. We ignore the
 * ip/port (meaningless over USB), de-dup the pattern, and wire a forward for it:
 * dservAddMatch + dpointSetScript -> usbioSendFrame (glob-capable, timestamp-exact).
 * So a USB box self-declares what to forward -- no static post-pins wiring needed. */
static void usbio_autowire(usbio_info_t *info, const char *line)
{
  char pat[96];
  if (sscanf(line, "%%match %*s %*s %95s", pat) != 1) return;   /* 4th token = pattern */
  for (int i = 0; i < info->nwired; i++) if (!strcmp(info->wired[i], pat)) return;  /* already wired */
  if (info->nwired < USBIO_MAXWIRE) strncpy(info->wired[info->nwired++], pat, sizeof info->wired[0] - 1);

  char script[320];
  snprintf(script, sizeof script,
    "if {![llength [info procs usbio_forward]]} "
    "{proc usbio_forward {dp data} {usbioSendFrame $dp [dservTimestamp $dp] $data}} ; "
    "catch {dservAddMatch {%s}} ; catch {dpointSetScript {%s} usbio_forward}", pat, pat);
  tclserver_queue_script(info->tclserver, script, 1);          /* run in the interp, no reply */
}

/* Text path: "setdata <string>" -> datapoint; "%match ..." -> auto-wire a forward. */
static void process_text_line(usbio_info_t *info, char *line, int len)
{
  if (len > 8 && !strncmp(line, "setdata ", 8)) {
    ds_datapoint_t *dpoint = dpoint_from_string(line + 8, len - 8);
    if (dpoint) {
      if (!dpoint->timestamp) dpoint->timestamp = tclserver_now(info->tclserver);
      tclserver_set_point(info->tclserver, dpoint);
    }
  }
  else if (len > 7 && !strncmp(line, "%match ", 7)) {
    usbio_autowire(info, line);
  }
  /* (%reg lines are ignored -- registration is implicit over USB) */
}

/* Feed a chunk of received bytes through the dual-mode framer. */
static void usbio_feed(usbio_info_t *info, const uint8_t *data, int n)
{
  for (int i = 0; i < n; i++) {
    uint8_t b = data[i];
    if (info->fr_mode == 0) {                                 /* idle: pick mode by first byte */
      if (b == DPOINT_BINARY_MSG_CHAR) { info->fr_mode = 1; info->fr[0] = b; info->fr_have = 1; }
      else if (b == '\n' || b == '\r') { /* skip blank */ }
      else { info->fr_mode = 2; info->fr[0] = b; info->fr_have = 1; }
    }
    else if (info->fr_mode == 1) {                            /* binary: collect exactly 128 bytes */
      info->fr[info->fr_have++] = b;
      if (info->fr_have >= DPOINT_BINARY_FIXED_LENGTH) {
        process_binary_frame(info, info->fr);
        info->fr_mode = 0; info->fr_have = 0;
      }
    }
    else {                                                    /* text: to newline */
      if (b == '\n' || b == '\r') {
        info->fr[info->fr_have] = '\0';
        process_text_line(info, (char *) info->fr, info->fr_have);
        info->fr_mode = 0; info->fr_have = 0;
      } else if (info->fr_have < (int) sizeof info->fr - 1) {
        info->fr[info->fr_have++] = b;
      } else { info->fr_mode = 0; info->fr_have = 0; }         /* overflow -> resync */
    }
  }
}

/* Reader thread: poll with a timeout so a silent device never blocks us and we can
 * be stopped by clearing ->running. Never closes the fd (usbioClose owns that). */
static void *workerThread(void *arg)
{
  usbio_info_t *info = (usbio_info_t *) arg;
  uint8_t buf[4096];
  struct pollfd pfd = { .fd = info->usbio_fd, .events = POLLIN };

  while (info->running) {
    int pr = poll(&pfd, 1, 200);                 /* 200 ms -> re-check ->running */
    if (pr < 0) { if (errno == EINTR) continue; break; }
    if (pr == 0) continue;                        /* timeout */
    if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) break;   /* device gone */
    if (pfd.revents & POLLIN) {
      ssize_t n = read(info->usbio_fd, buf, sizeof buf);
      if (n > 0) usbio_feed(info, buf, (int) n);
      else if (n == 0) break;                     /* EOF */
      else if (errno != EAGAIN && errno != EINTR) break;
    }
  }
  info->running = 0;
  return NULL;
}

/* Stop the reader (if any) and close the fd -- join first so we never close an fd
 * with a read in flight. Bounded: the worker checks ->running every <=200 ms. */
static void usbio_stop(usbio_info_t *info)
{
  if (info->usbio_fd < 0) return;
  info->running = 0;
  pthread_join(info->worker, NULL);
  close(info->usbio_fd);
  info->usbio_fd = -1;
}

static int configure_serial_port(int fd)
{
  struct termios ser;
  tcflush(fd, TCIFLUSH);
  tcflush(fd, TCOFLUSH);
  if (tcgetattr(fd, &ser) < 0) return -1;
  cfmakeraw(&ser);                        /* raw: binary frames pass untouched */
  if (tcsetattr(fd, TCSANOW, &ser) < 0) return -2;
  return 0;
}

/* Write exactly len bytes, tolerating EAGAIN (fd is non-blocking). Returns bytes
 * written (== len on success). A partial frame would desync the box, so callers
 * treat < len as an error. */
static int write_all(int fd, const unsigned char *buf, int len)
{
  int off = 0, guard = 0;
  while (off < len) {
    ssize_t w = write(fd, buf + off, (size_t) (len - off));
    if (w > 0) { off += (int) w; guard = 0; continue; }
    if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
      if (++guard > 2000) break;                 /* device not draining -> give up */
      struct timespec ts = { 0, 200000 }; nanosleep(&ts, NULL);  /* 0.2 ms */
      continue;
    }
    break;                                        /* real error */
  }
  return off;
}

/* usbioSend <text>  -- write a raw text command + newline (legacy/CLI helper). */
static int usbio_send_command(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *objv[])
{
  usbio_info_t *info = (usbio_info_t *) data;
  if (objc < 2) { Tcl_WrongNumArgs(interp, 1, objv, "command"); return TCL_ERROR; }
  if (info->usbio_fd < 0) return TCL_OK;

  Tcl_Size clen;
  char *cmd = Tcl_GetStringFromObj(objv[1], &clen);
  unsigned char line[512];
  int n = (int) clen; if (n > (int) sizeof line - 1) n = (int) sizeof line - 1;
  memcpy(line, cmd, n); line[n] = '\n';
  int w = write_all(info->usbio_fd, line, n + 1);
  if (w != n + 1) { Tcl_AppendResult(interp, Tcl_GetString(objv[0]), ": send error", NULL); return TCL_ERROR; }
  Tcl_SetObjResult(interp, Tcl_NewIntObj(w));
  return TCL_OK;
}

/* usbioSendFrame <name> <timestamp> <value>  -- build one 128-byte binary dserv
 * frame ('>' + payload, zero-padded) and write it. The value rides as a STRING (the
 * box parses string-or-numeric); the timestamp is preserved so a forwarded
 * ess/in_obs edge anchors the box clock correctly. */
static int usbio_sendframe_command(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *objv[])
{
  usbio_info_t *info = (usbio_info_t *) data;
  if (objc < 4) { Tcl_WrongNumArgs(interp, 1, objv, "name timestamp value"); return TCL_ERROR; }
  if (info->usbio_fd < 0) return TCL_OK;

  char *name = Tcl_GetString(objv[1]);
  Tcl_WideInt ts;
  if (Tcl_GetWideIntFromObj(interp, objv[2], &ts) != TCL_OK) return TCL_ERROR;
  Tcl_Size vlen;
  unsigned char *val = (unsigned char *) Tcl_GetStringFromObj(objv[3], &vlen);

  ds_datapoint_t *dp = dpoint_new(name, (uint64_t) ts, DSERV_STRING, (uint32_t) vlen, val);
  if (!dp) return TCL_OK;

  unsigned char frame[DPOINT_BINARY_FIXED_LENGTH];
  memset(frame, 0, sizeof frame);
  frame[0] = DPOINT_BINARY_MSG_CHAR;
  int sz = DPOINT_BINARY_FIXED_LENGTH - 1;          /* space after the '>' */
  int rc = dpoint_to_binary(dp, frame + 1, &sz);    /* >0 bytes written, 0 = too big */
  dpoint_free(dp);
  if (rc <= 0) { Tcl_AppendResult(interp, "usbioSendFrame: name+value too large", NULL); return TCL_ERROR; }

  int w = write_all(info->usbio_fd, frame, sizeof frame);   /* always the full 128 */
  Tcl_SetObjResult(interp, Tcl_NewIntObj(w));
  return TCL_OK;
}

static int usbio_open_command(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *objv[])
{
  usbio_info_t *info = (usbio_info_t *) data;
  if (objc < 2) { Tcl_WrongNumArgs(interp, 1, objv, "port"); return TCL_ERROR; }

  usbio_stop(info);                              /* cleanly stop any prior reader */

  int fd = open(Tcl_GetString(objv[1]), O_NOCTTY | O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]), ": error opening port \"",
                     Tcl_GetString(objv[1]), "\"", NULL);
    return TCL_ERROR;
  }
  int ret = configure_serial_port(fd);
  info->usbio_fd = fd;
  info->fr_mode = 0; info->fr_have = 0;           /* fresh framer per open */
  info->nwired = 0;                               /* re-learn forwards from the box */
  info->running = 1;
  if (pthread_create(&info->worker, NULL, workerThread, info) != 0) {
    info->running = 0; close(fd); info->usbio_fd = -1;
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]), ": worker thread failed", NULL);
    return TCL_ERROR;
  }
  Tcl_SetObjResult(interp, Tcl_NewIntObj(ret));
  return TCL_OK;
}

static int usbio_close_command(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *objv[])
{
  (void) interp; (void) objc; (void) objv;
  usbio_stop((usbio_info_t *) data);
  return TCL_OK;
}

/*****************************************************************************
 *
 * EXPORT
 *
 *****************************************************************************/

#ifdef WIN32
EXPORT(int,Dserv_usbio_Init) (Tcl_Interp *interp)
#else
  int Dserv_usbio_Init(Tcl_Interp *interp)
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
  g_usbioInfo.usbio_fd = -1;
  g_usbioInfo.running = 0;
  g_usbioInfo.fr_mode = 0;
  g_usbioInfo.fr_have = 0;
  g_usbioInfo.nwired = 0;
  g_usbioInfo.tclserver = tclserver_get_from_interp(interp);

  Tcl_CreateObjCommand(interp, "usbioOpen",
                       (Tcl_ObjCmdProc *) usbio_open_command, (ClientData) &g_usbioInfo, NULL);
  Tcl_CreateObjCommand(interp, "usbioClose",
                       (Tcl_ObjCmdProc *) usbio_close_command, (ClientData) &g_usbioInfo, NULL);
  Tcl_CreateObjCommand(interp, "usbioSend",
                       (Tcl_ObjCmdProc *) usbio_send_command, (ClientData) &g_usbioInfo, NULL);
  Tcl_CreateObjCommand(interp, "usbioSendFrame",
                       (Tcl_ObjCmdProc *) usbio_sendframe_command, (ClientData) &g_usbioInfo, NULL);
  return TCL_OK;
}
