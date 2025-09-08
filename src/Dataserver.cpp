#include "sharedqueue.h"
#include "Dataserver.h"
#include "dpoint_process.h"

static int process_requests(Dataserver *dserv);

Dataserver::Dataserver(int argc, char **argv, int port): argc(argc), argv(argv)
{
  m_bDone = false;
  tcpport = port;

  process_thread = std::thread(&process_requests, this);
  net_thread = std::thread(&Dataserver::start_tcp_server, this);
  send_thread = std::thread(&Dataserver::process_send_requests, this);
  logger_thread = std::thread(&Dataserver::process_log_requests, this);
}

Dataserver::~Dataserver()
{
  shutdown();
  net_thread.detach();
  logger_thread.join();
  send_thread.join();
  process_thread.join();
  datapoint_table.clear();
}

int64_t Dataserver::now(void)
{
  std::chrono::time_point<std::chrono::high_resolution_clock> now = 
    std::chrono::high_resolution_clock::now();    
  auto duration = now.time_since_epoch();
  return
    ((int64_t) std::chrono::duration_cast<std::chrono::microseconds>(duration).
     count());
}
  
/*
 * Core datapoint table manipulation functions
 */
  
int Dataserver::add_datapoint_to_table(char *varname,
				       ds_datapoint_t *dpoint)
{
  return datapoint_table.replace(varname, dpoint);
}
  
int Dataserver::update_datapoint(ds_datapoint_t *dpoint)
{
  return datapoint_table.update(dpoint);
}

int Dataserver::find_datapoint(char *varname)
{
  return datapoint_table.exists(varname);
}

ds_datapoint_t *Dataserver::get_datapoint(char *varname)
{
  return datapoint_table.getcopy(varname);
}
  
int Dataserver::delete_datapoint(char *varname)
{
  return datapoint_table.deletepoint(varname);
}

/*
 * clear single point from table
 */
int Dataserver::clear(char *varname)
{
  return datapoint_table.deletepoint(varname);
}

/*
 * clear entire table
 */
void Dataserver::clear()
{
  datapoint_table.clear();
}

ds_datapoint_t *Dataserver::process(ds_datapoint_t *dpoint)
{
  ds_datapoint_t *dp;
  /* execute loaded processors for each datapoint */
  if (process_dpoint(dpoint, &dp) == DPOINT_PROCESS_DSERV) {
    return dpoint_copy(dp);
  }
  return nullptr;
}

void Dataserver::trigger(ds_datapoint_t *dpoint)
{
  if (trigger_matches.is_match(dpoint->varname)) {
    std::string script;
    if (trigger_scripts.find(dpoint->varname, script)) {
      client_request_t client_request;
      client_request.type = REQ_TRIGGER;
      client_request.script = std::move(script);
      client_request.dpoint = dpoint_copy(dpoint);
      queue.push_back(client_request);
    }
  }
}

void Dataserver::set_key_dpoint(void)
{
  std::string keys = datapoint_table.get_keys();
  set((char *) Dataserver::KEYS_POINT_NAME,
      (char *) keys.c_str());
}

void Dataserver::set(ds_datapoint_t &dpoint)
{
  auto dp = dpoint_copy(&dpoint);
  set(dp);
}

void Dataserver::set(char *varname, char *value)
{
  ds_datapoint_t *dp = dpoint_new(varname, now(), DSERV_STRING,
				  strlen(value),
				  (unsigned char *) value);
  set(dp);
}
  
void Dataserver::set(ds_datapoint_t *dpoint)
{
  // make a copy to share to other queue functions
  ds_datapoint_t *dp = dpoint_copy(dpoint);
  
  // add to datatable and note if replaced or new point name
  int replaced = add_datapoint_to_table(dpoint->varname, dpoint);
  
  // process the new point and store result if processor generates result
  ds_datapoint_t *processed_dpoint = process(dp);
  
  // call trigger function and add to notify and logger scripts
  trigger(dp);
  add_to_notify_queue(dp);
  add_to_logger_queue(dp);

  // keep a string of keys as datapoint so clients can monitor
  if (!replaced)
    set_key_dpoint();

  if (processed_dpoint)
    set(processed_dpoint);
  
  // done with copy 
  dpoint_free(dp);
  
}

int Dataserver::copy(char *from_varname, char *to_varname)
{
  ds_datapoint_t *dp = get_datapoint(from_varname);
  if (!dp) {
    return 0;  // source doesn't exist
  }
  
  // Free the old varname and set the new one
  free(dp->varname);
  dp->varname = strdup(to_varname);
  dp->varlen = strlen(to_varname);
  
  // set it - the pointer version takes ownership
  set(dp);
  
  return 1;  // success
}

void Dataserver::update(ds_datapoint_t *dpoint)
{
  /*
   * if update_datapoint returns 1, then point needs to be freed
   *  because it was _not_ inserted into the table
   */
  int updated = update_datapoint(dpoint);
  ds_datapoint_t *processed_dpoint = process(dpoint);

  trigger(dpoint);
  add_to_notify_queue(dpoint);
  add_to_logger_queue(dpoint);

  // keep a string of keys as datapoint so clients can monitor
  if (!updated)
    set_key_dpoint();

  if (processed_dpoint)
    set(processed_dpoint);
    
  /* free if update successful because original point not needed */
  if (updated) dpoint_free(dpoint);
}

int Dataserver::touch(char *varname)
{
  ds_datapoint_t *dp = get_datapoint(varname);
  int found = 0;
  if (dp) {
    found = 1;
    ds_datapoint_t *processed_dpoint = process(dp);
    trigger(dp);
    add_to_notify_queue(dp);
    add_to_logger_queue(dp);

    if (processed_dpoint)
      set(processed_dpoint);

    // free the copy returned by get_datapoint()
    dpoint_free(dp);
  }
  return found;
}

/* see if point exists */
int Dataserver::exists(char *varname)
{
  return find_datapoint(varname);
}

/* get a copy of dpoint from the table */
int Dataserver::get(char *varname, ds_datapoint_t **dpoint)
{
  ds_datapoint_t *dp = get_datapoint(varname);
  if (dp) {
    if (dpoint) *dpoint = dp;
    else dpoint_free(dp);
  }
  return (dp != nullptr);
}
  
char *Dataserver::get_table_keys(void)
{
  std::string keys = datapoint_table.get_keys();
  char *retstr = strdup(keys.c_str());
  return retstr;
}

char *Dataserver::get_dg_dir(void)
{
  std::string dir = datapoint_table.get_dg_dir();
  char *retstr = strdup(dir.c_str());
  return retstr;
}

void Dataserver::add_trigger(char *match, int every, char *script) 
{
  MatchSpec m(match, every);
  trigger_matches.insert(match, m);
  trigger_scripts.insert(match, std::string(script));
}
  
void Dataserver::remove_trigger(char *match)
{
  trigger_matches.remove(match);
  trigger_scripts.remove(match);
}

void Dataserver::remove_all_triggers(void)
{
  trigger_matches.clear();
  trigger_scripts.clear();
}

int Dataserver::tcpip_register(char *host, int port, int flags)
{
  add_new_send_client(host, port, flags);
  return 1;
}

int Dataserver::tcpip_unregister(char *host, int port)
{
  remove_send_client(host, port);
  return 1;
}

int Dataserver::tcpip_add_match(char *host, int port, char *match, int every)
{
  char key[128];
    
  SendClient *send_client;
  snprintf(key, sizeof(key), "%s:%d", host, port);

  if (!send_table.find(key, &send_client)) return 0;

  MatchSpec m(match, every);
  send_client->matches.insert(match, m);
  return 1;
}

int Dataserver::tcpip_remove_match(char *host, int port, char *match)
{
  char key[128];
    
  SendClient *send_client;
  snprintf(key, sizeof(key), "%s:%d", host, port);

  if (!send_table.find(key, &send_client)) return 0;
  send_client->matches.remove(match);
  return 1;
}

int Dataserver::client_add_match(std::string key, char *match, int every)
{
  SendClient *send_client;
  if (!send_table.find(key.c_str(), &send_client)) return 0;
  MatchSpec m(match, every);
  send_client->matches.insert(match, m);
  return 1;
}

int Dataserver::client_add_exact_match(std::string key, char *match, int every)
{
  SendClient *send_client;
  if (!send_table.find(key.c_str(), &send_client)) return 0;
  MatchSpec m(match, MatchSpec::MATCH_EXACT, every);
  send_client->matches.insert(match, m);
  return 1;
}

int Dataserver::client_remove_match(std::string key, char *match)
{
  SendClient *send_client;
  if (!send_table.find(key.c_str(), &send_client)) return 0;
  send_client->matches.remove(match);
  return 1;
}

int Dataserver::client_remove_all_matches(std::string key)
{
  SendClient *send_client;
  if (!send_table.find(key.c_str(), &send_client)) return 0;
  send_client->matches.clear();
  return 1;
}

std::string Dataserver::get_matches(char *host, int port)
{
  char key[128];
    
  SendClient *send_client;
  snprintf(key, sizeof(key), "%s:%d", host, port);

  if (!send_table.find(key, &send_client)) return std::string("{}");
  return (send_client->matches.to_string());
}

std::string Dataserver::get_logger_clients(void)
{
  return log_table.clients();
}

int Dataserver::logger_client_open(std::string filename, bool overwrite)
{
  return (add_new_log_client(filename, overwrite));
}

int Dataserver::logger_client_close(std::string filename)
{
  return (remove_log_client(filename));
}

int Dataserver::logger_client_pause(std::string filename)
{
  return (pause_log_client(filename));
}

int Dataserver::logger_client_start(std::string filename)
{
  return (start_log_client(filename));
}

int Dataserver::logger_add_match(char *path, char *match,
				 int every, int obs, int bufsize)
{
  return (log_add_match(path, match, every, obs, bufsize));
}
  

void Dataserver::shutdown_message(SharedQueue<client_request_t> *q)
{
  client_request_t client_request;
  client_request.type = REQ_SHUTDOWN;
  q->push_back(client_request);
}

void Dataserver::shutdown(void)
{
  static ds_datapoint_t shutdown_dpoint;
  shutdown_dpoint.flags = DSERV_DPOINT_SHUTDOWN_FLAG;

  m_bDone = true;
  shutdown_message(&queue);
  notify_queue.push_back(&shutdown_dpoint);
  logger_queue.push_back(&shutdown_dpoint);
}

bool Dataserver::isDone()
{
  return m_bDone;
}



/***********************************************************************/
/*                      Exported Tcl Bound Commands                    */
/***********************************************************************/

Tcl_Obj *dpoint_to_tclobj(Tcl_Interp *interp,
				 ds_datapoint_t *dpoint)
{
  Tcl_Obj *obj = NULL;
  Tcl_Obj *elt;
  int i, n;
    
  if (!dpoint) return NULL;

  if (!dpoint->data.len) return Tcl_NewObj();
    
  switch (dpoint->data.type) {
  case DSERV_BYTE:
    if (dpoint->data.len == sizeof(unsigned char)) {
      obj = Tcl_NewIntObj(*((int *) dpoint->data.buf));
    }
    else {
      obj = Tcl_NewByteArrayObj(dpoint->data.buf, dpoint->data.len);
    }
    break;
  case DSERV_STRING:
  case DSERV_JSON:
    obj = Tcl_NewStringObj((char *) dpoint->data.buf, dpoint->data.len);
    break;
  case DSERV_FLOAT:
    if (dpoint->data.len == sizeof(float)) {
      obj = Tcl_NewDoubleObj(*((float *) dpoint->data.buf));
    }
    else {
      float *p = (float *) dpoint->data.buf;
      n = dpoint->data.len/sizeof(float);
      elt = Tcl_NewDoubleObj(*p++);
      obj = Tcl_NewListObj(1, &elt);
      for (i = 1; i < n; i++) {
	elt = Tcl_NewDoubleObj(*p++);
	Tcl_ListObjAppendElement(interp, obj, elt);
      }
    }
    break;
  case DSERV_DOUBLE:
    if (dpoint->data.len == sizeof(double)) {
      obj = Tcl_NewDoubleObj(*((double *) dpoint->data.buf));
    }
    else {
      double *p = (double *) dpoint->data.buf;
      n = dpoint->data.len/sizeof(double);
      elt = Tcl_NewDoubleObj(*p++);
      obj = Tcl_NewListObj(1, &elt);
      for (i = 1; i < n; i++) {
	elt = Tcl_NewDoubleObj(*p++);
	Tcl_ListObjAppendElement(interp, obj, elt);
      }
    }
    break;
  case DSERV_SHORT:
    if (dpoint->data.len == sizeof(short)) {
      obj = Tcl_NewIntObj(*((short *) dpoint->data.buf));
    }
    else {
      short *p = (short *) dpoint->data.buf;
      n = dpoint->data.len/sizeof(short);
      elt = Tcl_NewIntObj(*p++);
      obj = Tcl_NewListObj(1, &elt);
      for (i = 1; i < n; i++) {
	elt = Tcl_NewIntObj(*p++);
	Tcl_ListObjAppendElement(interp, obj, elt);
      }
    }
    break;
  case DSERV_INT:
    if (dpoint->data.len == sizeof(int)) {
      obj = Tcl_NewIntObj(*((int *) dpoint->data.buf));
    }
    else {
      int *p = (int *) dpoint->data.buf;
      n = dpoint->data.len/sizeof(int);
      elt = Tcl_NewIntObj(*p++);
      obj = Tcl_NewListObj(1, &elt);
      for (i = 1; i < n; i++) {
	elt = Tcl_NewIntObj(*p++);
	Tcl_ListObjAppendElement(interp, obj, elt);
      }
    }
    break;
  case DSERV_DG:
  case DSERV_MSGPACK:
  case DSERV_ARROW:
  case DSERV_JPEG:
  case DSERV_PPM:
    obj = Tcl_NewByteArrayObj(dpoint->data.buf, dpoint->data.len);
    break;
  case DSERV_SCRIPT:
  case DSERV_TRIGGER_SCRIPT:
    obj = Tcl_NewStringObj((char *) dpoint->data.buf, dpoint->data.len);
    break;
  case DSERV_EVT:
  case DSERV_NONE:
  case DSERV_UNKNOWN:
    obj = NULL;
    break;
  }
  return obj;
}

int now_command(ClientData data, Tcl_Interp * interp, int objc,
		Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  auto ts = ds->now();
  Tcl_SetObjResult(interp, Tcl_NewWideIntObj(ts));
  return TCL_OK;
}

int dserv_keys_command(ClientData data, Tcl_Interp * interp, int objc,
		       Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  std::string keys = ds->get_table_keys();
  Tcl_SetObjResult(interp, Tcl_NewStringObj(keys.c_str(), -1));
  return TCL_OK;
}

int dserv_dgdir_command(ClientData data, Tcl_Interp * interp, int objc,
			Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  std::string dir = ds->get_dg_dir();
  Tcl_SetObjResult(interp, Tcl_NewStringObj(dir.c_str(), -1));
  return TCL_OK;
}

int dserv_clear_command(ClientData data, Tcl_Interp * interp, int objc,
			Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  int result = TCL_OK;
  
  if (objc == 1) {
    ds->clear();
  }
  else {
    for (int i = 1; i < objc; i++) {
      ds->clear(Tcl_GetString(objv[i]));
    }
  }
  return result;
}

int dserv_exists_command(ClientData data, Tcl_Interp * interp, int objc,
		      Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  Tcl_Obj *obj;
  int exists;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "varname");
    return TCL_ERROR;
  }
  
  exists = ds->find_datapoint(Tcl_GetString(objv[1]));
  Tcl_SetObjResult(interp, Tcl_NewIntObj(exists));

  return TCL_OK;
}


int dserv_get_command(ClientData data, Tcl_Interp * interp, int objc,
		      Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  Tcl_Obj *obj;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "varname");
    return TCL_ERROR;
  }
  
  ds_datapoint_t *dpoint;
  dpoint = ds->get_datapoint(Tcl_GetString(objv[1]));
  
  if (!dpoint) {
    Tcl_AppendResult(interp, "dpoint \"",
		     Tcl_GetString(objv[1]),
		     "\" not found", NULL);
    return TCL_ERROR;
  }
  
  obj = dpoint_to_tclobj(interp, dpoint);
  if (obj)
    Tcl_SetObjResult(interp, obj);
  
  dpoint_free(dpoint);
  
  return TCL_OK;
}

int dserv_info_command(ClientData data, Tcl_Interp * interp, int objc,
                       Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  Tcl_Obj *dictObj;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "varname");
    return TCL_ERROR;
  }
  
  ds_datapoint_t *dpoint;
  dpoint = ds->get_datapoint(Tcl_GetString(objv[1]));
  
  if (!dpoint) {
    Tcl_AppendResult(interp, "dpoint \"",
                     Tcl_GetString(objv[1]),
                     "\" not found", NULL);
    return TCL_ERROR;
  }
  
  // Create a new dictionary object
  dictObj = Tcl_NewDictObj();
  
  // Add varname
  Tcl_DictObjPut(interp, dictObj, 
                 Tcl_NewStringObj("varname", -1),
                 Tcl_NewStringObj(dpoint->varname, -1));
  
  // Add timestamp
  Tcl_DictObjPut(interp, dictObj,
                 Tcl_NewStringObj("timestamp", -1),
                 Tcl_NewWideIntObj(dpoint->timestamp));
  
  // Add data type as string
  const char *typeStr;
  switch (dpoint->data.type) {
    case DSERV_BYTE:           typeStr = "BYTE"; break;
    case DSERV_STRING:         typeStr = "STRING"; break;
    case DSERV_FLOAT:          typeStr = "FLOAT"; break;
    case DSERV_DOUBLE:         typeStr = "DOUBLE"; break;
    case DSERV_SHORT:          typeStr = "SHORT"; break;
    case DSERV_INT:            typeStr = "INT"; break;
    case DSERV_DG:             typeStr = "DG"; break;
    case DSERV_SCRIPT:         typeStr = "SCRIPT"; break;
    case DSERV_TRIGGER_SCRIPT: typeStr = "TRIGGER_SCRIPT"; break;
    case DSERV_EVT:            typeStr = "EVT"; break;
    case DSERV_NONE:           typeStr = "NONE"; break;
    case DSERV_JSON:           typeStr = "JSON"; break;
    case DSERV_ARROW:          typeStr = "ARROW"; break;
    case DSERV_MSGPACK:        typeStr = "MSGPACK"; break;
    case DSERV_JPEG:           typeStr = "JPEG"; break;
    case DSERV_PPM:            typeStr = "PPM"; break;
    default:                   typeStr = "UNKNOWN"; break;
  }
  Tcl_DictObjPut(interp, dictObj,
                 Tcl_NewStringObj("type", -1),
                 Tcl_NewStringObj(typeStr, -1));
  
  // Add type as numeric value
  Tcl_DictObjPut(interp, dictObj,
                 Tcl_NewStringObj("type_id", -1),
                 Tcl_NewIntObj(dpoint->data.type));
  
  // Add data length
  Tcl_DictObjPut(interp, dictObj,
                 Tcl_NewStringObj("length", -1),
                 Tcl_NewIntObj(dpoint->data.len));
  
  // Add flags if they're meaningful
  Tcl_DictObjPut(interp, dictObj,
                 Tcl_NewStringObj("flags", -1),
                 Tcl_NewIntObj(dpoint->flags));
  
  // Special handling for EVT type - add event-specific info
  if (dpoint->data.e.dtype == DSERV_EVT) {
    Tcl_DictObjPut(interp, dictObj,
                   Tcl_NewStringObj("event_type", -1),
                   Tcl_NewIntObj(dpoint->data.e.type));
    
    Tcl_DictObjPut(interp, dictObj,
                   Tcl_NewStringObj("event_subtype", -1),
                   Tcl_NewIntObj(dpoint->data.e.subtype));
    
    Tcl_DictObjPut(interp, dictObj,
                   Tcl_NewStringObj("event_puttype", -1),
                   Tcl_NewIntObj(dpoint->data.e.puttype));
  }
  
  // Calculate element count for array types
  if (dpoint->data.len > 0 && dpoint->data.buf != NULL) {
    int element_count = 0;
    switch (dpoint->data.type) {
      case DSERV_BYTE:
        element_count = dpoint->data.len;
        break;
      case DSERV_SHORT:
        element_count = dpoint->data.len / sizeof(uint16_t);
        break;
      case DSERV_INT:
        element_count = dpoint->data.len / sizeof(uint32_t);
        break;
      case DSERV_FLOAT:
        element_count = dpoint->data.len / sizeof(float);
        break;
      case DSERV_DOUBLE:
        element_count = dpoint->data.len / sizeof(double);
        break;
      default:
        element_count = -1;  // Not an array type
        break;
    }
    
    if (element_count > 0) {
      Tcl_DictObjPut(interp, dictObj,
                     Tcl_NewStringObj("element_count", -1),
                     Tcl_NewIntObj(element_count));
    }
  }
  
  // Set the dictionary as the result
  Tcl_SetObjResult(interp, dictObj);
  
  // Clean up
  dpoint_free(dpoint);
  
  return TCL_OK;
}

int dserv_copy_command(ClientData data, Tcl_Interp * interp, int objc,
                       Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  
  if (objc != 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "from_varname to_varname");
    return TCL_ERROR;
  }
  
  int result = ds->copy(Tcl_GetString(objv[1]), Tcl_GetString(objv[2]));
  Tcl_SetObjResult(interp, Tcl_NewIntObj(result));
  
  return TCL_OK;
}

int dserv_setdata_command (ClientData data, Tcl_Interp *interp,
			   int objc, Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  Tcl_Size len;
  int rc;
  unsigned char *datastr;
  int datatype;
  Tcl_WideInt ts;
    
  if (objc < 5) {
    Tcl_WrongNumArgs(interp, 1,
		     (Tcl_Obj **)objv, "var timestamp datatype bytes");
    return TCL_ERROR;
  }
    
  //get timestamp
  if (Tcl_GetWideIntFromObj(interp, objv[2], &ts) != TCL_OK)
    return TCL_ERROR;
  if (!ts) ts = ds->now();
    
    
  // get datatype
  if (Tcl_GetIntFromObj(interp, objv[3], &datatype) != TCL_OK) {
    return TCL_ERROR;
  }
    
  // get data
  datastr = Tcl_GetByteArrayFromObj(objv[4], &len);
  if (!datastr) { 
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]), ": invalid data",
		     (char *) NULL);
    return TCL_ERROR;
  }
    
  ds_datapoint_t *dpoint = dpoint_new(Tcl_GetString(objv[1]), ts ? ts : ds->now(),
				      (ds_datatype_t) datatype, len, datastr);
  ds->set(dpoint);
    
  return (TCL_OK);
}
  
int dserv_setdata64_command (ClientData data, Tcl_Interp *interp,
			     int objc, Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  Tcl_Size len;
  int rc;
  char *datastr;
  unsigned char buf[256];
  unsigned int outlen = sizeof(buf);
  int datatype;
  Tcl_WideInt ts;
    
  if (objc < 5) {
    Tcl_WrongNumArgs(interp, 1, objv, "var timestamp datatype b64_data");
    return TCL_ERROR;
  }
    
  //get timestamp
  if (Tcl_GetWideIntFromObj(interp, objv[2], &ts) != TCL_OK)
    return TCL_ERROR;
  if (!ts) ts = ds->now();
    
    
  // get datatype
  if (Tcl_GetIntFromObj(interp, objv[3], &datatype) != TCL_OK) {
    return TCL_ERROR;
  }
    
  // get data
  datastr = Tcl_GetStringFromObj(objv[4], &len);
  if (!datastr) return TCL_ERROR;
    
  // decode data
  if (base64decode(datastr, len, buf, &outlen) != 0) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]), ": buffer exceeded",
		     (char *) NULL);
    return TCL_ERROR;
  }

  ds_datapoint_t *dpoint = dpoint_new(Tcl_GetString(objv[1]), ts ? ts : ds->now(),
				      (ds_datatype_t) datatype, outlen, buf);
  ds->set(dpoint);
    
  return (TCL_OK);
}
  
int dserv_timestamp_command(ClientData data, Tcl_Interp *interp,
			    int objc, Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  ds_datapoint_t *dpoint;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "var");
    return TCL_ERROR;
  }
  
  dpoint = ds->get_datapoint(Tcl_GetString(objv[1]));
  
  if (!dpoint) {
    Tcl_AppendResult(interp, "dpoint \"",
		     Tcl_GetString(objv[1]),
		     "\" not found", NULL);
    return TCL_ERROR;
  }
    
  Tcl_SetObjResult(interp, Tcl_NewWideIntObj(dpoint->timestamp));
  dpoint_free(dpoint);
    
  return TCL_OK;
}
   
int dserv_touch_command(ClientData data, Tcl_Interp * interp, int objc,
			Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "varname");
    return TCL_ERROR;
  }
  ds->touch(Tcl_GetString(objv[1]));
  return TCL_OK;
}

int dserv_set_command(ClientData data, Tcl_Interp * interp, int objc,
		      Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "varname value");
    return TCL_ERROR;
  }
  
  ds->set(Tcl_GetString(objv[1]), Tcl_GetString(objv[2]));
  
  return TCL_OK;
}

int dserv_eval_command(ClientData data, Tcl_Interp * interp, int objc,
		       Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "script");
    return TCL_ERROR;
  }
  std::string s(ds->eval(Tcl_GetString(objv[1])));
  
  Tcl_SetObjResult(interp, Tcl_NewStringObj(s.data(), s.size()));
  
  return TCL_OK;
}

int process_get_param_command(ClientData data, Tcl_Interp * interp, int objc,
			      Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  char *retstr;
  int index = 0;
    
  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "processor param [index]");
    return TCL_ERROR;
  }
  if (objc > 3) {
    if (Tcl_GetIntFromObj(interp, objv[3], &index) != TCL_OK) {
      return TCL_ERROR;
    }
  }
  retstr = process_get_param(Tcl_GetString(objv[1]),
			     Tcl_GetString(objv[2]),
			     index);
  if (retstr) {
    Tcl_SetObjResult(interp, Tcl_NewStringObj(retstr, strlen(retstr)));
  }
  return TCL_OK;
}

int process_set_param_command(ClientData data,
			      Tcl_Interp * interp, int objc,
			      Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  int ret;
  int index = 0;
    
  if (objc < 4) {
    Tcl_WrongNumArgs(interp, 1, objv, "processor param value [index]");
    return TCL_ERROR;
  }
  if (objc > 4) {
    if (Tcl_GetIntFromObj(interp, objv[4], &index) != TCL_OK) {
      return TCL_ERROR;
    }
  }
  ds_datapoint_t *out;
  ret = process_set_param(Tcl_GetString(objv[1]),
			  Tcl_GetString(objv[2]),
			  Tcl_GetString(objv[3]),
			  index, ds->now(), &out);
  if (ret == DPOINT_PROCESS_DSERV) {
    ds_datapoint_t *dp = dpoint_copy(out);
    ds->set(dp);
  }
  Tcl_SetObjResult(interp, Tcl_NewIntObj(ret));
  return TCL_OK;
}



static int process_load_command(ClientData data, Tcl_Interp * interp, int objc,
				 Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  int ret;
    
  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "processor_path name");
    return TCL_ERROR;
  }
  ret = process_load(Tcl_GetString(objv[1]), Tcl_GetString(objv[2]));
  if (ret < 0) {
    std::string result_str;
    result_str = "error loading processor (" + std::to_string(ret) + ")";
    Tcl_AppendResult(interp, result_str.c_str(), NULL);
    return TCL_ERROR;
  }
  else return TCL_OK;
}

static int process_attach_command(ClientData data, Tcl_Interp * interp, int objc,
				  Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  int ret;
    
  if (objc < 4) {
    Tcl_WrongNumArgs(interp, 1, objv, "name varname processor_name");
    return TCL_ERROR;
  }
  ret = process_attach(Tcl_GetString(objv[1]),
		       Tcl_GetString(objv[2]),
		       Tcl_GetString(objv[3]));
  if (ret < 0) {
    std::string result_str;
    result_str = "error attaching processor (" + std::to_string(ret) + ")";
    Tcl_AppendResult(interp, result_str.c_str(), NULL);
    return TCL_ERROR;
  }
  else return TCL_OK;
}

static int trigger_add_command(ClientData data, Tcl_Interp * interp, int objc,
			       Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  int every;
  if (objc < 4) {
    Tcl_WrongNumArgs(interp, 1, objv, "varname every script");
    return TCL_ERROR;
  }
  if (Tcl_GetIntFromObj(interp, objv[2], &every) != TCL_OK) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]), ": invalid argument", NULL);
    return TCL_ERROR;
  }
    
  ds->add_trigger(Tcl_GetString(objv[1]), every, Tcl_GetString(objv[3]));
  return TCL_OK;
}

static int trigger_remove_command(ClientData data, Tcl_Interp * interp, int objc,
				  Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  int every;
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "varname");
    return TCL_ERROR;
  }
  ds->remove_trigger(Tcl_GetString(objv[1]));
  return TCL_OK;
}

static int trigger_remove_all_command(ClientData data, Tcl_Interp * interp, int objc,
				       Tcl_Obj * const objv[])
{
  Dataserver *ds = (Dataserver *) data;
  int every;
  ds->remove_all_triggers();
  return TCL_OK;
}


static void add_tcl_commands(Tcl_Interp *interp, Dataserver *dserv)
{
  Tcl_CreateObjCommand(interp, "now", now_command, dserv, NULL);
  Tcl_CreateObjCommand(interp, "triggerAdd",
		       trigger_add_command, dserv, NULL);
  Tcl_CreateObjCommand(interp, "triggerRemove",
		       trigger_remove_command, dserv, NULL);
  Tcl_CreateObjCommand(interp, "triggerRemoveAll",
		       trigger_remove_all_command, dserv, NULL);
  Tcl_CreateObjCommand(interp, "dservExists",
		       dserv_exists_command, dserv, NULL); 
  Tcl_CreateObjCommand(interp, "dservGet",
		       dserv_get_command, dserv, NULL);
  Tcl_CreateObjCommand(interp, "dservInfo",
		       dserv_info_command, dserv, NULL);
  Tcl_CreateObjCommand(interp, "dservCopy",
		       dserv_copy_command, dserv, NULL);  
  Tcl_CreateObjCommand(interp, "dservTouch",
		       dserv_touch_command, dserv, NULL);
  Tcl_CreateObjCommand(interp, "dservTimestamp",
		       dserv_timestamp_command, dserv, NULL);
  Tcl_CreateObjCommand(interp, "dservSet",
		       dserv_set_command, dserv, NULL);
  Tcl_CreateObjCommand(interp, "dservSetData",
		       dserv_setdata_command, dserv, NULL);
  Tcl_CreateObjCommand(interp, "dservSetData64",
		       dserv_setdata64_command, dserv, NULL);
  Tcl_CreateObjCommand(interp, "dservClear",
		       dserv_clear_command, dserv, NULL);
  Tcl_CreateObjCommand(interp, "dservKeys",
		       dserv_keys_command, dserv, NULL);
  Tcl_CreateObjCommand(interp, "dservDGDir",
		       dserv_dgdir_command, dserv, NULL);

  Tcl_CreateObjCommand(interp, "processLoad",
		       process_load_command, dserv, NULL);
  Tcl_CreateObjCommand(interp, "processAttach",
		       process_attach_command, dserv, NULL);
  /*
   * For now we limit get/set to being called from TclServer
   *  as these are not thread safe
   */
#if 0    
  Tcl_CreateObjCommand(interp, "processGetParam",
		       process_get_param_command, dserv, NULL);
  Tcl_CreateObjCommand(interp, "processSetParam",
		       process_set_param_command, dserv, NULL);
#endif
  return;
}


static int Tcl_StimAppInit(Tcl_Interp *interp, Dataserver *dserv)
{
  if (Tcl_Init(interp) == TCL_ERROR) return TCL_ERROR;
  
  add_tcl_commands(interp, dserv);
  
  return TCL_OK;
}

static Tcl_Interp *setup_tcl(Dataserver *dserv)
{
  Tcl_Interp *interp;
  
  Tcl_FindExecutable(dserv->argv[0]);
  interp = Tcl_CreateInterp();
  if (!interp) {
    std::cerr << "Error initialializing tcl interpreter" << std::endl;
    return interp;
  }
#if 0
  if (TclZipfs_Mount(interp, "/usr/local/dserv/dataserver.zip", "app", NULL) != TCL_OK) {
    //    std::cerr << "Dataserver: error mounting zipfs" << std::endl;
  }
  else {
    //    std::cerr << "Mounted zipfs" << std::endl;
  }
#endif

  TclZipfs_AppHook(&dserv->argc, &dserv->argv);

  /*
   * Invoke application-specific initialization.
   */
  
  if (Tcl_StimAppInit(interp, dserv) != TCL_OK) {
    std::cerr << "application-specific initialization failed: ";
    std::cerr << Tcl_GetStringResult(interp) << std::endl;
  }
  else {
    Tcl_SourceRCFile(interp);
  }
  
  return interp;
}
  
std::string Dataserver::eval(std::string script)
{
  SharedQueue<std::string> rqueue;

  client_request_t client_request;
  client_request.type = REQ_SCRIPT;
  client_request.rqueue = &rqueue;
  client_request.script = script;
    
  // std::cout << "TCL Request: " << std::string(buf, n) << std::endl;
    
  queue.push_back(client_request);
    
  /* rqueue will be available after command has been processed */
  std::string s(client_request.rqueue->front());
  client_request.rqueue->pop_front();

  return s;
}

void Dataserver::eval_noreply(std::string script)
{
  SharedQueue<std::string> rqueue;
    
  client_request_t client_request;
  client_request.type = REQ_SCRIPT_NOREPLY;
  client_request.script = script;
    
  // std::cout << "TCL Request: " << std::string(buf, n) << std::endl;
    
  std::unique_lock<std::mutex> mlock(mutex);
  queue.push_back(client_request);
  cond.wait(mlock);
    
}
  
static int process_requests(Dataserver *dserv) {
  
  // Create Tcl interpreter for storing datapoints and executing scripts
  Tcl_Interp *interp = setup_tcl(dserv);

  int retcode;
  client_request_t req;
    
  /* process until receive a message saying we are done */
  while (!dserv->m_bDone) {
    req = dserv->queue.front();
    dserv->queue.pop_front();
    switch(req.type) {
    case REQ_SCRIPT:
      {
	const char *script = req.script.c_str();

	retcode = Tcl_Eval(interp, script);
	const char *rcstr = Tcl_GetStringResult(interp);
	  
	if (retcode == TCL_OK) {
	  if (rcstr) {
	    req.rqueue->push_back(std::string(rcstr));
	  }
	  else {
	    req.rqueue->push_back("");
	  }
	}
	else {
	  if (rcstr) {
	    req.rqueue->push_back("!TCL_ERROR "+std::string(rcstr));
	    //      std::cout << "Error: " + std::string(rcstr) << std::endl;
	      
	  }
	  else {
	    req.rqueue->push_back("Error:");
	  }
	}
      }
      break;
    case REQ_SCRIPT_NOREPLY:
      {
	const char *script = req.script.c_str();
	retcode = Tcl_Eval(interp, script);
      }
      break;
    case REQ_TRIGGER:
      {
	const char *script = req.script.c_str();
	ds_datapoint_t *dpoint = req.dpoint;
	
	Tcl_Obj *commandArray[3];
	commandArray[0] = Tcl_NewStringObj(script, -1);

	/* name of dpoint (special for DSERV_EVTs */
	if (dpoint->data.e.dtype != DSERV_EVT) {
	  /* point name */
	  commandArray[1] = Tcl_NewStringObj(dpoint->varname, dpoint->varlen);
	  /* data as Tcl_Obj */
	  commandArray[2] = dpoint_to_tclobj(interp, dpoint);
	}
	else {
	  /* convert eventlog/events -> evt:TYPE:SUBTYPE notation */
	  char evt_namebuf[32];
	  snprintf(evt_namebuf, sizeof(evt_namebuf), "evt:%d:%d",
		   dpoint->data.e.type, dpoint->data.e.subtype);
	  commandArray[1] = Tcl_NewStringObj(evt_namebuf, -1);
	  
	  /* create a repackaged dpoint to pass to dpoint_to_tclobj */
	  ds_datapoint_t e_dpoint;
	  e_dpoint.data.type = (ds_datatype_t) dpoint->data.e.puttype;
	  e_dpoint.data.len = dpoint->data.len;
	  e_dpoint.data.buf = dpoint->data.buf;
	  
	  /* data as Tcl_Obj */
	  commandArray[2] = dpoint_to_tclobj(interp, &e_dpoint);
	}

	/* done with this point */
	dpoint_free(dpoint);

	/* incr ref count on command arguments */
	for (int i = 0; i < 3; i++) { Tcl_IncrRefCount(commandArray[i]); }

	/* call command */
	retcode = Tcl_EvalObjv(interp, 3, commandArray, 0);

	/* decr ref count on command arguments */
	for (int i = 0; i < 3; i++) { Tcl_DecrRefCount(commandArray[i]); }
      }
      break;
    default:
      break;
    }
  }

  Tcl_DeleteInterp(interp);
  //  std::cout << "Dataserver process thread ended" << std::endl;

  return 0;
}
  
  
void Dataserver::start_tcp_server(void)
{
  struct sockaddr_in address;
  struct sockaddr client_address;
  socklen_t client_address_len = sizeof(client_address);
  int new_socket_fd;		// client socket
  int on = 1;
    
  //std::cout << "server on port " << std::to_string(tcpport) << std::endl;

  /* Initialise IPv4 address. */
  memset(&address, 0, sizeof(struct sockaddr_in));
  address.sin_family = AF_INET;
  address.sin_port = htons(tcpport);
  address.sin_addr.s_addr = INADDR_ANY;

        
  /* Create TCP socket. */
  if ((socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    perror("socket");
    return;
  }

  /* Allow this server to reuse the port immediately */
  setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  
  /* Bind address to socket. */
  if (bind(socket_fd, (const struct sockaddr *) &address,
	   sizeof (struct sockaddr)) == -1) {
    perror("bind");
    return;
  }

  /* Listen on socket. */
  if (listen(socket_fd, 20) == -1) {
    perror("listen");
    return;
  }

  while (1) {
    /* Accept connection to client. */
    new_socket_fd = accept(socket_fd, &client_address, &client_address_len);
    if (new_socket_fd == -1) {
      perror("accept");
      continue;
    }
      
    setsockopt(new_socket_fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

    // Create a thread and transfer the new stream to it.
    std::thread thr(tcp_client_process, this, new_socket_fd);
    thr.detach();
  }
}

/***********************************************************************/
/***************************** send support ****************************/
/***********************************************************************/

int Dataserver::add_to_notify_queue(ds_datapoint_t *dpoint)
{
  ds_datapoint_t *dp = dpoint_copy(dpoint);
  notify_queue.push_back(dp);
  return 0;
}

int Dataserver::move_to_notify_queue(ds_datapoint_t *dpoint)
{
  notify_queue.push_back(dpoint);
  return 0;
}

int Dataserver::process_send_requests(void) {
  int retcode;
  ds_datapoint_t *dpoint;
  SendClient *send_client;
    
  /* process until receive a message saying we are done */
  while (!m_bDone) {
      
    dpoint = notify_queue.front();
    notify_queue.pop_front();

    if (dpoint->flags & DSERV_DPOINT_SHUTDOWN_FLAG) {
      continue;
    }
    
    // loop through all send_clients and decide if inactive
    // or if point matches subscription
    send_table.forward_dpoint(dpoint);

    /* dpoints need to be freed after forwarding */
    dpoint_free(dpoint);
  }

  // send clients are all closed in the send_table destructor
  
  //  std::cout << "Notify process thread ended" << std::endl;
  return 0;
}


/*
 * add_new_send_client
 *
 * create client thread to manage send requests for TCP/IP notifications
 */
int Dataserver::add_new_send_client(char *host, int port, uint8_t flags)
{
  int send_socket;
    
  int newentry;
  SendClient *send_client;

  std::thread send_thread_id;

  char key[128];
  snprintf(key, sizeof(key), "%s:%d", host, port);

  // remove existing table;
  if (send_table.find(key, &send_client)) {
    send_table.remove(key);
    send_client->dpoint_queue.push_back(&send_client->shutdown_dpoint);
  }

  // attempt to open new socket
  send_socket =  open_send_sock(host, port);
  if (send_socket < 0)
    return 0;
    
  // create a new entry for this client
  send_client = new SendClient(send_socket, host, port, flags);
  send_thread_id = std::thread(&SendClient::send_client_process,
			       send_client);
  send_thread_id.detach();
  send_table.insert(key, send_client);

  return 1;
}

/*
 * add_new_send_client
 *
 * create client thread to manage send requests for TCP/IP notifications
 */
std::string Dataserver::add_new_send_client(SharedQueue<client_request_t> *queue)
{
  static std::atomic<int> client_counter{0};
  
  // Create a unique client ID combining pointer and counter
  char client_id[64];
  snprintf(client_id, sizeof(client_id), "queue_%p_%d", (void *) queue, client_counter.fetch_add(1));
  std::string client_name = std::string(client_id);
  const char *key = client_name.c_str();
  
  // Remove existing entry if it exists
  SendClient *existing_client;
  if (send_table.find(key, &existing_client)) {
    send_table.remove(key);
    existing_client->dpoint_queue.push_back(&existing_client->shutdown_dpoint);
  }

  // Create a new entry for this client
  SendClient *send_client = new SendClient(queue);
  std::thread send_thread_id = std::thread(&SendClient::send_client_process, send_client);
  send_thread_id.detach();
  send_table.insert(key, send_client);

  return client_name;
}

int Dataserver::remove_send_client_by_id(std::string client_id)
{
  SendClient *send_client;
  
  if (send_table.find(client_id, &send_client)) {
    send_table.remove(client_id);
    send_client->dpoint_queue.push_back(&send_client->shutdown_dpoint);
    return 1;
  }
  return 0;
}

int Dataserver::remove_send_client(char *host, int port)
{
  SendClient *send_client;
    
  char key[128];
  snprintf(key, sizeof(key), "%s:%d", host, port);
    
  if (send_table.find(key, &send_client)) {
    send_table.remove(key);
    send_client->dpoint_queue.push_back(&send_client->shutdown_dpoint);
    return 1;
  }
  else return 0;
}
  
/* open a client socket connection and return socket */  
int Dataserver::open_send_sock(char *host, int port)
{			 
  struct sockaddr_in sendserver;
  struct hostent *hp;
  int sendsock;
  int on = 1;
  int flags, blocking;
  fd_set myset; 
  struct timeval tv; 
  unsigned int lon, valopt;
  int res;
    
  sendsock = socket(AF_INET, SOCK_STREAM, 0);
    
  if (sendsock < 0) {
    return -1;
  }
    
  memset(&sendserver, 0, sizeof(struct sockaddr_in));
  sendserver.sin_family = AF_INET;
  hp = gethostbyname(host);
  if (hp == 0) {
    return -1;
  }
    
  memcpy(&sendserver.sin_addr, hp->h_addr, hp->h_length);
  sendserver.sin_port = htons(port);
    
  /* set socket to nonblocking mode for the connect step */
#ifdef _WIN32
  on = 1;
  if (ioctl(sendsock, FIONBIO, &on) < 0) {
    perror("ioctl F_SETFL, FNDELAY");
    close(sendsock);
    return -1;
  }
#else
  blocking = 1;
  flags = fcntl(sendsock, F_GETFL, 0);
  if (flags == -1) return -1;
  flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
  fcntl(sendsock, F_SETFL, flags); 
#endif
    
  if ((res = connect(sendsock, (struct sockaddr *) &sendserver,
		     sizeof(sendserver))) < 0) {
    if (res < 0) { 
      if (errno == EINPROGRESS) { 
	do { 
	  tv.tv_sec = 1; 
	  tv.tv_usec = 0; 
	  FD_ZERO(&myset); 
	  FD_SET(sendsock, &myset); 
	  res = select(sendsock+1, NULL, &myset, NULL, &tv); 
	  if (res < 0 && errno != EINTR) { 
	    return -1;
	  } 
	  else if (res > 0) { 
	    // Socket selected for write 
	    lon = sizeof(int); 
	    if (getsockopt(sendsock, SOL_SOCKET, SO_ERROR,
			   (void*)(&valopt), &lon) < 0) { 
	      return -1;
	    } 
	    // Check the value returned... 
	    if (valopt) { 
	      return -1;
	    }
	      
	    // success!
	    break;
	  } 
	  else {
	    // This will happen when requester is behind firewall, e.g.
	    //  fprintf(stderr, "Timeout in select() - Cancelling!\n"); 
	    return -1;
	  } 
	} while (1); 
      } 
      else { 
	return -1;
      }
    }
      
    /* set socket back to blocking mode for the connect step */
#ifdef _WIN32
    on = 0;
    if (ioctl(sendsock, FIONBIO, &on) < 0) {
      perror("ioctl F_SETFL, FNDELAY");
      close(sendsock);
      return -1;
    }
#else
    blocking = 0;
    flags = fcntl(sendsock, F_GETFL, 0);
    if (flags == -1) return -1;
    flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    fcntl(sendsock, F_SETFL, flags);
#endif
      
    /* Don't buffer packets */
    on = 1;
    setsockopt(sendsock, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
  }
  return sendsock;
}


/***********************************************************************/
/****************************** log support ****************************/
/***********************************************************************/

int Dataserver::add_to_logger_queue(ds_datapoint_t *dpoint)
{
  ds_datapoint_t *dp = dpoint_copy(dpoint);
  logger_queue.push_back(dp);
  return 0;
}

int Dataserver::move_to_logger_queue(ds_datapoint_t *dpoint)
{
  logger_queue.push_back(dpoint);
  return 0;
}

int Dataserver::process_log_requests(void) {
  int retcode;
  ds_datapoint_t *dpoint;
  LogClient *log_client;
    
  /* process until receive a message saying we are done */
  while (!m_bDone) {
      
    dpoint = logger_queue.front();
    logger_queue.pop_front();

    if (dpoint->flags & DSERV_DPOINT_SHUTDOWN_FLAG) {
      continue;
    }
	
    /*
     * loop through all logger_clients and forward if subscribed
     */
    log_table.forward_dpoint(dpoint);
      
    /* dpoints need to be freed after forwarding */
    if (!(dpoint->flags & DSERV_DPOINT_DONTFREE_FLAG)) {
      dpoint_free(dpoint);
      //	std::cout << "dpoint: " << dpoint->varname << std::endl;
    }
  }

  // log clients are all closed in the log_table destructor
  
  //  std::cout << "Logger process thread ended" << std::endl;
  
  return 0;
}
  
static int open_log_file(std::string filename, bool overwrite = false)
{
  int fd;
    
  if (!overwrite) {
    fd = open(filename.c_str(),
	      O_WRONLY | O_EXCL | O_CREAT,
	      S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH );
  }
  else {
    fd = open(filename.c_str(),
	      O_WRONLY | O_TRUNC | O_CREAT,
	      S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH );
  }
  return fd;
} 
  
int Dataserver::add_new_log_client(std::string filename, bool overwrite)
{
  int newentry;
  std::thread log_thread_id;
  LogClient *log_client;
  int fd;
    
  // remove existing table;
  if (log_table.find(filename, &log_client))
    return 0;
    
  fd = open_log_file(filename, overwrite);
  if (fd < 0) return -1;
    
  log_client = new LogClient(filename, fd);

  // give access to log_table so it can remove itself
  log_client->log_table = &log_table;
  
  log_thread_id = std::thread(&LogClient::log_client_process,
			      log_client);

  // wait for new thread to signal it's opened the file
  std::unique_lock<std::mutex> mlock(log_client->mutex);
  while (!log_client->initialized) log_client->cond.wait(mlock);
  mlock.unlock();
    
  log_thread_id.detach();

  log_table.insert(filename, log_client);

  return 1;
}
  
int Dataserver::remove_log_client(std::string filename)

{
  LogClient *log_client;

  if (log_table.find(filename, &log_client)) {
    log_client->dpoint_queue.push_back(&log_client->shutdown_dpoint);
    return 1;
  }
  else return 0;
}

int Dataserver::pause_log_client(std::string filename)

{
  LogClient *log_client;
    
  if (log_table.find(filename, &log_client)) {
    log_client->dpoint_queue.push_back(&log_client->pause_dpoint);
    return 1;
  }
  else return 0;
}

int Dataserver::start_log_client(std::string filename)

{
  LogClient *log_client;
    
  if (log_table.find(filename, &log_client)) {
    log_client->dpoint_queue.push_back(&log_client->start_dpoint);
    return 1;
  }
  else return 0;
}

int Dataserver::log_add_match(std::string filename, std::string varname,
			      int every, int obs, int buflen)
{
  LogClient *log_client;
  int rval = 0;

  if (log_table.find(filename, &log_client)) {
    LogMatchSpec *match =
      new LogMatchSpec(varname.c_str(), every, obs, buflen);

    log_client->matches.insert(match->matchstr, match);
    log_client->obs_limited_matches += match->obs_limited;
    rval = 1;
  }
  return rval;
}


static int doRead(int fd, char *buf, int size)
{
  int rbytes = 0;
  int n = 0;
  
  while(rbytes < size) {
    if((n = read(fd, buf + rbytes, size - rbytes)) < 0) {
      perror("reading from client socket");
      return -1;
    }
    
    rbytes += n;
  }
  return 0;
}

static void iovSet(struct iovec *iov, char *buf, int n)
{
  iov->iov_base = buf;
  iov->iov_len = n;
}

int Dataserver::tcp_process_request(Dataserver *ds,
				    char buf[], int nbytes, int msgsock,
				    char **repbuf, int *repsize, int *repalloc)
{
  int i;
  char *p;
  ds_datapoint_t *dpoint;
  /*
   * NOTE: these static buffers should be replaced to ensure
   *       this function is thread safe
   */
  char path[256], *dstring_buf;
  int dstring_size, dstring_bufsize;
  int status = -1;

  if (repalloc) *repalloc = 0;
  
  while (nbytes > 0) {
    if (buf[nbytes-1] == '\n' ||
	buf[nbytes-1] == '\r') {
      buf[nbytes-1] = '\0';
      nbytes--;
    }
    else break;
  }
  if (nbytes > 0) {
    i = 0;
    if (nbytes) {
      if (buf[0] == '%') {
	if (nbytes > 7 &&
	    buf[1] == 'v' && !strcmp(&buf[1], "version")) {
	  *repbuf = strdup("3.0");
	  *repsize = strlen(*repbuf);
	  *repalloc = 1;
	  status = 1;
	}

	else if (nbytes > 7 &&
		 buf[1] == 'g' && buf[2] == 'e' &&
		 buf[3] == 't' && buf[4] == 'k' &&
		 buf[5] == 'e' && buf[6] == 'y' &&
		 buf[7] == 's') {
	  char *keys = ds->get_table_keys();
	  *repbuf = keys;
	  *repsize = strlen(keys);
	  *repalloc = 1;
	  status = 1;
	}
	
	else if (nbytes > 5 &&
		 buf[1] == 'd' && buf[2] == 'g' &&
		 buf[3] == 'd' && buf[4] == 'i' &&
		 buf[5] == 'r') {
	  char *dgdir = ds->get_dg_dir();
	  *repbuf = dgdir;
	  *repsize = strlen(dgdir);
	  *repalloc = 1;
	  status = 1;
	}
	
	else if (nbytes > 5 &&
		 buf[1] == 'r' && buf[2] == 'e' &&
		 buf[3] == 'g' && buf[4] == ' ') {
	  char host[64];
	  int port;
	  int binary = 0;
	  
	  p = &buf[5];
	  if (sscanf(p, "%63s %d %d", host, &port, &binary) == 3)
	    {
	      status = ds->tcpip_register(host, port, binary);
	    }
	  else if (sscanf(p, "%63s %d", host, &port) == 2)
	    {
	      status = ds->tcpip_register(host, port, binary);
	    }
	  else {
	    goto error;
	  }
	  *repsize = 0;
	  *repalloc = 0;
	}

	/* unreg */
	else if (nbytes > 7 &&
		 buf[1] == 'u' && buf[2] == 'n' &&
		 buf[3] == 'r' && buf[4] == 'e' &&
		 buf[5] == 'g' && buf[6] == ' ') {
	  char host[64];
	  int port;
	  
	  p = &buf[7];
	  if (sscanf(p, "%63s %d", host, &port) == 2)
	    {
	      status = ds->tcpip_unregister(host, port);
	    }
	  else {
	    goto error;
	  }
	  *repsize = 0;
	  *repalloc = 0;
	}

#if 0
	else if (nbytes > 8 &&
		 buf[1] == 's' && buf[2] == 'e' &&
		 buf[3] == 'n' && buf[4] == 'd' &&
		 buf[5] == 'i' && buf[6] == 'n' &&
		 buf[7] == 'f' && buf[8] == 'o')
	  {
	    char *keys = ds->get_sendinfo();
	    *repbuf = keys;
	    *repsize = strlen(keys);
	    *repalloc = 1;
	    status = 1;
	  }

#endif
	/* dserv_set */
	else if (nbytes > 5 &&
		 buf[1] == 's' && buf[2] == 'e' &&
		 buf[3] == 't' && buf[4] == ' ') {
	  int len;
	  char *p2 = &buf[5];
	  char *val, *var;
	  i = 0;
	  p = strchr(p2, '=');

	  if (!p) goto error;

	  len = (&buf[0]+nbytes)-(p+1);
	  val = (char *) malloc(len);
	  memcpy(val, p+1, len);
	  
	  // really should be more restrictive on variable names
	  while (p2[i++] != '=');
	  
	  var = (char *) malloc(i);
	  memcpy(var, p2, i-1);
	  var[i-1] = '\0';
	  
	  dpoint = (ds_datapoint_t *) malloc(sizeof(ds_datapoint_t));
	  dpoint_set(dpoint, var, now(), DSERV_STRING,
		     len, (unsigned char *) val);		     


	  // move to table so don't free here
	  ds->set(dpoint);
	  
	  *repsize = 0;
	  *repalloc = 0;

	  status = 1; // success
	}


	/* setdata */
	else if (nbytes > 9 &&
		 buf[1] == 's' && buf[2] == 'e' &&
		 buf[3] == 't' && buf[4] == 'd' &&
		 buf[5] == 'a' && buf[6] == 't' &&
		 buf[7] == 'a' && buf[8] == ' ') {
	  char *p2 = &buf[9];

	  dpoint = dpoint_from_string(p2, nbytes-9);
	  
	  if (dpoint) {
	    if (!dpoint->timestamp) dpoint->timestamp = now();
	    ds->set(dpoint);
	    
	    *repsize = 0;
	    *repalloc = 0;
	    
	    status = 1; // success
	  }
	  else {
	    status = 0;
	  }
	}

	
	/* dserv_get */
	else if (nbytes > 5 &&
		 buf[1] == 'g' && buf[2] == 'e' &&
		 buf[3] == 't' && buf[4] == ' ') {
	  char *var = &buf[5];

	  status = ds->get(var, &dpoint);

	  if (status) {
	    dstring_bufsize = dpoint_string_size(dpoint);
	    dstring_buf = (char *) malloc(dstring_bufsize);
	    dstring_size =
	      dpoint_to_string(dpoint, dstring_buf, dstring_bufsize);
	    *repbuf = dstring_buf;
	    *repsize = dstring_size;
	    *repalloc = 1;

	    dpoint_free(dpoint);
	  }
	  else {
	    *repsize = 0;
	    status = -1;
	  }
	}
	
	/* dserv_touch */
	else if (nbytes > 7 &&
		 buf[1] == 't' && buf[2] == 'o' &&
		 buf[3] == 'u' && buf[4] == 'c' &&
		 buf[5] == 'h' && buf[6] == ' ') {
	  char *var = &buf[7];

	  status = ds->touch(var);

	  *repsize = 0;
	  *repalloc = 0;
	  status = (status == DSERV_OK);
	}
	
	/* dserv_clear */
	else if (nbytes > 7 &&
		 buf[1] == 'c' && buf[2] == 'l' &&
		 buf[3] == 'e' && buf[4] == 'a' &&
		 buf[5] == 'r' && buf[6] == ' ') {
	  char *var = &buf[7];

	  status = ds->clear(var);

	  *repsize = 0;
	  *repalloc = 0;
	  status = (status == DSERV_OK);
	}

	/* dserv_getsize */
	else if (nbytes > 9 &&
		 buf[1] == 'g' && buf[2] == 'e' &&
		 buf[3] == 't' && buf[4] == 's' &&
		 buf[5] == 'i' && buf[6] == 'z' &&
		 buf[7] == 'e' && buf[8] == ' ') {
	  char *var = &buf[9];

	  status = ds->get(var, &dpoint);
	  
	  if (status) {
	    char *rep_buf = (char *) malloc(32);
	    snprintf(rep_buf, sizeof(rep_buf), "%d", dpoint->data.len);
	    dpoint_free(dpoint);
	    status = 1;
	    *repbuf = rep_buf;
	    *repsize = strlen(rep_buf);
	    *repalloc = 1;
	  }
	  else {
	    *repsize = 0;
	    status = -1;
	    *repalloc = 0;
	  }
	}
	
	/* match */
	else if (nbytes > 7 &&
		 buf[1] == 'm' && buf[2] == 'a' &&
		 buf[3] == 't' && buf[4] == 'c' &&
		 buf[5] == 'h' && buf[6] == ' ') {
	  char *p = &buf[7];

	  /* these should be dynamic */
	  char host[64];
	  char match[128];
	  int port, every;

	  if (sscanf(p, "%63s %d %127s %d", host, &port, match, &every) == 4) {
	    status = ds->tcpip_add_match(host, port, match, every);
	  }
	  else if (sscanf(p, "%63s %d %127s", host, &port, match) == 3) {
	    status = ds->tcpip_add_match(host, port, match, 1);
	  }
	  *repsize = 0;
	  *repalloc = 0;
	  status = 1;
	}

	else if (nbytes > 9 &&
		 buf[1] == 'u' && buf[2] == 'n' &&
		 buf[3] == 'm' && buf[4] == 'a' &&
		 buf[5] == 't' && buf[6] == 'c' &&
		 buf[7] == 'h' && buf[8] == ' ') {
	  char *p = &buf[9];
	  
	  /* these should be dynamic */
	  char host[64];
	  char match[128];
	  int port;

	  if (sscanf(p, "%63s %d %127s", host, &port, match) == 3) {
	    status = ds->tcpip_remove_match(host, port, match);
	  }
	  *repsize = 0;
	  *repalloc = 0;
	  status = (status == DSERV_OK);
	}

	else if (nbytes > 10 &&
		 buf[1] == 'g' && buf[2] == 'e' &&
		 buf[3] == 't' && buf[4] == 'm' &&
		 buf[5] == 'a' && buf[6] == 't' &&
		 buf[7] == 'c' && buf[8] == 'h' &&
		 buf[9] == ' ') {
	  char *p = &buf[10];
	  
	  /* these should be dynamic */
	  char host[64];
	  int port;
	  char *matches;
	  
	  if (sscanf(p, "%63s %d", host, &port) == 2) {
	    std::string matchstr = ds->get_matches(host, port);
  	    *repbuf = strdup(matchstr.c_str());
	    *repsize = matchstr.size();
	    *repalloc = 1;
	    status = 1;
	  }
	  else {
	    matches = NULL;
  	    *repsize = 0;
	    *repalloc = 0;
	    status = 0;
	  }
	}

	/**** Log Functions ****/
	
	else if (nbytes > 9 &&
		 buf[1] == 'l' && buf[2] == 'o' &&
		 buf[3] == 'g' && buf[4] == 'o' &&
		 buf[5] == 'p' && buf[6] == 'e' &&
		 buf[7] == 'n' && buf[8] == ' ') {
	  char *p = &buf[9];
	  int overwrite;
	  if (sscanf(p, "%255s %d", path, &overwrite) == 2) {
	    status = ds->logger_client_open(path, overwrite);
	  }
	  else  {
	    status = ds->logger_client_open(p, 0);
	  }
	  *repsize = 0;
	  *repalloc = 0;
	  status = (status == DSERV_OK);
	}	
	
	else if (nbytes > 10 &&
		 buf[1] == 'l' && buf[2] == 'o' &&
		 buf[3] == 'g' && buf[4] == 'c' &&
		 buf[5] == 'l' && buf[6] == 'o' &&
		 buf[7] == 's' && buf[8] == 'e' &&
		 buf[9] == ' ') {
	  char *p = &buf[10];

	  status = ds->logger_client_close(p);

	  *repsize = 0;
	  *repalloc = 0;
	  status = (status == DSERV_OK);
	}	
	else if (nbytes > 10 &&
		 buf[1] == 'l' && buf[2] == 'o' &&
		 buf[3] == 'g' && buf[4] == 'm' &&
		 buf[5] == 'a' && buf[6] == 't' &&
		 buf[7] == 'c' && buf[8] == 'h' &&
		 buf[9] == ' ') {
	  char *p = &buf[10];
	  int every, obs, bufsize;
	  char match[64];

	  if (sscanf(p, "%255s %63s %d %d %d",
		     path, match, &every, &obs, &bufsize) == 5) {

	    status = ds->logger_add_match(path, match, every,
					  obs, bufsize);
	  }

	  *repsize = 0;
	  *repalloc = 0;
	  status = (status == DSERV_OK);
	}	
	else if (nbytes > 10 &&
		 buf[1] == 'l' && buf[2] == 'o' &&
		 buf[3] == 'g' && buf[4] == 's' &&
		 buf[5] == 't' && buf[6] == 'a' &&
		 buf[7] == 'r' && buf[8] == 't' &&
		 buf[9] == ' ') {
	  char *p = &buf[10];

	  status = ds->logger_client_start(p);

	  *repsize = 0;
	  *repalloc = 0;
	  status = (status == DSERV_OK);
	}	

	else if (nbytes > 10 &&
		 buf[1] == 'l' && buf[2] == 'o' &&
		 buf[3] == 'g' && buf[4] == 'p' &&
		 buf[5] == 'a' && buf[6] == 'u' &&
		 buf[7] == 's' && buf[8] == 'e' &&
		 buf[9] == ' ') {
	  char *p = &buf[10];
	  
	  status = ds->logger_client_pause(p);
	  
	  *repsize = 0;
	  *repalloc = 0;
	  status = (status == DSERV_OK);
	}	
	else {		/* unknown % command */
	error:
	  *repsize = 0;
	  status = 0;
	}
      }
    }
  }
  return status;
}

  
void
Dataserver::tcp_client_process(Dataserver *ds, int sockfd)
{
  char buf[4096];
  double start;
  int rc;
  char rcbuf[64];
  int rcsize;
  int rval;
  int repalloc;
  char *repbuf;
  int repsize;
  
  char newline_buf[2] = "\n";
  int bytes_to_send = 0;

  // for creating vectored writes
  struct iovec iovs[3];
  
  do
    {
      memset(buf, 0, sizeof(buf));
      rc = -1;

      rval = read(sockfd, buf, 1);

      if (rval != 1)
	goto close_up;

      if (buf[0] == '%')
	{
	  if (rval == 1) {
	    rval = read(sockfd, &buf[1], sizeof(buf) - 1);
	    rval += 1;
	  }
	  repsize = repalloc = 0;
	  // will potentially allocate reply buffer 
	  rc = tcp_process_request(ds, buf, rval, sockfd,
				   &repbuf, &repsize, &repalloc);

	  snprintf(rcbuf, sizeof(rcbuf), "%d ", rc);
	  rcsize = strlen(rcbuf);
	  
	  iovSet(&iovs[0], rcbuf, rcsize);
	  iovSet(&iovs[1], repbuf, repsize);
	  iovSet(&iovs[2], newline_buf, 1);
	  bytes_to_send = rcsize + repsize + 1;
	  
	  rval = writev(sockfd, iovs, 3);

	  // free allocated reply buffer
	  if (repalloc)
	    free(repbuf);

	  if (rval < 0)
	    {
	      // perror("writing stream message");
	      goto close_up;
	    }
	  else if (rval == 0)
	    {
	      // printf("Ending connection\n");
	      goto close_up;
	    }
	  else if (rval < bytes_to_send)
	    {
	      // printf("incomplete transfer\n");
	      goto close_up;
	    }
	}
      else if (buf[0] == '@')
	{
	  unsigned int varlen, datalen, inlen, outlen;
	  int datatype;
	  ds_datapoint_t *dpoint;
	  char *varname, *inbuf;
	  unsigned char *databuf;

	  if (rval == 1)
	    {
	      rval = read(sockfd, &buf[1], 31 - 1);
	      rval += 1;
	    }
	  buf[rval] = '\0';

	  if (rval > 5 &&
	      buf[1] == 's' && buf[2] == 'e' &&
	      buf[3] == 't' && buf[4] == ' ')
	    {
	      if (sscanf(&buf[5], "%d %d %d", &varlen,
			 &datatype, &datalen) != 3)
		goto close_up;
	      
	      
	      // Acknowledge to prevent buffering on sender side
	      rval = write(sockfd, newline_buf, 1); 

	      varname = (char *) malloc(varlen);
	      doRead(sockfd, varname, varlen);
	      varname[varlen - 2] = '\0';
	      
	      //	      std::cerr << "point name: " << varname << std::endl;
	      
	      // Acknowledge and read data
	      rval = write(sockfd, newline_buf, 1); 
	  
	     
	      if (datatype == DSERV_STRING || datatype == DSERV_SCRIPT || datatype == DSERV_JSON)
		{
		  databuf = (unsigned char *) malloc(datalen);
		  doRead(sockfd, (char *)databuf, datalen);
		  databuf[datalen - 2] = '\0';
		  dpoint = (ds_datapoint_t *) malloc(sizeof(ds_datapoint_t));
		  dpoint_set(dpoint, varname, now(),
			     (ds_datatype_t) datatype,
			     datalen - 2, (unsigned char *) databuf);
		}
	      else if (datatype != DSERV_DG)
		{
		  inlen = (((4 * datalen / 3) + 3) & ~3) + 2;
		  inbuf = (char *) malloc(inlen);
		  doRead(sockfd, inbuf, inlen);
		  inbuf[inlen - 2] = '\0';

		  databuf = (unsigned char *) malloc(datalen);
		  outlen = datalen;
		  base64decode(inbuf, inlen - 2, databuf, &outlen);
		  free(inbuf);

		  dpoint = (ds_datapoint_t *) malloc(sizeof(ds_datapoint_t));
		  dpoint_set(dpoint, varname, now(),
			     (ds_datatype_t) datatype,
			     datalen, (unsigned char *) databuf);
		}
	      else // (DG, MSGPACK, ARROW, JPEG, PPM binary formats)
		{
		  inlen = datalen + 2;
		  inbuf = (char *) malloc(inlen);
		  doRead(sockfd, inbuf, inlen);
		  inbuf[inlen - 2] = '\0';

		  outlen = (inlen * 4) / 3 + 1;
		  databuf = (unsigned char *) malloc(outlen);
		  base64decode(inbuf, inlen - 2, databuf, &outlen);
		  free(inbuf);

		  dpoint = (ds_datapoint_t *)malloc(sizeof(ds_datapoint_t));
		  dpoint_set(dpoint, varname, now(),
			     (ds_datatype_t) datatype,
			     outlen, (unsigned char *) databuf);
		}

	      // set new dpoint, memory managed by ds
	      ds->set(dpoint);

	      rc = 1;
	      snprintf(rcbuf, sizeof(rcbuf), "%d", rc);
	      rcsize = strlen(rcbuf);

	      iovSet(&iovs[0], rcbuf, rcsize);
	      iovSet(&iovs[1], newline_buf, 1);
	      bytes_to_send = rcsize + 1;
	      rval = writev(sockfd, iovs, 2);
	    }

	  else if (rval > 5 &&
		   buf[1] == 'g' && buf[2] == 'e' &&
		   buf[3] == 't' && buf[4] == ' ')
	    {

	      if (sscanf(&buf[5], "%d", &varlen) != 1)
		goto close_up;
	      
	      // Acknowledge to prevent buffering on sender side
	      rval = write(sockfd, newline_buf, 1);

	      varname = (char *) malloc(varlen);
	      doRead(sockfd, varname, varlen);
	      varname[varlen - 2] = '\0';
	      
	      rc = ds->get(varname, &dpoint);
	      if (!rc)
		{
		  rc = 0;
		  snprintf(rcbuf, sizeof(rcbuf), "%d", rc);
		  rcsize = strlen(rcbuf);
		  iovSet(&iovs[0], rcbuf, rcsize);
		  iovSet(&iovs[1], newline_buf, 1);
		  bytes_to_send = rcsize + 1;
		  rval = writev(sockfd, iovs, 2);
		}
	      else
		{
		  int dstring_bufsize = dpoint_string_size(dpoint);
		  char *dstring_buf = (char *) malloc(dstring_bufsize);
		  int dstring_size = dpoint_to_string(dpoint, dstring_buf,
						      dstring_bufsize);
		  snprintf(rcbuf, sizeof(rcbuf), "%d", dstring_size);
		  rcsize = strlen(rcbuf);
		  iovSet(&iovs[0], rcbuf, rcsize);
		  iovSet(&iovs[1], newline_buf, 1);
		  bytes_to_send = rcsize + 1;
		  rval = writev(sockfd, iovs, 2);
		  
		  // ack from client
		  doRead(sockfd, rcbuf, 1);

		  iovSet(&iovs[0], dstring_buf, dstring_size);
		  iovSet(&iovs[1], newline_buf, 1);
		  bytes_to_send = dstring_size + 1;
		  rval = writev(sockfd, iovs, 2);

		  free(dstring_buf);
		  dpoint_free(dpoint);
		}
	    }

	  if (rval < 0)
	    {
	      // perror("writing stream message");
	      goto close_up;
	    }
	  else if (rval == 0)
	    {
	      // printf("Ending connection\n");
	      goto close_up;
	    }
	  else if (rval < bytes_to_send)
	    {
	      // printf("incomplete transfer\n");
	      goto close_up;
	    }
	}
      
      else if (buf[0] == '<') {
	ds_datapoint_t *dpoint;
	uint16_t varlen;
	char *varname;

	/* next 2 bytes are length of varname */
	rval = read(sockfd, &varlen, sizeof(uint16_t));
	if (rval < sizeof(uint16_t)) goto close_up;

	/* next varlen bytes are varname */
	varname = (char *) malloc(varlen+1);
	if (varlen) read(sockfd, varname, varlen);

	/* null terminate the varname string */
	varname[varlen] = '\0';

	/* lookup the varname */
	int status = ds->get(varname, &dpoint);
	free(varname);

	if (status) {	  
	  int point_bufsize = dpoint_binary_size(dpoint);
	  unsigned char *point_buf = (unsigned char *) malloc(point_bufsize);
	  int bsize = dpoint_to_binary(dpoint, point_buf, &point_bufsize);

	  /* we return the size of the dpoint (int) and dpoint buffer */
	  iovs[0].iov_base = &point_bufsize;
	  iovs[0].iov_len = sizeof(int);
	  iovs[1].iov_base = point_buf;
	  iovs[1].iov_len = point_bufsize;
	  
	  bytes_to_send = sizeof(uint32_t)+point_bufsize;
	  
	  rval = writev(sockfd, iovs, 2);

	  free(point_buf);
	  
	  dpoint_free(dpoint);
	}
	else {
	  int point_bufsize = 0;
	  write(sockfd, &point_bufsize, sizeof(int));
	}
      }
      
      else if (buf[0] == DPOINT_BINARY_MSG_CHAR)
	{
	  uint16_t varlen;
	  uint32_t datalen;
	  uint32_t datatype;
	  uint64_t timestamp;
		  
	  ds_datapoint_t *dpoint;
	  char *varname;
	  unsigned char *databuf;

	  // The fixed length buffer (including the '>') 
	  char buffer[128];
	  char *bufptr = &buffer[0];
	  
	  if (doRead(sockfd, buffer, sizeof(buffer)-1) < 0) {
	    goto close_up;
	  }

	  varlen = *((uint16_t *) bufptr);
	  bufptr += sizeof(uint16_t);

	  varname = (char *) malloc(varlen+1);
	  if (!varname) goto close_up;

	  memcpy(varname, bufptr, varlen);
	  varname[varlen] = '\0';
	  bufptr += varlen;

	  timestamp = *((uint64_t *) bufptr);
	  bufptr += sizeof(uint64_t);

	  datatype = *((uint32_t *) bufptr);
	  bufptr += sizeof(uint32_t);

	  datalen = *((uint32_t *) bufptr);
	  bufptr += sizeof(uint32_t);

	  databuf = (unsigned char *) malloc(datalen);
	  if (!databuf) {
	    free(varname);
	    goto close_up;
	  }

	  memcpy(databuf, bufptr, datalen);

	  dpoint = (ds_datapoint_t *) malloc(sizeof(ds_datapoint_t));
	  dpoint_set(dpoint, varname, timestamp ? timestamp : now(), 
		     (ds_datatype_t) datatype,
		     datalen, (unsigned char *) databuf);
	  // set new dpoint, memory managed by ds
	  ds->set(dpoint);
	}

      else
	{
	  // process other TCP/IP commands here
	}
    } while (rval > 0);

 close_up:
  close(sockfd);
  //  printf("closing up\n");

}
