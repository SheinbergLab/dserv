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
 * Returns FULL replacement text (for terminal use).
 * Example: "set tcl_pl" -> "set tcl_platform set tcl_patchLevel"
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
 * Get completion tokens for editor use
 * 
 * Returns JUST THE TOKEN to insert (not full replacement text).
 * Example: "set tcl_pl" -> "tcl_platform tcl_patchLevel"
 *          "[dl_from" -> "dl_fromto"
 *          "tcl_platform(o" -> "os) osVersion)"
 * 
 * This is what editors need - just the part to insert at cursor position.
 * 
 * @param interp Tcl interpreter to query
 * @param partial Partial text to complete
 * @return Vector of token strings (just the completion part)
 */
std::vector<std::string> getCompletionTokens(Tcl_Interp* interp, const std::string& partial);

/**
 * Get filename/path completions using glob
 * Expands partial paths and adds trailing / for directories
 * 
 * @param interp Tcl interpreter to query
 * @param partial Partial path to complete (e.g., "/home/da", "./data/", "~/")
 * @param dirsOnly If true, only complete directories
 * @return Vector of matching filenames/paths with trailing / for directories
 */
std::vector<std::string> getFilenameCompletions(Tcl_Interp* interp, 
                                                 const std::string& partial,
                                                 bool dirsOnly = false);

/**
 * Tcl command callback: complete <partial>
 * Returns Tcl list of completion candidates (full replacement text for terminals)
 * 
 * Usage from Tcl:
 *   complete "info c"  -> {info commands info complete info coroutine ...}
 *   complete "set tcl_pl" -> {set tcl_platform set tcl_patchLevel}
 */
int TclCompleteCmd(ClientData clientData, Tcl_Interp* interp, 
                   int objc, Tcl_Obj* const objv[]);

/**
 * Tcl command callback: complete_token <partial>
 * Returns Tcl list of just the token portion (for editor use)
 * 
 * Usage from Tcl:
 *   complete_token "info c"  -> {commands complete coroutine ...}
 *   complete_token "set tcl_pl" -> {tcl_platform tcl_patchLevel}
 *   complete_token "[dl_from" -> {dl_fromto}
 *   complete_token "tcl_platform(o" -> {os) osVersion)}
 */
int TclCompleteTokenCmd(ClientData clientData, Tcl_Interp* interp, 
                        int objc, Tcl_Obj* const objv[]);

/**
 * Register the completion commands in the given interpreter
 * Registers both 'complete' (for terminals) and 'complete_token' (for editors)
 * Call this during interpreter initialization
 * 
 * @param interp Tcl interpreter
 */
void RegisterCompletionCommands(Tcl_Interp* interp);

// Deprecated: Use RegisterCompletionCommands instead
// Kept for backward compatibility
inline void RegisterCompletionCommand(Tcl_Interp* interp) {
    RegisterCompletionCommands(interp);
}

} // namespace TclCompletion

#endif // TCL_COMPLETION_H
