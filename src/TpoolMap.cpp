/*
 * TpoolMap.cpp — parallel Tcl script evaluation for dserv
 *
 * Provides a tpool_map command that spawns lightweight worker threads,
 * each with its own Tcl_Interp.  Workers get only the dlsh/package
 * infrastructure — no dserv registration, no event dispatching, no
 * network listeners.  This avoids side effects on the shared Dataserver.
 *
 * Usage from Tcl:
 *   tpool_map n setup_script work_script ?-threads N? ?-args dict? ?-seed 0|1?
 *
 * The work script runs with these variables pre-set:
 *   $n          - number of work units for this worker
 *   $worker_id  - 0-based worker index
 *   $args_dict  - the value passed via -args (default: "")
 *
 * Each worker's result is expected to be a serialized dg (dg_toString).
 *
 * Returns a Tcl dict:
 *   results   - list of per-worker byte arrays (dg_toString output)
 *   missing   - number of work units that failed
 *   errors    - list of error messages
 *   n_threads - number of threads used
 *
 * ---------------------------------------------------------------
 * Integration — add to add_tcl_commands() in TclServer.cpp:
 *
 *   extern int TpoolMap_Init(Tcl_Interp *interp, TclServer *tserv);
 *   TpoolMap_Init(interp, tserv);
 * ---------------------------------------------------------------
 */

#include "TclServer.h"
#include <vector>
#include <string>
#include <thread>
#include <sstream>
#include <algorithm>
#include <iostream>

#ifndef TPOOL_MAP_MAX_THREADS
#define TPOOL_MAP_MAX_THREADS 64
#endif

/*
 * Per-worker state
 */
struct tpool_worker_t {
    int         argc;
    char      **argv;
    std::string script;       // setup + work combined
    std::string result;       // serialized dg on success
    std::string error;        // non-empty on failure
    int         batch_n;      // work units assigned
    int         worker_id;
};

/*
 * Worker thread function.
 *
 * Creates a bare Tcl_Interp, initializes just enough for dlsh
 * and package loading (via zipfs), evals the combined script,
 * captures the result or error, and cleans up.  No TclServer,
 * no Dataserver interaction.
 */
static void tpool_worker_func(tpool_worker_t *w)
{
    /* Create interpreter */
    Tcl_Interp *interp = Tcl_CreateInterp();
    if (!interp) {
        w->error = "failed to create Tcl interpreter";
        return;
    }

    /* Initialize Tcl core (sets up auto_path etc) */
    if (Tcl_Init(interp) != TCL_OK) {
        w->error = std::string("Tcl_Init failed: ")
                   + Tcl_GetStringResult(interp);
        Tcl_DeleteInterp(interp);
        Tcl_FinalizeThread();
        return;
    }

    /* Bootstrap auto_path for dlsh packages.
     * Note: zipfs is already mounted by the main thread at startup;
     * we just need to set auto_path so the worker can find packages. */
    const char *bootstrap = R"(
        set _base [file join [zipfs root] dlsh]
        set ::auto_path [linsert $::auto_path 0 ${_base}/lib]
    )";
    Tcl_Eval(interp, bootstrap);

    /* Evaluate the combined setup + work script */
    int rc = Tcl_Eval(interp, w->script.c_str());

    if (rc == TCL_OK) {
        /* Get result as byte array to preserve binary dg_toString data.
         * Tcl_GetStringResult would force UTF-8 conversion and corrupt
         * binary content. */
        Tcl_Obj *resultObj = Tcl_GetObjResult(interp);
        Tcl_Size len;
        const unsigned char *bytes = Tcl_GetByteArrayFromObj(resultObj, &len);
        if (bytes && len > 0) {
            w->result.assign((const char *)bytes, len);
        } else {
            /* Fall back to string representation */
            const char *res = Tcl_GetStringResult(interp);
            w->result = res ? res : "";
        }
        w->error.clear();
    } else {
        const char *err = Tcl_GetStringResult(interp);
        w->error = err ? err : "unknown error";
        w->result.clear();
    }

    Tcl_DeleteInterp(interp);

    /* Clean up all Tcl thread-local storage for this thread.
     * Without this, every worker thread leaks TLS allocated by
     * Tcl core and loaded packages (box2d, dlsh, etc.).  */
    Tcl_FinalizeThread();
}

/*
 * Detect number of available CPUs.
 */
static int tpool_detect_cpus()
{
    int n = std::thread::hardware_concurrency();
    return (n > 0) ? n : 4;
}

/*
 * tpool_map Tcl command implementation.
 */
static int tpool_map_command(ClientData data, Tcl_Interp *interp,
                             int objc, Tcl_Obj *objv[])
{
    TclServer *tclserver = (TclServer *)data;

    if (objc < 4) {
        Tcl_WrongNumArgs(interp, 1, objv,
            "n setup_script work_script ?-threads N? ?-args dict? ?-seed 0|1?");
        return TCL_ERROR;
    }

    /* --- parse positional args --- */
    int n;
    if (Tcl_GetIntFromObj(interp, objv[1], &n) != TCL_OK)
        return TCL_ERROR;

    std::string setup_script = Tcl_GetString(objv[2]);
    std::string work_script  = Tcl_GetString(objv[3]);

    /* --- parse options --- */
    int num_threads = std::max(1, tpool_detect_cpus() - 1);
    std::string args_dict;
    int seed_workers = 1;

    for (int i = 4; i < objc; i += 2) {
        if (i + 1 >= objc) {
            Tcl_AppendResult(interp, "option requires a value: ",
                             Tcl_GetString(objv[i]), NULL);
            return TCL_ERROR;
        }
        std::string opt = Tcl_GetString(objv[i]);
        if (opt == "-threads") {
            if (Tcl_GetIntFromObj(interp, objv[i + 1], &num_threads) != TCL_OK)
                return TCL_ERROR;
            if (num_threads < 1) num_threads = 1;
            if (num_threads > TPOOL_MAP_MAX_THREADS)
                num_threads = TPOOL_MAP_MAX_THREADS;
        } else if (opt == "-args") {
            args_dict = Tcl_GetString(objv[i + 1]);
        } else if (opt == "-seed") {
            if (Tcl_GetIntFromObj(interp, objv[i + 1], &seed_workers) != TCL_OK)
                return TCL_ERROR;
        } else {
            Tcl_AppendResult(interp, "unknown option: ", opt.c_str(), NULL);
            return TCL_ERROR;
        }
    }

    /* --- trivial / degenerate cases --- */
    if (n <= 0) {
        Tcl_Obj *dict = Tcl_NewDictObj();
        Tcl_DictObjPut(interp, dict,
            Tcl_NewStringObj("results", -1), Tcl_NewListObj(0, NULL));
        Tcl_DictObjPut(interp, dict,
            Tcl_NewStringObj("missing", -1), Tcl_NewIntObj(0));
        Tcl_DictObjPut(interp, dict,
            Tcl_NewStringObj("errors", -1), Tcl_NewListObj(0, NULL));
        Tcl_DictObjPut(interp, dict,
            Tcl_NewStringObj("n_threads", -1), Tcl_NewIntObj(0));
        Tcl_SetObjResult(interp, dict);
        return TCL_OK;
    }

    if (num_threads > n) num_threads = n;

    /* --- partition work across threads --- */
    int per_thread = n / num_threads;
    int remainder  = n % num_threads;

    std::vector<tpool_worker_t> workers(num_threads);

    for (int i = 0; i < num_threads; i++) {
        workers[i].batch_n   = per_thread + (i < remainder ? 1 : 0);
        workers[i].worker_id = i;
        workers[i].argc      = tclserver->argc;
        workers[i].argv      = tclserver->argv;

        /*
         * Build the combined script for this worker.
         * Variables are set via [set] with list-quoted values,
         * then the setup script runs, then the work script is
         * stored in a variable and eval'd.
         */
        std::ostringstream ss;

        /* Set worker variables */
        ss << "set n "            << workers[i].batch_n << "\n";
        ss << "set worker_id "    << i                  << "\n";
        ss << "set seed_workers " << seed_workers       << "\n";

        /* args_dict — use Tcl list quoting for safety */
        {
            Tcl_Obj *tmp = Tcl_NewStringObj(args_dict.c_str(), -1);
            Tcl_IncrRefCount(tmp);
            Tcl_Obj *listed = Tcl_NewListObj(1, &tmp);
            Tcl_IncrRefCount(listed);
            ss << "set args_dict " << Tcl_GetString(listed) << "\n";
            Tcl_DecrRefCount(listed);
            Tcl_DecrRefCount(tmp);
        }

        /* Setup script */
        ss << setup_script << "\n";

        /* Seed RNG if requested */
        if (seed_workers) {
            ss << "if {![catch {dl_srand 0} _seed]} {\n"
               << "    expr {srand($_seed)}\n"
               << "    unset _seed\n"
               << "}\n";
        }

        /* Work script — stored in variable and eval'd to avoid
         * quoting issues with direct embedding */
        {
            Tcl_Obj *tmp = Tcl_NewStringObj(work_script.c_str(), -1);
            Tcl_IncrRefCount(tmp);
            Tcl_Obj *listed = Tcl_NewListObj(1, &tmp);
            Tcl_IncrRefCount(listed);
            ss << "set _tpool_work " << Tcl_GetString(listed) << "\n";
            Tcl_DecrRefCount(listed);
            Tcl_DecrRefCount(tmp);
        }
        ss << "eval $_tpool_work\n";

        workers[i].script = ss.str();
    }

    /* --- launch worker threads --- */
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(tpool_worker_func, &workers[i]);
    }

    /* --- join all workers (deterministic cleanup) --- */
    for (auto &t : threads) {
        t.join();
    }

    /* --- collect results --- */
    Tcl_Obj *errors_list = Tcl_NewListObj(0, NULL);
    Tcl_Obj *results_list = Tcl_NewListObj(0, NULL);
    int missing = 0;

    for (int i = 0; i < num_threads; i++) {
        if (!workers[i].error.empty()) {
            std::string msg = "worker " + std::to_string(i) + ": "
                              + workers[i].error;
            Tcl_ListObjAppendElement(interp, errors_list,
                Tcl_NewStringObj(msg.c_str(), -1));
            missing += workers[i].batch_n;
            std::cerr << "tpool_map: " << msg << std::endl;
        } else if (workers[i].result.empty()) {
            std::string msg = "worker " + std::to_string(i)
                              + ": empty result";
            Tcl_ListObjAppendElement(interp, errors_list,
                Tcl_NewStringObj(msg.c_str(), -1));
            missing += workers[i].batch_n;
        } else {
            /* Use byte array to preserve binary dg_toString data */
            Tcl_ListObjAppendElement(interp, results_list,
                Tcl_NewByteArrayObj(
                    (const unsigned char *)workers[i].result.c_str(),
                    workers[i].result.size()));
        }
    }

    /* --- build return dict --- */
    Tcl_Obj *dict = Tcl_NewDictObj();
    Tcl_DictObjPut(interp, dict,
        Tcl_NewStringObj("results", -1),
        results_list);
    Tcl_DictObjPut(interp, dict,
        Tcl_NewStringObj("missing", -1),
        Tcl_NewIntObj(missing));
    Tcl_DictObjPut(interp, dict,
        Tcl_NewStringObj("errors", -1),
        errors_list);
    Tcl_DictObjPut(interp, dict,
        Tcl_NewStringObj("n_threads", -1),
        Tcl_NewIntObj(num_threads));

    Tcl_SetObjResult(interp, dict);

    int collected = n - missing;
    std::cout << "tpool_map: " << collected << "/" << n
              << " work units collected (" << num_threads
              << " threads, " << missing << " missing)"
              << std::endl;

    return TCL_OK;
}

/*
 * Register the tpool_map command.
 * Call from add_tcl_commands() in TclServer.cpp.
 */
int TpoolMap_Init(Tcl_Interp *interp, TclServer *tserv)
{
    Tcl_CreateObjCommand(interp, "tpool_map",
                         (Tcl_ObjCmdProc *)tpool_map_command,
                         (ClientData)tserv, NULL);
    return TCL_OK;
}
