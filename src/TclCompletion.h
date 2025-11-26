// TclCompletion.h
// Standalone Tcl tab completion library
// No dependencies except Tcl itself

#ifndef TCL_COMPLETION_H
#define TCL_COMPLETION_H

#include <vector>
#include <string>
#include <tcl.h>

namespace TclCompletion {

/**
 * Get completion candidates for a partial command/variable/proc
 * 
 * Supports:
 *  - Commands: info commands $partial*
 *  - Procs: info procs $partial*
 *  - Variables: info globals $partial* (if starts with $)
 *  - Namespace-qualified: namespace eval ns {info commands}
 *  - Embedded commands: [expr, {format "test [info c"}, etc.
 *  - Context-aware completions:
 *    * After set/unset/global/variable: completes variable names
 *    * After namespace eval: completes namespace names
 *    * After array: completes array variable names
 *    * After proc: completes existing proc names
 * 
 * @param interp Tcl interpreter to query
 * @param partial Partial text to complete (e.g., "info c", "str", "$tcl_")
 * @return Deduplicated vector of completion candidates
 */
std::vector<std::string> getCompletions(Tcl_Interp* interp, const std::string& partial);

/**
 * Get completions from a remote interpreter via 'send'
 * Requires that the target interpreter has the 'complete' command registered
 * 
 * @param mainInterp Main interpreter (with send command)
 * @param targetInterp Name of target interpreter
 * @param partial Partial text to complete
 * @return Vector of completion candidates
 */
std::vector<std::string> getRemoteCompletions(Tcl_Interp* mainInterp,
                                               const std::string& targetInterp,
                                               const std::string& partial);

/**
 * Tcl command callback: complete <partial>
 * Returns Tcl list of completion candidates
 * 
 * Usage from Tcl:
 *   complete "info c"  → {info commands info complete info coroutine ...}
 */
int TclCompleteCmd(ClientData clientData, Tcl_Interp* interp, 
                   int objc, Tcl_Obj* const objv[]);

/**
 * Register the 'complete' command in the given interpreter
 * Call this during interpreter initialization
 * 
 * @param interp Tcl interpreter
 */
void RegisterCompletionCommand(Tcl_Interp* interp);

} // namespace TclCompletion

#endif // TCL_COMPLETION_H
