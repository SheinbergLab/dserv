#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include <Datapoint.h>
#include <dpoint_process.h>
#include "prmutil.h"

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
  int center_x[NWIN];
  int center_y[NWIN];
  int plusminus_x[NWIN];
  int plusminus_y[NWIN];
  int refractory_count[NWIN];
  int refractory_countdown[NWIN];
  ds_datapoint_t status_dpoint;
  ds_datapoint_t settings_dpoint;
  uint16_t last_x, last_y;
} process_params_t;

typedef struct window_settings_s {
  uint16_t win,
    active,
    state,
    type,
    center_x,
    center_y,
    plusminus_x,
    plusminus_y,
    refractory_count,
    refractory_countdown;
} window_settings_t;

void *newProcessParams(void)
{
  process_params_t *p = calloc(1, sizeof(process_params_t));
  int i;

  for (i = 0; i < NWIN; i++) {
    p->active[i] = WINDOW_INACTIVE;
    p->state[i] = WINDOW_UNDEFINED;
    p->type[i] = WINDOW_ELLIPSE;
    p->center_x[i] = 400;
    p->center_y[i] = 320;
    p->plusminus_x[i] = 100;
    p->plusminus_y[i] = 100;
    p->refractory_count[i] = 0;
    p->refractory_countdown[i] = 0;
  }

  // region updates
  p->status_dpoint.flags = 0;
  p->status_dpoint.varname = strdup("proc/touch_windows/status");
  p->status_dpoint.varlen = strlen(p->status_dpoint.varname);
  p->status_dpoint.data.type = DSERV_SHORT;
  p->status_dpoint.data.len = 4*sizeof(uint16_t);
  p->status_dpoint.data.buf = malloc(p->status_dpoint.data.len);

  // parameter updates
  p->settings_dpoint.flags = 0;
  p->settings_dpoint.varname = strdup("proc/touch_windows/settings");
  p->settings_dpoint.varlen = strlen(p->settings_dpoint.varname);
  p->settings_dpoint.data.type = DSERV_SHORT;
  p->settings_dpoint.data.len = sizeof(window_settings_t);
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
  int dx, dy;
  if (!p->active[win]) return 0;
  switch (p->type[win]) {
  case WINDOW_ELLIPSE:
    dx =  p->last_x - p->center_x[win];
    dy =  p->last_y - p->center_y[win];
    inside =
      (((dx*dx) / (float) (p->plusminus_x[win]*p->plusminus_x[win])) +
       ((dy*dy) / (float) (p->plusminus_y[win]*p->plusminus_y[win]))) < 1.0;
    break;
  case WINDOW_RECTANGLE:
    dx =  p->last_x - p->center_x[win];
    dy =  p->last_y - p->center_y[win];
    inside = (abs(dx) < p->plusminus_x[win]) && (abs(dy) < p->plusminus_y[win]);
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
  int dummyInt;

  if (win < 0 || win > NWIN)
    return 0;
  
  PARAM_ENTRY params[] = {
    { "active",      &p->active[win],      &dummyInt,   PU_INT },
    { "state",       &p->state[win],       &dummyInt,   PU_INT },
    { "type",        &p->type[win],        &dummyInt,   PU_INT },
    { "center_x",    &p->center_x[win],    &dummyInt,   PU_INT },
    { "center_y",    &p->center_y[win],    &dummyInt,   PU_INT },
    { "plusminus_x", &p->plusminus_x[win], &dummyInt,   PU_INT },
    { "plusminus_y", &p->plusminus_y[win], &dummyInt,   PU_INT },
    { "refractory_count",  &p->refractory_count[win], &dummyInt,   PU_INT },
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
  
  int dummyInt;


  /* if the special dpoint param name is passed, changed the dpoint name */
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
  
  /* by passing in "settings" as the param to set, kick an param update */
  if (!strcmp(name, params_str)) {
    result = DPOINT_PROCESS_DSERV;
  }
  
  else {
    
    int was_active = p->active[win];
    
    PARAM_ENTRY params[] = {
      { "active",      &p->active[win],      &dummyInt,   PU_INT },
      { "state",       &p->state[win],       &dummyInt,   PU_INT },
      { "type",        &p->type[win],        &dummyInt,   PU_INT },
      { "center_x",    &p->center_x[win],    &dummyInt,   PU_INT },
      { "center_y",    &p->center_y[win],    &dummyInt,   PU_INT },
      { "plusminus_x", &p->plusminus_x[win], &dummyInt,   PU_INT },
      { "plusminus_y", &p->plusminus_y[win], &dummyInt,   PU_INT },
      { "refractory_count",  &p->refractory_count[win], &dummyInt,   PU_INT },
      { "", NULL, NULL, PU_NULL }
    };
    
    if (puSetParamEntry(&params[0], name, 1, vals)) {
      result = DPOINT_PROCESS_IGNORE;
    }
    
    /* If window just activated/deactived set state to undefined to ensure update */
    if ( !was_active && p->active[win] ||
	 was_active && !p->active[win] )  {
      p->state[win] = WINDOW_UNDEFINED;
      p->refractory_countdown[win] = 0;
    }
  }

  if (result == DPOINT_PROCESS_DSERV) {
    p->settings_dpoint.timestamp = pinfo->timestamp;
    
    settings.win = win;
    settings.active = p->active[win];
    settings.state = p->state[win];
    settings.type = p->type[win];
    settings.center_x = p->center_x[win];
    settings.center_y = p->center_y[win];
    settings.plusminus_x = p->plusminus_x[win];
    settings.plusminus_y = p->plusminus_y[win];
    settings.refractory_count = p->refractory_count[win];
    settings.refractory_countdown = p->refractory_countdown[win];

    memcpy(p->settings_dpoint.data.buf, &settings, sizeof(window_settings_t));
    pinfo->dpoint = &p->settings_dpoint;
  }
  
  return result;
}

int onProcess(dpoint_process_info_t *pinfo, void *params)
{
  process_params_t *p = (process_params_t *) params;
  int dx, dy, x, y;
  if (strcmp(pinfo->input_dpoint->varname, "mtouch/touch"))
    return DPOINT_PROCESS_IGNORE;

  if (pinfo->input_dpoint->data.type != DSERV_STRING)
    return DPOINT_PROCESS_IGNORE;

  char buf[64];
  strncpy(buf,
	  (const char *) pinfo->input_dpoint->data.buf,
	  pinfo->input_dpoint->data.len);
  if (sscanf(buf, "%d %d %d %d", &dx, &dy, &x, &y) != 4)
    return DPOINT_PROCESS_IGNORE;

  int i;
  int inside;
  int retval = DPOINT_PROCESS_IGNORE;
  uint16_t changes = 0, states = 0;

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
      dx =  x - p->center_x[i];
      dy =  y - p->center_y[i];
      inside =
	(((dx*dx) / (float) (p->plusminus_x[i]*p->plusminus_x[i])) +
	 ((dy*dy) / (float) (p->plusminus_y[i]*p->plusminus_y[i]))) < 1.0;
      break;
    case WINDOW_RECTANGLE:
      dx =  x - p->center_x[i];
      dy =  y - p->center_y[i];
      inside = (abs(dx) < p->plusminus_x[i]) && (abs(dy) < p->plusminus_y[i]);
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
    uint16_t *vals = (uint16_t *) p->status_dpoint.data.buf;
    vals[0] = changes;
    vals[1] = states;
    vals[2] = y;
    vals[3] = x;
    p->status_dpoint.timestamp = pinfo->input_dpoint->timestamp;
    pinfo->dpoint = &p->status_dpoint;
  }
  return retval;
}
