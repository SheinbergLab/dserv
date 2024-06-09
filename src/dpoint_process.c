/*
 * dpoint_process.c - attach c functions to datapoints
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sched.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <fcntl.h>
#include <errno.h>

#include <math.h>
#include <dlfcn.h>

#include "uthash.h"
#include "dpoint_process.h"

static void pabort(const char *s)
{
  perror(s);
  abort();
}

/*
 * processFunctions can be loaded from shared objects 
 */
typedef struct dpointProcessFunctionInfo_s {
  char *name;
  void *handle;
  DPOINT_PROCESS_FUNC pfunc;
  DPOINT_PROCESS_NEWPARAM_FUNC newparamfunc;
  DPOINT_PROCESS_FREEPARAM_FUNC freeparamfunc;
  DPOINT_PROCESS_SETPARAM_FUNC setparamfunc;
  DPOINT_PROCESS_SETPARAM_FUNC getparamfunc;
  UT_hash_handle hh;	/* make hashable                          */
                        /* (https://troydhanson.github.io/uthash/ */
} dpointProcessFunctionInfo_t;

/*
 * Structure to hold processor name, point name, and process function names
 */
typedef struct dpointProcessInfo_s {
  char                         *name;
  char                         *varname;
  DPOINT_PROCESS_FUNC           process;
  DPOINT_PROCESS_FREEPARAM_FUNC free_params;
  DPOINT_PROCESS_SETPARAM_FUNC  set_param;
  DPOINT_PROCESS_SETPARAM_FUNC  get_param;
  void                         *process_params;
  UT_hash_handle                hh;
} dpointProcessInfo_t;

/* Table of process functions */
dpointProcessFunctionInfo_t *processFunctionTable = NULL;

/* Table of process mappings */
dpointProcessInfo_t *processTable = NULL;

void add_process_function_info(char *name,
			       void *handle,
			       DPOINT_PROCESS_FUNC pfunc,
			       DPOINT_PROCESS_NEWPARAM_FUNC newparamfunc,
			       DPOINT_PROCESS_FREEPARAM_FUNC freeparamfunc,
			       DPOINT_PROCESS_SETPARAM_FUNC setparamfunc,
			       DPOINT_PROCESS_SETPARAM_FUNC getparamfunc) {
  dpointProcessFunctionInfo_t *p;
  
  /* check if already exists, otherwise allocate and add to table */
  HASH_FIND_STR(processFunctionTable, name, p);
  if (p == NULL) {
    p = (dpointProcessFunctionInfo_t *) malloc(sizeof *p);
    p->name = strdup(name);
    HASH_ADD_STR(processFunctionTable, name, p);
  }
  
  /* now update the rest of the structure */
  p->handle = handle;
  p->pfunc = pfunc;
  p->newparamfunc = newparamfunc;
  p->freeparamfunc = freeparamfunc;
  p->setparamfunc = setparamfunc;
  p->getparamfunc = getparamfunc;
}


int process_dpoint(ds_datapoint_t *dpoint, ds_datapoint_t **out)
{
  dpointProcessInfo_t *p;

  for (p = processTable; p != NULL; p = p->hh.next) {
    if (!strcmp(p->varname, dpoint->varname)) {
      dpoint_process_info_t pinfo;
      pinfo.input_dpoint = dpoint;

      int rc = p->process(&pinfo, p->process_params);
      
      if (rc == DPOINT_PROCESS_DSERV) {
	*out = pinfo.dpoint;
      }
      return rc;
    }
  }

  return DPOINT_PROCESS_IGNORE;
}

int process_attach(char *name,
		   char *varname,
		   char *processfuncname) {
  
  /* find processfunc info */
  dpointProcessFunctionInfo_t *pfunc;
  HASH_FIND_STR(processFunctionTable, processfuncname, pfunc);
  if (pfunc == NULL) return -1;

  /* check if already entry exists, otherwise allocate and add to table */
  dpointProcessInfo_t *p;
  HASH_FIND_STR(processTable, name, p);
  if (p == NULL) {
    p = (dpointProcessInfo_t *) malloc(sizeof *p);
    p->name = strdup(name);
    HASH_ADD_STR(processTable, name, p);
  }
  else {
    if (p->varname) free(p->varname);
    if (p->process_params)
      p->free_params(p->process_params);
  }
  
  p->varname = strdup(varname);
  p->process = pfunc->pfunc;
  p->free_params = pfunc->freeparamfunc;
  p->set_param = pfunc->setparamfunc;
  p->get_param = pfunc->getparamfunc;
  p->process_params = pfunc->newparamfunc();

  return 0;
}

int process_set_param(char *name, char *pname, char *pval, int index,
		      uint64_t timestamp, ds_datapoint_t **out)
{
  dpointProcessInfo_t *p;
  HASH_FIND_STR(processTable, name, p);
  if (p == NULL) return -1;

  
  dpoint_process_param_setting_t psetting;
  psetting.timestamp = timestamp;
  psetting.pval = &pval;
  psetting.index = index;
  psetting.params = (void *) p->process_params;
  psetting.pname = pname;

  int rc;
  rc = p->set_param(&psetting);
  if (rc == DPOINT_PROCESS_DSERV) {
    *out = psetting.dpoint;
  }
  return rc;
}

char *process_get_param(char *name, char *pname, int index)
{
  dpointProcessInfo_t *p;


  HASH_FIND_STR(processTable, name, p);
  if (p == NULL) {
    return NULL;
  }

  char *pval;			/* will be set if successful */
  dpoint_process_param_setting_t psetting;
  psetting.pval = &pval;
  psetting.index = index;
  psetting.params = (void *) p->process_params;
  psetting.pname = pname;

  
  if (!p->get_param(&psetting)) return NULL;
  else return pval;
}

int process_load(char *shared_object_name, char *pname)
{
  
  void *handle;
  DPOINT_PROCESS_FUNC pfunc;
  DPOINT_PROCESS_NEWPARAM_FUNC newpfunc;
  DPOINT_PROCESS_FREEPARAM_FUNC freepfunc;
  DPOINT_PROCESS_SETPARAM_FUNC setpfunc;
  DPOINT_PROCESS_SETPARAM_FUNC getpfunc;

  dpointProcessFunctionInfo_t *pinfo;
  
  /* Open a shared library. */
  handle = dlopen( shared_object_name,  RTLD_NOW);
  if (!handle) return DPOINT_PROCESS_NOT_FOUND;

  /* Find the address of the process function */
  pfunc = (DPOINT_PROCESS_FUNC) dlsym( handle, "onProcess" );
  if (!pfunc) {
    dlclose(handle);
    return DPOINT_PROCESS_NO_PROCESS;
  }

  /* Find the address of param creation function */
  newpfunc = (DPOINT_PROCESS_NEWPARAM_FUNC) dlsym( handle, "newProcessParams" );
  if (!newpfunc) {
    dlclose(handle);
    return DPOINT_PROCESS_NO_NEW_PARAMS;
  }

  freepfunc = (DPOINT_PROCESS_FREEPARAM_FUNC) dlsym( handle, "freeProcessParams" );
  if (!freepfunc) {
    dlclose(handle);
    return DPOINT_PROCESS_NO_FREE_PARAMS;
  }
  
  /* Find the address of param setting function */
  setpfunc = (DPOINT_PROCESS_SETPARAM_FUNC) dlsym( handle, "setProcessParams" );
  if (!setpfunc) {
    dlclose(handle);
    return DPOINT_PROCESS_NO_SET_PARAM;
  }

  /* Find the address of param setting function */
  getpfunc = (DPOINT_PROCESS_SETPARAM_FUNC) dlsym( handle, "getProcessParams" );
  if (!getpfunc) {
    dlclose(handle);
    return DPOINT_PROCESS_NO_GET_PARAM;
  }

  add_process_function_info(pname, handle, pfunc,
			    newpfunc, freepfunc,
			    setpfunc, getpfunc);
  return DPOINT_PROCESS_OK;
}


