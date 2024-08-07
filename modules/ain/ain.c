/*
 * NAME
 *   ain.c
 *
 * DESCRIPTION
 *   This module acquires analog signals from an MCP320x chip using the SPI
 * bus using a periodic timer and sends the acquired data point to dserv.
 *
 *   Sampling is also supported, by providing sample aggregate statistics for
 * multiple samples.
 *
 * DPOINTS
 *   uint16_t ${PREFIX}/vals
 *   int      ${PREFIX}/interval_ms
 *   float    ${PREFIX}/sampler/${ID}/vals
 *   int      ${PREFIX}/sampler/${ID}/set
 *
 * AUTHOR
 *   DLS, 06/24-08/24
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

typedef enum  {
  SampleMean,
  SampleMin,
  SampleMax,
  SampleMinMax,
} SamplerOp;

/*
 * A sampler holds info about the desired number of samples,
 * current count, and the samples themselves.  An active sampler
 * can be stopped or reset, but can't re-sample if active.
 */
typedef struct sampler_s
{
  volatile sig_atomic_t active; /* currently sampling?        */
  int nchannels;		/* how many channels to track */
  int loop;			/* oneshot (0) or repeat (1)  */
  int current_count;		/* #samples we've acq */
  int sample_count;		/* number of samples to track */
  uint16_t *samples;		/* array of raw samples       */
  SamplerOp op;			/* computation done on sample */
  char *sample_dpoint_name_vals;/* dserv sampler vals         */
  char *sample_dpoint_name_status; /* dserv sampler status    */
} sampler_t;

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
  pthread_mutex_t sampler_mutex;/* avoid collisions         */
  int maxsamplers;		/* max samplers allowed     */
  sampler_t **samplers;		/* pointers to all samplers */
} ain_info_t;

/* global to this module */
static ain_info_t g_ainInfo;


/***********************************************************/
/*                   Sampler Support                       */
/***********************************************************/

static sampler_t *sampler_create(int id, int nsamples, int nchan,
				 int loop, SamplerOp op,
				 char *prefix)
{
  sampler_t *s = (sampler_t *) calloc(1, sizeof(sampler_t));
  s->active = 0;
  s->nchannels = nchan;
  s->loop = loop;
  s->current_count = 0;
  s->sample_count = nsamples;
  s->samples =
    (uint16_t *) calloc(nsamples, nchan*sizeof(uint16_t));
  s->op = op;

  /* enough space for both /vals and /set name strings */
  int pointname_sz = strlen(prefix)+32;

  s->sample_dpoint_name_vals = (char *) malloc(pointname_sz);
  snprintf(s->sample_dpoint_name_vals, pointname_sz,
	   "%s/samplers/%d/vals", prefix, id);

  s->sample_dpoint_name_status = (char *) malloc(pointname_sz);
  snprintf(s->sample_dpoint_name_status, pointname_sz,
	   "%s/samplers/%d/status", prefix, id);
  
  return s;
}

static void sampler_destroy(sampler_t *s)
{
  if (s->sample_dpoint_name_vals) free(s->sample_dpoint_name_vals);
  if (s->sample_dpoint_name_status) free(s->sample_dpoint_name_status);
  if (s->samples) free(s->samples);
  free(s);
}

static int sampler_add(ain_info_t *info,
		       sampler_t *sampler,
		       int slot)
{
  int result = 0;

  if (slot < 0 || slot >= info->maxsamplers)
    return -1;
  
  pthread_mutex_lock(&info->sampler_mutex);
  if (info->samplers[slot]) {
    sampler_destroy(info->samplers[slot]);
    result = 1;			/* indicate old sampler was replaced */
  }
  info->samplers[slot] = sampler;
  pthread_mutex_unlock(&info->sampler_mutex);
  return result;
}

static int sampler_remove(ain_info_t *info, int id)
{
  if (id < 0 || id >= info->maxsamplers) return -1;
  if (info->samplers[id]) {
    pthread_mutex_lock(&info->sampler_mutex);
    sampler_destroy(info->samplers[id]);
    info->samplers[id] = NULL;
    pthread_mutex_unlock(&info->sampler_mutex);
    return 1;
  }
  return 0;
}

static void sampler_remove_all(ain_info_t *info)
{
  for (int i = 0; i < info->maxsamplers; i++) {
    sampler_remove(info, i);
  }
}

/*
 * set status in appropriate dserv variable
 */
static void sampler_set_dserv_status(ain_info_t *info,
				    sampler_t *s,
				    int status)
{
  ds_datapoint_t *dp = dpoint_new(s->sample_dpoint_name_status,
				  tclserver_now(info->tclserver),
				  DSERV_INT,
				  sizeof(int), 
				  (unsigned char *) &status);
  tclserver_set_point(info->tclserver, dp);
}

/*
 * samples acquired, perform the operation and forward result
 */
static int sampler_do_op(ain_info_t *info, sampler_t *s)
{
  if (s->current_count < s->sample_count) return 0;

  switch(s->op) {
  case SampleMean:
    {
      
      float means[4];
      uint64_t sums[4] = {0,0,0,0};
      for (int i = 0; i < s->sample_count; i++) {
	for (int c = 0; c < s->nchannels; c++) {
	  sums[c] += s->samples[i*s->nchannels+c];
	}
      }
      for (int c = 0; c < s->nchannels; c++) {
	means[c] = (float) (sums[c]/(double) s->sample_count);
      }

      /* push the means to dserv */
      ds_datapoint_t *dp = dpoint_new(s->sample_dpoint_name_vals,
				      tclserver_now(info->tclserver),
				      DSERV_FLOAT,
				      sizeof(float)*s->nchannels,
				      (unsigned char *) means);
      tclserver_set_point(info->tclserver, dp);

      /* and set flag that it's been updated */
      sampler_set_dserv_status(info, s, 1);
    }
    break;
  default:
    break;
  }
  return 0;
}

/*
 * add new sample to this sampler
 */
static int sampler_new_sample(ain_info_t *info,
			      sampler_t *s,
			      uint16_t vals[])
{
  int result = 0;
  if (!s->active || s->current_count == s->sample_count) return 0;

  /* move new samples in */
  memcpy(&s->samples[s->current_count*s->nchannels],
	 vals,
	 sizeof(uint16_t)*s->nchannels);
  s->current_count++;

  /* if have sample_count samples, do op and reset */
  if (s->current_count >= s->sample_count) {
    sampler_do_op(info, s);
    s->current_count = 0;
    if (!s->loop) s->active = 0;
    result = 1;
  }
  return result;
}

static int sampler_process(ain_info_t *info, uint16_t vals[])
{
  pthread_mutex_lock(&info->sampler_mutex);
  for (int i = 0; i < info->maxsamplers; i++) {
    if (info->samplers[i] && info->samplers[i]->active) {
      sampler_new_sample(info, info->samplers[i], vals);
    }
  }
  pthread_mutex_unlock(&info->sampler_mutex);        
  
  return 0;
}


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
  uint16_t vals[4];
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
      tclserver_set_point(info->tclserver, dp);

      /* handle any active sample processing requests */
      sampler_process(info, vals);
    }
  }

  free(adc_point_name);		/* buffer allocated above */
}

#endif

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

static int ain_sampler_add_command (ClientData data, Tcl_Interp *interp,
				    int objc, Tcl_Obj *objv[])
{
  ain_info_t *info = (ain_info_t *) data;
  SamplerOp op = SampleMean;
  char *name;
  int nchannels;
  int nsamples;
  int loop = 0;
  int slot;
  
  if (objc < 4) {
    Tcl_WrongNumArgs(interp, 1, objv, "slot nchannels nsamples ?loop?");
    return TCL_ERROR;
  }
  if (Tcl_GetIntFromObj(interp, objv[1], &slot) != TCL_OK)
    return TCL_ERROR;
  if (Tcl_GetIntFromObj(interp, objv[2], &nchannels) != TCL_OK)
    return TCL_ERROR;
  if (Tcl_GetIntFromObj(interp, objv[3], &nsamples) != TCL_OK)
    return TCL_ERROR;
  
  if (slot < 0 || slot >= info->maxsamplers) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		     ": sampler slot out of range",
		     NULL);
    return TCL_ERROR;
  }

  if (nsamples <= 0) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		     ": nsamples out of range",
		     NULL);
    return TCL_ERROR;
  }

  /* quietly fail if we are on a system without an ADC */
  if (info->nchan == 0) {
    return TCL_OK;
  }
    
  if (nchannels < 0 || nchannels > info->nchan) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		     ": nchannels out of range",
		     NULL);
    return TCL_ERROR;
  }
  
  sampler_t *s = sampler_create(slot, nsamples, nchannels, loop, op,
				info->dpoint_prefix);
  
  if (!s) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		     ": error creating sampler",
		     NULL);
    return TCL_ERROR;
  }
  
  int result = sampler_add(info, s, slot);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(result));
  return TCL_OK;
}

static int ain_sampler_remove_command (ClientData data, Tcl_Interp *interp,
				       int objc, Tcl_Obj *objv[])
{
  ain_info_t *info = (ain_info_t *) data;
  int slot = -1;
  int result;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "slot");
    return TCL_ERROR;
  }
  if (Tcl_GetIntFromObj(interp, objv[1], &slot) != TCL_OK)
    return TCL_ERROR;

  if (slot < 0 || slot >= info->maxsamplers) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		     ": sampler slot out of range",
		     NULL);
    return TCL_ERROR;
  }

  result = sampler_remove(info, slot);
  
  Tcl_SetObjResult(interp, Tcl_NewIntObj(result));
  return TCL_OK;
}

static int ain_sampler_start_command (ClientData data, Tcl_Interp *interp,
				      int objc, Tcl_Obj *objv[])
{
  ain_info_t *info = (ain_info_t *) data;
  int slot = -1;
  int result;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "slot");
    return TCL_ERROR;
  }
  if (Tcl_GetIntFromObj(interp, objv[1], &slot) != TCL_OK)
    return TCL_ERROR;

  if (slot < 0 || slot >= info->maxsamplers) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		     ": sampler slot out of range",
		     NULL);
    return TCL_ERROR;
  }

  if (!info->samplers[slot]) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		     ": sampler slot not set",
		     NULL);
    return TCL_ERROR;
  }

  /* quietly fail if we are on a system without an ADC */
  if (info->nchan == 0) {
    return TCL_OK;
  }
    
  if (!info->samplers[slot]->active) {
    /* not active, so shouldn't need to be protected by mutex */
    info->samplers[slot]->current_count = 0;

    /* not protected by mutex, but as it's atomic, should be OK */
    info->samplers[slot]->active = 1;

    /* and set flag that it's not yet updated */
    sampler_set_dserv_status(info, info->samplers[slot], 0);
    
    result = 1;
  }
  else {
    result = 0;			/* already active */
  }

  Tcl_SetObjResult(interp, Tcl_NewIntObj(result));
  return TCL_OK;
}

static int ain_sampler_stop_command (ClientData data, Tcl_Interp *interp,
				     int objc, Tcl_Obj *objv[])
{
  ain_info_t *info = (ain_info_t *) data;
  int slot = -1;
  int result;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "slot");
    return TCL_ERROR;
  }
  if (Tcl_GetIntFromObj(interp, objv[1], &slot) != TCL_OK)
    return TCL_ERROR;
  
  if (slot < 0 || slot >= info->maxsamplers) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		     ": sampler slot out of range",
		     NULL);
    return TCL_ERROR;
  }
  
  if (!info->samplers[slot]) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		     ": sampler slot not set",
		     NULL);
    return TCL_ERROR;
  }
  if (info->samplers[slot]->active) {
    /* not protected by mutex, but as it's atomic, should be OK */
    info->samplers[slot]->active = 0;
    result = 1;
  }
  else {
    result = 0;			/* already active */
  }

  Tcl_SetObjResult(interp, Tcl_NewIntObj(result));
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
      Tcl_InitStubs(interp, "8.6", 0)
#else
      Tcl_PkgRequire(interp, "Tcl", "8.6", 0)
#endif
      == NULL) {
    return TCL_ERROR;
  }

  g_ainInfo.tclserver = tclserver_get();
  g_ainInfo.dpoint_prefix = malloc(strlen(DEFAULT_ADC_DPOINT_PREFIX)+1);
  strcpy(g_ainInfo.dpoint_prefix, DEFAULT_ADC_DPOINT_PREFIX);

  int maxsamplers = 8;
  g_ainInfo.maxsamplers = maxsamplers;
  g_ainInfo.samplers = (sampler_t **) calloc(g_ainInfo.maxsamplers,
					     sizeof(sampler_t *));
  pthread_mutex_init(&g_ainInfo.sampler_mutex, NULL);
    
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

  Tcl_CreateObjCommand(interp, "ainSamplerAdd",
		       (Tcl_ObjCmdProc *) ain_sampler_add_command,
		       (ClientData) &g_ainInfo,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "ainSamplerRemove",
		       (Tcl_ObjCmdProc *) ain_sampler_remove_command,
		       (ClientData) &g_ainInfo,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "ainSamplerStart",
		       (Tcl_ObjCmdProc *) ain_sampler_start_command,
		       (ClientData) &g_ainInfo,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "ainSamplerStop",
		       (Tcl_ObjCmdProc *) ain_sampler_stop_command,
		       (ClientData) &g_ainInfo,
		       (Tcl_CmdDeleteProc *) NULL);

  return TCL_OK;
}
