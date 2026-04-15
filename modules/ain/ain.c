/*
 * NAME
 *   ain.c
 *
 * DESCRIPTION
 *   This module acquires analog signals from an MCP320x chip using the SPI
 * bus using a periodic timer and sends the acquired data point to dserv.
 *
 * DPOINTS
 *   uint16_t ${PREFIX}/vals
 *   int      ${PREFIX}/interval_ms
 *
 * AUTHOR
 *   DLS, 06/24-08/25
 *
 */

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <stdatomic.h>

#ifdef __linux__
#include <sys/timerfd.h>
#include <linux/spi/spidev.h>
#define SPIDEV_PATH "/dev/spidev0.0"
#endif

#include <tcl.h>

#include "Datapoint.h"
#include "tclserver_api.h"

const char *DEFAULT_ADC_DPOINT_PREFIX = "ain";
#define MAX_CHAN 8

typedef struct ain_info_s
{
  tclserver_t *tclserver;
  int fd;			/* spidev fd                */
  int timer_fd;			/* timerfd_XXX fd           */
#ifdef __linux__
  pthread_t timer_thread_id;
#endif
  int interval_ms;		/* how often to acq (ms)    */
  int nchan;			/* number of channels acqd  */
  int invert_signals[MAX_CHAN];	/* invert 0 - 4095          */
  char *dpoint_prefix;		/* e.g. "ain" -> "ain/vals" */
  pthread_mutex_t prefix_mutex; /* protects writes to dpoint_prefix */
  atomic_int prefix_version;	/* bumped whenever prefix changes   */
} ain_info_t;

/* global to this module */
static ain_info_t g_ainInfo;

#ifdef __linux__
/*
 * Batched MCP3204 read.
 *
 * Issues nchan SPI transfers in a single SPI_IOC_MESSAGE(nchan) ioctl with
 * cs_change=1 between transfers so the chip select cycles (each MCP3204
 * conversion requires its own CS cycle). This replaces an earlier version
 * that did one ioctl per channel; at 1 kHz acquisition the kernel-entry
 * overhead was noticeable CPU-wise on the Pi.
 *
 * Returns 0 on success, -1 on SPI failure.
 */
int mcp3204_read(int fd, int nchan, uint16_t buf[])
{
  if (nchan <= 0 || nchan > MAX_CHAN) return -1;

  struct spi_ioc_transfer xfers[MAX_CHAN];
  uint8_t tx[MAX_CHAN][3];
  uint8_t rx[MAX_CHAN][3];

  memset(xfers, 0, sizeof(xfers));
  memset(rx, 0, sizeof(rx));

  /* MCP3204 command format:
   *   byte 0: 0000 0110  (start bit + single-ended select)
   *   byte 1: D1 D0 0000 0000  (channel in top two bits)
   *   byte 2: don't care
   * Response:
   *   byte 1 low nibble = result bits 11..8
   *   byte 2            = result bits  7..0
   */
  for (int i = 0; i < nchan; i++) {
    tx[i][0] = 0x06;
    tx[i][1] = (uint8_t)(i << 6);
    tx[i][2] = 0x00;

    xfers[i].tx_buf     = (unsigned long)tx[i];
    xfers[i].rx_buf     = (unsigned long)rx[i];
    xfers[i].len        = 3;
    xfers[i].cs_change  = 1; /* deselect between transfers */
  }
  /* Let CS release naturally after the last transfer */
  xfers[nchan - 1].cs_change = 0;

  int status = ioctl(fd, SPI_IOC_MESSAGE(nchan), xfers);
  if (status < 0) {
    /* perror is noisy at 1 kHz; caller decides whether to log */
    return -1;
  }

  for (int i = 0; i < nchan; i++) {
    buf[i] = (uint16_t)(((rx[i][1] & 0x0F) << 8) | rx[i][2]);
  }
  return 0;
}

void *acquire_thread(void *arg)
{
  ain_info_t *info = (ain_info_t *) arg;

  uint64_t exp;
  ssize_t s;

  uint16_t vals[MAX_CHAN] = {0};
  /* MCP3204 is 12-bit, so samples span 0..4095 inclusive. */
  const int max_val = (1 << 12) - 1;

  /* Cache the "<prefix>/vals" point name; rebuild whenever the prefix
   * version changes (ainSetPrefix bumps the atomic). The version check
   * is a single atomic load on the normal path; the full mutex/snprintf
   * only runs when the prefix has actually changed. */
  char cached_name[128];
  cached_name[0] = '\0';
  int cached_version = -1;

  /* Rate-limit consecutive SPI error logs so a broken bus doesn't spam */
  int error_streak = 0;

  while (1) {
    s = read(info->timer_fd, &exp, sizeof(uint64_t));
    if (s != sizeof(uint64_t)) continue;

    /* Refresh cached point name if prefix changed */
    int v = atomic_load(&info->prefix_version);
    if (v != cached_version) {
      pthread_mutex_lock(&info->prefix_mutex);
      snprintf(cached_name, sizeof(cached_name),
               "%s/vals", info->dpoint_prefix);
      pthread_mutex_unlock(&info->prefix_mutex);
      cached_version = v;
    }

    if (mcp3204_read(info->fd, info->nchan, vals) < 0) {
      if (error_streak == 0) {
        fprintf(stderr, "ain: SPI read failed\n");
      }
      if (error_streak < 1000) error_streak++;
      continue;
    }
    error_streak = 0;

    for (int i = 0; i < info->nchan; i++) {
      if (info->invert_signals[i]) vals[i] = max_val - vals[i];
    }

    /* fill the data point */
    ds_datapoint_t *dp = dpoint_new(cached_name,
                                    tclserver_now(info->tclserver),
                                    DSERV_SHORT,
                                    sizeof(uint16_t) * info->nchan,
                                    (unsigned char *) vals);

    /* update would be better than set here (after the first set) */
    tclserver_set_point(info->tclserver, dp);
  }

  /* unreachable; kept for clarity */
  return NULL;
}

#endif

/* for simulation, we can set the number of channels explicitly */
static int ain_set_nchan_command(ClientData data, Tcl_Interp *interp,
                                 int objc, Tcl_Obj *objv[])
{
    ain_info_t *info = (ain_info_t *) data;
    int nchan;
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "nchan");
        return TCL_ERROR;
    }
    
    if (Tcl_GetIntFromObj(interp, objv[1], &nchan) != TCL_OK)
        return TCL_ERROR;
        
    if (nchan < 1 || nchan > MAX_CHAN) {
        Tcl_AppendResult(interp, "nchan must be between 1 and ", 
                         Tcl_NewIntObj(MAX_CHAN), NULL);
        return TCL_ERROR;
    }

    // quietly fail if we have hardware installed
    if (g_ainInfo.fd != -1) {
        return TCL_OK;
    }
    
    info->nchan = nchan;
    return TCL_OK;
}

static int ain_start_command (ClientData data, Tcl_Interp *interp,
			      int objc, Tcl_Obj *objv[])
{
  ain_info_t *info = (ain_info_t *) data;
  int ms = 10;			/* default to 100Hz */

  if (objc > 1) {
    if (Tcl_GetIntFromObj(interp, objv[1], &ms) != TCL_OK)
      return TCL_ERROR;
  }
  info->interval_ms = ms;

  /* push the interval_ms variable to dserv */
  int interval_sz = strlen(info->dpoint_prefix)+32;
  char *interval_point_name =  (char *) malloc(interval_sz);
  snprintf(interval_point_name, interval_sz, "%s/interval_ms",
	   info->dpoint_prefix);
  ds_datapoint_t *dp = dpoint_new(interval_point_name,
				  tclserver_now(info->tclserver),
				  DSERV_INT,
				  sizeof(int),
				  (unsigned char *) &info->interval_ms);
  tclserver_set_point(info->tclserver, dp);
  free(interval_point_name);
  
#ifdef __linux__
  struct itimerspec new_value;
  struct timespec now;

  if (clock_gettime(CLOCK_REALTIME, &now) == -1)
    return TCL_ERROR;
  
  /* Create a CLOCK_REALTIME absolute timer with initial
     expiration and interval as specified in command line */
  
  new_value.it_value.tv_sec = now.tv_sec;
  new_value.it_value.tv_nsec = now.tv_nsec + ms*1000000;
  new_value.it_interval.tv_sec = 0;
  new_value.it_interval.tv_nsec = ms*1000000;
  
  timerfd_settime(info->timer_fd, TFD_TIMER_ABSTIME, &new_value, NULL);
#endif

  return TCL_OK;
}

static int ain_stop_command (ClientData data, Tcl_Interp *interp,
			       int objc, Tcl_Obj *objv[])
{
  ain_info_t *info = (ain_info_t *) data;
  info->interval_ms = 0;

  /* printf("stopping analog acquisition\n"); */

#ifdef __linux__
  struct itimerspec new_value;
  
  new_value.it_value.tv_sec = 0;
  new_value.it_value.tv_nsec = 0;
  new_value.it_interval.tv_sec = 0;
  new_value.it_interval.tv_nsec = 0;
  timerfd_settime(info->timer_fd, TFD_TIMER_ABSTIME, &new_value, NULL);
#endif
  return TCL_OK;
}

/*
 * ainSetPrefix <prefix>
 *
 * Change the dpoint prefix used for published datapoints. The module
 * publishes "<prefix>/vals" from the acquisition thread; changing the
 * prefix takes effect on the next sample.
 *
 * This is the hook that lets a consumer subsystem (e.g. a dedicated
 * slider subprocess) own its own datapoint namespace instead of
 * contending with the historical "ain/vals" convention.
 */
static int ain_set_prefix_command (ClientData data, Tcl_Interp *interp,
                                   int objc, Tcl_Obj *objv[])
{
  ain_info_t *info = (ain_info_t *) data;

  if (objc != 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "prefix");
    return TCL_ERROR;
  }

  Tcl_Size len;
  const char *new_prefix = Tcl_GetStringFromObj(objv[1], &len);
  if (len <= 0 || len > 64) {
    Tcl_AppendResult(interp, "prefix must be 1..64 characters", NULL);
    return TCL_ERROR;
  }

  char *copy = (char *) malloc(len + 1);
  if (!copy) {
    Tcl_AppendResult(interp, "out of memory", NULL);
    return TCL_ERROR;
  }
  memcpy(copy, new_prefix, len + 1);

  pthread_mutex_lock(&info->prefix_mutex);
  char *old = info->dpoint_prefix;
  info->dpoint_prefix = copy;
  pthread_mutex_unlock(&info->prefix_mutex);

  /* Bump version so the acquire thread picks up the new name */
  atomic_fetch_add(&info->prefix_version, 1);

  free(old);
  return TCL_OK;
}

/*
 * ainGetInfo
 *
 * Return the current acquisition state as a Tcl dict:
 *   nchan        (int)   currently active channel count
 *   interval_ms  (int)   timer interval (0 if stopped)
 *   prefix       (str)   current dpoint prefix
 *   hardware     (bool)  1 if SPI device is open, 0 for simulation
 */
static int ain_get_info_command (ClientData data, Tcl_Interp *interp,
                                 int objc, Tcl_Obj *objv[])
{
  ain_info_t *info = (ain_info_t *) data;

  if (objc != 1) {
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  Tcl_Obj *dict = Tcl_NewDictObj();

  Tcl_DictObjPut(interp, dict,
                 Tcl_NewStringObj("nchan", -1),
                 Tcl_NewIntObj(info->nchan));
  Tcl_DictObjPut(interp, dict,
                 Tcl_NewStringObj("interval_ms", -1),
                 Tcl_NewIntObj(info->interval_ms));

  pthread_mutex_lock(&info->prefix_mutex);
  Tcl_DictObjPut(interp, dict,
                 Tcl_NewStringObj("prefix", -1),
                 Tcl_NewStringObj(info->dpoint_prefix, -1));
  pthread_mutex_unlock(&info->prefix_mutex);

  Tcl_DictObjPut(interp, dict,
                 Tcl_NewStringObj("hardware", -1),
                 Tcl_NewIntObj(info->fd >= 0 ? 1 : 0));

  Tcl_SetObjResult(interp, dict);
  return TCL_OK;
}

static int ain_invert_signal_command (ClientData data, Tcl_Interp *interp,
				      int objc, Tcl_Obj *objv[])
{
  ain_info_t *info = (ain_info_t *) data;
  int chan, invert, old;
  if (objc != 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "chan invert?");
    return TCL_ERROR;
  }
  
  if (Tcl_GetIntFromObj(interp, objv[1], &chan) != TCL_OK)
    return TCL_ERROR;
  if (chan < 0 || chan >= MAX_CHAN) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		     ": channel out of range",
		     NULL);
    return TCL_ERROR;
  }
  if (Tcl_GetIntFromObj(interp, objv[2], &invert) != TCL_OK)
    return TCL_ERROR;
  invert = (invert != 0);

  old = info->invert_signals[chan];
  info->invert_signals[chan] = invert;

  Tcl_SetObjResult(interp, Tcl_NewIntObj(old));
  
  return TCL_OK;
}

/*****************************************************************************
 * EXPORT
 *****************************************************************************/

#ifdef WIN32
EXPORT(int,Dserv_ain_Init) (Tcl_Interp *interp)
#else
int Dserv_ain_Init(Tcl_Interp *interp)
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

  g_ainInfo.tclserver = tclserver_get_from_interp(interp);
  g_ainInfo.dpoint_prefix = malloc(strlen(DEFAULT_ADC_DPOINT_PREFIX)+1);
  strcpy(g_ainInfo.dpoint_prefix, DEFAULT_ADC_DPOINT_PREFIX);

  pthread_mutex_init(&g_ainInfo.prefix_mutex, NULL);
  atomic_store(&g_ainInfo.prefix_version, 0);

  /* Default channel count for both hardware and simulation paths. The
   * hardware init block below may overwrite this, but having a sane
   * default up-front means ainSetNchan isn't required in simulation. */
  g_ainInfo.nchan = 2;
  for (int i = 0; i < MAX_CHAN; i++) g_ainInfo.invert_signals[i] = 0;

#ifdef __linux__
  /*
   * could do this in another function, but for now, just default to
   * the SPIDEV_PATH set above
   */
  g_ainInfo.fd = open(SPIDEV_PATH, O_RDWR);
  if (g_ainInfo.fd >= 0) {
    int status;
    static uint8_t mode = 0;
    status = ioctl(g_ainInfo.fd, SPI_IOC_WR_MODE, &mode);
    if (status == -1) return TCL_ERROR;
    
    static uint8_t bits = 8;
    status = ioctl(g_ainInfo.fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    if (status == -1) return TCL_ERROR;

    static uint32_t speed = 1000000;
    status = ioctl(g_ainInfo.fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
    if (status == -1) return TCL_ERROR;

    g_ainInfo.timer_fd = timerfd_create(CLOCK_REALTIME, 0);
    if (g_ainInfo.timer_fd == -1) return TCL_ERROR;

    /* nchan and invert_signals were already initialized above
     * (they apply to both hardware and simulation paths) */

    if (pthread_create(&g_ainInfo.timer_thread_id, NULL, acquire_thread,
		       (void *) &g_ainInfo)) {
      return TCL_ERROR;
    }
    pthread_detach(g_ainInfo.timer_thread_id);
  }
#else
  g_ainInfo.fd = -1;
  g_ainInfo.timer_fd = -1;
#endif

  Tcl_CreateObjCommand(interp, "ainSetNchan",
		       (Tcl_ObjCmdProc *) ain_set_nchan_command,
		       (ClientData) &g_ainInfo,
		       (Tcl_CmdDeleteProc *) NULL);
  
  Tcl_CreateObjCommand(interp, "ainStart",
		       (Tcl_ObjCmdProc *) ain_start_command,
		       (ClientData) &g_ainInfo,
		       (Tcl_CmdDeleteProc *) NULL);
  
  Tcl_CreateObjCommand(interp, "ainStop",
		       (Tcl_ObjCmdProc *) ain_stop_command,
		       (ClientData) &g_ainInfo,
		       (Tcl_CmdDeleteProc *) NULL);

  Tcl_CreateObjCommand(interp, "ainInvertSignal",
		       (Tcl_ObjCmdProc *) ain_invert_signal_command,
		       (ClientData) &g_ainInfo,
		       (Tcl_CmdDeleteProc *) NULL);

  Tcl_CreateObjCommand(interp, "ainSetPrefix",
		       (Tcl_ObjCmdProc *) ain_set_prefix_command,
		       (ClientData) &g_ainInfo,
		       (Tcl_CmdDeleteProc *) NULL);

  Tcl_CreateObjCommand(interp, "ainGetInfo",
		       (Tcl_ObjCmdProc *) ain_get_info_command,
		       (ClientData) &g_ainInfo,
		       (Tcl_CmdDeleteProc *) NULL);

  return TCL_OK;
}
