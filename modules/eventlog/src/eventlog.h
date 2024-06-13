#ifndef EVENTLOG_H
#define EVENTLOG_H

#include "Datapoint.h"

  typedef enum
    {
      PUT_unknown,  //unknown or complex variable data, evt_put fails
      PUT_null,	    //no variable data to evt_put
      PUT_string,   //evt_put variable args are chars (NUL terminated)
      PUT_short,    //evt_put variable args are shorts
      PUT_long,	    //evt_put variable args are longs
      PUT_float,    //evt_put variable args are floats
      PUT_double,   //evt_put variable args are doubles
      PUT_TYPES
    } PUT_TYPE;


  enum { E_MAGIC, E_NAME };
  
  /* Enumerated SUBTYPE names for consistency between event files */
  enum { E_USER_START, E_USER_QUIT, E_USER_RESET, E_USER_SYSTEM };
  enum { E_TRACE_ACT, E_TRACE_TRANS, E_TRACE_WAKE, E_TRACE_DEBUG };
  enum { E_PARAM_NAME, E_PARAM_VAL };
  enum { E_ID_ESS, E_ID_SUBJECT };
  enum { E_EMLOG_STOP, E_EMLOG_START, E_EMLOG_RATE };
  enum { E_FIXSPOT_OFF, E_FIXSPOT_ON, E_FIXSPOT_SET };
  enum { E_EMPARAMS_SCALE, E_EMPARAMS_CIRC, E_EMPARAMS_RECT };
  enum { E_STIMULUS_OFF, E_STIMULUS_ON, E_STIMULUS_SET };
  enum { E_PATTERN_OFF, E_PATTERN_ON, E_PATTERN_SET };
  enum { E_SAMPLE_OFF, E_SAMPLE_ON, E_SAMPLE_SET };
  enum { E_PROBE_OFF, E_PROBE_ON, E_PROBE_SET };
  enum { E_CUE_OFF, E_CUE_ON, E_CUE_SET };
  enum { E_TARGET_OFF, E_TARGET_ON };
  enum { E_DISTRACTOR_OFF, E_DISTRACTOR_ON };
  enum { E_FIXATE_OUT, E_FIXATE_IN, E_FIXATE_REFIXATE };
  enum { E_RESP_LEFT, E_RESP_RIGHT, E_RESP_BOTH, E_RESP_NONE,
	 E_RESP_MULTI, E_RESP_EARLY };
  enum { E_ENDTRIAL_INCORRECT, E_ENDTRIAL_CORRECT, E_ENDTRIAL_ABORT };
  enum { E_ABORT_EM, E_ABORT_LEVER, E_ABORT_NORESPONSE, E_ABORT_STIM };
  enum { E_ENDOBS_WRONG, E_ENDOBS_CORRECT, E_ENDOBS_QUIT, E_ENDOBS_ABORT };
  enum { E_PHYS_RESP, E_PHYS_SPO2, E_PHYS_AWPRESSURE, E_PHYS_PULSE };
  enum { E_MRI_TRIGGER };
  
  typedef struct event_s
  {
    unsigned char    type;			//event type
    unsigned char    subtype;		//event subtype
    uint64_t         tstamp;
    unsigned char    puttype;              //datatype of this event's parameters
    unsigned char    ndata;		//number of bytes in following data
    char data[256];
  } event_t;

  typedef struct _nametype {
    char name[64];			/* Name of this event */
    char types[2];			/* Time/Put Types     */
  } nametype_t;
  
#endif
