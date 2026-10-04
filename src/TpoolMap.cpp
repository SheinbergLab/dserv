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
 * tpool_queue — the same workers, PULLING work instead of being dealt it
 *
 *   tpool_queue tasks setup_script work_script ?-threads N? ?-want K?
 *               ?-args dict? ?-seed 0|1?
 *
 * tpool_map divides n units evenly and once, so when units differ widely
 * in cost most workers finish early and idle while a few grind through
 * the hard ones. tpool_queue keeps a shared counter over the list
 * `tasks`: each worker sets up once, then loops -- take the next task,
 * run the work script on it, hand back the result, take the next -- until
 * the tasks run out or K results have come in (-want; 0 = no quota).
 * Every worker is busy for the whole call.
 *
 * The work script runs once per task with these variables set:
 *   $task        - the task (one element of `tasks`, any Tcl value)
 *   $task_index  - its 0-based index in `tasks`
 *   $worker_id   - 0-based worker index
 *   $args_dict   - the value passed via -args (default: "")
 * An EMPTY result means "nothing made for this task" and is not an error
 * (a generator whose draw failed returns ""); with -want, only non-empty
 * results count toward the quota. A task that raises an error is recorded
 * and the worker carries on.
 *
 * Returns a Tcl dict:
 *   results   - list of {task_index result} pairs, in completion order
 *               (result is a byte array, as for tpool_map)
 *   made      - number of non-empty results
 *   tried     - number of tasks started
 *   errors    - list of error messages ("task I: ..." or "worker W: ...")
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
#include "TclInterpInit.h"
#include <vector>
#include <string>
#include <thread>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <atomic>
#include <mutex>

#ifndef TPOOL_MAP_MAX_THREADS
#define TPOOL_MAP_MAX_THREADS 64
#endif

/*
 * Per-worker state
 */
struct tpool_worker_t {
    int         argc;
    char      **argv;
    std::string prelude;      // worker variables + setup script (package loads)
    std::string work;         // the work script -- the parallel payload
    std::string result;       // serialized dg on success
    std::string error;        // non-empty on failure
    int         batch_n;      // work units assigned
    int         worker_id;
};

/*
 * Build a worker's interpreter and run its prelude (worker variables +
 * the caller's setup script). Returns NULL, with `error` set and the
 * thread's Tcl state finalized, if anything fails.
 *
 * Everything that can allocate a Tcl global mutex for the first time runs
 * under the lock: interpreter construction AND the setup script, because
 * `package require` is where a package's own Tcl_Mutexes get created and
 * every worker would otherwise reach the same one simultaneously. See
 * TclInterpInit.h for why Tcl cannot be trusted with that first touch.
 *
 * Serialising the setup costs one package-load per worker in sequence,
 * once per call. The work script -- the reason the pool exists -- runs
 * after the lock is dropped, fully in parallel.
 */
static Tcl_Interp *tpool_worker_interp(const std::string &prelude,
                                       std::string &error)
{
    std::lock_guard<std::mutex> tcl_init_guard(tcl_interp_init_lock());

    /* Create interpreter */
    Tcl_Interp *interp = Tcl_CreateInterp();
    if (!interp) {
        error = "failed to create Tcl interpreter";
        return NULL;
    }

    /* Initialize Tcl core (sets up auto_path etc) */
    if (Tcl_Init(interp) != TCL_OK) {
        error = std::string("Tcl_Init failed: ")
                + Tcl_GetStringResult(interp);
        Tcl_DeleteInterp(interp);
        Tcl_FinalizeThread();
        return NULL;
    }

    /* Bootstrap auto_path for dlsh packages.
     * Note: zipfs is already mounted by the main thread at startup;
     * we just need to set auto_path so the worker can find packages. */
    const char *bootstrap = R"(
        set _base [file join [zipfs root] dlsh]
        set ::auto_path [linsert $::auto_path 0 ${_base}/lib]
    )";
    Tcl_Eval(interp, bootstrap);

    /* Worker variables + setup script, still serialised */
    if (Tcl_Eval(interp, prelude.c_str()) != TCL_OK) {
        const char *err = Tcl_GetStringResult(interp);
        error = err ? err : "unknown error";
        Tcl_DeleteInterp(interp);
        Tcl_FinalizeThread();
        return NULL;
    }
    return interp;
}

/*
 * A script's result as raw bytes: a byte array where it is one (binary
 * dg_toString data, which Tcl_GetStringResult would corrupt by forcing
 * UTF-8), the string representation otherwise.
 */
static std::string tpool_result_bytes(Tcl_Interp *interp)
{
    Tcl_Obj *resultObj = Tcl_GetObjResult(interp);
    Tcl_Size len;
    const unsigned char *bytes = Tcl_GetByteArrayFromObj(resultObj, &len);
    if (bytes && len > 0)
        return std::string((const char *)bytes, len);
    const char *res = Tcl_GetStringResult(interp);
    return res ? res : "";
}

/* `value` as one Tcl list element, safe to embed after "set name ". */
static std::string tpool_quote(const std::string &value)
{
    Tcl_Obj *tmp = Tcl_NewStringObj(value.c_str(), (Tcl_Size)value.size());
    Tcl_IncrRefCount(tmp);
    Tcl_Obj *listed = Tcl_NewListObj(1, &tmp);
    Tcl_IncrRefCount(listed);
    std::string out = Tcl_GetString(listed);
    Tcl_DecrRefCount(listed);
    Tcl_DecrRefCount(tmp);
    return out;
}

/*
 * Worker thread function.
 *
 * Creates a bare Tcl_Interp, initializes just enough for dlsh
 * and package loading (via zipfs), evals the setup and then the work
 * script, captures the result or error, and cleans up.  No TclServer,
 * no Dataserver interaction.
 */
static void tpool_worker_func(tpool_worker_t *w)
{
    Tcl_Interp *interp = tpool_worker_interp(w->prelude, w->error);
    if (!interp) {
        w->result.clear();
        return;
    }

    /* The parallel payload, with no lock held */
    int rc = Tcl_Eval(interp, w->work.c_str());

    if (rc == TCL_OK) {
        w->result = tpool_result_bytes(interp);
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
         * Build this worker's two scripts.
         *
         * The prelude -- variables plus the setup script -- runs while the
         * worker still holds the interpreter-construction lock, because that
         * is where `package require` first touches a package's Tcl globals.
         * The work script is kept separate so it can run unlocked, in
         * parallel, which is the whole point of the pool.
         *
         * Variables are set via [set] with list-quoted values, then the setup
         * script runs, then the work script is stored in a variable and eval'd.
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

        workers[i].prelude = ss.str();

        /* Work script — stored in variable and eval'd to avoid
         * quoting issues with direct embedding */
        std::ostringstream ws;
        {
            Tcl_Obj *tmp = Tcl_NewStringObj(work_script.c_str(), -1);
            Tcl_IncrRefCount(tmp);
            Tcl_Obj *listed = Tcl_NewListObj(1, &tmp);
            Tcl_IncrRefCount(listed);
            ws << "set _tpool_work " << Tcl_GetString(listed) << "\n";
            Tcl_DecrRefCount(listed);
            Tcl_DecrRefCount(tmp);
        }
        ws << "eval $_tpool_work\n";

        workers[i].work = ws.str();
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
 * Shared state of one tpool_queue call.
 */
struct tpool_queue_t {
    std::vector<std::string> tasks;
    std::string work;                 // the work script, run once per task
    int         want = 0;             // stop after this many non-empty results (0 = no quota)

    std::atomic<size_t> next{0};      // index of the next task to hand out
    std::atomic<int>    made{0};      // non-empty results so far
    std::atomic<bool>   stop{false};

    std::mutex mutex;                 // guards results and errors
    std::vector<std::pair<size_t, std::string>> results;
    std::vector<std::string> errors;
};

struct tpool_queue_worker_t {
    tpool_queue_t *q;
    std::string    prelude;
    int            worker_id;
};

/*
 * Queue worker: set up once, then pull tasks until there are none left or
 * the quota is met.
 */
static void tpool_queue_worker_func(tpool_queue_worker_t *w)
{
    tpool_queue_t *q = w->q;
    std::string error;
    Tcl_Interp *interp = tpool_worker_interp(w->prelude, error);
    if (!interp) {
        std::lock_guard<std::mutex> g(q->mutex);
        q->errors.push_back("worker " + std::to_string(w->worker_id) + ": " + error);
        return;
    }

    /* compiled once, evaluated per task */
    Tcl_Obj *work = Tcl_NewStringObj(q->work.c_str(), (Tcl_Size)q->work.size());
    Tcl_IncrRefCount(work);

    while (!q->stop.load()) {
        size_t i = q->next.fetch_add(1);
        if (i >= q->tasks.size()) break;

        const std::string &task = q->tasks[i];
        Tcl_SetVar2Ex(interp, "task", NULL,
                      Tcl_NewStringObj(task.c_str(), (Tcl_Size)task.size()),
                      TCL_GLOBAL_ONLY);
        Tcl_SetVar2Ex(interp, "task_index", NULL,
                      Tcl_NewWideIntObj((Tcl_WideInt)i), TCL_GLOBAL_ONLY);

        int rc = Tcl_EvalObjEx(interp, work, TCL_EVAL_GLOBAL);
        /* a work script that ends in `return $x` is the usual way to hand
         * back a value (as tpool_map's callers do) */
        if (rc == TCL_RETURN) rc = TCL_OK;
        if (rc == TCL_OK) {
            std::string res = tpool_result_bytes(interp);
            if (!res.empty()) {
                std::lock_guard<std::mutex> g(q->mutex);
                /* a result that lands after the quota was met is dropped,
                 * so the caller gets exactly `want` */
                if (q->want > 0 && q->made.load() >= q->want) {
                    q->stop.store(true);
                } else {
                    q->results.emplace_back(i, std::move(res));
                    if (q->made.fetch_add(1) + 1 >= q->want && q->want > 0)
                        q->stop.store(true);
                }
            }
        } else {
            const char *err = Tcl_GetStringResult(interp);
            std::lock_guard<std::mutex> g(q->mutex);
            if (q->errors.size() < 64)
                q->errors.push_back("task " + std::to_string(i) + ": "
                                    + (err ? err : "unknown error"));
        }
        Tcl_ResetResult(interp);
    }

    Tcl_DecrRefCount(work);
    Tcl_DeleteInterp(interp);
    Tcl_FinalizeThread();
}

/*
 * tpool_queue Tcl command implementation.
 */
static int tpool_queue_command(ClientData data, Tcl_Interp *interp,
                               int objc, Tcl_Obj *objv[])
{
    if (objc < 4) {
        Tcl_WrongNumArgs(interp, 1, objv,
            "tasks setup_script work_script ?-threads N? ?-want K? ?-args dict? ?-seed 0|1?");
        return TCL_ERROR;
    }

    Tcl_Size ntasks;
    Tcl_Obj **taskv;
    if (Tcl_ListObjGetElements(interp, objv[1], &ntasks, &taskv) != TCL_OK)
        return TCL_ERROR;

    std::string setup_script = Tcl_GetString(objv[2]);

    tpool_queue_t q;
    q.work = Tcl_GetString(objv[3]);
    q.tasks.reserve(ntasks);
    for (Tcl_Size i = 0; i < ntasks; i++) {
        Tcl_Size len;
        const char *str = Tcl_GetStringFromObj(taskv[i], &len);
        q.tasks.emplace_back(str, len);
    }

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
        } else if (opt == "-want") {
            if (Tcl_GetIntFromObj(interp, objv[i + 1], &q.want) != TCL_OK)
                return TCL_ERROR;
            if (q.want < 0) q.want = 0;
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

    if ((Tcl_Size)num_threads > ntasks) num_threads = (int)ntasks;

    std::vector<tpool_queue_worker_t> workers(num_threads);
    for (int i = 0; i < num_threads; i++) {
        std::ostringstream ss;
        ss << "set worker_id "    << i            << "\n";
        ss << "set seed_workers " << seed_workers << "\n";
        ss << "set args_dict "    << tpool_quote(args_dict) << "\n";
        ss << setup_script << "\n";
        if (seed_workers) {
            ss << "if {![catch {dl_srand 0} _seed]} {\n"
               << "    expr {srand($_seed)}\n"
               << "    unset _seed\n"
               << "}\n";
        }
        workers[i].q         = &q;
        workers[i].prelude   = ss.str();
        workers[i].worker_id = i;
    }

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; i++)
        threads.emplace_back(tpool_queue_worker_func, &workers[i]);
    for (auto &t : threads)
        t.join();

    /* --- build return dict (all workers joined: no locking needed) --- */
    Tcl_Obj *results_list = Tcl_NewListObj(0, NULL);
    for (auto &r : q.results) {
        Tcl_Obj *pair[2];
        pair[0] = Tcl_NewWideIntObj((Tcl_WideInt)r.first);
        pair[1] = Tcl_NewByteArrayObj((const unsigned char *)r.second.c_str(),
                                      (Tcl_Size)r.second.size());
        Tcl_ListObjAppendElement(interp, results_list, Tcl_NewListObj(2, pair));
    }
    Tcl_Obj *errors_list = Tcl_NewListObj(0, NULL);
    for (auto &e : q.errors) {
        Tcl_ListObjAppendElement(interp, errors_list,
                                 Tcl_NewStringObj(e.c_str(), -1));
        std::cerr << "tpool_queue: " << e << std::endl;
    }
    size_t tried = std::min(q.next.load(), q.tasks.size());

    Tcl_Obj *dict = Tcl_NewDictObj();
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("results", -1), results_list);
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("made", -1),
                   Tcl_NewIntObj((int)q.results.size()));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("tried", -1),
                   Tcl_NewWideIntObj((Tcl_WideInt)tried));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("errors", -1), errors_list);
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("n_threads", -1),
                   Tcl_NewIntObj(num_threads));
    Tcl_SetObjResult(interp, dict);

    std::cout << "tpool_queue: " << q.results.size() << " made from "
              << tried << "/" << q.tasks.size() << " tasks ("
              << num_threads << " threads, " << q.errors.size()
              << " errors)" << std::endl;

    return TCL_OK;
}

/*
 * Register the tpool_map and tpool_queue commands.
 * Call from add_tcl_commands() in TclServer.cpp.
 */
int TpoolMap_Init(Tcl_Interp *interp, TclServer *tserv)
{
    Tcl_CreateObjCommand(interp, "tpool_map",
                         (Tcl_ObjCmdProc *)tpool_map_command,
                         (ClientData)tserv, NULL);
    Tcl_CreateObjCommand(interp, "tpool_queue",
                         (Tcl_ObjCmdProc *)tpool_queue_command,
                         (ClientData)tserv, NULL);
    return TCL_OK;
}
