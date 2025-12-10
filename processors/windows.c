#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include <Datapoint.h>
#include <dpoint_process.h>
#include "prmutil.h"

/*
 * Window Processor
 * 
 * Monitors eye position and determines if position is inside/outside defined windows.
 * Supports both rectangular and elliptical windows with refractory periods.
 * 
 * Auto-detects input type:
 *   - DSERV_SHORT (uint16_t) - ADC units (legacy)
 *   - DSERV_FLOAT - degrees visual angle
 * 
 * Coordinates are expected as [y, x] pairs (matching ain/vals convention).
 */

enum { WINDOW_UNDEFINED, WINDOW_IN, WINDOW_OUT };
enum { WINDOW_INACTIVE, WINDOW_ACTIVE };
enum { WINDOW_NOT_INITIALIZED, WINDOW_INITIALIZED };
enum { WINDOW_RECTANGLE, WINDOW_ELLIPSE };

#define NWIN (8)

static char *status_str = "status";
static char *params_str = "settings";

typedef struct process_params_s {
  int active[NWIN];		/* are we active?         */
  int state[NWIN];		/* are we in or out       */
  int type[NWIN];		/* ellipse or rectangle   */
  float center_x[NWIN];		/* now float for degrees  */
  float center_y[NWIN];
  float plusminus_x[NWIN];	/* half-width/radius      */
  float plusminus_y[NWIN];	/* half-height/radius     */
  int refractory_count[NWIN];
  int refractory_countdown[NWIN];
  
  /* Input type detection */
  int input_type;		/* DSERV_SHORT or DSERV_FLOAT */
  int type_locked;		/* has type been detected?    */
  
  /* Last position (stored as float regardless of input type) */
  float last_x, last_y;
  
  ds_datapoint_t status_dpoint;
  ds_datapoint_t settings_dpoint;
  
  int dummyInt;
} process_params_t;

typedef struct window_settings_s {
  uint16_t win,
    active,
    state,
    type,
    center_x_scaled,      /* stored as value * 10 */
    center_y_scaled,
    plusminus_x_scaled,
    plusminus_y_scaled,
    refractory_count,
    refractory_countdown;
} window_settings_t;

void *newProcessParams(void)
{
  process_params_t *p = calloc(1, sizeof(process_params_t));
  int i;

  /* Type detection state */
  p->input_type = -1;
  p->type_locked = 0;

  for (i = 0; i < NWIN; i++) {
    p->active[i] = WINDOW_INACTIVE;
    p->state[i] = WINDOW_UNDEFINED;
    p->type[i] = WINDOW_ELLIPSE;
    /* Default to ADC-like values for backward compatibility */
    p->center_x[i] = 2047.0;
    p->center_y[i] = 2047.0;
    p->plusminus_x[i] = 200.0;
    p->plusminus_y[i] = 200.0;
    p->refractory_count[i] = 20;
    p->refractory_countdown[i] = 0;
  }

  // region updates - changes/states as uint16, positions as float
  p->status_dpoint.flags = 0;
  p->status_dpoint.varname = strdup("proc/windows/status");
  p->status_dpoint.varlen = strlen(p->status_dpoint.varname);
  p->status_dpoint.data.type = DSERV_FLOAT;  // float array
  p->status_dpoint.data.len = 4*sizeof(float);
  p->status_dpoint.data.buf = malloc(p->status_dpoint.data.len);

  // parameter updates - all floats
  p->settings_dpoint.flags = 0;
  p->settings_dpoint.varname = strdup("proc/windows/settings");
  p->settings_dpoint.varlen = strlen(p->settings_dpoint.varname);
  p->settings_dpoint.data.type = DSERV_FLOAT;
  p->settings_dpoint.data.len = 10*sizeof(float);  // 10 float values
  p->settings_dpoint.data.buf = malloc(p->settings_dpoint.data.len);
  
  return p;
}

void freeProcessParams(void *pstruct)
{
  process_params_t *p = (process_params_t *) pstruct;
  
  free(p->status_dpoint.varname);
  free(p->status_dpoint.data.buf);

  free(p->settings_dpoint.varname);
  free(p->settings_dpoint.data.buf);

  free(p);
}

int check_state(process_params_t *p, int win)
{
  int inside;
  float dx, dy;
  if (!p->active[win]) return 0;
  switch (p->type[win]) {
  case WINDOW_ELLIPSE:
    dx = p->last_x - p->center_x[win];
    dy = p->last_y - p->center_y[win];
    inside =
      (((dx*dx) / (p->plusminus_x[win]*p->plusminus_x[win])) +
       ((dy*dy) / (p->plusminus_y[win]*p->plusminus_y[win]))) < 1.0;
    break;
  case WINDOW_RECTANGLE:
    dx = p->last_x - p->center_x[win];
    dy = p->last_y - p->center_y[win];
    inside = (fabsf(dx) < p->plusminus_x[win]) && (fabsf(dy) < p->plusminus_y[win]);
    break;
  }
  return inside;
}


int getProcessParams(dpoint_process_param_setting_t *pinfo)
{
  char *result_str;
  int win = pinfo->index;
  char *name = pinfo->pname;
  process_params_t *p = (process_params_t *) pinfo->params;
  int inside;

  if (win < 0 || win > NWIN)
    return 0;
  
  PARAM_ENTRY params[] = {
    { "active",      &p->active[win],      &p->dummyInt,   PU_INT },
    { "state",       &p->state[win],       &p->dummyInt,   PU_INT },
    { "type",        &p->type[win],        &p->dummyInt,   PU_INT },
    { "center_x",    &p->center_x[win],    &p->dummyInt,   PU_FLOAT },
    { "center_y",    &p->center_y[win],    &p->dummyInt,   PU_FLOAT },
    { "plusminus_x", &p->plusminus_x[win], &p->dummyInt,   PU_FLOAT },
    { "plusminus_y", &p->plusminus_y[win], &p->dummyInt,   PU_FLOAT },
    { "refractory_count", &p->refractory_count[win], &p->dummyInt, PU_INT },
    { "input_type",  &p->input_type,       &p->dummyInt,   PU_INT },
    { "type_locked", &p->type_locked,      &p->dummyInt,   PU_INT },
    { "", NULL, NULL, PU_NULL }
  };

  if (!strcmp(name, "state") && pinfo->pval) {
    inside = check_state(p, win);
    if (inside)
      *pinfo->pval = "1";
    else
      *pinfo->pval = "0";
    return 1;
  }
    
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
  int win = pinfo->index;
  char *name = pinfo->pname;
  char **vals = pinfo->pval;
  process_params_t *p = (process_params_t *) pinfo->params;
  window_settings_t settings;

  /* if the special dpoint param name is passed, change the dpoint name */
  if (!strcmp(name, "dpoint")) {
    /* status */
    if (p->status_dpoint.varname) free(p->status_dpoint.varname);
    p->status_dpoint.varname = malloc(strlen(vals[0])+2+strlen(status_str));
    sprintf(p->status_dpoint.varname, "%s/%s", vals[0], status_str);
    p->status_dpoint.varlen = strlen(p->status_dpoint.varname);

    /* params */
    if (p->settings_dpoint.varname) free(p->settings_dpoint.varname);
    p->settings_dpoint.varname = malloc(strlen(vals[0])+2+strlen(params_str));
    sprintf(p->settings_dpoint.varname, "%s/%s", vals[0], params_str);
    p->settings_dpoint.varlen = strlen(p->settings_dpoint.varname);
    return DPOINT_PROCESS_IGNORE;
  }

  if (win < 0 || win > NWIN) return -1;
  
  /* by passing in "settings" as the param to set, kick a param update */
  if (!strcmp(name, params_str)) {
    result = DPOINT_PROCESS_DSERV;
  }
  
  else {
    
    int was_active = p->active[win];
    
    PARAM_ENTRY params[] = {
      { "active",      &p->active[win],      &p->dummyInt,   PU_INT },
      { "state",       &p->state[win],       &p->dummyInt,   PU_INT },
      { "type",        &p->type[win],        &p->dummyInt,   PU_INT },
      { "center_x",    &p->center_x[win],    &p->dummyInt,   PU_FLOAT },
      { "center_y",    &p->center_y[win],    &p->dummyInt,   PU_FLOAT },
      { "plusminus_x", &p->plusminus_x[win], &p->dummyInt,   PU_FLOAT },
      { "plusminus_y", &p->plusminus_y[win], &p->dummyInt,   PU_FLOAT },
      { "refractory_count", &p->refractory_count[win], &p->dummyInt, PU_INT },
      { "", NULL, NULL, PU_NULL }
    };
    
    if (puSetParamEntry(&params[0], name, 1, vals)) {
      result = DPOINT_PROCESS_IGNORE;
    }
    
    /* If window just activated/deactivated set state to undefined to ensure update */
    if ( !was_active && p->active[win] ||
	 was_active && !p->active[win] )  {
      p->state[win] = WINDOW_UNDEFINED;
      p->refractory_countdown[win] = 0;
    }
  }

  if (result == DPOINT_PROCESS_DSERV) {
    p->settings_dpoint.timestamp = pinfo->timestamp;
    
    /* Pack as float array: [win, active, state, type, center_x, center_y, 
                             plusminus_x, plusminus_y, refractory_count, refractory_countdown] */
    float *vals = (float *) p->settings_dpoint.data.buf;
    vals[0] = (float)win;
    vals[1] = (float)p->active[win];
    vals[2] = (float)p->state[win];
    vals[3] = (float)p->type[win];
    vals[4] = p->center_x[win];
    vals[5] = p->center_y[win];
    vals[6] = p->plusminus_x[win];
    vals[7] = p->plusminus_y[win];
    vals[8] = (float)p->refractory_count[win];
    vals[9] = (float)p->refractory_countdown[win];
    
    pinfo->dpoint = &p->settings_dpoint;
  }
  
  return result;
}

int onProcess(dpoint_process_info_t *pinfo, void *params)
{
  process_params_t *p = (process_params_t *) params;
  float x, y;
  float dx, dy;
  int i;
  int inside;
  int retval = DPOINT_PROCESS_IGNORE;
  uint16_t changes = 0, states = 0;

  /* Auto-detect input type on first sample */
  if (!p->type_locked) {
    if (pinfo->input_dpoint->data.type == DSERV_SHORT ||
        pinfo->input_dpoint->data.type == DSERV_FLOAT) {
      p->input_type = pinfo->input_dpoint->data.type;
      p->type_locked = 1;
    } else {
      /* Unsupported type - ignore */
      return DPOINT_PROCESS_IGNORE;
    }
  }

  /* Validate type matches what we locked in */
  if (pinfo->input_dpoint->data.type != p->input_type)
    return DPOINT_PROCESS_IGNORE;

  /* Extract coordinates based on detected type */
  if (p->input_type == DSERV_FLOAT) {
    /* Float input (degrees visual angle) */
    if (pinfo->input_dpoint->data.len < 2*sizeof(float))
      return DPOINT_PROCESS_IGNORE;
    
    float *float_vals = (float *) pinfo->input_dpoint->data.buf;
    x = float_vals[0];
    y = float_vals[1];
    
  }

  /* store these away */
  p->last_x = x;
  p->last_y = y;
  
  /* check all windows for any changes */
  for (i = 0; i < NWIN; i++) {
    if (!p->active[i]) {
      if (p->state[i] == WINDOW_UNDEFINED) {
	states &= ~(1 << i);
	p->state[i] = WINDOW_OUT;
	retval = DPOINT_PROCESS_DSERV;
      }
      continue;
    }
    
    switch (p->type[i]) {
    case WINDOW_ELLIPSE:
      dx = x - p->center_x[i];
      dy = y - p->center_y[i];
      inside =
	(((dx*dx) / (p->plusminus_x[i]*p->plusminus_x[i])) +
	 ((dy*dy) / (p->plusminus_y[i]*p->plusminus_y[i]))) < 1.0;
      break;
    case WINDOW_RECTANGLE:
      dx = x - p->center_x[i];
      dy = y - p->center_y[i];
      inside = (fabsf(dx) < p->plusminus_x[i]) && (fabsf(dy) < p->plusminus_y[i]);
      break;
    }
        
    if (inside) {
      if (p->state[i] != WINDOW_IN) {
	p->state[i] = WINDOW_IN;
	p->refractory_countdown[i] = 0;	

	changes |= (1 << i);

	retval = DPOINT_PROCESS_DSERV;
      } 
      states |= (1 << i);
    }
    else {
      if (p->state[i] != WINDOW_OUT) {
	if (p->refractory_count[i]) {
	  if (!p->refractory_countdown[i]) {
	    p->refractory_countdown[i] = p->refractory_count[i];
	    continue;
	  }
	  if (p->refractory_countdown[i] != 1) {
	    p->refractory_countdown[i]--;
	    continue;
	  }
	}
	p->refractory_countdown[i] = 0;
	p->state[i] = WINDOW_OUT;
	changes |= (1 << i);
	retval = DPOINT_PROCESS_DSERV;
      } 
      states &= ~(1 << i);
    }
  }
  
  if (retval == DPOINT_PROCESS_DSERV) {
    /* Status buffer layout: [changes:float, states:float, x:float, y:float] 
     * All values as floats - changes and states are integers but stored as float
     * Positions in degrees, can be negative
     */
    float *vals = (float *) p->status_dpoint.data.buf;
    vals[0] = (float)changes;
    vals[1] = (float)states;
    vals[2] = x;
    vals[3] = y;
    
    p->status_dpoint.timestamp = pinfo->input_dpoint->timestamp;
    pinfo->dpoint = &p->status_dpoint;
  }

  return retval;
}
