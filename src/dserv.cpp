#include <iostream>
#include <chrono>
#include <future>
#include <csignal>
#ifndef _MSC_VER
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

#include "sharedqueue.h"
#include "Dataserver.h"
#include "TclServer.h"
#include "MeshManager.h"
#include "ObjectRegistry.h"
#include "cxxopts.hpp"
#include "dserv.h"
#include "tclserver_api.h"
#include "mdns_advertise.h"

#include "dservConfig.h"

#include "tclserver_api.h"

// A registry for main tclserver and subprocesses
ObjectRegistry<TclServer> TclServerRegistry;

// Provide hooks for loaded modules
Dataserver *dserver;
TclServer *tclserver;

// Mesh support
std::unique_ptr<MeshManager> meshManager;
MeshManager* get_mesh_manager(void) { 
  return meshManager.get(); 
}

// These should be part of an api 
extern "C" {

	Dataserver *get_ds(void) { return dserver; }
	TclServer *get_tclserver(void) { return tclserver; }
	
	tclserver_t* tclserver_get_from_interp(Tcl_Interp *interp) {
		TclServer* server = (TclServer*)Tcl_GetAssocData(interp, 
		       "tclserver_instance", NULL);
		return server ? (tclserver_t*)server : nullptr; // fallback to main
	}
	
	void tclserver_set_point(tclserver_t *tclserver, ds_datapoint_t *dp)
	{
	  ((TclServer *) tclserver)->set_point(dp);
	}
	
	uint64_t tclserver_now(tclserver_t *tclserver)
	{
	  return ((TclServer *) tclserver)->now();
	}
	
	
  void tclserver_queue_script(tclserver_t *tclserver,
			      const char *script, int no_reply) 
	{
	  TclServer *ts = static_cast<TclServer*>(tclserver);
	  
	  client_request_t req;
	  req.type = (request_t) (no_reply ? REQ_SCRIPT_NOREPLY : REQ_SCRIPT);
	  req.script = std::string(script);
	  
	  ts->queue.push_back(req);
	}
}

static std::atomic<bool> shutdownInProgress{false};

void signalHandler(int signum) {
  // Prevent double execution
  if (shutdownInProgress.exchange(true)) {
    std::cout << "\nForced exit (second signal)" << std::endl;
    std::_Exit(1);  // Immediate exit without destructors
    return;
  }
  
  std::cout << "\nShutting down gracefully..." << std::endl;
  
  // Stop mesh networking first
  if (meshManager) {
    std::cout << "Stopping mesh manager..." << std::endl;
    meshManager->stop();
    std::cout << "Resetting mesh manager..." << std::endl;
    meshManager.reset();
  }

  // Shutdown all subprocesses cleanly
  std::cout << "Shutting down subprocesses..." << std::endl;
  std::vector<std::string> names = TclServerRegistry.getNames();
  for (const auto& name : names) {
    if (name != "dserv" && !name.empty()) {
      TclServer* child = TclServerRegistry.getObject(name);
      if (child && child->getInterp()) {
        std::cout << "  Shutting down: " << name << std::endl;
        child->eval("exit");
      }
    }
  }
  // Brief wait for subprocesses to finish cleanup
  std::this_thread::sleep_for(std::chrono::milliseconds(500));  
  
  std::cout << "Deleting TclServer..." << std::endl;
  delete tclserver;
  std::cout << "Deleting Dataserver..." << std::endl;
  delete dserver;
  std::cout << "Clean shutdown complete." << std::endl;
  exit(0);
}

void setVersionInfo(TclServer* tclserver) {
    std::string version_str = dserv_VERSION;
    
    // Set as a datapoint
    tclserver->eval("dservSet system/version \"" + version_str + "\"");
    
    // Also set as a Tcl variable for direct access
    tclserver->eval("set ::dserv_version \"" + version_str + "\"");
    
    std::cout << "dserv version " << version_str << " initialized" << std::endl;
}


std::string getHostname() {
  char hostname[256];
  if (gethostname(hostname, sizeof(hostname)) == 0) {
    return std::string(hostname);
  }
  return "unknown";
}

/*
 * mainline
 */
int main(int argc, char *argv[])
{
  bool version = false;
  bool help = false;
  bool enable_mesh = true;
  int mesh_port = 12348;
  int mesh_discovery_port = 12346;
  int mesh_websocket_port = 2569;
  int mesh_tcl_port = 2575;
  std::string mesh_appliance_id;
  std::string mesh_appliance_name;
  std::string trigger_script;
  std::string configuration_script;
  std::string www_path;
  
  cxxopts::Options options("dserv", "Data server");
  options.add_options()
    ("h,help", "Print help", cxxopts::value<bool>(help))
    ("t,tscript", "Trigger script path",
     cxxopts::value<std::string>(trigger_script))
    ("c,cscript", "Configuration script path",
     cxxopts::value<std::string>(configuration_script))
    ("v,version", "Version", cxxopts::value<bool>(version))
    ("w,www", "Static file serving directory",
     cxxopts::value<std::string>(www_path))    
    ("m,mesh", "Enable/disable mesh networking", cxxopts::value<bool>(enable_mesh))
    ("mesh-port", "Mesh HTTP port", cxxopts::value<int>(mesh_port)->default_value("12348"))
    ("mesh-discovery-port", "Mesh discovery port", cxxopts::value<int>(mesh_discovery_port)->default_value("12346"))
    ("mesh-id", "Mesh appliance ID (defaults to hostname)", cxxopts::value<std::string>(mesh_appliance_id))
    ("mesh-name", "Mesh appliance name (defaults to 'Lab Station <hostname>')", cxxopts::value<std::string>(mesh_appliance_name));

  try {
    auto result = options.parse(argc, argv);
  } catch (const cxxopts::exceptions::parsing& e) {
    std::cerr << "Error parsing options: " << e.what() << std::endl;
    // Explicit exit, rather than abort, for testing with ctest.
    exit(-1);
  }

  if (version) {
    std::cout << dserv_VERSION << std::endl;
    exit(0);
  }
  
  if (help) {
    std::cout << options.help({"", "Group"}) << std::endl;
    std::cout << std::endl;
    std::cout << "Mesh Networking Examples:" << std::endl;
    std::cout << "  dserv --mesh                                    # Enable with defaults" << std::endl;
    std::cout << "  dserv --mesh --mesh-id=lab_station_1           # Custom appliance ID" << std::endl;
    std::cout << "  dserv --mesh --mesh-port=12350                 # Custom HTTP port" << std::endl;
    std::cout << "  dserv --mesh --mesh-discovery-port=12351       # Custom discovery port" << std::endl;
    exit(0);
  }

  std::signal(SIGINT, signalHandler);

  // Create core dserv components
  dserver = new Dataserver(argc, argv);

  TclServerConfig tclserver_config("dserv", 2570, 2560, 2565);

  // Include default www path if not specified
  if (www_path.empty()) {
    // Check if default location exists
    struct stat st;
    if (stat("/usr/local/dserv/www", &st) == 0 && S_ISDIR(st.st_mode)) {
      www_path = "/usr/local/dserv/www";
    }
  }
  
  tclserver_config.www_path = www_path;
  tclserver = new TclServer(argc, argv, dserver, tclserver_config);
  
  TclServerRegistry.registerObject("dserv", tclserver);

  setVersionInfo(tclserver);

  // Initialize mesh networking if enabled
  if (enable_mesh) {
    meshManager = MeshManager::createAndStart(dserver, tclserver, argc, argv,
					      mesh_appliance_id,
					      mesh_appliance_name,
					      mesh_port, mesh_discovery_port,
					      mesh_websocket_port);
   }

  // Run initialization scripts
  if (!trigger_script.empty()) {
    auto result = dserver->eval(std::string("source ")+trigger_script);
    if (result.starts_with("!TCL_ERROR ")) std::cerr << result;
  }
  
  if (!configuration_script.empty()) {
    auto result = tclserver->eval(std::string("source ")+configuration_script);
    if (result.starts_with("!TCL_ERROR ")) std::cerr << result << std::endl;
  }

  /* use mdns to advertise services */
  advertise_services(dserver->port(), tclserver->newline_port());

  std::promise<void>().get_future().wait();
}
