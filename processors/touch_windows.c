#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

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
  ds_datapoint_t status_dpoint;
  ds_datapoint_t settings_dpoint;

  /* see THREADS above onProcess */
  pthread_mutex_t lock;
  char *pending_status;		/* status rename for the next sample, or NULL */
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
    plusminus_y;
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

  pthread_mutex_init(&p->lock, NULL);
  p->pending_status = NULL;
  
  return p;
}

void freeProcessParams(void *pstruct)
{
  process_params_t *p = (process_params_t *) pstruct;
  
  free(p->status_dpoint.varname);
  free(p->status_dpoint.data.buf);

  free(p->settings_dpoint.varname);
  free(p->settings_dpoint.data.buf);

  pthread_mutex_destroy(&p->lock);
  free(p->pending_status);

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


static int get_param_locked(dpoint_process_param_setting_t *pinfo)
{
  char *result_str;
  int win = pinfo->index;
  char *name = pinfo->pname;
  process_params_t *p = (process_params_t *) pinfo->params;
  int inside;
  int dummyInt;

  if (win < 0 || win >= NWIN)
    return 0;
  
  PARAM_ENTRY params[] = {
    { "active",      &p->active[win],      &dummyInt,   PU_INT },
    { "state",       &p->state[win],       &dummyInt,   PU_INT },
    { "type",        &p->type[win],        &dummyInt,   PU_INT },
    { "center_x",    &p->center_x[win],    &dummyInt,   PU_INT },
    { "center_y",    &p->center_y[win],    &dummyInt,   PU_INT },
    { "plusminus_x", &p->plusminus_x[win], &dummyInt,   PU_INT },
    { "plusminus_y", &p->plusminus_y[win], &dummyInt,   PU_INT },
    { "", NULL, NULL, PU_NULL }
  };

  if (!strcmp(name, "state") && pinfo->pval) {
    *pinfo->pval = "0";  /* touches are one-shot, state only via onProcess */
    return 1;
  }  
  
  result_str = puGetParamEntry(&params[0], name);
  if (result_str && pinfo->pval) {
    *pinfo->pval = result_str;
    return 1;
  }		 
  return 0;
}

static int set_param_locked(dpoint_process_param_setting_t *pinfo)
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
    char *st = malloc(strlen(vals[0])+2+strlen(status_str));
    sprintf(st, "%s/%s", vals[0], status_str);
    char *se = malloc(strlen(vals[0])+2+strlen(params_str));
    sprintf(se, "%s/%s", vals[0], params_str);

    /* ess re-sends the SAME name on every system load: nothing to do */
    if (!strcmp(st, p->status_dpoint.varname)) {
      free(st);
      free(p->pending_status);	/* an older pending rename is undone */
      p->pending_status = NULL;
    } else {
      /* status is emitted by onProcess on the sample thread and copied
         there after it returns: install it on that thread, next sample */
      free(p->pending_status);
      p->pending_status = st;
    }

    /* settings is emitted by this function and copied by our caller,
       on this thread, after we return: safe to swap here */
    if (strcmp(se, p->settings_dpoint.varname)) {
      free(p->settings_dpoint.varname);
      p->settings_dpoint.varname = se;
      p->settings_dpoint.varlen = strlen(se);
    } else {
      free(se);
    }
    return DPOINT_PROCESS_IGNORE;
  }

  if (win < 0 || win >= NWIN) return -1;
  
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
      { "", NULL, NULL, PU_NULL }
    };
    
    if (puSetParamEntry(&params[0], name, 1, vals)) {
      result = DPOINT_PROCESS_IGNORE;
    }
    
    /* If window just activated/deactived set state to undefined */
    if ( !was_active && p->active[win] ||
	 was_active && !p->active[win] )  {
      p->state[win] = WINDOW_UNDEFINED;
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

    memcpy(p->settings_dpoint.data.buf, &settings, sizeof(window_settings_t));
    pinfo->dpoint = &p->settings_dpoint;
  }
  
  return result;
}

static int process_locked(dpoint_process_info_t *pinfo, void *params)
{
  process_params_t *p = (process_params_t *) params;
  
  // Accept the unified mtouch/event instead of mtouch/touchvals  
  if (strcmp(pinfo->input_dpoint->varname, "mtouch/event"))
    return DPOINT_PROCESS_IGNORE;

  // Now expecting 3 uint16_t values: x, y, event_type
  if (pinfo->input_dpoint->data.type != DSERV_SHORT ||
      pinfo->input_dpoint->data.len < 3*sizeof(uint16_t))
    return DPOINT_PROCESS_IGNORE;

  uint16_t *touch_vals = (uint16_t *) pinfo->input_dpoint->data.buf;
  int x = touch_vals[0];
  int y = touch_vals[1];
  int event_type = touch_vals[2]; // 0=press, 1=drag, 2=release
  
  int dx, dy, i;
  int inside;
  int retval = DPOINT_PROCESS_IGNORE;
  uint16_t changes = 0, states = 0;

  /* store these away */
  p->last_x = x;
  p->last_y = y;
  
  // Special handling for RELEASE events - force all windows OUT
  if (event_type == 2) { // RELEASE
    for (i = 0; i < NWIN; i++) {
      if (!p->active[i]) {
        if (p->state[i] == WINDOW_UNDEFINED) {
          states &= ~(1 << i);
          p->state[i] = WINDOW_OUT;
          retval = DPOINT_PROCESS_DSERV;
        }
        continue;
      }
      
      // Any active window that was IN should go OUT on release
      if (p->state[i] == WINDOW_IN) {
        p->state[i] = WINDOW_OUT;
        changes |= (1 << i);
        retval = DPOINT_PROCESS_DSERV;
      }
      
      // All windows are OUT after release
      states &= ~(1 << i);
    }
  }
  else if (event_type == 0) {
    // Only PRESS events trigger geometric window entry/exit
    for (i = 0; i < NWIN; i++) {
      if (!p->active[i]) {
        if (p->state[i] == WINDOW_UNDEFINED) {
          states &= ~(1 << i);
          p->state[i] = WINDOW_OUT;
          retval = DPOINT_PROCESS_DSERV;
        }
        continue;
      }
      
      // Calculate geometric inside/outside
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
          changes |= (1 << i);
          retval = DPOINT_PROCESS_DSERV;
        } 
        states |= (1 << i);
      }
      else {
        if (p->state[i] != WINDOW_OUT) {
          p->state[i] = WINDOW_OUT;
          changes |= (1 << i);
          retval = DPOINT_PROCESS_DSERV;
        } 
        states &= ~(1 << i);
      }
    }
  }
  // Ignore DRAG events (type 1) - no window state changes

  /* Every PRESS and RELEASE publishes status, changed or not. ESS wakes
     its state machine on this status, not on mtouch/event: it hears
     mtouch/event first, before this status, so a wake there would see
     the previous states. A press outside every window changes nothing
     (release already forced all OUT) but must still wake ESS, so a
     mistouch is seen -- with states 0, which is then current. */
  if (event_type == 0 || event_type == 2)
    retval = DPOINT_PROCESS_DSERV;

  if (retval == DPOINT_PROCESS_DSERV) {
    uint16_t *vals = (uint16_t *) p->status_dpoint.data.buf;
    vals[0] = changes;
    vals[1] = states;
    vals[2] = x;
    vals[3] = y;
    p->status_dpoint.timestamp = pinfo->input_dpoint->timestamp;
    pinfo->dpoint = &p->status_dpoint;
  }
  
  return retval;
}

/*
 * THREADS. onProcess runs on the thread that set mtouch/event; the
 * param calls run on the caller's (an ess interp's) -- concurrently, and
 * dserv locks neither. One mutex per instance makes each param call and
 * each sample atomic with respect to the other.
 *
 * The output-name rename needs more than the lock: dserv copies the status
 * dpoint AFTER onProcess returns, outside it, so freeing the old name from
 * setProcessParams could free it under that copy (ess renames on every
 * system load, while samples flow). The new status name is installed here,
 * at the start of the next sample, on the thread that made the previous
 * copy -- after it. This assumes one thread publishes the input, which is
 * also what the shared status buffer already assumed.
 *
 * NOT covered: em_region_set / touch_region_set send type, center and size
 * as separate processSetParam calls, and a sample can land between them
 * and see a half-moved window.
 */
int onProcess(dpoint_process_info_t *pinfo, void *params)
{
  process_params_t *p = (process_params_t *) params;
  pthread_mutex_lock(&p->lock);
  if (p->pending_status) {
    free(p->status_dpoint.varname);
    p->status_dpoint.varname = p->pending_status;
    p->status_dpoint.varlen = strlen(p->status_dpoint.varname);
    p->pending_status = NULL;
  }
  int rc = process_locked(pinfo, params);
  pthread_mutex_unlock(&p->lock);
  return rc;
}

int getProcessParams(dpoint_process_param_setting_t *pinfo)
{
  process_params_t *p = (process_params_t *) pinfo->params;
  pthread_mutex_lock(&p->lock);
  int rc = get_param_locked(pinfo);
  pthread_mutex_unlock(&p->lock);
  return rc;
}

int setProcessParams(dpoint_process_param_setting_t *pinfo)
{
  process_params_t *p = (process_params_t *) pinfo->params;
  pthread_mutex_lock(&p->lock);
  int rc = set_param_locked(pinfo);
  pthread_mutex_unlock(&p->lock);
  return rc;
}
