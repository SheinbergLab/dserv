#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include <Datapoint.h>
#include <dpoint_process.h>
#include "prmutil.h"

enum { SIDE_NONE = 0, SIDE_RIGHT, SIDE_UP, SIDE_LEFT, SIDE_DOWN };
  
typedef struct process_params_s {
  int state;			/* are we > threshold */
  float threshold;		/* what is threshold? */
  float scale;			/* scale factor       */
  ds_datapoint_t *dpoint;
} process_params_t;


void *newProcessParams(void)
{
  process_params_t *p = calloc(1, sizeof(process_params_t));
  p->state = -1;
  p->threshold = 9.0;
  p->scale = 0.005;

  p->dpoint = calloc(1, sizeof(ds_datapoint_t));
  p->dpoint->flags = 0;
  p->dpoint->varname = strdup("ain/proc/udlr");
  p->dpoint->varlen = strlen(p->dpoint->varname);
  p->dpoint->data.type = DSERV_SHORT;
  p->dpoint->data.len = 3*sizeof(uint16_t);
  p->dpoint->data.buf = malloc(p->dpoint->data.len);
  
  return p;
}

int freeProcessParams(void *pstruct)
{
  
  process_params_t *p = (process_params_t *) pstruct;
  free(p->dpoint->varname);
  free(p->dpoint->data.buf);
  free(p->dpoint);
  free(p);
  return 0;
}

int getProcessParams(dpoint_process_param_setting_t *pinfo)
{
  char *result_str;
  char *name = pinfo->pname;
  process_params_t *p = (process_params_t *) pinfo->params;
  
  int dummyInt;

  PARAM_ENTRY params[] = {
			  { "state",      &p->state,            &dummyInt,   PU_INT },
			  { "threshold",  &p->threshold,        &dummyInt,   PU_FLOAT },
			  { "scale",      &p->scale,            &dummyInt,   PU_FLOAT },
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
  char *name = pinfo->pname;
  char **vals = pinfo->pval;
  process_params_t *p = (process_params_t *) pinfo->params;

  int dummyInt;
  PARAM_ENTRY params[] = {
			  { "state",      &p->state,            &dummyInt,   PU_INT },
			  { "threshold",  &p->threshold,        &dummyInt,   PU_FLOAT},
			  { "scale",      &p->scale,            &dummyInt,   PU_FLOAT},
			  { "", NULL, NULL, PU_NULL}
  };

  /* if the special dpoint param name is passed, changed the dpoint name */
  if (!strcmp(name, "dpoint")) {
    if (p->dpoint->varname) free(p->dpoint->varname);
    p->dpoint->varname = strdup(vals[0]);
    p->dpoint->varlen = strlen(p->dpoint->varname);
  }
  else {
    puSetParamEntry(&params[0], name, 1, vals);
  }
  
  return DPOINT_PROCESS_IGNORE;
}


int onProcess(dpoint_process_info_t *pinfo, void *params)
{
  process_params_t *p = (process_params_t *) params;

  if (strcmp(pinfo->input_dpoint->varname, "ain/vals"))
    return DPOINT_PROCESS_IGNORE;

  if (pinfo->input_dpoint->data.type != DSERV_SHORT ||
      pinfo->input_dpoint->data.len < 2*sizeof(uint16_t))
    return DPOINT_PROCESS_IGNORE;
  
  uint16_t *vals = (uint16_t *) pinfo->input_dpoint->data.buf;
  
  uint16_t side = SIDE_NONE;
  float scale = p->scale;
  float threshold = p->threshold;
  float x = vals[1];
  float y = vals[0];
  x = (x-2048.)*scale;
  y = (y-2048.)*scale;
  float rad;
  int retval = DPOINT_PROCESS_IGNORE;

  
  if (x*x+y*y > threshold) {
    if (p->state < 0 || !p->state) {
      rad =atan2(y,x);
      
      if (rad >= -.785 && rad < .785) {
	side = SIDE_RIGHT;
      }
      else if (rad >= -2.355 && rad < -.785) {
	side = SIDE_UP;
      }
      else if (rad >= .785 && rad < 2.355) {
	side = SIDE_DOWN;
      }
      else {
	side = SIDE_LEFT;
      }
      p->state = 1;
      retval = DPOINT_PROCESS_DSERV;
    }
  }
  else {
    if (p->state) {
      p->state = 0;
      retval = DPOINT_PROCESS_DSERV;
    }
  }

  if (retval == DPOINT_PROCESS_DSERV) {
    uint16_t *vals = (uint16_t *) p->dpoint->data.buf;
    vals[0] = side;
    vals[1] = vals[1];
    vals[2] = vals[0];
    p->dpoint->timestamp = pinfo->input_dpoint->timestamp;
    pinfo->dpoint = p->dpoint;
  }
  return retval;
}
