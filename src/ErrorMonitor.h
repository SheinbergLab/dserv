#ifndef ERRORMONITOR_H
#define ERRORMONITOR_H

#include <tcl.h>
#include <string>

// Forward declarations
class TclServer;

/**
 * Lightweight ErrorMonitor for a single TclServer/Interpreter.
 * 
 * Designed to be used as a local variable in the process thread:
 *   ErrorMonitor errorMonitor(tserv);
 *   errorMonitor.enable();
 * 
 * Gets interpreter from tserv->getInterp() at construction.
 * No dynamic allocation needed.
 */
class ErrorMonitor {
public:
  /**
   * Constructor - gets interpreter from TclServer
   * @param tserv Pointer to owning TclServer
   */
  explicit ErrorMonitor(TclServer* tserv);
  
  /**
   * Destructor - cleans up trace if enabled
   */
  ~ErrorMonitor();
  
  /**
   * Enable error monitoring
   * @return true if successful
   */
  bool enable();
  
  /**
   * Disable error monitoring
   */
  bool disable();
  
  /**
   * Check if error monitoring is enabled
   * @return true if enabled
   */
  bool isEnabled() const { return enabled; }
  
  /**
   * Register the errormon Tcl command with an interpreter
   * @param interp The Tcl interpreter
   * @param monitor Pointer to the ErrorMonitor instance
   */
  static void registerCommand(Tcl_Interp* interp, ErrorMonitor* monitor);
  
private:
  TclServer* tclserver;
  Tcl_Interp* interp;      // The ONE interpreter we're monitoring (from tserv->getInterp())
  bool enabled;
  
  /**
   * Variable trace callback - called when errorInfo is written
   */
  static char* traceCallback(ClientData clientData, Tcl_Interp* interp,
			     const char* name1, const char* name2, int flags);
  
  /**
   * Internal error handler - processes the error and pushes to datapoint
   */
  void handleError();
  
  /**
   * Tcl command callback: errormon enable|disable|status
   */
  static int errormonCommandProc(ClientData clientData, Tcl_Interp* interp,
				 int objc, Tcl_Obj* const objv[]);
};

#endif // ERRORMONITOR_H
