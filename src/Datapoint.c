#include <stdlib.h>
#include <string.h>
#include "Datapoint.h"
#include <jansson.h>		/* for JSON support */

/*****************************************************************************/
/*******************************dpoint helpers *******************************/
/*****************************************************************************/

ds_datapoint_t *dpoint_set(ds_datapoint_t *dp,
			   char *varname,
			   uint64_t timestamp,
			   ds_datatype_t type,
			   uint32_t len,
			   unsigned char *data)
{
  dp->varlen = strlen(varname);
  dp->flags = 0x00;
  dp->timestamp = timestamp;
  dp->varname = varname;
  dp->data.len = len;
  dp->data.type = type;
  dp->data.buf = len ? data : NULL;
  return dp;
}

ds_datapoint_t *dpoint_new_nocopy(char *varname,
				  uint64_t timestamp,
				  ds_datatype_t type,
				  uint32_t len,
				  unsigned char *data)
{
  ds_datapoint_t *new_dp = NULL;
  new_dp = (ds_datapoint_t *) malloc(sizeof(ds_datapoint_t));
  new_dp->varlen = strlen(varname);
  new_dp->flags = 0x00;
  new_dp->timestamp = timestamp;
  new_dp->varname = varname;
  new_dp->data.len = len;
  new_dp->data.type = type;
  new_dp->data.buf = len ? data : NULL;
  return new_dp;
}


ds_datapoint_t *dpoint_new(char *varname,
			   uint64_t timestamp,
			   ds_datatype_t type,
			   uint32_t len,
			   unsigned char *data)
{
  ds_datapoint_t *new_dp = NULL;
  new_dp = (ds_datapoint_t *) malloc(sizeof(ds_datapoint_t));
  new_dp->varlen = strlen(varname);
  new_dp->flags = 0x00;
  new_dp->timestamp = timestamp;
  new_dp->varname = strdup(varname);
  new_dp->data.len = len;
  new_dp->data.type = type;
  if (new_dp->data.len) {
    new_dp->data.buf = (unsigned char *) malloc(len);
    memcpy(new_dp->data.buf, data, len);
  }
  else {
    new_dp->data.buf = NULL;
  }
  return new_dp;
}

ds_datapoint_t *dpoint_copy(ds_datapoint_t *d)
{
  ds_datapoint_t *new_dp = NULL;
  if (d) {
    new_dp = (ds_datapoint_t *) malloc(sizeof(ds_datapoint_t));
    memcpy(new_dp, d, sizeof(ds_datapoint_t));
    if (d->varname) new_dp->varname = strdup(d->varname);
    if (d->data.len) {
      new_dp->data.buf = (unsigned char *) malloc(d->data.len);
      memcpy(new_dp->data.buf, d->data.buf, d->data.len);
    }
    else new_dp->data.buf = NULL;
  }
  return new_dp;
}

void dpoint_free(ds_datapoint_t *d)
{
  if (d) {
    if (d->varname) {
      free(d->varname);
    }
    if (d->data.buf) {
      free(d->data.buf);
    }
    free(d);
  }
}

// how big a buffer is needed for the b64 encoded data?
int dpoint_b64_size(ds_datapoint_t *d)
{
  if (!d->data.len)
    return 0;
  else
    return ((4 * d->data.len / 3) + 3) & ~3;
}

int dpoint_string_size(ds_datapoint_t *d)
{
  if (d->data.type == DSERV_STRING ||
      d->data.type == DSERV_SCRIPT ||
      d->data.type == DSERV_JSON)
    return d->varlen+64+d->data.len;
  else
    return d->varlen+64+dpoint_b64_size(d);
}

int dpoint_binary_size(ds_datapoint_t *dpoint)
{
  return dpoint_to_binary(dpoint, NULL, NULL);  
}

int dpoint_to_binary(ds_datapoint_t *dpoint, unsigned char *buf, int *maxsize)
{
  int total_bytes = 0;
  int bufidx = 0;
  uint16_t varlen = strlen(dpoint->varname);

  // Start by seeing how much space we need
  total_bytes += sizeof(uint16_t); // varlen
  total_bytes += varlen;           // strlen(varname)
  total_bytes += sizeof(uint64_t); // timestamp
  total_bytes += sizeof(uint32_t); // datatype
  total_bytes += sizeof(uint32_t); // datalen
  total_bytes += dpoint->data.len;          // data

  // just wanted to know how much space we need
  if (!maxsize)
    return total_bytes;

  // buffer provided is too small
  if (total_bytes > *maxsize)
    return 0;

  memcpy(&buf[bufidx], &varlen, sizeof(uint16_t));
  bufidx += sizeof(uint16_t);

  memcpy(&buf[bufidx], dpoint->varname, varlen);
  bufidx += varlen;

  memcpy(&buf[bufidx], &dpoint->timestamp, sizeof(uint64_t));
  bufidx += sizeof(uint64_t);

  memcpy(&buf[bufidx], &dpoint->data.type, sizeof(uint32_t));
  bufidx += sizeof(uint32_t);

  memcpy(&buf[bufidx], &dpoint->data.len, sizeof(uint32_t));
  bufidx += sizeof(uint32_t);

  memcpy(&buf[bufidx], dpoint->data.buf, dpoint->data.len);
  bufidx += dpoint->data.len;

  return bufidx;
}

ds_datapoint_t *dpoint_from_binary(char *buf, int buflen)
{
  ds_datapoint_t *dpoint =
    (ds_datapoint_t *) calloc(1, sizeof(ds_datapoint_t));

  int bufidx = 0;

  memcpy(&dpoint->varlen, &buf[bufidx], sizeof(uint16_t));
  bufidx+=sizeof(uint16_t);

  dpoint->varname = malloc(dpoint->varlen+1);
  memcpy(dpoint->varname, &buf[bufidx], dpoint->varlen);
  dpoint->varname[dpoint->varlen] = '\0';
  bufidx += dpoint->varlen;

  memcpy(&dpoint->timestamp, &buf[bufidx], sizeof(uint64_t));
  bufidx += sizeof(uint64_t);

  memcpy(&dpoint->data.type, &buf[bufidx], sizeof(uint32_t));
  bufidx += sizeof(uint32_t);

  memcpy(&dpoint->data.len, &buf[bufidx], sizeof(uint32_t));
  bufidx += sizeof(uint32_t);

  dpoint->data.buf = (unsigned char *) malloc(dpoint->data.len);
  memcpy(dpoint->data.buf, &buf[bufidx], dpoint->data.len);
  bufidx += dpoint->data.len;

  return dpoint;
}

int dpoint_to_string(ds_datapoint_t *d, char *buf, int size)
{
  int needed, b64_need;
  int n;
  int datatype;
  char *name, evt_namebuf[32];

  if (d->data.e.dtype == DSERV_EVT) {
    snprintf(evt_namebuf, sizeof(evt_namebuf), "evt:%d:%d", d->data.e.type, d->data.e.subtype);
    name = evt_namebuf;
    datatype = d->data.e.puttype;
  }
  else {
    name = d->varname;
    datatype = d->data.type;
  }
#ifdef __linux__
  needed = snprintf(buf, size, "%s %d %" PRIu64 " %d {",
#else
  needed = snprintf(buf, size, "%s %d %llu %d {",
#endif
                    name, datatype, d->timestamp, d->data.len);

  // can't fit meaningful reply
  if (needed > size-5) return 0;

  n = strlen(buf);

  switch (datatype) {
  case DSERV_STRING:
  case DSERV_SCRIPT:
  case DSERV_JSON:
    // check this!
    if ((n+d->data.len+3) > size) {
      memcpy(&buf[n], "...}", 4);
      buf[n+4] = '\0';
      return n+4;
    }
    else {
      if (d->data.len) {
	memcpy(&buf[n], d->data.buf, d->data.len);
	buf[n+d->data.len] = '}';
	buf[n+d->data.len+1] = '\0';
	return n+d->data.len+1;
      }
      else {
	buf[n] = '}';
	buf[n+1] = '\0';
	return n+1;
      }
    }
    break;
  default:
    b64_need = dpoint_b64_size(d);

    if ((n+b64_need+3) > size) {
      memcpy(&buf[n], "...}", 4);
      buf[n+4] = '\0';
      return n+4;
    }
    else {
      base64encode(d->data.buf, d->data.len, &buf[n], size-n-5);      
      buf[n+b64_need] = '}';
      buf[n+b64_need+1] = '\0';
      return n+b64_need+1;
    }
    break;
  }
  return 0;
}

ds_datapoint_t *dpoint_from_string(char *str, int len)
{
  ds_datapoint_t *d = NULL;
  char *varname;
  int varlen;
  ds_datatype_t datatype;
  uint64_t timestamp;
  unsigned int inlen, outlen, datalen;
  unsigned char *databuf;

  int i;
  char *stops[5], *starts[5], *p = str;
  int nstops = 0, nstarts = 0;
  int pre = 1, opening_brace = 0, inspace = 0;

  // ugly parsing loop to pull out 5 arguments, trying to
  //  account for spaces and braces...
  // could surely be optimized!
  
  for (i = 0; i < len; i++) {
    if (pre) {
      if (p[i] == ' ') {
	inspace = 1;
	continue;
      }
      else {
	pre = 0;
	inspace = 0;
	starts[nstarts++] = str+i;
	continue;
      }
    }
    if (!opening_brace) {
      if (!inspace && p[i] == ' ') {
	inspace = 1;
	stops[nstops++] = str+i;
	continue;
      }
      else if (inspace && p[i] != ' ') {
	if (nstops==4 && !opening_brace && p[i] == '{') {
	  opening_brace = 1;
	  inspace = 0;
	}
	else {
	  inspace = 0;
	  starts[nstarts++] = str+i;
	}
	continue;
      }
    }
    else {
      if (p[i] != ' ') {
	starts[nstarts++] = str+i;
      }
    }
    if (nstarts == 5) break;
  }
  // now scan for final brace

  for (; i < len; i++) {
    if (p[i] == '}') {
      stops[nstops++] = str+i;
      break;
    }
  }

  if (nstarts != 5 || nstops != 5)
    return NULL;
    
  varlen = stops[0]-starts[0];

  varname = (char *) malloc(varlen+1);
  memcpy(varname, starts[0], varlen);
  varname[varlen] = '\0';

  datatype = (ds_datatype_t) strtoul(starts[1], &stops[1], 0);
  timestamp = strtoull(starts[2], &stops[2], 0);
  datalen = strtoul(starts[3], &stops[3], 0);
  inlen = stops[4]-starts[4];

  if (datatype == DSERV_STRING || datatype == DSERV_SCRIPT || datatype == DSERV_JSON) {
    databuf = (unsigned char *) malloc(datalen+1);
    memcpy(databuf, starts[4], datalen);
    databuf[datalen] = '\0';
    // printf("%s %d %lu %d %s\n", varname, datatype, timestamp, datalen, databuf);
  }
  else {
    databuf = (unsigned char *) malloc(datalen);
    outlen = datalen;
    base64decode (starts[4], inlen, databuf, &outlen);
#if DEBUG_OUTPUT
    {
      char result[1024];
      memcpy(result, starts[4], inlen);
      starts[4][inlen] = '\0';
      printf("%s %d %lu %d/%d {%s}\n", varname, datatype, timestamp, datalen, outlen, result);

      base64encode(databuf, datalen, result, sizeof(result));
      printf("%s %d %lu %d/%d {%s}\n", varname, datatype, timestamp, datalen, outlen, result);
    }
#endif    
  }

  d = (ds_datapoint_t *) malloc(sizeof(ds_datapoint_t));
  dpoint_set(d, varname, timestamp, datatype, datalen, databuf);
  
  return d;
}

char *dpoint_to_json(ds_datapoint_t *dpoint)
{
  int i;  
  int n;
  json_t *array;
  int json_flags = 0;
  json_t *json_dpoint, *json_list;
  char *json_str;

  /* DSERV_EVTs are special */
  if (dpoint->data.e.dtype == DSERV_EVT) {
    json_dpoint = json_object();

    json_object_set_new(json_dpoint, "name",
			json_string(dpoint->varname));
    json_object_set_new(json_dpoint, "timestamp",
			json_integer(dpoint->timestamp));
    json_object_set_new(json_dpoint, "dtype",
			json_integer(dpoint->data.e.dtype));
    json_object_set_new(json_dpoint, "e_type",
			json_integer(dpoint->data.e.type));
    json_object_set_new(json_dpoint, "e_subtype",
			json_integer(dpoint->data.e.subtype));
    json_object_set_new(json_dpoint, "e_dtype",
			json_integer(dpoint->data.e.puttype));

    /* Event PUT_types have been recoded into DSERV datatypes */
    if (dpoint->data.e.puttype == DSERV_STRING) {
      json_object_set_new(json_dpoint, "e_params",
			  json_stringn((const char *) dpoint->data.buf,
				       dpoint->data.len));
    }
    else {
      array = json_array();
      switch (dpoint->data.e.puttype) {
      case DSERV_SHORT:
	{
	  uint16_t *vals = (uint16_t *) dpoint->data.buf;
	  n = dpoint->data.len/sizeof(uint16_t);
	  for (i = 0; i < n; i++) {
	    json_array_append_new(array, json_integer(vals[i]));
	  }
	}
	break;
      case DSERV_INT:
	{
	  uint32_t *vals = (uint32_t *) dpoint->data.buf;
	  n = dpoint->data.len/sizeof(uint32_t);
	  for (i = 0; i < n; i++) {
	    json_array_append_new(array, json_integer(vals[i]));
	  }
	}
	break;
      case DSERV_FLOAT:
	{
	  float *vals = (float *) dpoint->data.buf;
	  n = dpoint->data.len/sizeof(float);
	  for (i = 0; i < n; i++) {
	    json_array_append_new(array, json_real(vals[i]));
	  }
	}
	break;
      case DSERV_DOUBLE:
	{
	  double *vals = (double *) dpoint->data.buf;
	  n = dpoint->data.len/sizeof(double);
	  for (i = 0; i < n; i++) {
	    json_array_append_new(array, json_real(vals[i]));
	  }
	}
	break;
      default:
	{
	  /* just an empty set of params */
	}
	break;
      }
      json_object_set_new(json_dpoint, "e_params", array);
    }
  }

  else {
    json_dpoint = json_object();

    json_object_set_new(json_dpoint, "name",
			json_string(dpoint->varname));
    json_object_set_new(json_dpoint, "timestamp",
			json_integer(dpoint->timestamp));
    json_object_set_new(json_dpoint, "dtype",
			json_integer(dpoint->data.type));

    switch (dpoint->data.type) {
    case DSERV_BYTE:
      {
	uint8_t *vals = (uint8_t *) dpoint->data.buf;
	n = dpoint->data.len;
	if (n == 1) {
	  json_object_set_new(json_dpoint, "data", json_integer(vals[0]));
	}
	else {
	  array = json_array();
	  for (i = 0; i < n; i++) {
	    json_array_append_new(array, json_integer(vals[i]));
	  }
	  json_object_set_new(json_dpoint, "data", array);
	}
      }
      break;
    case DSERV_FLOAT:
      {
	float *vals = (float *) dpoint->data.buf;
	n = dpoint->data.len/sizeof(float);
	if (n == 1) {
	  json_object_set_new(json_dpoint, "data", json_real(vals[0]));
	}
	else {
	  array = json_array();
	  for (i = 0; i < n; i++) {
	    json_array_append_new(array, json_real(vals[i]));
	  }
	  json_object_set_new(json_dpoint, "data", array);
	}
      }
      break;
    case DSERV_DOUBLE:
      {
	double *vals = (double *) dpoint->data.buf;
	n = dpoint->data.len/sizeof(double);
	if (n == 1) {
	  json_object_set_new(json_dpoint, "data", json_real(vals[0]));
	}
	else {
	  array = json_array();
	  for (i = 0; i < n; i++) {
	    json_array_append_new(array, json_real(vals[i]));
	  }
	  json_object_set_new(json_dpoint, "data", array);
	}
      }
      break;
    case DSERV_SHORT:
      {
	uint16_t *vals = (uint16_t *) dpoint->data.buf;
	n = dpoint->data.len/sizeof(short);
	if (n == 1) {
	  json_object_set_new(json_dpoint, "data", json_integer(vals[0]));
	}
	else {
	  array = json_array();
	  for (i = 0; i < n; i++) {
	    json_array_append_new(array, json_integer(vals[i]));
	  }
	}
	json_object_set_new(json_dpoint, "data", array);
      }
      break;
    case DSERV_INT:
      {
	uint32_t *vals = (uint32_t *) dpoint->data.buf;
	n = dpoint->data.len/sizeof(int);
	if (n == 1) {
	  json_object_set_new(json_dpoint, "data", json_integer(vals[0]));
	}
	else {
	  array = json_array();
	  for (i = 0; i < n; i++) {
	    json_array_append_new(array, json_integer(vals[i]));
	  }
	}
	json_object_set_new(json_dpoint, "data", array);
      }
      break;
    case DSERV_STRING:
    case DSERV_SCRIPT:
    case DSERV_TRIGGER_SCRIPT:
    case DSERV_JSON:
      {
	if (!dpoint->data.len) {
	  json_object_set_new(json_dpoint, "data", json_string(""));
	} else {
	  json_object_set_new(json_dpoint, "data",
			      json_stringn((const char *) dpoint->data.buf,
					   dpoint->data.len));
	}
      }
      break;
    case DSERV_DG:
      {
	/* turn to b64 data */
	int b64_need = dpoint_b64_size(dpoint);
	char *buf = (char *) malloc(b64_need);
	base64encode(dpoint->data.buf, dpoint->data.len, buf, b64_need);
	json_object_set_new(json_dpoint, "data",
			    json_stringn(buf, b64_need));
	free(buf);
      }
      break;
    default:
      json_decref(json_dpoint);
      return NULL;
      break;
    }
  }
  
  json_str = json_dumps(json_dpoint, json_flags);
  json_decref(json_dpoint);
  return json_str;
}
