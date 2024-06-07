#include <iostream>
#include <chrono>
#include <future>

#include "sharedqueue.h"
#include "Dataserver.h"
#include "TclServer.h"
#include "Daemonizer.hpp"
#include "cxxopts.hpp"
#include "dserv.h"

// Provide hooks for loaded modules
Dataserver *dserver;
Dataserver *get_ds(void) { return dserver; }

TclServer *tclserver;
TclServer *get_tclserver(void) { return tclserver; }

int main(int argc, char *argv[])
{
  bool verbose = false;
  bool daemonize = false;
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
    ("d,daemon", "Daemonize", cxxopts::value<bool>(daemonize))
    ("v,verbose", "Verbose", cxxopts::value<bool>(verbose));
  
  auto result = options.parse(argc, argv);
  
  if (help) {
    std::cout << options.help({"", "Group"}) << std::endl;
    exit(0);
  }
  
  Daemonizer daemon;
  if (daemonize) daemon.start();
  
  Dataserver dserv(argc, argv);
  dserver = &dserv;		// set the global variable used for modules

  TclServer tcl(argc, argv, &dserv);
  tclserver = &tcl;
  
  if (!trigger_script.empty()) {
    auto result = dserv.eval(std::string("source ")+trigger_script);
    if (result.starts_with("!TCL_ERROR ")) std::cerr << result;
  }
  if (!configuration_script.empty()) {
    auto result = tcl.eval(std::string("source ")+configuration_script);
    if (result.starts_with("!TCL_ERROR ")) std::cerr << result << std::endl;
  }
  
  std::promise<void>().get_future().wait();  
  
}
