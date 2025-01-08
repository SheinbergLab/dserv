#include <iostream>
#include <chrono>
#include <future>
#include <csignal>
#ifndef _MSC_VER
#include <pthread.h>
#endif

#include "sharedqueue.h"
#include "Dataserver.h"
#include "TclServer.h"
#include "cxxopts.hpp"
#include "dserv.h"
#include "tclserver_api.h"
#include "mdns_advertise.h"

#include "dservConfig.h"

// Provide hooks for loaded modules
Dataserver *dserver;
TclServer *tclserver;

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
  delete tclserver;
  delete dserver;
  exit(0);
}

/*
 * mainline
 */
int main(int argc, char *argv[])
{
  bool version = false;
  bool help = false;
  std::string trigger_script;
  std::string configuration_script;

  cxxopts::Options options("dserv", "Data server");
  options.add_options()
    ("h,help", "Print help", cxxopts::value<bool>(help))
    ("t,tscript", "Trigger script path",
     cxxopts::value<std::string>(trigger_script))
    ("c,cscript", "Configuration script path",
     cxxopts::value<std::string>(configuration_script))
    ("v,version", "Version", cxxopts::value<bool>(version));

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
    exit(0);
  }

  std::signal(SIGINT, signalHandler);

  dserver = new Dataserver(argc, argv);
  tclserver = new TclServer(argc, argv, dserver);

  if (!trigger_script.empty()) {
    auto result = dserver->eval(std::string("source ")+trigger_script);
    if (result.starts_with("!TCL_ERROR ")) std::cerr << result;
  }
  if (!configuration_script.empty()) {
    auto result = tclserver->eval(std::string("source ")+configuration_script);
    if (result.starts_with("!TCL_ERROR ")) std::cerr << result << std::endl;
  }

  /* use mdns to advertise services */
  advertise_services(dserver->port(), tclserver->port());

  std::promise<void>().get_future().wait();
}
