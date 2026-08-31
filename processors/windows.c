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
 * INPUT CONTRACT: DSERV_FLOAT, [x, y, ...] in degrees of visual angle, origin
 * at screen center. Attached to eyetracking/position (config/triggers.tcl),
 * which every eye source in config/emconf.tcl -- video, analog and virtual --
 * publishes as `binary format ff $h_deg $v_deg`. Only the leading two floats
 * are read, so a longer payload (ess/roam/pos is {x y t}) rides along fine.
 *
 * DSERV_SHORT input is REJECTED, not converted. This file used to read raw
 * MCP3204 ADC counts off ain/vals and compare them against centers also held
 * in ADC units; centers now arrive in degrees from ess::em_fixwin_set, so
 * accepting counts would silently test ~2000 against ~5 and report garbage
 * window states. There is no scale factor to apply here that would be right --
 * the raw->degrees calibration (offset, gain, invert, swap, or a biquadratic
 * fit) lives in em::process_analog, upstream, and that is where an integer
 * eye source belongs. See processors/windows.c.SHORT for the pre-degrees
 * version if the ADC-unit history is needed.
 */

enum { WINDOW_UNDEFINED, WINDOW_IN, WINDOW_OUT };
enum { WINDOW_INACTIVE, WINDOW_ACTIVE };
enum { WINDOW_NOT_INITIALIZED, WINDOW_INITIALIZED };
enum { WINDOW_RECTANGLE, WINDOW_ELLIPSE };

#define NWIN (8)

/* Centers are degrees of visual angle, so an unconfigured window has to be
 * parked somewhere no eye can reach. No display subtends 1000 deg, so a
 * window switched on before em_region_set has been called reports OUT --
 * the same unreachable-by-default behaviour the old ADC-midpoint 2047 gave,
 * restated in the units this file now works in. */
#define WINDOW_UNSET_CENTER (1000.0)

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
  int input_type;		/* DSERV_FLOAT once locked    */
  int type_locked;		/* has a valid sample landed? */
  int warned_type;		/* last rejected type warned about */

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
  p->warned_type = -1;

  for (i = 0; i < NWIN; i++) {
    p->active[i] = WINDOW_INACTIVE;
    p->state[i] = WINDOW_UNDEFINED;
    p->type[i] = WINDOW_ELLIPSE;
    /* Degrees of visual angle; parked out of reach until em_region_set */
    p->center_x[i] = WINDOW_UNSET_CENTER;
    p->center_y[i] = WINDOW_UNSET_CENTER;
    p->plusminus_x[i] = 1.0;
    p->plusminus_y[i] = 1.0;
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
    *pinfo->pval = (p->state[win] == WINDOW_IN) ? "1" : "0";
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

  /* DSERV_FLOAT degrees is the only accepted input -- see the file header for
   * why an integer eye source has to be converted upstream rather than here.
   * Complain once per offending type: this runs at the eye sample rate, so a
   * per-sample message would bury the log, and staying silent is what let the
   * old missing-else branch feed uninitialized x/y into every window test. */
  if (pinfo->input_dpoint->data.type != DSERV_FLOAT ||
      pinfo->input_dpoint->data.len < 2*sizeof(float)) {
    if (p->warned_type != (int) pinfo->input_dpoint->data.type) {
      p->warned_type = (int) pinfo->input_dpoint->data.type;
      fprintf(stderr,
	      "windows: ignoring %s (type %d, %u bytes): this processor "
	      "requires DSERV_FLOAT (%d) [x y] in degrees of visual angle. "
	      "No window states will be produced from this input.\n",
	      pinfo->input_dpoint->varname ?
	        pinfo->input_dpoint->varname : "(unnamed dpoint)",
	      (int) pinfo->input_dpoint->data.type,
	      pinfo->input_dpoint->data.len,
	      (int) DSERV_FLOAT);
    }
    return DPOINT_PROCESS_IGNORE;
  }

  p->input_type = DSERV_FLOAT;
  p->type_locked = 1;

  {
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
    default:
      /* `type` is a free-form int through the param API; an unrecognized
       * shape reads OUT rather than leaving `inside` uninitialized. */
      inside = 0;
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
