#include "TclServer.h"

TclServer::TclServer(int argc, char **argv,
		     Dataserver *dserv, int port)
{
  m_bDone = false;
  tcpport = port;
  ds = dserv;
  
  // create a connection to dataserver so we can subscribe to datapoints
  client_name = ds->add_new_send_client(&queue);
  
  // create Tcl interpreter for executing scripts
  setup_tcl(argc, argv);
  
  net_thread = std::thread(&TclServer::start_tcp_server, this);
  process_thread = std::thread(&TclServer::process_requests, this);
}

TclServer::~TclServer()
{
  shutdown();
  net_thread.detach();
  process_thread.detach();
}

void TclServer::shutdown(void)
{
  m_bDone = true;
}

bool TclServer::isDone()
{
  return m_bDone;
}

void TclServer::start_tcp_server(void)
{
  struct sockaddr_in address;
  struct sockaddr client_address;
  socklen_t client_address_len = sizeof(client_address);
  int new_socket_fd;		// client socket
  int on = 1;
  
  //    std::cout << "opening server on port " << std::to_string(tcpport) << std::endl;
  
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
    std::thread thr(tcp_client_process, new_socket_fd, &queue);
    thr.detach();
  }
}

int TclServer::sourceFile(const char *filename)
{
  if (!interp) {
    std::cerr << "no tcl interpreter" << std::endl;
    return TCL_ERROR;
  }
  
  return Tcl_EvalFile(interp, filename);
}

/********************************* now *********************************/

int TclServer::now_command (ClientData data, Tcl_Interp *interp,
			    int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  
  Tcl_SetObjResult(interp, Tcl_NewWideIntObj(ds->now()));
  return TCL_OK;
}


int TclServer::dserv_add_match_command(ClientData data, Tcl_Interp * interp,
				       int objc,
				       Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  
  Tcl_Obj *obj;
  int every = 1;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "varname [every]");
    return TCL_ERROR;
  }
  if (objc > 2) {
    if (Tcl_GetIntFromObj(interp, objv[2], &every) != TCL_OK) {
      return TCL_ERROR;
    }
  }
  
  ds->client_add_match(tclserver->client_name, Tcl_GetString(objv[1]), every);
  return TCL_OK;
}

int TclServer::dserv_add_exact_match_command(ClientData data, Tcl_Interp * interp,
					     int objc,
					     Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  
  Tcl_Obj *obj;
  int every = 1;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "varname [every]");
    return TCL_ERROR;
  }
  if (objc > 2) {
    if (Tcl_GetIntFromObj(interp, objv[2], &every) != TCL_OK) {
      return TCL_ERROR;
    }
  }
  
  ds->client_add_exact_match(tclserver->client_name, Tcl_GetString(objv[1]), every);
  return TCL_OK;
}

int TclServer::dserv_remove_match_command(ClientData data, Tcl_Interp * interp,
					  int objc,
					  Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  Tcl_Obj *obj;
  int every = 1;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "varname");
    return TCL_ERROR;
  }
  ds->client_remove_match(tclserver->client_name, Tcl_GetString(objv[1]));
  return TCL_OK;
}

int TclServer::dserv_remove_all_matches_command(ClientData data,
						Tcl_Interp * interp,
						int objc,
						Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  ds->client_remove_all_matches(tclserver->client_name);
  return TCL_OK;
}




int TclServer::dserv_log_open_command(ClientData data, Tcl_Interp *interp,
			     int objc, Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  int status;
  int overwrite = 0;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "path [overwrite]");
    return TCL_ERROR;
  }
  
  if (objc > 2) {
    if (Tcl_GetIntFromObj(interp, objv[2], &overwrite) != TCL_OK)
      return TCL_ERROR;
  }
  
  status = ds->logger_client_open(Tcl_GetString(objv[1]), overwrite);
  return (status > 0) ? TCL_OK : TCL_ERROR;
}

int TclServer::dserv_log_close_command(ClientData data, Tcl_Interp *interp,
				       int objc, Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  int status;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "path");
    return TCL_ERROR;
  }
  
  status = ds->logger_client_close(Tcl_GetString(objv[1]));
  return (status > 0) ? TCL_OK : TCL_ERROR;
}

int TclServer::dserv_log_pause_command(ClientData data, Tcl_Interp *interp,
				       int objc, Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  int status;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "path");
    return TCL_ERROR;
  }
  
  status = ds->logger_client_pause(Tcl_GetString(objv[1]));
  return (status > 0) ? TCL_OK : TCL_ERROR;
}

int TclServer::dserv_log_start_command(ClientData data, Tcl_Interp *interp,
				       int objc, Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  int status;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "path");
    return TCL_ERROR;
  }
  
  status = ds->logger_client_start(Tcl_GetString(objv[1]));
  return (status > 0) ? TCL_OK : TCL_ERROR;
}

int TclServer::dserv_log_add_match_command(ClientData data, Tcl_Interp *interp,
					   int objc, Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  int status;
  int obs_limited = 0;
  int buffer_size = 0;
  int every = 1;
  
  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "path match [obs_limited buffer_size]");
    return TCL_ERROR;
  }
  
  if (objc > 3) {
    if (Tcl_GetIntFromObj(interp, objv[3], &obs_limited) != TCL_OK)
      return TCL_ERROR;
  }
  
  if (objc > 4) {
    if (Tcl_GetIntFromObj(interp, objv[4], &buffer_size) != TCL_OK)
      return TCL_ERROR;
  }
  
  status = ds->logger_add_match(Tcl_GetString(objv[1]),
				Tcl_GetString(objv[2]),
				every, obs_limited, buffer_size);
  return (status > 0) ? TCL_OK : TCL_ERROR;
}



  
int TclServer::timer_tick_command (ClientData data, Tcl_Interp *interp,
				   int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  int id, ms;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "?timerid? start");
    return TCL_ERROR;
  }
  
  if (objc < 3) {		/* default to timer 0 */
    id = 0;
    if (Tcl_GetIntFromObj(interp, objv[1], &ms) != TCL_OK)
      return TCL_ERROR;
  }
  
  else {
    if (Tcl_GetIntFromObj(interp, objv[1], &id) != TCL_OK)
      return TCL_ERROR;
    
    if (id >= tclserver->ntimers) {
      const char *msg = "invalid timer";
      Tcl_SetResult(interp, (char *) msg, TCL_STATIC);
      return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[2], &ms) != TCL_OK)
      return TCL_ERROR;
  }
  
  tclserver->timers[id]->arm_ms(ms);
  tclserver->timers[id]->fire();
  
  Tcl_SetObjResult(interp, Tcl_NewIntObj(id));
  return TCL_OK;
}

int TclServer::timer_reset_command (ClientData data, Tcl_Interp *interp,
				    int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  int id, ms;
  
  if (objc < 2) {		/* default to timer 0 */
    id = 0;
  }
  
  else {
    if (Tcl_GetIntFromObj(interp, objv[1], &id) != TCL_OK)
      return TCL_ERROR;
    
    if (id >= tclserver->ntimers) {
      const char *msg = "invalid timer";
      Tcl_SetResult(interp, (char *) msg, TCL_STATIC);
      return TCL_ERROR;
    }
  }
  
  tclserver->timers[id]->reset();
  Tcl_SetObjResult(interp, Tcl_NewIntObj(id));
  return TCL_OK;
}

int TclServer::timer_tick_interval_command (ClientData data, Tcl_Interp *interp,
					    int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  int id, ms, interval_ms, nrepeats = -1;
  
  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "?timerid? start interval");
    return TCL_ERROR;
  }
  
  if (objc < 4) {
    id = 0;
    if (Tcl_GetIntFromObj(interp, objv[1], &ms) != TCL_OK)
      return TCL_ERROR;
    
    if (Tcl_GetIntFromObj(interp, objv[2], &interval_ms) != TCL_OK)
      return TCL_ERROR;
  }
  else {
    if (Tcl_GetIntFromObj(interp, objv[1], &id) != TCL_OK)
      return TCL_ERROR;
    
    if (id >= tclserver->ntimers) {
      const char *msg = "invalid timer";
      Tcl_SetResult(interp, (char *) msg, TCL_STATIC);
      return TCL_ERROR;
    }
    
    if (Tcl_GetIntFromObj(interp, objv[2], &ms) != TCL_OK)
      return TCL_ERROR;
    
    if (Tcl_GetIntFromObj(interp, objv[3], &interval_ms) != TCL_OK)
      return TCL_ERROR;
  }
  
  if (objc > 4) {
    if (Tcl_GetIntFromObj(interp, objv[4], &nrepeats) != TCL_OK)
      return TCL_ERROR;
  }
  
  tclserver->timers[id]->arm_ms(ms, interval_ms, nrepeats);
  tclserver->timers[id]->fire();
  
  Tcl_SetObjResult(interp, Tcl_NewIntObj(id));
  return TCL_OK;
}

int TclServer::timer_expired_command (ClientData data, Tcl_Interp *interp,
				      int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  int timer;
  
  if (objc < 2) {
    timer = 0;
  }
  else {
    if (Tcl_GetIntFromObj(interp, objv[1], &timer) != TCL_OK)
      return TCL_ERROR;
    
    if (timer >= tclserver->ntimers) {
      const char *msg = "invalid timer";
      Tcl_AppendResult(interp, (char *) msg, NULL);
      return TCL_ERROR;
    }
  }
  Tcl_SetObjResult(interp, Tcl_NewIntObj(tclserver->timers[timer]->expired));
  return TCL_OK;
}

int TclServer::timer_set_script_command (ClientData data, Tcl_Interp *interp,
					 int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  int id;
  char *script;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "?timerid? script");
    return TCL_ERROR;
  }
  
  if (objc < 3) {
    id = 0;
    script = Tcl_GetString(objv[1]); 
  }
  else {
    if (Tcl_GetIntFromObj(interp, objv[1], &id) != TCL_OK)
      return TCL_ERROR;
    
    if (id >= tclserver->ntimers) {
      const char *msg = "invalid timer";
      Tcl_SetResult(interp, (char *) msg, TCL_STATIC);
      return TCL_ERROR;
    }
    script = Tcl_GetString(objv[2]);
  }
  
  tclserver->timer_scripts.insert(id,
				  std::move(std::string(Tcl_GetString(objv[2]))));
  return TCL_OK;
}

int TclServer::timer_remove_script_command (ClientData data, Tcl_Interp *interp,
					    int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  int id;
  
  if (objc < 1) {
    id = 0;
  }
  else {
    if (Tcl_GetIntFromObj(interp, objv[1], &id) != TCL_OK)
      return TCL_ERROR;
    
    if (id >= tclserver->ntimers) {
      const char *msg = "invalid timer";
      Tcl_SetResult(interp, (char *) msg, TCL_STATIC);
      return TCL_ERROR;
    }
  }
  
  tclserver->timer_scripts.remove(id);
  
  return TCL_OK;
}
  
  
int TclServer::timer_status_command (ClientData data, Tcl_Interp *interp,
				     int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  int i, ntimers;
  Tcl_Obj *elt;
  Tcl_Obj *l = Tcl_NewListObj(0, NULL);
  
  for (i = 0; i < tclserver->ntimers; i++) {
    elt = Tcl_NewIntObj(tclserver->timers[i]->expired);
    Tcl_ListObjAppendElement(interp, l, elt);
  }
  
  Tcl_SetObjResult(interp, l);
  return TCL_OK;
}

int TclServer::dpoint_set_script_command (ClientData data, Tcl_Interp *interp,
					  int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  
  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "varname script");
    return TCL_ERROR;
  }
  
  tclserver->dpoint_scripts.insert(std::string(Tcl_GetString(objv[1])),
				   std::string(Tcl_GetString(objv[2])));
  
  return TCL_OK;
}

int TclServer::dpoint_remove_script_command (ClientData data, Tcl_Interp *interp,
					     int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "varname");
    return TCL_ERROR;
  }
  
  tclserver->dpoint_scripts.remove(std::string(Tcl_GetString(objv[1])));
  
  return TCL_OK;
  }

int TclServer::dpoint_remove_all_scripts_command (ClientData data,
						  Tcl_Interp *interp,
						  int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  tclserver->dpoint_scripts.clear();
  return TCL_OK;
}

int TclServer::print_command (ClientData data, Tcl_Interp *interp,
			      int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  
  ds_datapoint_t dpoint;
  char *s;
  int len;
  int rc;
  
  if (objc < 2) {
    Tcl_WrongNumArgs(interp, 1, objv, "string");
    return TCL_ERROR;
  }
  
  s = Tcl_GetStringFromObj(objv[1], &len);
  if (!s) return TCL_ERROR;
  
  /* fill the data point */
  dpoint_set(&dpoint, (char *) tclserver->PRINT_DPOINT_NAME, 
	     tclserver->ds->now(), DSERV_STRING, len, (unsigned char *) s);
  
  /* send to dserv */
  tclserver->ds->set(dpoint);
  
  return TCL_OK;
}


void TclServer::add_tcl_commands(Tcl_Interp *interp)
{
  /* use the generic Dataserver commands for these */
  Tcl_CreateObjCommand(interp, "dpointGet",
		       Dataserver::dserv_get_command, this->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservGet",
		       Dataserver::dserv_get_command, this->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservSet",
		       Dataserver::dserv_set_command, this->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservTouch",
		       Dataserver::dserv_touch_command, this->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservTimestamp",
		       Dataserver::dserv_timestamp_command, this->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservSetData",
		       Dataserver::dserv_setdata_command, this->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservSetData64",
		       Dataserver::dserv_setdata64_command, this->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservClear",
		       Dataserver::dserv_clear_command, this->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservEval",
		       Dataserver::dserv_eval_command, this->ds, NULL);

  Tcl_CreateObjCommand(interp, "processGetParam",
		       Dataserver::process_get_param_command, this->ds, NULL);
  Tcl_CreateObjCommand(interp, "processSetParam",
		       Dataserver::process_set_param_command, this->ds, NULL);

  /* these are specific to TclServers */
  Tcl_CreateObjCommand(interp, "now",
		       (Tcl_ObjCmdProc *) now_command,
		       this, NULL);
  
  Tcl_CreateObjCommand(interp, "dservAddMatch",
		       dserv_add_match_command, this, NULL);
  Tcl_CreateObjCommand(interp, "dservAddExactMatch",
		       dserv_add_exact_match_command, this, NULL);
  Tcl_CreateObjCommand(interp, "dservRemoveMatch",
		       dserv_remove_match_command, this, NULL);
  Tcl_CreateObjCommand(interp, "dservRemoveAllMatches",
		       dserv_remove_all_matches_command, this, NULL);
  
  Tcl_CreateObjCommand(interp, "dservLoggerOpen",
		       dserv_log_open_command, this, NULL);
  Tcl_CreateObjCommand(interp, "dservLoggerClose",
		       dserv_log_close_command, this, NULL);
  Tcl_CreateObjCommand(interp, "dservLoggerPause",
		       dserv_log_pause_command, this, NULL);
  Tcl_CreateObjCommand(interp, "dservLoggerStart",
		       dserv_log_start_command, this, NULL);
  Tcl_CreateObjCommand(interp, "dservLoggerResume",
		       dserv_log_start_command, this, NULL);
  Tcl_CreateObjCommand(interp, "dservLoggerAddMatch",
		       dserv_log_add_match_command, this, NULL);
  
  Tcl_CreateObjCommand(interp, "dpointSetScript",
		       (Tcl_ObjCmdProc *) dpoint_set_script_command,
		       this, NULL);
  Tcl_CreateObjCommand(interp, "dpointRemoveScript",
		       (Tcl_ObjCmdProc *) dpoint_remove_script_command,
		       this, NULL);
  Tcl_CreateObjCommand(interp, "dpointRemoveAllScripts",
		       (Tcl_ObjCmdProc *) dpoint_remove_all_scripts_command,
		       this, NULL);
  
  Tcl_CreateObjCommand(interp, "timerTick",
		       (Tcl_ObjCmdProc *) timer_tick_command,
		       (ClientData) this,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "timerReset",
		       (Tcl_ObjCmdProc *) timer_reset_command,
		       (ClientData) this,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "timerTickInterval",
		       (Tcl_ObjCmdProc *) timer_tick_interval_command,
		       (ClientData) this,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "timerExpired",
		       (Tcl_ObjCmdProc *) timer_expired_command,
		       (ClientData) this,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "timerSetScript",
		       (Tcl_ObjCmdProc *) timer_set_script_command,
		       (ClientData) this,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "timerRemoveScript",
		       (Tcl_ObjCmdProc *) timer_remove_script_command,
		       (ClientData) this,
		       (Tcl_CmdDeleteProc *) NULL);
  Tcl_CreateObjCommand(interp, "timerStatus",
		       (Tcl_ObjCmdProc *) timer_status_command,
		       (ClientData) this,
		       (Tcl_CmdDeleteProc *) NULL);
  
  Tcl_CreateObjCommand(interp, "print",
		       (Tcl_ObjCmdProc *) print_command, this, NULL);
  
  Tcl_LinkVar(interp, "tcpPort", (char *) &tcpport,
	      TCL_LINK_INT | TCL_LINK_READ_ONLY);
  Tcl_LinkVar(interp, "nTimers", (char *) &ntimers,
              TCL_LINK_INT | TCL_LINK_READ_ONLY);
  
  return;
}

int TclServer::Tcl_StimAppInit(Tcl_Interp *interp)
{
  if (Tcl_Init(interp) == TCL_ERROR) return TCL_ERROR;
  
  add_tcl_commands(interp);
  
  return TCL_OK;
}

int TclServer::setup_tcl(int argc, char **argv)
{
  Tcl_FindExecutable(argv[0]);
  interp = Tcl_CreateInterp();
  if (!interp) {
    std::cerr << "Error initialializing tcl interpreter" << std::endl;
  }
  
  /*
   * Invoke application-specific initialization.
   */
  
  if (Tcl_StimAppInit(interp) != TCL_OK) {
    std::cerr << "application-specific initialization failed: ";
    std::cerr << Tcl_GetStringResult(interp) << std::endl;
  }
  else {
    Tcl_SourceRCFile(interp);
  }
  
  return TCL_OK;
}

int TclServer::timer_callback(int timerid)
{
  client_request_t req;
  req.type = REQ_TIMER;
  req.timer_id = timerid;
  queue.push_back(req);
  return 0;
}

/* queue up a point to be set from other threads */
void TclServer::set_point(ds_datapoint_t *dp)
{
  client_request_t req;
  req.type = REQ_DPOINT;
  req.dpoint = dp;
  queue.push_back(req);
}

int TclServer::process_requests(void) {
  using namespace std::placeholders;
  
  int retcode;
  client_request_t req;
  
  for (auto i = 0; i < ntimers; i++) {
    TTimer *timer = new TTimer(i);
    timers.push_back(timer);
    timer->add_callback(std::bind(&TclServer::timer_callback, this, _1));
  }
  
  /* process until receive a message saying we are done */
  while (!m_bDone) {
    
    req = queue.front();
    queue.pop_front();
    
    switch (req.type) {
    case REQ_SCRIPT:
      {
	const char *script = req.script.c_str();
	
	std::unique_lock<std::mutex> mlock(mutex);
	retcode = Tcl_Eval(interp, script);
	const char *rcstr = Tcl_GetStringResult(interp);
	mlock.unlock();
	cond.notify_one(); // notify one waiting thread
	
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
	std::unique_lock<std::mutex> mlock(mutex);
	retcode = Tcl_Eval(interp, script);
	mlock.unlock();
	cond.notify_one(); // notify one waiting thread
      }
      break;
    case REQ_TIMER:
      {
	// evaluate a timer script
	std::string script;
	if (timer_scripts.find(req.timer_id, script)) {
	  std::unique_lock<std::mutex> mlock(mutex);
	  retcode = Tcl_Eval(interp, script.c_str());
	  mlock.unlock();
	  cond.notify_one(); // notify one waiting thread
	}
      }
      break;
    case REQ_DPOINT:
      {
	ds->set(req.dpoint);
      }
      break;
    case REQ_DPOINT_SCRIPT:
      {
	ds_datapoint_t *dpoint = req.dpoint;
	std::string script;
	
	// evaluate a dpoint script
	if (dpoint_scripts.find(std::string(dpoint->varname), script)) {
	  std::unique_lock<std::mutex> mlock(mutex);
	  retcode = Tcl_Eval(interp, script.c_str());
	  mlock.unlock();
	  cond.notify_one(); // notify one waiting thread
	}
	dpoint_free(dpoint);
      }
    default:
      break;
    }
  }
  
  // should delete timers here...
  
  return 0;
}
  
int TclServer::queue_size(void)
{
  return queue.size();
}

std::string TclServer::eval(char *s)
{
  std::string script(s);
  return eval(script);
}

std::string TclServer::eval(std::string script)
{
  static SharedQueue<std::string> rqueue;
  client_request_t client_request;
  client_request.type = REQ_SCRIPT;
  client_request.rqueue = &rqueue;
  client_request.script = script;
  
  // std::cout << "TCL Request: " << std::string(buf, n) << std::endl;
  
  queue.push_back(client_request);
  
  //      queue->push_back(std::string(buf, n));
  
  /* rqueue will be available after command has been processed */
  std::string s(client_request.rqueue->front());
  client_request.rqueue->pop_front();
  
  return s;
}

void TclServer::eval_noreply(char *s)
{
  std::string script(s);
  eval_noreply(script);
}

void TclServer::eval_noreply(std::string script)
{
  static SharedQueue<std::string> rqueue;
  client_request_t client_request;
  client_request.type = REQ_SCRIPT_NOREPLY;
  client_request.script = script;
  
  queue.push_back(client_request);
  std::unique_lock<std::mutex> mlock(mutex);
  cond.wait(mlock);
}

void TclServer::tcp_client_process(int sockfd,
				   SharedQueue<client_request_t> *queue)
{
  // fix this...
  char buf[16384];
  double start;
  int rval, wrval;
  
  // each client has its own request structure and reply queue
  SharedQueue<std::string> rqueue;
  client_request_t client_request;
  client_request.rqueue = &rqueue;
  
  while ((rval = read(sockfd, buf, sizeof(buf))) > 0) {
    client_request.script = std::string(buf, rval);
    
    //std::cout << "TCL Request: " << std::string(buf, res.value()) << std::endl;
    
    queue->push_back(client_request);
    
    //      queue->push_back(std::string(buf, n));
    
    /* rqueue will be available after command has been processed */
    std::string s(client_request.rqueue->front());
    client_request.rqueue->pop_front();
    
    //std::cout << "TCL Result: " << s << std::endl;
    
    // Add a newline, and send the buffer including the null termination
    s = s+"\n";
    wrval = write(sockfd, s.c_str(), s.size());
  }
  // std::cout << "Connection closed from " << sock.peer_address() << std::endl;
  close(sockfd);
}

