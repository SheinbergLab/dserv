/**
 * JSServer.h - QuickJS-NG based subprocess for dserv
 * 
 * Mirrors TclServer architecture but with embedded QuickJS interpreter.
 * Can coexist with TclServer - both are just different scripting frontends
 * to the same dserv datapoint infrastructure.
 * 
 * Uses QuickJS-NG: https://github.com/quickjs-ng/quickjs
 */

#ifndef JSSERVER_H
#define JSSERVER_H

#include <string>
#include <thread>
#include <atomic>
#include <map>
#include <vector>

// Use existing dserv infrastructure (match TclServer.h include order)
#include "Datapoint.h"
#include "Dataserver.h"
#include "ClientRequest.h"

// QuickJS-NG header
#include "quickjs.h"

// Callback info for dpoint handlers
struct JSCallbackInfo {
    JSValue func;           // The JS callback function
    std::string pattern;    // Original pattern for matching
    bool is_glob;           // true if pattern contains wildcards
};

/**
 * JSServer - A QuickJS interpreter subprocess
 * 
 * Like TclServer, but runs JavaScript instead of Tcl.
 * Same queue-based architecture, same dpoint integration.
 */
class JSServer {
public:
    // Constructors
    JSServer(Dataserver *dserv, const std::string& name);
    ~JSServer();
    
    // Evaluation interface
    std::string eval(const std::string& script);
    void eval_noreply(const std::string& script);
    
    // Lifecycle
    void shutdown();
    bool isDone() const { return m_bDone; }
    
    // Subprocess linking (same as TclServer)
    void set_linked(bool linked) { m_linked = linked; }
    bool is_linked() const { return m_linked; }
    
    // Public for C callback access
    Dataserver *ds;
    std::string name;
    std::string client_name;
    SharedQueue<client_request_t> queue;
    
    // Dpoint callback storage (public for C callback access)
    std::vector<JSCallbackInfo> dpoint_callbacks;
    
private:
    std::atomic<bool> m_bDone{false};
    std::atomic<bool> m_linked{false};
    
    JSRuntime *rt = nullptr;
    JSContext *ctx = nullptr;
    
    std::thread process_thread;
    
    // Internal methods
    void process_requests();
    JSContext* setup_js();
    void register_js_functions(JSContext *ctx);
    
    // Execute callbacks for a dpoint
    void dispatch_dpoint_callbacks(ds_datapoint_t *dpoint);
    
    // Convert dpoint data to JSValue
    JSValue dpoint_to_jsvalue(ds_datapoint_t *dpoint);
    
    // Pattern matching helper
    bool pattern_matches(const std::string& pattern, const std::string& name, bool is_glob);
    
    // Cleanup callbacks on shutdown
    void cleanup_callbacks();
};

#endif // JSSERVER_H
