#ifndef DATAPOINT_H
#define DATAPOINT_H

/*
 * Hard limits on a single datapoint. DSERV_MAX_DATA_LEN is the one
 * ceiling for "how big can a point be" — the TCP receive paths
 * (Dataserver.cpp) and the websocket transport (TclServer.cpp, as
 * maxPayloadLength/maxBackpressure) all derive from it, so a point
 * dserv accepts is a point every transport can carry.
 */
#define DSERV_MAX_VARNAME_LEN (512)
#define DSERV_MAX_DATA_LEN (128 * 1024 * 1024)

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
  DSERV_ARROW = 12,
  DSERV_MSGPACK = 13,
  DSERV_JPEG = 14,
  DSERV_PPM = 15,
  DSERV_INT64 = 16,
  DSERV_UNKNOWN,
} ds_datatype_t;

typedef enum {
  DSERV_DPOINT_NOT_INITIALIZED_FLAG = 0x01,
  DSERV_DPOINT_DONTFREE_FLAG = 0x02,
  DSERV_DPOINT_LOGPAUSE_FLAG = 0x04,
  DSERV_DPOINT_LOGSTART_FLAG = 0x08,
  DSERV_DPOINT_SHUTDOWN_FLAG = 0x10,
  DSERV_DPOINT_LOGFLUSH_FLAG = 0x20,
  DSERV_DPOINT_LOGCLOSE_FLAG = 0x40,	/* close one log client (varname = filename);
					   distinct from SHUTDOWN, which ends the
					   logger thread itself */

  /*
   * Attribute bits (DSERV_DPOINT_ATTR_MASK) describe a point and travel
   * with it through every copy; unlike the control bits above they are
   * never routed as logger/lifecycle commands.  Any code that treats
   * "flags != 0" as meaning "control point" must mask these off first
   * (see LogClient::log_point).
   *
   * PRIVATE: the payload never leaves the process except to log files.
   * Denied to every read surface -- subscription fan-out (all send
   * clients, so both network subscribers and Tcl dpoint scripts),
   * dservGet / %get / websocket get, dservWhen level-seeding, and
   * trigger scripts.  The name and metadata stay visible (dservKeys,
   * dservInfo, dservTimestamp) so a rig can confirm a private producer
   * is alive without seeing its data.  Producers set this bit at
   * creation (C modules) or publish via dservSetPrivate; it cannot be
   * injected from the wire (all wire parsers zero flags) and there is
   * deliberately no way to unset it on an existing point.
   */
  DSERV_DPOINT_PRIVATE_FLAG = 0x100,
} ds_datapoint_flag_t;

#define DSERV_DPOINT_ATTR_MASK (0xFF00)
#define DPOINT_IS_PRIVATE(dp) (((dp)->flags & DSERV_DPOINT_PRIVATE_FLAG) != 0)
  
enum { DSERV_CREATE, DSERV_CLEAR, DSERV_SET, DSERV_GET, DSERV_GET_EVENT };
enum { DSERV_GET_FIRST_KEY, DSERV_GET_NEXT_KEY };

#define DPOINT_BINARY_MSG_CHAR '>'
#define DPOINT_BINARY_FIXED_LENGTH (128)

/* Variable-length binary datapoint push: like '>' but length-prefixed instead
 * of padded to a fixed frame, so payloads of any size go in one fire-and-forget
 * message with no base64 and no '@set' handshake. Layout after the lead char:
 *   varlen(u16) type(u32) datalen(u32) timestamp(u64) varname[varlen] data[datalen]
 * (18-byte fixed header, little-endian, then the two length-prefixed blobs). */
#define DPOINT_BINARY_VAR_MSG_CHAR '}'
#define DPOINT_BINARY_VAR_HEADER_LEN (2 + 4 + 4 + 8)

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
