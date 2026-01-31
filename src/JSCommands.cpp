/**
 * JSCommands.cpp - Tcl commands for managing JavaScript subprocesses
 * 
 * Provides:
 *   jsprocess ?name? ?script?  - Create a new JS subprocess
 *   jssend name script         - Send JS code to a subprocess (waits for result)
 *   jseval name script         - Alias for jssend
 *   jssend_async name script   - Send JS code without waiting
 *   jskill name                - Shutdown a JS subprocess
 *   jslist                     - List all JS subprocesses
 */

#include <tcl.h>
#include <string>
#include <atomic>

#include "TclServer.h"
#include "JSServer.h"
#include "JSCommands.h"
#include "ObjectRegistry.h"

// Registry for JS subprocesses (separate from TclServer registry)
static ObjectRegistry<JSServer> JSServerRegistry;

// Counter for auto-generated names
static std::atomic<int> js_name_counter{0};

/**
 * jsprocess ?-link? ?name? ?script?
 * 
 * Create a new JavaScript subprocess.
 */
static int jsprocess_command(ClientData data, Tcl_Interp *interp,
                             int objc, Tcl_Obj *const objv[])
{
    TclServer *tclserver = (TclServer *)data;
    std::string script;
    bool link_connection = false;
    int arg_idx = 1;
    
    // Parse options
    while (arg_idx < objc && Tcl_GetString(objv[arg_idx])[0] == '-') {
        std::string opt = Tcl_GetString(objv[arg_idx]);
        if (opt == "-link") {
            link_connection = true;
            arg_idx++;
        } else {
            Tcl_AppendResult(interp, "unknown option: ", opt.c_str(), NULL);
            return TCL_ERROR;
        }
    }
    
    std::string name;
    
    // Generate name if -link and no name provided
    if (link_connection && arg_idx >= objc) {
        do {
            name = "js_" + std::to_string(js_name_counter.fetch_add(1));
        } while (JSServerRegistry.exists(name));
    } else if (arg_idx < objc) {
        name = Tcl_GetString(objv[arg_idx++]);
    } else {
        Tcl_WrongNumArgs(interp, 1, objv, "?-link? ?name? ?script?");
        return TCL_ERROR;
    }
    
    // Check for script argument
    if (arg_idx < objc) {
        script = Tcl_GetString(objv[arg_idx]);
    }
    
    // Check if already exists
    if (JSServerRegistry.exists(name)) {
        Tcl_AppendResult(interp, "jsprocess: \"", name.c_str(), 
                        "\" already exists", NULL);
        return TCL_ERROR;
    }
    
    // Create the JS subprocess
    JSServer *child = new JSServer(tclserver->ds, name);
    
    // Link to connection if requested
    if (link_connection) {
        child->set_linked(true);
    }
    
    // Register it
    JSServerRegistry.registerObject(name, child);
    
    // Run initial script if provided
    if (!script.empty()) {
        std::string result = child->eval(script);
        if (result.rfind("!JS_ERROR ", 0) == 0) {
            // Error during init - clean up and report
            Tcl_AppendResult(interp, result.c_str(), NULL);
            JSServerRegistry.unregisterObject(name);
            delete child;
            return TCL_ERROR;
        }
    }
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj(name.c_str(), -1));
    return TCL_OK;
}

/**
 * jssend name script
 * 
 * Send JavaScript code to a subprocess and wait for the result.
 */
static int jssend_command(ClientData data, Tcl_Interp *interp,
                          int objc, Tcl_Obj *const objv[])
{
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "name script");
        return TCL_ERROR;
    }
    
    std::string name = Tcl_GetString(objv[1]);
    std::string script = Tcl_GetString(objv[2]);
    
    // Find the subprocess
    JSServer *jsserver = JSServerRegistry.getObject(name);
    if (!jsserver) {
        Tcl_AppendResult(interp, "jsprocess \"", name.c_str(), 
                        "\" not found", NULL);
        return TCL_ERROR;
    }
    
    // Evaluate and get result
    std::string result = jsserver->eval(script);
    
    // Check for JS error
    if (result.rfind("!JS_ERROR ", 0) == 0) {
        Tcl_AppendResult(interp, result.substr(10).c_str(), NULL);
        return TCL_ERROR;
    }
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj(result.c_str(), -1));
    return TCL_OK;
}

/**
 * jssend_async name script
 * 
 * Send JavaScript code to a subprocess without waiting for result.
 */
static int jssend_async_command(ClientData data, Tcl_Interp *interp,
                                int objc, Tcl_Obj *const objv[])
{
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "name script");
        return TCL_ERROR;
    }
    
    std::string name = Tcl_GetString(objv[1]);
    std::string script = Tcl_GetString(objv[2]);
    
    JSServer *jsserver = JSServerRegistry.getObject(name);
    if (!jsserver) {
        Tcl_AppendResult(interp, "jsprocess \"", name.c_str(),
                        "\" not found", NULL);
        return TCL_ERROR;
    }
    
    // Send without waiting
    jsserver->eval_noreply(script);
    
    return TCL_OK;
}

/**
 * jskill name
 * 
 * Shutdown and remove a JavaScript subprocess.
 */
static int jskill_command(ClientData data, Tcl_Interp *interp,
                          int objc, Tcl_Obj *const objv[])
{
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "name");
        return TCL_ERROR;
    }
    
    std::string name = Tcl_GetString(objv[1]);
    
    JSServer *jsserver = JSServerRegistry.getObject(name);
    if (!jsserver) {
        Tcl_AppendResult(interp, "jsprocess \"", name.c_str(),
                        "\" not found", NULL);
        return TCL_ERROR;
    }
    
    // Shutdown and cleanup
    jsserver->shutdown();
    JSServerRegistry.unregisterObject(name);
    delete jsserver;
    
    return TCL_OK;
}

/**
 * jslist
 * 
 * List all JavaScript subprocesses.
 */
static int jslist_command(ClientData data, Tcl_Interp *interp,
                          int objc, Tcl_Obj *const objv[])
{
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, "");
        return TCL_ERROR;
    }
    
    Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);
    
    auto allObjects = JSServerRegistry.getAllObjects();
    for (const auto& [name, server] : allObjects) {
        Tcl_ListObjAppendElement(interp, listObj, 
                                 Tcl_NewStringObj(name.c_str(), -1));
    }
    
    Tcl_SetObjResult(interp, listObj);
    return TCL_OK;
}

/**
 * jsexists name
 * 
 * Check if a JavaScript subprocess exists.
 */
static int jsexists_command(ClientData data, Tcl_Interp *interp,
                            int objc, Tcl_Obj *const objv[])
{
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "name");
        return TCL_ERROR;
    }
    
    std::string name = Tcl_GetString(objv[1]);
    bool exists = JSServerRegistry.exists(name);
    
    Tcl_SetObjResult(interp, Tcl_NewIntObj(exists ? 1 : 0));
    return TCL_OK;
}

/**
 * Register all JS commands with a Tcl interpreter.
 */
int JSCommands_Init(Tcl_Interp *interp, TclServer *tserv)
{
    Tcl_CreateObjCommand(interp, "jsprocess", jsprocess_command,
                         (ClientData)tserv, NULL);
    
    Tcl_CreateObjCommand(interp, "jssend", jssend_command,
                         (ClientData)tserv, NULL);
    
    // Alias for jssend
    Tcl_CreateObjCommand(interp, "jseval", jssend_command,
                         (ClientData)tserv, NULL);
    
    Tcl_CreateObjCommand(interp, "jssend_async", jssend_async_command,
                         (ClientData)tserv, NULL);
    
    Tcl_CreateObjCommand(interp, "jskill", jskill_command,
                         (ClientData)tserv, NULL);
    
    Tcl_CreateObjCommand(interp, "jslist", jslist_command,
                         (ClientData)tserv, NULL);
    
    Tcl_CreateObjCommand(interp, "jsexists", jsexists_command,
                         (ClientData)tserv, NULL);
    
    return TCL_OK;
}

/**
 * Cleanup all JS subprocesses.
 */
void JSCommands_Shutdown()
{
    auto allObjects = JSServerRegistry.getAllObjects();
    for (auto& [name, server] : allObjects) {
        server->shutdown();
        delete server;
    }
    // Clear registry
    for (const auto& [name, server] : allObjects) {
        JSServerRegistry.unregisterObject(name);
    }
}

/**
 * Get the JS subprocess registry.
 */
ObjectRegistry<JSServer>& GetJSServerRegistry()
{
    return JSServerRegistry;
}
