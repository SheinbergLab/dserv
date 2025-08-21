#include <iostream>
#include <chrono>
#include <future>
#include <csignal>
#ifndef _MSC_VER
#include <pthread.h>
#include <unistd.h>
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

Dataserver *get_ds(void) { return dserver; }
TclServer *get_tclserver(void) { return tclserver; }

tclserver_t *tclserver_get(void) { return (tclserver_t *) tclserver; }
void tclserver_set_point(tclserver_t *tclserver, ds_datapoint_t *dp)
{
  ((TclServer *) tclserver)->set_point(dp);
}

uint64_t tclserver_now(tclserver_t *tclserver)
{
  return ((TclServer *) tclserver)->now();
}

void signalHandler(int signum) {
  std::cout << "Shutting down..." << std::endl;
  
  // Stop mesh networking first
  if (meshManager) {
    meshManager->stop();
    meshManager.reset();
  }
  
  delete tclserver;
  delete dserver;
  exit(0);
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

  cxxopts::Options options("dserv", "Data server");
  options.add_options()
    ("h,help", "Print help", cxxopts::value<bool>(help))
    ("t,tscript", "Trigger script path",
     cxxopts::value<std::string>(trigger_script))
    ("c,cscript", "Configuration script path",
     cxxopts::value<std::string>(configuration_script))
    ("v,version", "Version", cxxopts::value<bool>(version))
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
    std::cout << dserv_VERSION_MAJOR << "." << dserv_VERSION_MINOR << std::endl;
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
  tclserver = new TclServer(argc, argv, dserver, "ess", 2570, 2560, 2565);
  TclServerRegistry.registerObject("ess", tclserver);

  // Initialize mesh networking if enabled
  if (enable_mesh) {
    meshManager = MeshManager::createAndStart(dserver, tclserver, argc, argv,
                                             mesh_appliance_id, mesh_appliance_name,
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
