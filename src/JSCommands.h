/**
 * JSCommands.h - Tcl commands for managing JavaScript subprocesses
 */

#ifndef JSCOMMANDS_H
#define JSCOMMANDS_H

#include <tcl.h>

// Forward declarations
class TclServer;
class JSServer;
template<typename T> class ObjectRegistry;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register all JS commands with a Tcl interpreter.
 * 
 * Commands registered:
 *   jsprocess ?-link? ?name? ?script?  - Create JS subprocess
 *   jssend name script                  - Send JS code, wait for result
 *   jseval name script                  - Alias for jssend
 *   jssend_async name script            - Send JS code, don't wait
 *   jskill name                         - Shutdown JS subprocess
 *   jslist                              - List all JS subprocesses
 *   jsexists name                       - Check if subprocess exists
 * 
 * @param interp  The Tcl interpreter
 * @param tserv   The TclServer instance (provides access to Dataserver)
 * @return TCL_OK on success
 */
int JSCommands_Init(Tcl_Interp *interp, TclServer *tserv);

/**
 * Cleanup all JS subprocesses.
 * Call this during dserv shutdown.
 */
void JSCommands_Shutdown(void);

#ifdef __cplusplus
}

/**
 * Get the JS subprocess registry.
 * For advanced use cases where direct access to JSServer instances is needed.
 */
ObjectRegistry<JSServer>& GetJSServerRegistry();

#endif

#endif /* JSCOMMANDS_H */
