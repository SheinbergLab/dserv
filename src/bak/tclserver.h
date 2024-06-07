#ifndef TCLSERVER_H
#define TCLSERVER_H

#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <queue>

#include <stdlib.h>

#ifndef _WIN32
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#endif

#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/select.h>
#include <fcntl.h>

#ifdef __QNX__
#include <sys/socket.h>
#endif

#include "Datapoint.h"
#include <tcl.h>

class TTimer: public Timer {
private:
  ds_datapoint_t timer_datapoint;
  std::string timername;
  
  
public:
  TTimer(int id): Timer(id)
  {
    timername = "timer/" + std::to_string(id);
    dpoint_set(&timer_datapoint, (char *) timername.c_str(), 0,
	       DSERV_SCRIPT, 0, NULL);
  }
  char *name(void) { return (char *) timername.c_str(); }
};

class Tclserver
{
  std::string client_name;
  int socket_fd;
  int tcpport;
  std::thread net_thread;
  std::thread process_thread;
  
  Tcl_Interp *interp;
  Dataserver *ds;
  
  std::atomic<bool> m_bDone;
  
  // for client requests
  SharedQueue<client_request_t> queue;
  
  // trigger scripts for subscribed datapoints
  TriggerDict trigger_scripts;
  
  // number of timers
  const int ntimers = 8;
  std::vector<TTimer *> timers;

  // scripts attached to dpoints
  TriggerDict dpoint_scripts;
  
public:
  
  static void
  tcp_client_process(int sock,
		     SharedQueue<client_request_t> *queue);
  
  Tclserver(int argc, char **argv,
	    Dataserver *dserv, int port = 2570)
  {
    m_bDone = false;
    tcpport = port;
    ds = dserv;
    
    // create a connection to dataserver so we can subscribe to datapoints
    client_name = ds->add_new_send_client(&queue);
    
    // create Tcl interpreter for executing scripts
    setup_tcl(argc, argv);
    
    net_thread = std::thread(&Tclserver::start_tcp_server, this);
    process_thread = std::thread(&Tclserver::process_requests, this);
  }
  
  ~Tclserver()
  {
    shutdown();
    net_thread.detach();
    process_thread.detach();
  }
  
  void shutdown(void)
  {
    m_bDone = true;
  }
  
  bool isDone()
  {
    return m_bDone;
  }
  
  void start_tcp_server(void)
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
  
  int sourceFile(const char *filename)
  {
    if (!interp) {
      std::cerr << "no tcl interpreter" << std::endl;
      return TCL_ERROR;
    }
    
    return Tcl_EvalFile(interp, filename);
  }

/********************************* now *********************************/

  static int now_command (ClientData data, Tcl_Interp *interp,
		     int objc, Tcl_Obj *objv[])
  {
    Tclserver *tclserver = (Tclserver *) data;
    Dataserver *ds = tclserver->ds;

    Tcl_SetObjResult(interp, Tcl_NewWideIntObj(ds->now()));
    return TCL_OK;
  }

  
  static int dserv_add_match_command(ClientData data, Tcl_Interp * interp,
				     int objc,
				     Tcl_Obj * const objv[])
  {
    Tclserver *tclserver = (Tclserver *) data;
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
  
  static int dserv_remove_match_command(ClientData data, Tcl_Interp * interp,
					int objc,
					Tcl_Obj * const objv[])
  {
    Tclserver *tclserver = (Tclserver *) data;
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
  

  static int timer_tick_command (ClientData data, Tcl_Interp *interp,
				 int objc, Tcl_Obj *objv[])
  {
    Tclserver *tclserver = (Tclserver *) data;
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

  static int timer_reset_command (ClientData data, Tcl_Interp *interp,
				 int objc, Tcl_Obj *objv[])
  {
    Tclserver *tclserver = (Tclserver *) data;
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
  
  static int timer_tick_interval_command (ClientData data, Tcl_Interp *interp,
					  int objc, Tcl_Obj *objv[])
  {
    Tclserver *tclserver = (Tclserver *) data;
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
  
  static int timer_expired_command (ClientData data, Tcl_Interp *interp,
				    int objc, Tcl_Obj *objv[])
  {
    Tclserver *tclserver = (Tclserver *) data;
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
  
#if 0
  int timerSetScriptCmd (ClientData data, Tcl_Interp *interp,
		       int objc, Tcl_Obj *objv[])
{
  device_attr_t *dattr = (device_attr_t *) data;
  QPCSH_STATE *s = dattr->state;
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
    
    if (id >= dattr->state->ntimers) {
      Tcl_SetResult(interp, "invalid timer", TCL_STATIC);
      return TCL_ERROR;
    }
    script = Tcl_GetString(objv[2]);
  }
  
  add_script_to_table(s, s->timer_names[id], script);

  return TCL_OK;
}

int timerRemoveScriptCmd (ClientData data, Tcl_Interp *interp,
			  int objc, Tcl_Obj *objv[])
{
  device_attr_t *dattr = (device_attr_t *) data;
  QPCSH_STATE *s = dattr->state;
  int id;
  
  if (objc < 1) {
    id = 0;
  }
  else {
    if (Tcl_GetIntFromObj(interp, objv[1], &id) != TCL_OK)
      return TCL_ERROR;
    
    if (id >= dattr->state->ntimers) {
      Tcl_SetResult(interp, "invalid timer", TCL_STATIC);
      return TCL_ERROR;
    }
  }
  
  add_script_to_table(s, s->timer_names[id], NULL);

  return TCL_OK;
}


int timerStatusCmd (ClientData data, Tcl_Interp *interp,
		    int objc, Tcl_Obj *objv[])
{
  device_attr_t *dattr = (device_attr_t *) data;
  int i, ntimers;
  Tcl_Obj *elt;
  Tcl_Obj *l = Tcl_NewListObj(0, NULL);
  ntimers = dattr->state->ntimers;
  
  for (i = 0; i < ntimers; i++) {
    elt = Tcl_NewIntObj(dattr->state->timers[i].expired);
    Tcl_ListObjAppendElement(interp, l, elt);
  }

  Tcl_SetObjResult(interp, l);
  return TCL_OK;
}

#endif

  static int dpoint_add_script_command (ClientData data, Tcl_Interp *interp,
					int objc, Tcl_Obj *objv[])
  {
    Tclserver *tclserver = (Tclserver *) data;
    Dataserver *ds = tclserver->ds;
    
    if (objc < 3) {
      Tcl_WrongNumArgs(interp, 1, objv, "varname script");
      return TCL_ERROR;
    }
    
    tclserver->dpoint_scripts.insert(std::string(Tcl_GetString(objv[1])),
				     std::string(Tcl_GetString(objv[2])));
    
    return TCL_OK;
  }
  

  void add_tcl_commands(Tcl_Interp *interp)
  {
    /* use the generic Dataserver commands for these */
    Tcl_CreateObjCommand(interp, "dservGet",
			 Dataserver::dserv_get_command, this->ds, NULL);
    Tcl_CreateObjCommand(interp, "dservTouch",
			 Dataserver::dserv_touch_command, this->ds, NULL);
    Tcl_CreateObjCommand(interp, "dservTimestamp",
			 Dataserver::dserv_timestamp_command, this->ds, NULL);
    Tcl_CreateObjCommand(interp, "dservSet",
			 Dataserver::dserv_set_command, this->ds, NULL);
    Tcl_CreateObjCommand(interp, "dservSetData",
			 Dataserver::dserv_setdata_command, this->ds, NULL);
    Tcl_CreateObjCommand(interp, "dservSetData64",
			 Dataserver::dserv_setdata64_command, this->ds, NULL);
    Tcl_CreateObjCommand(interp, "dservClear",
			 Dataserver::dserv_clear_command, this->ds, NULL);
    Tcl_CreateObjCommand(interp, "dservEval",
			 Dataserver::dserv_eval_command, this->ds, NULL);

    /* these are specific to Tclservers */
    Tcl_CreateObjCommand(interp, "now",
			 (Tcl_ObjCmdProc *) now_command,
			 this, NULL);

    Tcl_CreateObjCommand(interp, "dservAddMatch",
			 dserv_add_match_command, this, NULL);
    Tcl_CreateObjCommand(interp, "dservRemoveMatch",
			 dserv_add_match_command, this, NULL);

    Tcl_CreateObjCommand(interp, "dpointAddScript",
			 (Tcl_ObjCmdProc *) dpoint_add_script_command,
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
#if 0
    Tcl_CreateObjCommand(interp, "timerStatus",
			 (Tcl_ObjCmdProc *) timerStatusCmd,
			 (ClientData) this,
			 (Tcl_CmdDeleteProc *) NULL);
#endif
    
    return;
  }
  
  int Tcl_StimAppInit(Tcl_Interp *interp)
  {
    if (Tcl_Init(interp) == TCL_ERROR) return TCL_ERROR;
    
    add_tcl_commands(interp);

    return TCL_OK;
  }

  int setup_tcl(int argc, char **argv)
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

  int timer_callback(int timerid)
  {
    ds_datapoint_t *dpoint =
      dpoint_new((char *) timers[timerid]->name(),
		 Dataserver::now(),
		 DSERV_INT, 0, nullptr);
    ds->set(dpoint);
    return 0;
  }
  
  int process_requests(void) {

    using namespace std::placeholders;
    
    int retcode;
    client_request_t req;

    for (auto i = 0; i < ntimers; i++) {
      TTimer *timer = new TTimer(i);
      timers.push_back(timer);
      timer->add_callback(std::bind(&Tclserver::timer_callback, this, _1));
    }
    
    /* process until receive a message saying we are done */
    while (!m_bDone) {
      
      req = queue.front();
      queue.pop_front();

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
	    const char *rcstr = Tcl_GetStringResult(interp);
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
      case REQ_DPOINT:
	{
	  ds_datapoint_t *dpoint = req.dpoint;
	  std::string script;
	  
	  if (dpoint_scripts.find(std::string(dpoint->varname), script))
	    retcode = Tcl_Eval(interp, script.c_str());
	  
	  dpoint_free(dpoint);
	}
      default:
	break;
      }
    }

    return 0;
  }
  
  int queue_size(void)
  {
    return queue.size();
  }

  std::string eval(char *s)
  {
    std::string script(s);
    return eval(script);
  }
  
  std::string eval(std::string script)
  {
    static SharedQueue<std::string> rqueue;
    client_request_t client_request;
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
    
};

void Tclserver::tcp_client_process(int sockfd,
				   SharedQueue<client_request_t> *queue)
{
  // fix this...
  char buf[16384];
  double start;
  int rval;
  
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
    write(sockfd, s.c_str(), s.size());
  }
  // std::cout << "Connection closed from " << sock.peer_address() << std::endl;
  close(sockfd);
}

#endif  // TCLSERVER_H
