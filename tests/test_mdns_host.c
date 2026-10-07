/*
 * test_mdns_host.c -- a bare Tcl interp that can `load` a dserv module.
 *
 * dserv modules resolve tclserver_* and dpoint_new from the executable
 * that loads them (dserv exports them; the modules are linked with
 * undefined symbols allowed). A plain tclsh has none of those, so
 * `load dserv_mdns.dylib` would fail at bind time. This host exports
 * inert stand-ins -- the module sees a NULL tclserver and publishes
 * nothing -- and then runs the Tcl test script given on the command line.
 *
 *   test_mdns_host tests/test_mdns.tcl build/modules/dserv_mdns.so
 *
 * argv[2..] land in ::argv for the script.
 */

#include <stdio.h>
#include <stdint.h>
#include <tcl.h>

#include "Datapoint.h"
#include "tclserver_api.h"

tclserver_t *tclserver_get_from_interp(Tcl_Interp *interp) { (void) interp; return NULL; }
uint64_t tclserver_now(tclserver_t *t) { (void) t; return 0; }
int64_t tclserver_clock_epoch_offset_us(void) { return 0; }
void tclserver_set_point(tclserver_t *t, ds_datapoint_t *dp) { (void) t; (void) dp; }
void tclserver_queue_script(tclserver_t *t, const char *s, int n) { (void) t; (void) s; (void) n; }
ds_datapoint_t *dpoint_new(char *name, uint64_t ts, ds_datatype_t type,
                           uint32_t len, unsigned char *data)
{
    (void) name; (void) ts; (void) type; (void) len; (void) data;
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s script.tcl [args...]\n", argv[0]);
        return 2;
    }
    Tcl_FindExecutable(argv[0]);
    Tcl_Interp *interp = Tcl_CreateInterp();

    /* init.tcl is nice to have (clock, etc.) but the test does not need
       it, and a build host may not have the Tcl library installed. */
    Tcl_Init(interp);
    Tcl_ResetResult(interp);

    Tcl_Obj *args = Tcl_NewListObj(0, NULL);
    for (int i = 2; i < argc; i++)
        Tcl_ListObjAppendElement(interp, args, Tcl_NewStringObj(argv[i], -1));
    Tcl_SetVar2Ex(interp, "argv", NULL, args, TCL_GLOBAL_ONLY);
    Tcl_SetVar2Ex(interp, "argc", NULL, Tcl_NewIntObj(argc - 2), TCL_GLOBAL_ONLY);

    int rc = Tcl_EvalFile(interp, argv[1]);
    if (rc != TCL_OK) {
        Tcl_Obj *opts = Tcl_GetReturnOptions(interp, rc);
        Tcl_Obj *key = Tcl_NewStringObj("-errorinfo", -1);
        Tcl_Obj *info = NULL;
        Tcl_IncrRefCount(key);
        Tcl_DictObjGet(NULL, opts, key, &info);
        fprintf(stderr, "FAIL: %s\n", info ? Tcl_GetString(info)
                                           : Tcl_GetStringResult(interp));
        Tcl_DecrRefCount(key);
        Tcl_DecrRefCount(opts);
    }
    Tcl_DeleteInterp(interp);
    Tcl_Finalize();
    return rc == TCL_OK ? 0 : 1;
}
