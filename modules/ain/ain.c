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
} ain_info_t;

/* global to this module */
static ain_info_t g_ainInfo;

#ifdef __linux__
int spi_transfer(int fd, unsigned char send[], unsigned char receive[], int length) {
  struct spi_ioc_transfer	transfer;
  transfer.tx_buf = (unsigned long) send;
  transfer.rx_buf = (unsigned long) receive;
  transfer.len = length;
  transfer.speed_hz = 0;
  transfer.delay_usecs = 0;
  transfer.bits_per_word = 0;
  transfer.cs_change = 0;
  transfer.tx_nbits = 0;
  transfer.rx_nbits = 0;
  transfer.pad = 0;
  int status = ioctl(fd, SPI_IOC_MESSAGE(1), &transfer);
  if (status < 0) {
    perror("SPI: Transfer SPI_IOC_MESSAGE Failed");
    return -1;
  }
  return status;
}

int mcp3204_read(int fd, int nchan, uint16_t buf[])
{
  uint8_t send[3], receive[3];
  send[0] = 0b00000110;     // Reading single-ended input from channel 0

  for(int i=0; i<nchan; i++) {
    send[1] = (i << 6);
    spi_transfer(fd, send, receive, 3);
    buf[i] = (uint16_t) ((receive[1]&0b00001111)<<8)|receive[2];
  }
  return 0;
}

void *acquire_thread(void *arg)
{
  ain_info_t *info = (ain_info_t *) arg;

  uint64_t exp;
  ssize_t s;
  
  ds_datapoint_t adc_dpoint;
  uint16_t vals[4] = {};
  const int max_val = (1 << 12);
  
  int name_sz = strlen(info->dpoint_prefix)+16;
  char *adc_point_name = (char *) malloc(name_sz);
  snprintf(adc_point_name, name_sz, "%s/vals", info->dpoint_prefix);

  while (1) {
    s = read(info->timer_fd, &exp, sizeof(uint64_t));
    if (s == sizeof(uint64_t)) {
      
      mcp3204_read(info->fd, info->nchan, vals);
      for (int i = 0; i < info->nchan; i++) {
	if (info->invert_signals[i]) vals[i] = max_val-vals[i];
      }
      
      /* fill the data point */
      ds_datapoint_t *dp = dpoint_new(adc_point_name,
				      tclserver_now(info->tclserver),
				      DSERV_SHORT,
				      sizeof(uint16_t)*info->nchan,
				      (unsigned char *) vals);

      /* update would be better than set here (after the first set) */
      tclserver_set_point(info->tclserver, dp);
    }
  }

  free(adc_point_name);		/* buffer allocated above */
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

  g_ainInfo.tclserver = tclserver_get();
  g_ainInfo.dpoint_prefix = malloc(strlen(DEFAULT_ADC_DPOINT_PREFIX)+1);
  strcpy(g_ainInfo.dpoint_prefix, DEFAULT_ADC_DPOINT_PREFIX);

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
    
    g_ainInfo.nchan = 2;
    for (int i = 0; i < MAX_CHAN; i++) g_ainInfo.invert_signals[i] = 0;
    
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

  return TCL_OK;
}
