#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include <Datapoint.h>
#include <dpoint_process.h>
#include "prmutil.h"

enum { SAMPLER_INACTIVE, SAMPLER_ACTIVE };
enum { SAMPLER_ONESHOT, SAMPLER_LOOP };
enum { OP_MEAN, OP_MIN, OP_MAX, OP_MINMAX };

#define MAX_CHANNELS 8
#define MAX_SAMPLES 10000

typedef struct process_params_s {
  /* Configuration */
  int active;                /* is sampler active? */
  int loop;                  /* oneshot or continuous */
  int operation;             /* which operation to perform */
  int nchannels;             /* number of channels to sample */
  int sample_count;          /* total samples to collect */
  int status_pending;        /* change status on next update? */
  
  /* Runtime state */
  int current_count;         /* samples collected so far */
  uint16_t *samples;         /* sample buffer */

  /* Sample rate tracking */
  int track_rate;            /* enable/disable */
  uint64_t first_sample_time;   
  uint64_t last_sample_time;    
  int rate_sample_count;        
  float current_rate;           
  int rate_update_interval;
  
  /* Output datapoints */
  ds_datapoint_t vals_dpoint;    /* computed values output */
  ds_datapoint_t status_dpoint;  /* status updates */
  ds_datapoint_t rate_dpoint;    /* sample_rate updates */
  
  /* For parameter utility */
  int dummyInt;
} process_params_t;

void *newProcessParams(void)
{
  process_params_t *p = calloc(1, sizeof(process_params_t));
  
  /* Default configuration */
  p->active = SAMPLER_INACTIVE;
  p->loop = SAMPLER_ONESHOT;
  p->operation = OP_MEAN;
  p->nchannels = 2;
  p->sample_count = 100;
  p->current_count = 0;
  p->status_pending = 0;
  p->track_rate = 0;
  p->rate_update_interval = 50;
  p->current_rate = 0.0;
  p->rate_sample_count = 0;
  
  /* Allocate sample buffer */
  p->samples = (uint16_t *) calloc(MAX_SAMPLES * MAX_CHANNELS, sizeof(uint16_t));
  
  /* Output for computed values */
  p->vals_dpoint.flags = 0;
  p->vals_dpoint.varname = strdup("proc/sampler/vals");
  p->vals_dpoint.varlen = strlen(p->vals_dpoint.varname);
  p->vals_dpoint.data.type = DSERV_FLOAT;
  p->vals_dpoint.data.len = MAX_CHANNELS * sizeof(float);
  p->vals_dpoint.data.buf = malloc(p->vals_dpoint.data.len);
  
  /* Status output (0=sampling, 1=complete) */
  p->status_dpoint.flags = 0;
  p->status_dpoint.varname = strdup("proc/sampler/status");
  p->status_dpoint.varlen = strlen(p->status_dpoint.varname);
  p->status_dpoint.data.type = DSERV_INT;
  p->status_dpoint.data.len = sizeof(int);
  p->status_dpoint.data.buf = malloc(p->status_dpoint.data.len);

  /* Output for sample rate */
  p->rate_dpoint.flags = 0;
  p->rate_dpoint.varname = strdup("proc/sampler/rate");
  p->rate_dpoint.varlen = strlen(p->rate_dpoint.varname);
  p->rate_dpoint.data.type = DSERV_FLOAT;
  p->rate_dpoint.data.len = sizeof(float);
  p->rate_dpoint.data.buf = malloc(p->rate_dpoint.data.len);
  
  return p;
}

void freeProcessParams(void *pstruct)
{
  process_params_t *p = (process_params_t *) pstruct;
  
  if (p->samples) free(p->samples);
  
  free(p->vals_dpoint.varname);
  free(p->vals_dpoint.data.buf);
  
  free(p->status_dpoint.varname);
  free(p->status_dpoint.data.buf);
  
  free(p->rate_dpoint.varname);
  free(p->rate_dpoint.data.buf);
  
  free(p);
}

int getProcessParams(dpoint_process_param_setting_t *pinfo)
{
  char *result_str;
  char *name = pinfo->pname;
  process_params_t *p = (process_params_t *) pinfo->params;
  
  PARAM_ENTRY params[] = {
    { "active",       &p->active,       &p->dummyInt, PU_INT },
    { "loop",         &p->loop,         &p->dummyInt, PU_INT },
    { "operation",    &p->operation,    &p->dummyInt, PU_INT },
    { "nchannels",    &p->nchannels,    &p->dummyInt, PU_INT },
    { "sample_count", &p->sample_count, &p->dummyInt, PU_INT },
    { "current_count",&p->current_count,&p->dummyInt, PU_INT },
    { "track_rate",   &p->track_rate,   &p->dummyInt, PU_INT },
    { "rate_update_interval", &p->rate_update_interval, &p->dummyInt, PU_INT },
    { "", NULL, NULL, PU_NULL }
  };
  
  result_str = puGetParamEntry(&params[0], name);
  if (result_str && pinfo->pval) {
    *pinfo->pval = result_str;
    return 1;
  }
  return 0;
}

int setProcessParams(dpoint_process_param_setting_t *pinfo)
{
  int result = DPOINT_PROCESS_IGNORE;
  char *name = pinfo->pname;
  char **vals = pinfo->pval;
  process_params_t *p = (process_params_t *) pinfo->params;
  
  /* Special handling for dpoint name change */
  if (!strcmp(name, "dpoint")) {
    if (p->vals_dpoint.varname) free(p->vals_dpoint.varname);
    p->vals_dpoint.varname = malloc(strlen(vals[0]) + 6);
    sprintf(p->vals_dpoint.varname, "%s/vals", vals[0]);
    p->vals_dpoint.varlen = strlen(p->vals_dpoint.varname);
    
    if (p->status_dpoint.varname) free(p->status_dpoint.varname);
    p->status_dpoint.varname = malloc(strlen(vals[0]) + 8);
    sprintf(p->status_dpoint.varname, "%s/status", vals[0]);
    p->status_dpoint.varlen = strlen(p->status_dpoint.varname);
    
    if (p->rate_dpoint.varname) free(p->rate_dpoint.varname);
    p->rate_dpoint.varname = malloc(strlen(vals[0]) + 6);
    sprintf(p->rate_dpoint.varname, "%s/rate", vals[0]);
    p->rate_dpoint.varlen = strlen(p->rate_dpoint.varname);
    
    return DPOINT_PROCESS_IGNORE;
  }

  /* Query current sample rate */
  if (!strcmp(name, "rate")) {
    memcpy(p->rate_dpoint.data.buf, &p->current_rate, sizeof(float));
    p->rate_dpoint.timestamp = pinfo->timestamp;
    pinfo->dpoint = &p->rate_dpoint;
    return DPOINT_PROCESS_DSERV;
  }

  /* Query current status */
  if (!strcmp(name, "status")) {
    /* Return the current status */
    int current_status;
    if (!p->active) {
      /* If inactive, check if we have completed samples */
      current_status = (p->current_count > 0) ? 1 : 0;
    } else {
      /* If active, we're still sampling */
      current_status = 0;
    }
    
    memcpy(p->status_dpoint.data.buf, &current_status, sizeof(int));
    p->status_dpoint.timestamp = pinfo->timestamp;
    pinfo->dpoint = &p->status_dpoint;
    return DPOINT_PROCESS_DSERV;
  }
  
  /* Handle start command */
  if (!strcmp(name, "start")) {
    if (!p->active) {
      p->active = SAMPLER_ACTIVE;
      p->current_count = 0;
      
      /* Signal that sampling has started (status = 0) */
      int status = 0;
      memcpy(p->status_dpoint.data.buf, &status, sizeof(int));
      p->status_dpoint.timestamp = pinfo->timestamp;
      pinfo->dpoint = &p->status_dpoint;
      return DPOINT_PROCESS_DSERV;
    }
    return DPOINT_PROCESS_IGNORE;
  }

  /* Handle stop command */
  if (!strcmp(name, "stop")) {
    p->active = SAMPLER_INACTIVE;
    return DPOINT_PROCESS_IGNORE;
  }

  /* Regular parameter updates */
  int was_active = p->active;
  
  PARAM_ENTRY params[] = {
    { "active",       &p->active,       &p->dummyInt, PU_INT },
    { "loop",         &p->loop,         &p->dummyInt, PU_INT },
    { "operation",    &p->operation,    &p->dummyInt, PU_INT },
    { "nchannels",    &p->nchannels,    &p->dummyInt, PU_INT },
    { "sample_count", &p->sample_count, &p->dummyInt, PU_INT },
    { "track_rate",   &p->track_rate,   &p->dummyInt, PU_INT },
    { "rate_update_interval", &p->rate_update_interval, &p->dummyInt, PU_INT },
    { "", NULL, NULL, PU_NULL }
  };
  
  if (puSetParamEntry(&params[0], name, 1, vals)) {
    /* Validate parameters */
    if (p->nchannels < 1) p->nchannels = 1;
    if (p->nchannels > MAX_CHANNELS) p->nchannels = MAX_CHANNELS;
    if (p->sample_count < 1) p->sample_count = 1;
    if (p->sample_count > MAX_SAMPLES) p->sample_count = MAX_SAMPLES;
    if (p->rate_update_interval < 1) p->rate_update_interval = 1;
    
    /* Reset if just activated */
    if (!was_active && p->active) {
      p->current_count = 0;
    }
    
    /* Reset rate tracking if just enabled */
    if (!strcmp(name, "track_rate") && p->track_rate) {
      p->rate_sample_count = 0;
      p->current_rate = 0.0;
    }
    
    result = DPOINT_PROCESS_IGNORE;
  }
  
  return result;
}

static void compute_operation(process_params_t *p)
{
  float *results = (float *) p->vals_dpoint.data.buf;
  
  switch (p->operation) {
  case OP_MEAN:
    {
      /* Calculate means for each channel */
      for (int c = 0; c < p->nchannels; c++) {
        uint64_t sum = 0;
        for (int i = 0; i < p->sample_count; i++) {
          sum += p->samples[i * p->nchannels + c];
        }
        results[c] = (float) sum / p->sample_count;
      }
    }
    break;
    
  case OP_MIN:
    {
      /* Find minimum for each channel */
      for (int c = 0; c < p->nchannels; c++) {
        uint16_t min = 0xFFFF;
        for (int i = 0; i < p->sample_count; i++) {
          uint16_t val = p->samples[i * p->nchannels + c];
          if (val < min) min = val;
        }
        results[c] = (float) min;
      }
    }
    break;
    
  case OP_MAX:
    {
      /* Find maximum for each channel */
      for (int c = 0; c < p->nchannels; c++) {
        uint16_t max = 0;
        for (int i = 0; i < p->sample_count; i++) {
          uint16_t val = p->samples[i * p->nchannels + c];
          if (val > max) max = val;
        }
        results[c] = (float) max;
      }
    }
    break;
    
  case OP_MINMAX:
    {
      /* Store min and max for each channel (2x channels output) */
      for (int c = 0; c < p->nchannels; c++) {
        uint16_t min = 0xFFFF, max = 0;
        for (int i = 0; i < p->sample_count; i++) {
          uint16_t val = p->samples[i * p->nchannels + c];
          if (val < min) min = val;
          if (val > max) max = val;
        }
        results[c*2] = (float) min;
        results[c*2 + 1] = (float) max;
      }
    }
    break;
  }
}

int onProcess(dpoint_process_info_t *pinfo, void *params)
{
  process_params_t *p = (process_params_t *) params;

  /* Check if we have a pending status update first */
  if (p->status_pending) {
    p->status_dpoint.timestamp = pinfo->input_dpoint->timestamp;
    pinfo->dpoint = &p->status_dpoint;
    p->status_pending = 0;

    return DPOINT_PROCESS_DSERV;
  }
  
  /* Track sample rate for ALL incoming samples (not just when actively sampling) */
  if (p->track_rate) {
    /* First sample in this rate window */
    if (p->rate_sample_count == 0) {
      p->first_sample_time = pinfo->input_dpoint->timestamp;
    }
    
    p->last_sample_time = pinfo->input_dpoint->timestamp;
    p->rate_sample_count++;
    
    /* Update rate periodically */
    if (p->rate_sample_count >= p->rate_update_interval && p->rate_sample_count > 1) {
      uint64_t time_diff = p->last_sample_time - p->first_sample_time;
      if (time_diff > 0) {
        /* Calculate rate in Hz (timestamps are in microseconds) */
        p->current_rate = (float)(p->rate_sample_count - 1) * 1000000.0 / time_diff;
        memcpy(p->rate_dpoint.data.buf, &p->current_rate, sizeof(float));
      }
      
      /* Reset window for next rate calculation */
      p->first_sample_time = p->last_sample_time;
      p->rate_sample_count = 1;
    }
  }
  
  /* Check if sampler is active */
  if (!p->active)
    return DPOINT_PROCESS_IGNORE;
  
  /* Validate input data */
  if (pinfo->input_dpoint->data.type != DSERV_SHORT ||
      pinfo->input_dpoint->data.len < p->nchannels * sizeof(uint16_t))
    return DPOINT_PROCESS_IGNORE;
  
  uint16_t *ain_vals = (uint16_t *) pinfo->input_dpoint->data.buf;
  
  /* Store the samples */
  memcpy(&p->samples[p->current_count * p->nchannels],
         ain_vals,
         p->nchannels * sizeof(uint16_t));
  p->current_count++;
  
  /* Check if we have enough samples */
  if (p->current_count >= p->sample_count) {
    /* Compute the operation */
    compute_operation(p);
    
    /* Set up output datapoint */
    p->vals_dpoint.timestamp = pinfo->input_dpoint->timestamp;
    if (p->operation == OP_MINMAX) {
      p->vals_dpoint.data.len = p->nchannels * 2 * sizeof(float);
    } else {
      p->vals_dpoint.data.len = p->nchannels * sizeof(float);
    }
    pinfo->dpoint = &p->vals_dpoint;
    
    /* Update status to indicate completion */
    int status = 1;
    memcpy(p->status_dpoint.data.buf, &status, sizeof(int));
    p->status_pending = 1;
    
    /* Reset for next round */
    p->current_count = 0;
    if (!p->loop) {
      p->active = SAMPLER_INACTIVE;
    }

    return DPOINT_PROCESS_DSERV;
  }
  
  return DPOINT_PROCESS_IGNORE;
}
