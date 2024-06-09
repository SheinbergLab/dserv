#ifndef DPOINT_PROCESS_H_
#define DPOINT_PROCESS_H_

#include "Datapoint.h"

enum { DPOINT_PROCESS_IGNORE, DPOINT_PROCESS_NOTIFY, DPOINT_PROCESS_DSERV };

enum {
  DPOINT_PROCESS_OK = 0,
  DPOINT_PROCESS_NOT_FOUND,
  DPOINT_PROCESS_NO_PROCESS,
  DPOINT_PROCESS_NO_NEW_PARAMS,
  DPOINT_PROCESS_NO_FREE_PARAMS,
  DPOINT_PROCESS_NO_SET_PARAM,
  DPOINT_PROCESS_NO_GET_PARAM
};
  
typedef struct dpoint_process_info_s {
  ds_datapoint_t *input_dpoint;
  char **result_str;
  ds_datapoint_t *dpoint;
} dpoint_process_info_t;

typedef struct dpoint_process_param_setting_s {
  char **pval;
  int index;
  char *pname;
  void *params;
  uint64_t timestamp;
  ds_datapoint_t *dpoint;
} dpoint_process_param_setting_t;
  
typedef int (*DPOINT_PROCESS_FUNC)(dpoint_process_info_t *info, void *);
typedef void * (*DPOINT_PROCESS_NEWPARAM_FUNC)(void);
typedef void * (*DPOINT_PROCESS_FREEPARAM_FUNC)(void *);
typedef int (*DPOINT_PROCESS_SETPARAM_FUNC)(dpoint_process_param_setting_t *p);

#ifdef __cplusplus
extern "C" {
#endif

  int process_load(char *shared_object_name, char *pname);
  int process_attach(char *name,
		     char *varname,
		     char *processfuncname);
  int process_set_param(char *name, char *pname, char *pval, int index,
			uint64_t timestamp, ds_datapoint_t **out);
  char *process_get_param(char *name, char *pname, int index);
  int process_dpoint(ds_datapoint_t *dpoint, ds_datapoint_t **out);
#ifdef __cplusplus
}
#endif

#endif /* DPOINT_PROCESS_H_ */
