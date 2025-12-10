#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include <Datapoint.h>
#include <dpoint_process.h>
#include "prmutil.h"

/*
 * Generic Sampler Processor
 * 
 * Collects samples from a datapoint stream and computes aggregates (mean, min, max).
 * Supports two modes:
 *   - Sample count mode: Collect N samples then compute
 *   - Time window mode: Collect samples over T seconds then compute
 * 
 * Auto-detects input type (DSERV_SHORT, DSERV_INT, or DSERV_FLOAT) on first sample.
 * 
 * Output datapoints:
 *   <name>/vals   - Computed aggregate values (float array)
 *   <name>/status - Sampling status (0=active, 1=complete)
 *   <name>/rate   - Current sample rate in Hz
 *   <name>/count  - Number of samples used in last computation
 */

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
  int sample_count;          /* samples mode: fixed count */
  float time_window;         /* time mode: duration in seconds */
  int use_time_window;       /* 0=sample count mode, 1=time window mode */
  int status_pending;        /* change status on next update? */
  int count_pending;         /* change count on next update? */
  
  /* Runtime state */
  int current_count;         /* samples collected so far */
  int last_computation_count; /* samples used in last computation */
  int input_type;            /* detected: DSERV_SHORT, DSERV_INT, or DSERV_FLOAT */
  int type_locked;           /* has type been detected yet? */
  uint16_t *samples_short;   /* sample buffer for uint16 */
  int32_t *samples_int;      /* sample buffer for int32 */
  float *samples_float;      /* sample buffer for float */
  
  /* Time window state */
  uint64_t window_start_time;

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
  ds_datapoint_t count_dpoint;   /* sample count output */
  
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
  p->time_window = 1.0;        /* default 1 second window */
  p->use_time_window = 0;      /* default to sample count mode */
  p->current_count = 0;
  p->last_computation_count = 0;
  p->status_pending = 0;
  p->count_pending = 0;
  p->track_rate = 0;
  p->rate_update_interval = 50;
  p->current_rate = 0.0;
  p->rate_sample_count = 0;
  
  /* Type detection state */
  p->input_type = -1;
  p->type_locked = 0;
  
  /* Allocate sample buffers for all supported types */
  p->samples_short = (uint16_t *) calloc(MAX_SAMPLES * MAX_CHANNELS, sizeof(uint16_t));
  p->samples_int = (int32_t *) calloc(MAX_SAMPLES * MAX_CHANNELS, sizeof(int32_t));
  p->samples_float = (float *) calloc(MAX_SAMPLES * MAX_CHANNELS, sizeof(float));
  
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
  
  /* Output for sample count used in computation */
  p->count_dpoint.flags = 0;
  p->count_dpoint.varname = strdup("proc/sampler/count");
  p->count_dpoint.varlen = strlen(p->count_dpoint.varname);
  p->count_dpoint.data.type = DSERV_INT;
  p->count_dpoint.data.len = sizeof(int);
  p->count_dpoint.data.buf = malloc(p->count_dpoint.data.len);
  
  return p;
}

void freeProcessParams(void *pstruct)
{
  process_params_t *p = (process_params_t *) pstruct;
  
  if (p->samples_short) free(p->samples_short);
  if (p->samples_int) free(p->samples_int);
  if (p->samples_float) free(p->samples_float);
  
  free(p->vals_dpoint.varname);
  free(p->vals_dpoint.data.buf);
  
  free(p->status_dpoint.varname);
  free(p->status_dpoint.data.buf);
  
  free(p->rate_dpoint.varname);
  free(p->rate_dpoint.data.buf);
  
  free(p->count_dpoint.varname);
  free(p->count_dpoint.data.buf);
  
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
    { "time_window",  &p->time_window,  &p->dummyInt, PU_FLOAT },
    { "use_time_window", &p->use_time_window, &p->dummyInt, PU_INT },
    { "current_count",&p->current_count,&p->dummyInt, PU_INT },
    { "last_computation_count",&p->last_computation_count,&p->dummyInt, PU_INT },
    { "track_rate",   &p->track_rate,   &p->dummyInt, PU_INT },
    { "rate_update_interval", &p->rate_update_interval, &p->dummyInt, PU_INT },
    { "input_type",   &p->input_type,   &p->dummyInt, PU_INT },
    { "type_locked",  &p->type_locked,  &p->dummyInt, PU_INT },
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
    
    if (p->count_dpoint.varname) free(p->count_dpoint.varname);
    p->count_dpoint.varname = malloc(strlen(vals[0]) + 7);
    sprintf(p->count_dpoint.varname, "%s/count", vals[0]);
    p->count_dpoint.varlen = strlen(p->count_dpoint.varname);
    
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
      current_status = (p->last_computation_count > 0) ? 1 : 0;
    } else {
      /* If active, we're still sampling */
      current_status = 0;
    }
    
    memcpy(p->status_dpoint.data.buf, &current_status, sizeof(int));
    p->status_dpoint.timestamp = pinfo->timestamp;
    pinfo->dpoint = &p->status_dpoint;
    return DPOINT_PROCESS_DSERV;
  }
  
  /* Query sample count from last computation */
  if (!strcmp(name, "count")) {
    memcpy(p->count_dpoint.data.buf, &p->last_computation_count, sizeof(int));
    p->count_dpoint.timestamp = pinfo->timestamp;
    pinfo->dpoint = &p->count_dpoint;
    return DPOINT_PROCESS_DSERV;
  }
  
  /* Handle start command */
  if (!strcmp(name, "start")) {
    if (!p->active) {
      p->active = SAMPLER_ACTIVE;
      p->current_count = 0;
      p->type_locked = 0;  /* Allow type re-detection on new start */
      /* window_start_time will be set on first sample */
      
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
    { "time_window",  &p->time_window,  &p->dummyInt, PU_FLOAT },
    { "use_time_window", &p->use_time_window, &p->dummyInt, PU_INT },
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
    if (p->time_window < 0.001) p->time_window = 0.001;  /* minimum 1ms */
    if (p->rate_update_interval < 1) p->rate_update_interval = 1;
    
    /* Reset if just activated */
    if (!was_active && p->active) {
      p->current_count = 0;
      p->type_locked = 0;  /* Allow type re-detection */
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
  int n_samples = p->current_count;  /* Use actual samples collected */
  
  if (n_samples == 0) return;  /* Safety check */
  
  if (p->input_type == DSERV_FLOAT) {
    /* Float-based operations */
    switch (p->operation) {
    case OP_MEAN:
      {
        for (int c = 0; c < p->nchannels; c++) {
          double sum = 0.0;  /* Use double for accumulation precision */
          for (int i = 0; i < n_samples; i++) {
            sum += p->samples_float[i * p->nchannels + c];
          }
          results[c] = (float)(sum / n_samples);
        }
      }
      break;
      
    case OP_MIN:
      {
        for (int c = 0; c < p->nchannels; c++) {
          float min = INFINITY;
          for (int i = 0; i < n_samples; i++) {
            float val = p->samples_float[i * p->nchannels + c];
            if (val < min) min = val;
          }
          results[c] = min;
        }
      }
      break;
      
    case OP_MAX:
      {
        for (int c = 0; c < p->nchannels; c++) {
          float max = -INFINITY;
          for (int i = 0; i < n_samples; i++) {
            float val = p->samples_float[i * p->nchannels + c];
            if (val > max) max = val;
          }
          results[c] = max;
        }
      }
      break;
      
    case OP_MINMAX:
      {
        for (int c = 0; c < p->nchannels; c++) {
          float min = INFINITY, max = -INFINITY;
          for (int i = 0; i < n_samples; i++) {
            float val = p->samples_float[i * p->nchannels + c];
            if (val < min) min = val;
            if (val > max) max = val;
          }
          results[c*2] = min;
          results[c*2 + 1] = max;
        }
      }
      break;
    }
  } else if (p->input_type == DSERV_INT) {
    /* int32_t-based operations */
    switch (p->operation) {
    case OP_MEAN:
      {
        for (int c = 0; c < p->nchannels; c++) {
          int64_t sum = 0;  /* Use int64 to avoid overflow */
          for (int i = 0; i < n_samples; i++) {
            sum += p->samples_int[i * p->nchannels + c];
          }
          results[c] = (float)sum / n_samples;
        }
      }
      break;
      
    case OP_MIN:
      {
        for (int c = 0; c < p->nchannels; c++) {
          int32_t min = INT32_MAX;
          for (int i = 0; i < n_samples; i++) {
            int32_t val = p->samples_int[i * p->nchannels + c];
            if (val < min) min = val;
          }
          results[c] = (float)min;
        }
      }
      break;
      
    case OP_MAX:
      {
        for (int c = 0; c < p->nchannels; c++) {
          int32_t max = INT32_MIN;
          for (int i = 0; i < n_samples; i++) {
            int32_t val = p->samples_int[i * p->nchannels + c];
            if (val > max) max = val;
          }
          results[c] = (float)max;
        }
      }
      break;
      
    case OP_MINMAX:
      {
        for (int c = 0; c < p->nchannels; c++) {
          int32_t min = INT32_MAX, max = INT32_MIN;
          for (int i = 0; i < n_samples; i++) {
            int32_t val = p->samples_int[i * p->nchannels + c];
            if (val < min) min = val;
            if (val > max) max = val;
          }
          results[c*2] = (float)min;
          results[c*2 + 1] = (float)max;
        }
      }
      break;
    }
  } else {
    /* uint16_t-based operations (DSERV_SHORT) */
    switch (p->operation) {
    case OP_MEAN:
      {
        for (int c = 0; c < p->nchannels; c++) {
          uint64_t sum = 0;
          for (int i = 0; i < n_samples; i++) {
            sum += p->samples_short[i * p->nchannels + c];
          }
          results[c] = (float)sum / n_samples;
        }
      }
      break;
      
    case OP_MIN:
      {
        for (int c = 0; c < p->nchannels; c++) {
          uint16_t min = 0xFFFF;
          for (int i = 0; i < n_samples; i++) {
            uint16_t val = p->samples_short[i * p->nchannels + c];
            if (val < min) min = val;
          }
          results[c] = (float)min;
        }
      }
      break;
      
    case OP_MAX:
      {
        for (int c = 0; c < p->nchannels; c++) {
          uint16_t max = 0;
          for (int i = 0; i < n_samples; i++) {
            uint16_t val = p->samples_short[i * p->nchannels + c];
            if (val > max) max = val;
          }
          results[c] = (float)max;
        }
      }
      break;
      
    case OP_MINMAX:
      {
        for (int c = 0; c < p->nchannels; c++) {
          uint16_t min = 0xFFFF, max = 0;
          for (int i = 0; i < n_samples; i++) {
            uint16_t val = p->samples_short[i * p->nchannels + c];
            if (val < min) min = val;
            if (val > max) max = val;
          }
          results[c*2] = (float)min;
          results[c*2 + 1] = (float)max;
        }
      }
      break;
    }
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
  
  /* Check if we have a pending count update */
  if (p->count_pending) {
    p->count_dpoint.timestamp = pinfo->input_dpoint->timestamp;
    pinfo->dpoint = &p->count_dpoint;
    p->count_pending = 0;

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
  
  /* Auto-detect input type on first sample */
  if (!p->type_locked) {
    if (pinfo->input_dpoint->data.type == DSERV_SHORT ||
        pinfo->input_dpoint->data.type == DSERV_INT ||
        pinfo->input_dpoint->data.type == DSERV_FLOAT) {
      p->input_type = pinfo->input_dpoint->data.type;
      p->type_locked = 1;
      
      /* Initialize time window if in time mode */
      if (p->use_time_window) {
        p->window_start_time = pinfo->input_dpoint->timestamp;
      }
    } else {
      /* Unsupported type - ignore */
      return DPOINT_PROCESS_IGNORE;
    }
  }
  
  /* Validate type matches what we locked in */
  if (pinfo->input_dpoint->data.type != p->input_type)
    return DPOINT_PROCESS_IGNORE;
  
  /* Validate data length based on detected type */
  int element_size;
  switch (p->input_type) {
    case DSERV_SHORT: element_size = sizeof(uint16_t); break;
    case DSERV_INT:   element_size = sizeof(int32_t); break;
    case DSERV_FLOAT: element_size = sizeof(float); break;
    default: return DPOINT_PROCESS_IGNORE;
  }
  
  if (pinfo->input_dpoint->data.len < p->nchannels * element_size)
    return DPOINT_PROCESS_IGNORE;
  
  /* Check buffer overflow */
  if (p->current_count >= MAX_SAMPLES)
    return DPOINT_PROCESS_IGNORE;
  
  /* Store the samples based on detected type */
  if (p->input_type == DSERV_FLOAT) {
    float *input_vals = (float *) pinfo->input_dpoint->data.buf;
    memcpy(&p->samples_float[p->current_count * p->nchannels],
           input_vals,
           p->nchannels * sizeof(float));
  } else if (p->input_type == DSERV_INT) {
    int32_t *input_vals = (int32_t *) pinfo->input_dpoint->data.buf;
    memcpy(&p->samples_int[p->current_count * p->nchannels],
           input_vals,
           p->nchannels * sizeof(int32_t));
  } else {
    uint16_t *input_vals = (uint16_t *) pinfo->input_dpoint->data.buf;
    memcpy(&p->samples_short[p->current_count * p->nchannels],
           input_vals,
           p->nchannels * sizeof(uint16_t));
  }
  p->current_count++;
  
  /* Check completion based on mode */
  int should_compute = 0;
  
  if (p->use_time_window) {
    /* Time window mode: check if enough time has elapsed */
    uint64_t elapsed_us = pinfo->input_dpoint->timestamp - p->window_start_time;
    float elapsed_sec = elapsed_us / 1000000.0;
    
    if (elapsed_sec >= p->time_window) {
      should_compute = 1;
    }
  } else {
    /* Sample count mode: check if we have enough samples */
    if (p->current_count >= p->sample_count) {
      should_compute = 1;
    }
  }
  
  if (should_compute) {
    /* Compute the operation using p->current_count samples */
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
    
    /* Update count with number of samples used */
    p->last_computation_count = p->current_count;
    memcpy(p->count_dpoint.data.buf, &p->last_computation_count, sizeof(int));
    p->count_pending = 1;
    
    /* Reset for next round */
    if (p->use_time_window) {
      p->window_start_time = pinfo->input_dpoint->timestamp;
    }
    p->current_count = 0;
    
    if (!p->loop) {
      p->active = SAMPLER_INACTIVE;
    }

    return DPOINT_PROCESS_DSERV;
  }
  
  return DPOINT_PROCESS_IGNORE;
}
