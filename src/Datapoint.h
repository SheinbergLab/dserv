#ifndef DATAPOINT_H
#define DATAPOINT_H

#include <inttypes.h>
#include "Base64.h"

// dserv datapoints

typedef enum {
  DSERV_BYTE = 0,
  DSERV_STRING = 1,
  DSERV_FLOAT = 2,
  DSERV_DOUBLE = 3,
  DSERV_SHORT = 4,
  DSERV_INT = 5,
  DSERV_DG = 6,
  DSERV_SCRIPT = 7,
  DSERV_TRIGGER_SCRIPT = 8,		/* will always be delivered to trigger thread */
  DSERV_EVT = 9,
  DSERV_NONE = 10,
  DSERV_JSON = 11,
  DSERV_UNKNOWN,
} ds_datatype_t;

typedef enum {
  DSERV_DPOINT_NOT_INITIALIZED_FLAG = 0x01,
  DSERV_DPOINT_DONTFREE_FLAG = 0x02,
  DSERV_DPOINT_LOGPAUSE_FLAG = 0x04,
  DSERV_DPOINT_LOGSTART_FLAG = 0x08,
  DSERV_DPOINT_SHUTDOWN_FLAG = 0x10,
  DSERV_DPOINT_LOGFLUSH_FLAG = 0x20,
} ds_datapoint_flag_t;
  
enum { DSERV_CREATE, DSERV_CLEAR, DSERV_SET, DSERV_GET, DSERV_GET_EVENT };
enum { DSERV_GET_FIRST_KEY, DSERV_GET_NEXT_KEY };

#define DPOINT_BINARY_MSG_CHAR '>'
#define DPOINT_BINARY_FIXED_LENGTH (128)

typedef struct ds_event_info_s {
  uint8_t dtype;
  uint8_t type;
  uint8_t subtype;
  uint8_t puttype;
} ds_event_info_t;

typedef struct ds_data
{
  union {
    ds_datatype_t type;
    ds_event_info_t e;
  };
  uint32_t len;
  unsigned char *buf;
} ds_data_t;

typedef struct ds_datapoint
{
  uint64_t timestamp;
  uint32_t flags;
  uint16_t varlen;                  // strlen(varname) - used to aid serialization
  char *varname;
  ds_data_t data;
} ds_datapoint_t;

#ifdef __cplusplus
extern "C" {
#endif

// Helper functions for datapoints
ds_datapoint_t *dpoint_new(char *varname,
			   uint64_t timestamp,
			   ds_datatype_t type,
			   uint32_t len,
			   unsigned char *data);
ds_datapoint_t *dpoint_new_nocopy(char *varname,
				  uint64_t timestamp,
				  ds_datatype_t type,
				  uint32_t len,
				  unsigned char *data);
ds_datapoint_t *dpoint_copy(ds_datapoint_t *d);

// don't malloc any new data, just assign
ds_datapoint_t *dpoint_set(ds_datapoint_t *dp,
			   char *varname,
			   uint64_t timestamp,
			   ds_datatype_t type,
			   uint32_t len,
			   unsigned char *data);


void dpoint_free(ds_datapoint_t *d);

int dpoint_binary_size(ds_datapoint_t *dpoint);
int dpoint_to_binary(ds_datapoint_t *dpoint, unsigned char *buf, int *size);
ds_datapoint_t *dpoint_from_binary(char *buf, int buflen);

  
int dpoint_string_size(ds_datapoint_t *d);
int dpoint_to_string(ds_datapoint_t *d, char *buf, int size);
ds_datapoint_t *dpoint_from_string(char *str, int len);

char *dpoint_to_json(ds_datapoint_t *d);

#ifdef __cplusplus
}
#endif


#endif
