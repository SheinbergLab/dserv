#include "ErrorMonitor.h"
#include "TclServer.h"
#include "Dataserver.h"
#include <jansson.h>
#include <cstring>
#include <chrono>
#include <iostream>

ErrorMonitor::ErrorMonitor(TclServer* tserv)
    : tclserver(tserv), 
      interp(tserv->getInterp()),  // Get interpreter from TclServer
      enabled(false) {
}

ErrorMonitor::~ErrorMonitor() {
    // Clean up trace if enabled
    disable();
}

bool ErrorMonitor::enable() {
    if (!interp) {
        std::cerr << "ErrorMonitor::enable: NULL interpreter" << std::endl;
        return false;
    }
    
    // If already enabled, nothing to do
    if (enabled) {
        return true;
    }
    
    // Install variable trace on errorInfo
  //    std::cerr << "ErrorMonitor " << (void*)this << " installing trace on interp " 
    //              << (void*)interp << " for " << tclserver->name << std::endl;
    
    int result = Tcl_TraceVar(interp, "errorInfo",
                             TCL_TRACE_WRITES | TCL_GLOBAL_ONLY,
                             traceCallback,
                             (ClientData)this);
    
    if (result != TCL_OK) {
        std::cerr << "ErrorMonitor::enable: Tcl_TraceVar failed" << std::endl;
        return false;
    }
    
    enabled = true;
    
    //    std::cerr << "ErrorMonitor enabled for: " << tclserver->name << std::endl;
    return true;
}

bool ErrorMonitor::disable() {
    if (!enabled || !interp) {
        return false;
    }
    
  //    std::cerr << "ErrorMonitor " << (void*)this << " removing trace from interp " 
    //              << (void*)interp << " for " << tclserver->name << std::endl;
    
    // Remove the variable trace
    Tcl_UntraceVar(interp, "errorInfo",
                  TCL_TRACE_WRITES | TCL_GLOBAL_ONLY,
                  traceCallback,
                  (ClientData)this);
    
    enabled = false;
    return true;
}

// Variable trace callback - called when errorInfo is written
// Variable trace callback - called when errorInfo is written
char* ErrorMonitor::traceCallback(ClientData clientData, Tcl_Interp* interp,
                                 const char* name1, const char* name2, int flags) {
    ErrorMonitor* monitor = static_cast<ErrorMonitor*>(clientData);
    
    // Only handle writes
    if (!(flags & TCL_TRACE_WRITES)) {
        return NULL;
    }
    
    // Handle the error - if trace fired, process it!
    monitor->handleError();
    
    return NULL;
}

void ErrorMonitor::handleError() {
    // Get error information from Tcl variables
    const char* errorInfo = Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY);
    const char* errorCode = Tcl_GetVar(interp, "errorCode", TCL_GLOBAL_ONLY);
    
    if (!errorInfo) errorInfo = "";
    if (!errorCode) errorCode = "";
    
    // Skip if errorInfo is empty or just whitespace
    std::string errStr(errorInfo);
    size_t start = errStr.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) {
      //        std::cerr << "  -> Empty errorInfo, skipping" << std::endl;
        return;
    }
    
    // Skip if errorCode is "NONE" (default/no error)
    if (strcmp(errorCode, "NONE") == 0) {
      //        std::cerr << "  -> errorCode is NONE, skipping" << std::endl;
        return;
    }
    
    //    std::cerr << "  -> Processing error for " << tclserver->name << std::endl;
    
    // Create JSON object using jansson
    json_t* root = json_object();
    
    json_object_set_new(root, "interpreter", json_string(tclserver->name.c_str()));
    
    // Timestamp in seconds
    int64_t timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    json_object_set_new(root, "timestamp", json_integer(timestamp));
    
    // Timestamp in milliseconds
    int64_t timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    json_object_set_new(root, "time_ms", json_integer(timestamp_ms));
    
    json_object_set_new(root, "errorInfo", json_string(errorInfo));
    json_object_set_new(root, "errorCode", json_string(errorCode));
    
    // Convert JSON to string
    char* json_str = json_dumps(root, JSON_COMPACT);
    
    if (json_str) {
        // Push to datapoint using proper dpoint_set pattern
        std::string dp_name = "error/" + tclserver->name;
        
        ds_datapoint_t dpoint;
        dpoint_set(&dpoint, 
                   (char*)dp_name.c_str(),
                   tclserver->ds->now(),
                   DSERV_STRING,
                   strlen(json_str),
                   (unsigned char*)json_str);
        
        tclserver->ds->set(dpoint);
        
	//        std::cerr << "  -> Error pushed to " << dp_name << std::endl;
        
        // Free JSON string
        free(json_str);
    }
    
    // Cleanup JSON object
    json_decref(root);
}

// Tcl command: errormon enable|disable|status
int ErrorMonitor::errormonCommandProc(ClientData clientData, Tcl_Interp* interp,
                                     int objc, Tcl_Obj* const objv[]) {
    ErrorMonitor* monitor = static_cast<ErrorMonitor*>(clientData);
    
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "enable|disable|status");
        return TCL_ERROR;
    }
    
    const char* subcmd = Tcl_GetString(objv[1]);
    
    if (strcmp(subcmd, "enable") == 0) {
      bool success = monitor->enable();
      Tcl_SetObjResult(interp, Tcl_NewBooleanObj(success));
      return TCL_OK;
      
    } else if (strcmp(subcmd, "disable") == 0) {
      bool success = monitor->disable();
      Tcl_SetObjResult(interp, Tcl_NewBooleanObj(success));
      return TCL_OK;
    } else if (strcmp(subcmd, "status") == 0) {
      Tcl_SetObjResult(interp, Tcl_NewBooleanObj(monitor->isEnabled()));
      return TCL_OK;
    } else {
      Tcl_SetObjResult(interp, Tcl_NewStringObj("Unknown subcommand. Use: enable, disable, or status", -1));
      return TCL_ERROR;
    }
}

void ErrorMonitor::registerCommand(Tcl_Interp* interp, ErrorMonitor* monitor) {
    Tcl_CreateObjCommand(interp, "errormon",
                        errormonCommandProc,
                        (ClientData)monitor,
                        NULL);
}
