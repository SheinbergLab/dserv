#include "TclServer.h"
#include "TclCommands.h"
#include "ObjectRegistry.h"
#include "dserv.h"
#include <vector>

static int process_requests(TclServer *tserv);
static Tcl_Interp *setup_tcl(TclServer *tserv);

TclServer::TclServer(int argc, char **argv,
		     Dataserver *dserv, int port,
		     socket_t socket_type):
  argc(argc), argv(argv), socket_type(socket_type)
{
  m_bDone = false;
  tcpport = port;
  ds = dserv;
  
  // create a connection to dataserver so we can subscribe to datapoints
  client_name = ds->add_new_send_client(&queue);

  // create a tcp/ip listener if port is not -1
  if (port >= 0)
    net_thread = std::thread(&TclServer::start_tcp_server, this);
  
  // the process thread
  process_thread = std::thread(&process_requests, this);
}

TclServer::~TclServer()
{
  shutdown();

  if (tcpport > 0) 
    net_thread.detach();
  
  process_thread.join();
}

void TclServer::shutdown(void)
{
  m_bDone = true;
  shutdown_message(&queue);
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
  
  //  std::cout << "opening server on port " << std::to_string(tcpport) << std::endl;
  
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

  //  std::cout << "listen socket: " << std::to_string(socket_fd) << std::endl;

  while (!m_bDone) {
    /* Accept connection to client. */
    new_socket_fd = accept(socket_fd, &client_address, &client_address_len);
    if (new_socket_fd == -1) {
      perror("accept");
      continue;
    }
    
    setsockopt(new_socket_fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    
    // Create a thread and transfer the new stream to it.
    if (socket_type == SOCKET_LINE) {
      std::thread thr(tcp_client_process, this, new_socket_fd, &queue);
      thr.detach();
    }
    else {
      std::thread thr(message_client_process, this, new_socket_fd, &queue);
      thr.detach();
    }
  }
  
  close(socket_fd);
}

/********************************* now *********************************/

static int now_command (ClientData data, Tcl_Interp *interp,
			    int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;

  
  Tcl_SetObjResult(interp, Tcl_NewWideIntObj(ds->now()));
  return TCL_OK;
}


/********************************* send ********************************/

static int send_command (ClientData data, Tcl_Interp *interp,
				 int objc, Tcl_Obj *objv[])
{
  TclServer *this_server = (TclServer *) data;
  
  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "server message");
    return TCL_ERROR;
  }
    
  auto tclserver = TclServerRegistry.getObject(Tcl_GetString(objv[1]));
  if (!tclserver) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		     ": server \"", Tcl_GetString(objv[1]), "\" not found",
		     NULL);
    return TCL_ERROR;
  }

  if (tclserver == this_server) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		     ": cannot send message to self", NULL);
    return TCL_ERROR;
  }


  SharedQueue<std::string> rqueue;
  client_request_t client_request;
  client_request.type = REQ_SCRIPT;
  client_request.rqueue = &rqueue;
  client_request.script = std::string(Tcl_GetString(objv[2]));
  
  tclserver->queue.push_back(client_request);

  /* rqueue will be available after command has been processed */
  /* NOTE: this can create a deadlock between two tclservers   */
  
  std::string s(client_request.rqueue->front());
  client_request.rqueue->pop_front();

  Tcl_SetObjResult(interp, Tcl_NewStringObj(s.c_str(), -1));
  return TCL_OK;
}

/***************************** send_noreply ****************************/

static int send_noreply_command (ClientData data, Tcl_Interp *interp,
				 int objc, Tcl_Obj *objv[])
{
  TclServer *this_server = (TclServer *) data;
  
  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "server message");
    return TCL_ERROR;
  }
    
  auto tclserver = TclServerRegistry.getObject(Tcl_GetString(objv[1]));
  if (!tclserver) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		     ": server \"", Tcl_GetString(objv[1]), "\" not found",
		     NULL);
    return TCL_ERROR;
  }

  if (tclserver == this_server) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		     ": cannot send message to self", NULL);
    return TCL_ERROR;
  }

  client_request_t client_request;
  client_request.type = REQ_SCRIPT_NOREPLY;
  client_request.script = std::string(Tcl_GetString(objv[2]));

  tclserver->queue.push_back(client_request);

  /* don't wait for a reply to the message, just return */
  return TCL_OK;
}


/******************************* process *******************************/

static int subprocess_command (ClientData data, Tcl_Interp *interp,
			       int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  int port;
  
  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv, "name port [startup_script]");
    return TCL_ERROR;
  }
  
  if (Tcl_GetIntFromObj(interp, objv[2], &port) != TCL_OK) {
    return TCL_ERROR;
  }

  TclServer *child = new TclServer(tclserver->argc, tclserver->argv,
				   tclserver->ds, port);
  TclServerRegistry.registerObject(Tcl_GetString(objv[1]), child);

  if (objc > 2) {
    std::string script = std::string(Tcl_GetString(objv[3]));
    auto result = child->eval(script);
    if (result.starts_with("!TCL_ERROR ")) {
      Tcl_AppendResult(interp, result.c_str(), NULL);
      delete child;
      return TCL_ERROR;
    }
  }
  
  Tcl_SetObjResult(interp, Tcl_NewStringObj(child->client_name.c_str(), -1));
  
  return TCL_OK;
}


static int getsubprocesses_command(ClientData clientData, Tcl_Interp *interp, 
				   int objc, Tcl_Obj *const objv[])
{
  try {
    // Get all object names 
    auto allObjects = TclServerRegistry.getAllObjects();
    std::vector<std::string> names;
    names.reserve(allObjects.size());
    for (const auto& pair : allObjects) {
      names.push_back(pair.first);
    }    
    
    // Create a new Tcl dictionary
    Tcl_Obj *dictObj = Tcl_NewDictObj();
    
    // Iterate through all objects
    for (const std::string& name : names) {
      TclServer* obj = TclServerRegistry.getObject(name);
      if (obj != nullptr) {
	// Create key (object name)
	Tcl_Obj *keyObj = Tcl_NewStringObj(name.c_str(), -1);
	Tcl_Obj *valueObj = Tcl_NewIntObj(obj->port());
        
	// Add key-value pair to dictionary
	if (Tcl_DictObjPut(interp, dictObj, keyObj, valueObj) != TCL_OK) {
	  return TCL_ERROR;
	}
      }
    }  
    
    // Set the result
    Tcl_SetObjResult(interp, dictObj);
    return TCL_OK;
    
  } catch (const std::exception& e) {
    Tcl_SetResult(interp, const_cast<char*>(e.what()), TCL_VOLATILE);
    return TCL_ERROR;
  }
}


static int dserv_add_match_command(ClientData data, Tcl_Interp * interp,
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

static int dserv_add_exact_match_command(ClientData data, Tcl_Interp * interp,
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

static int dserv_remove_match_command(ClientData data, Tcl_Interp * interp,
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

static int dserv_remove_all_matches_command(ClientData data,
						Tcl_Interp * interp,
						int objc,
						Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  ds->client_remove_all_matches(tclserver->client_name);
  return TCL_OK;
}


static int dserv_logger_clients_command(ClientData data, Tcl_Interp *interp,
					    int objc, Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  std::string clients;
  
  clients = ds->get_logger_clients();
  Tcl_SetObjResult(interp, Tcl_NewStringObj(clients.data(), clients.size()));
    
  return TCL_OK;
}


static int dserv_log_open_command(ClientData data, Tcl_Interp *interp,
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

static int dserv_log_close_command(ClientData data, Tcl_Interp *interp,
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

static int dserv_log_pause_command(ClientData data, Tcl_Interp *interp,
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

static int dserv_log_start_command(ClientData data, Tcl_Interp *interp,
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

static int dserv_log_add_match_command(ClientData data, Tcl_Interp *interp,
					   int objc, Tcl_Obj * const objv[])
{
  TclServer *tclserver = (TclServer *) data;
  Dataserver *ds = tclserver->ds;
  int status;
  int obs_limited = 0;
  int buffer_size = 0;
  int every = 1;
  
  if (objc < 3) {
    Tcl_WrongNumArgs(interp, 1, objv,
		     "path match [obs_limited buffer_size every]");
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
  
  if (objc > 5) {
    if (Tcl_GetIntFromObj(interp, objv[5], &every) != TCL_OK)
      return TCL_ERROR;
  }

  if (every <= 0) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		     ": invalid \"every\" argument",
		     NULL);
    return TCL_ERROR;
  }
  if (buffer_size < 0) {
    Tcl_AppendResult(interp, Tcl_GetString(objv[0]),
		     ": invalid buffer_size argument",
		     NULL);
    return TCL_ERROR;
  }
  status = ds->logger_add_match(Tcl_GetString(objv[1]),
				Tcl_GetString(objv[2]),
				every, obs_limited, buffer_size);
  return (status > 0) ? TCL_OK : TCL_ERROR;
}

static int dpoint_set_script_command (ClientData data, Tcl_Interp *interp,
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

static int dpoint_remove_script_command (ClientData data, Tcl_Interp *interp,
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

static int dpoint_remove_all_scripts_command (ClientData data,
					      Tcl_Interp *interp,
					      int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;
  tclserver->dpoint_scripts.clear();
  return TCL_OK;
}

static int print_command (ClientData data, Tcl_Interp *interp,
			  int objc, Tcl_Obj *objv[])
{
  TclServer *tclserver = (TclServer *) data;

  ds_datapoint_t dpoint;
  char *s;
  Tcl_Size len;
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

static void add_tcl_commands(Tcl_Interp *interp, TclServer *tserv)
{
  /* use the generic Dataserver commands for these */
  Tcl_CreateObjCommand(interp, "dpointExists",
		       dserv_exists_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservExists",
		       dserv_exists_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dpointGet",
		       dserv_get_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservGet",
		       dserv_get_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservSet",
		       dserv_set_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservTouch",
		       dserv_touch_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservTimestamp",
		       dserv_timestamp_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservSetData",
		       dserv_setdata_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservSetData64",
		       dserv_setdata64_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservClear",
		       dserv_clear_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservEval",
		       dserv_eval_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservKeys",
		       dserv_keys_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "dservDGDir",
		       dserv_dgdir_command, tserv->ds, NULL);

  Tcl_CreateObjCommand(interp, "processGetParam",
		       process_get_param_command, tserv->ds, NULL);
  Tcl_CreateObjCommand(interp, "processSetParam",
		       process_set_param_command, tserv->ds, NULL);

  /* these are specific to TclServers */
  Tcl_CreateObjCommand(interp, "now",
		       (Tcl_ObjCmdProc *) now_command,
		       tserv, NULL);

  Tcl_CreateObjCommand(interp, "subprocess",
		       (Tcl_ObjCmdProc *) subprocess_command,
		       tserv, NULL);
  Tcl_CreateObjCommand(interp, "subprocessInfo",
		       (Tcl_ObjCmdProc *) getsubprocesses_command,
		       tserv, NULL);

  Tcl_CreateObjCommand(interp, "send",
		       (Tcl_ObjCmdProc *) send_command,
		       tserv, NULL);
  Tcl_CreateObjCommand(interp, "sendNoReply",
		       (Tcl_ObjCmdProc *) send_noreply_command,
		       tserv, NULL);
  
  Tcl_CreateObjCommand(interp, "dservAddMatch",
		       dserv_add_match_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservAddExactMatch",
		       dserv_add_exact_match_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservRemoveMatch",
		       dserv_remove_match_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservRemoveAllMatches",
		       dserv_remove_all_matches_command, tserv, NULL);
  
  Tcl_CreateObjCommand(interp, "dservLoggerClients",
		       dserv_logger_clients_command, tserv, NULL);

  Tcl_CreateObjCommand(interp, "dservLoggerOpen",
		       dserv_log_open_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservLoggerClose",
		       dserv_log_close_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservLoggerPause",
		       dserv_log_pause_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservLoggerStart",
		       dserv_log_start_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservLoggerResume",
		       dserv_log_start_command, tserv, NULL);
  Tcl_CreateObjCommand(interp, "dservLoggerAddMatch",
		       dserv_log_add_match_command, tserv, NULL);
  
  Tcl_CreateObjCommand(interp, "dpointSetScript",
		       (Tcl_ObjCmdProc *) dpoint_set_script_command,
		       tserv, NULL);
  Tcl_CreateObjCommand(interp, "dpointRemoveScript",
		       (Tcl_ObjCmdProc *) dpoint_remove_script_command,
		       tserv, NULL);
  Tcl_CreateObjCommand(interp, "dpointRemoveAllScripts",
		       (Tcl_ObjCmdProc *) dpoint_remove_all_scripts_command,
		       tserv, NULL);

  Tcl_CreateObjCommand(interp, "print",
		       (Tcl_ObjCmdProc *) print_command, tserv, NULL);
  
  Tcl_LinkVar(interp, "tcpPort", (char *) &tserv->tcpport,
	      TCL_LINK_INT | TCL_LINK_READ_ONLY);
  return;
}

static int Tcl_StimAppInit(Tcl_Interp *interp, TclServer *tserv)
{
  if (Tcl_Init(interp) == TCL_ERROR) return TCL_ERROR;
  
  add_tcl_commands(interp, tserv);
  
  return TCL_OK;
}

static Tcl_Interp *setup_tcl(TclServer *tserv)
{
  Tcl_Interp *interp;
  
  Tcl_FindExecutable(tserv->argv[0]);
  interp = Tcl_CreateInterp();
  if (!interp) {
    std::cerr << "Error initialializing tcl interpreter" << std::endl;
    return interp;
  }
#if 0
  if (TclZipfs_Mount(interp, "/usr/local/dserv/tclserver.zip", "app", NULL) != TCL_OK) {
    //    std::cerr << "Tclserver: error mounting zipfs" << std::endl;
  }
  else {
    //    std::cerr << "Mounted zipfs" << std::endl;
  }
#endif
  
  TclZipfs_AppHook(&tserv->argc, &tserv->argv);

  
  /*
   * Invoke application-specific initialization.
   */
  
  if (Tcl_StimAppInit(interp, tserv) != TCL_OK) {
    std::cerr << "application-specific initialization failed: ";
    std::cerr << Tcl_GetStringResult(interp) << std::endl;
  }
  else {
    Tcl_SourceRCFile(interp);
  }
  
  return interp;
}

/* queue up a point to be set from other threads */
void TclServer::set_point(ds_datapoint_t *dp)
{
  client_request_t req;
  req.type = REQ_DPOINT;
  req.dpoint = dp;
  queue.push_back(req);
}

/*
 * run a tcl script for give datapoint
 */
static int dpoint_tcl_script(Tcl_Interp *interp,
			     const char *script,
			     ds_datapoint_t *dpoint)
{
  Tcl_Obj *commandArray[3];
  commandArray[0] = Tcl_NewStringObj(script, -1);
	  
  /* name of dpoint (special for DSERV_EVTs */
  if (dpoint->data.e.dtype != DSERV_EVT) {
    commandArray[1] = Tcl_NewStringObj(dpoint->varname,
				       dpoint->varlen);
    /* data as Tcl_Obj */
    commandArray[2] = dpoint_to_tclobj(interp, dpoint);
  }
  else {
    /* convert eventlog/events -> evt:TYPE:SUBTYPE notation */
    char evt_namebuf[32];
    snprintf(evt_namebuf, sizeof(evt_namebuf), "evt:%d:%d",
	     dpoint->data.e.type, dpoint->data.e.subtype);
    commandArray[1] = Tcl_NewStringObj(evt_namebuf, -1);
    
    /* create a placeholder for repackaged dpoint */
    ds_datapoint_t e_dpoint;
    e_dpoint.data.type = (ds_datatype_t) dpoint->data.e.puttype;
    e_dpoint.data.len = dpoint->data.len;
    e_dpoint.data.buf = dpoint->data.buf;
    
    /* data as Tcl_Obj */
    commandArray[2] = dpoint_to_tclobj(interp, &e_dpoint);
  }
  /* incr ref count on command arguments */
  for (int i = 0; i < 3; i++) { Tcl_IncrRefCount(commandArray[i]); }
  
  /* call command */
  int retcode = Tcl_EvalObjv(interp, 3, commandArray, 3);
  
  /* decr ref count on command arguments */
  for (int i = 0; i < 3; i++) { Tcl_DecrRefCount(commandArray[i]); }
  return retcode;
}

static int process_requests(TclServer *tserv)
{
  int retcode;
  client_request_t req;

  /* create a unique interpreter for this process queue */
  Tcl_Interp *interp = setup_tcl(tserv);
  
  /* process until receive a message saying we are done */
  while (!tserv->m_bDone) {
    
    req = tserv->queue.front();
    tserv->queue.pop_front();
    
    switch (req.type) {
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
    case REQ_DPOINT:
      {
	tserv->ds->set(req.dpoint);
      }
      break;
    case REQ_DPOINT_SCRIPT:
      {
	ds_datapoint_t *dpoint = req.dpoint;
	std::string script;
	std::string varname(dpoint->varname);
	// evaluate a dpoint script

	if (tserv->dpoint_scripts.find(varname, script)) {
	  ds_datapoint_t *dpoint = req.dpoint;
	  const char *dpoint_script = script.c_str();
	  int retcode = dpoint_tcl_script(interp, dpoint_script, dpoint);
	}
	else if (tserv->dpoint_scripts.find_match(varname, script)) {
	  ds_datapoint_t *dpoint = req.dpoint;
	  const char *dpoint_script = script.c_str();
	  int retcode = dpoint_tcl_script(interp, dpoint_script, dpoint);
	}

	dpoint_free(dpoint);
      }
    default:
      break;
    }
  }

  Tcl_DeleteInterp(interp);
//  std::cout << "TclServer process thread ended" << std::endl;
  
  return 0;
}
  
int TclServer::queue_size(void)
{
  return queue.size();
}

void TclServer::shutdown_message(SharedQueue<client_request_t> *q)
{
  client_request_t client_request;
  client_request.type = REQ_SHUTDOWN;
  q->push_back(client_request);
}

std::string TclServer::eval(char *s)
{
  std::string script(s);
  return eval(script);
}

std::string TclServer::eval(std::string script)
{
  SharedQueue<std::string> rqueue;
  client_request_t client_request;
  client_request.type = REQ_SCRIPT;
  client_request.rqueue = &rqueue;
  client_request.script = script;
  
  queue.push_back(client_request);

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
  SharedQueue<std::string> rqueue;
  client_request_t client_request;
  client_request.type = REQ_SCRIPT_NOREPLY;
  client_request.script = script;
  
  queue.push_back(client_request);
}

/*
 * tcp_client_process is CR/LF oriented
 *  incoming messages are terminated by newlines and responses append these
 */
void
TclServer::tcp_client_process(TclServer *tserv,
			      int sockfd,
			      SharedQueue<client_request_t> *queue)
{
  int rval;
  int wrval;
  char buf[1024];
  
  // each client has its own request structure and reply queue
  SharedQueue<std::string> rqueue;
  client_request_t client_request;
  client_request.rqueue = &rqueue;
  client_request.type = REQ_SCRIPT;

  std::string script;  
    
  while ((rval = recv(sockfd, buf, sizeof(buf), 0)) > 0) {
    for (int i = 0; i < rval; i++) {
      char c = buf[i];
      if (c == '\n') {
	// shutdown if main server has shutdown
	if (tserv->m_bDone) break;
	
	if (script.length() > 0) {
	  std::string s;
	  client_request.script = std::string(script);

	  // ignore certain commands, especially exit...
	  if (!client_request.script.compare(0, 4, "exit")) {
	    s = std::string("");
	  } else {
	    // push request onto queue for main thread to retrieve
	    queue->push_back(client_request);
	    
	    // rqueue will be available after command has been processed
	    s = client_request.rqueue->front();
	    client_request.rqueue->pop_front();
	    
	    //	  std::cout << "TCL Result: " << s << std::endl;
	  }
	  // Add a newline, and send the buffer including the null termination
	  s = s+"\n";
#ifndef _MSC_VER
	  wrval = write(sockfd, s.c_str(), s.size());
#else
	  wrval = send(sockfd, s.c_str(), s.size(), 0);
#endif
	  if (wrval < 0) {		// couldn't send to client
	    break;
	  }
	}
	script = "";
      }
      else {
	script += c;
      }
    }
  }
  //    std::cout << "Connection closed from " << sock.peer_address() << std::endl;
#ifndef _MSC_VER
  close(sockfd);
#else
  closesocket(sockfd);
#endif
}

static  void sendMessage(int socket, const std::string& message) {
  // Convert size to network byte order
  uint32_t msgSize = htonl(message.size()); 

  send(socket, (char *) &msgSize, sizeof(msgSize), 0);
  send(socket, message.c_str(), message.size(), 0);
}

static std::pair<char*, size_t> receiveMessage(int socket) {
    uint32_t msgSize;
    // Receive the size of the message
    ssize_t bytesReceived = recv(socket, (char *) &msgSize,
				 sizeof(msgSize), 0);
    if (bytesReceived <= 0) return {nullptr, 0};

    // Convert size from network byte order to host byte order
    msgSize = ntohl(msgSize); 

    // Allocate buffer for the message
    char* buffer = new char[msgSize];
    size_t totalBytesReceived = 0;
    while (totalBytesReceived < msgSize) {
        bytesReceived = recv(socket, buffer + totalBytesReceived,
			     msgSize - totalBytesReceived, 0);
        if (bytesReceived <= 0) {
	  delete[] buffer;
	  return {nullptr, 0}; // Connection closed or error
        }
        totalBytesReceived += bytesReceived;
    }
    
    return {buffer, msgSize};
}

/*
 * message_client_process is frame oriented with 32 size following by bytes
 *  response is similarly organized
 */
void
TclServer::message_client_process(TclServer *tserv,
				    int sockfd,
				  SharedQueue<client_request_t> *queue)
{
  int rval;
  int wrval;
  
  // each client has its own request structure and reply queue
  SharedQueue<std::string> rqueue;
  client_request_t client_request;
  client_request.rqueue = &rqueue;
  client_request.type = REQ_SCRIPT;
  
  std::string script;  

  while (true) {
    auto [buffer, msgSize] = receiveMessage(sockfd);
    if (buffer == nullptr) break;
    if (msgSize) {

      // shutdown if main server has shutdown
      if (tserv->m_bDone) break;

      client_request.script = std::string(buffer);
      std::string s;
      
      // ignore certain commands, especially exit...
      if (!client_request.script.compare(0, 4, "exit")) {
	s = std::string("");
      } else {
	// push request onto queue for main thread to retrieve
	queue->push_back(client_request);
	
	// rqueue will be available after command has been processed
	s = client_request.rqueue->front();
	client_request.rqueue->pop_front();
	//	std::cout << "TCL Result: " << s << std::endl;
      }

      // Send a response back to the client
      sendMessage(sockfd, s);
      
      delete[] buffer;
    }
  }
#ifndef _MSC_VER
  close(sockfd);
#else
  closesocket(sockfd);
#endif
}    

