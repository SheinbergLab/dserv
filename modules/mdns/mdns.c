/*
 * NAME
 *   mdns.c - advertise this dserv over DNS-SD (mDNS / Bonjour / Avahi)
 *
 * DESCRIPTION
 *   Publishes a `_dserv._tcp` service record so a client on the same
 *   link can find running dserv instances with a standard browse
 *   (dns-sd -B, avahi-browse, python-zeroconf, NWBrowser, ...) instead
 *   of a dserv-specific beacon.
 *
 *   dserv never speaks multicast itself. Everything goes through the
 *   OS responder over the dns_sd.h API: mDNSResponder on macOS, and on
 *   Linux the Avahi daemon via its Bonjour compatibility library
 *   (libavahi-compat-libdnssd). That keeps the cost inside dserv to one
 *   IPC socket and a thread that sleeps on it, and leaves the
 *   announce/probe/defend traffic to the daemon that already does it
 *   for the host's `.local` name.
 *
 *   Commands (one registration per interpreter):
 *
 *     mdnsRegister ?-name N? ?-type T? ?-port P? ?-txt {k v ...}? ?-dpoint D?
 *         Register. Defaults: name = NULL (the responder substitutes the
 *         host's own name), type _dserv._tcp, port 2560, no TXT, state
 *         published under mdns/state. Returns the requested name; the
 *         name the responder actually granted (it renames on conflict)
 *         arrives asynchronously in mdnsInfo and the state datapoint.
 *     mdnsUpdate {k v ...}
 *         Replace the TXT record in place; the registration stays up.
 *     mdnsUnregister
 *         Withdraw the record.
 *     mdnsInfo
 *         dict: registered name type port txt state
 *
 *   The state datapoint carries "registered <granted name>",
 *   "error <dns_sd code>" or "unregistered", so a GUI or a test can
 *   see the outcome without polling mdnsInfo.
 *
 *   A missing or stopped responder is not fatal: mdnsRegister raises a
 *   Tcl error with the dns_sd code, the caller logs it, and dserv runs
 *   on without an advertisement -- the registry heartbeat still covers
 *   discovery off-link.
 *
 * AUTHOR
 *   DLS
 *
 * DATE
 *   10/26
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/time.h>
#include <arpa/inet.h>          /* htons: Apple's dns_sd.h pulls it in, Avahi's does not */

#include <dns_sd.h>
#include <tcl.h>

/* Apple's dns_sd.h names the no-daemon case kDNSServiceErr_ServiceNotRunning
   (-65563). Avahi's compat header stops short of that constant and its
   shim reports a missing avahi-daemon as kDNSServiceErr_Unknown, so the
   startup hint keys on both. */
#define MDNS_ERR_SERVICE_NOT_RUNNING (-65563)

#include "Datapoint.h"
#include "tclserver_api.h"

#define MDNS_DEFAULT_TYPE   "_dserv._tcp"
#define MDNS_DEFAULT_PORT   2560
#define MDNS_DEFAULT_DPOINT "mdns"
#define MDNS_NAME_MAX       64      /* DNS-SD instance names are <= 63 bytes */
#define MDNS_TYPE_MAX       64
#define MDNS_DPOINT_MAX     64
#define MDNS_TXT_MAX        1024    /* well above anything we put in it */

typedef struct mdns_info_s {
    tclserver_t *tclserver;

    /* Configuration, as requested */
    char name[MDNS_NAME_MAX];       /* "" -> let the responder pick */
    char type[MDNS_TYPE_MAX];
    int  port;
    char dpoint[MDNS_DPOINT_MAX];

    /* Current TXT record, kept so mdnsInfo can show it and so mdnsUpdate
       can rebuild from a fresh dict without the responder round-trip. */
    unsigned char txt[MDNS_TXT_MAX];
    uint16_t      txt_len;
    Tcl_Obj      *txt_dict;         /* what the caller passed, for mdnsInfo */

    /* Registration */
    DNSServiceRef ref;
    int           registered;       /* a ref is live */
    pthread_t     thread;
    int           thread_running;
    volatile int  stop;

    /* Outcome, written by the responder callback on the worker thread */
    pthread_mutex_t lock;
    char   granted[MDNS_NAME_MAX];  /* name the responder actually registered */
    char   state[128];              /* "pending", "registered ...", "error N" */
} mdns_info_t;

/* ------------------------------------------------------------------------ */
/* state datapoint                                                           */
/* ------------------------------------------------------------------------ */

static void mdns_publish_state(mdns_info_t *info, const char *state)
{
    char name[MDNS_DPOINT_MAX + 8];
    snprintf(name, sizeof(name), "%s/state", info->dpoint);

    pthread_mutex_lock(&info->lock);
    strncpy(info->state, state, sizeof(info->state) - 1);
    info->state[sizeof(info->state) - 1] = '\0';
    pthread_mutex_unlock(&info->lock);

    if (!info->tclserver) return;   /* bare tclsh (tests) -- no bus */
    ds_datapoint_t *dp = dpoint_new(name, tclserver_now(info->tclserver),
                                    DSERV_STRING, (uint32_t) strlen(state),
                                    (unsigned char *) state);
    tclserver_set_point(info->tclserver, dp);
}

/* ------------------------------------------------------------------------ */
/* responder callback + worker thread                                        */
/* ------------------------------------------------------------------------ */

static void DNSSD_API mdns_register_reply(DNSServiceRef sdRef,
                                          DNSServiceFlags flags,
                                          DNSServiceErrorType errorCode,
                                          const char *name,
                                          const char *regtype,
                                          const char *domain,
                                          void *context)
{
    mdns_info_t *info = (mdns_info_t *) context;
    char state[128];

    if (errorCode != kDNSServiceErr_NoError) {
        snprintf(state, sizeof(state), "error %d", (int) errorCode);
        mdns_publish_state(info, state);
        return;
    }

    /* kDNSServiceFlagsAdd: the name is now on the air. Its absence means
       the record was withdrawn (we never ask for that except by
       deallocating, so it is informational). */
    if (flags & kDNSServiceFlagsAdd) {
        pthread_mutex_lock(&info->lock);
        strncpy(info->granted, name ? name : "", sizeof(info->granted) - 1);
        info->granted[sizeof(info->granted) - 1] = '\0';
        pthread_mutex_unlock(&info->lock);
        snprintf(state, sizeof(state), "registered %s", name ? name : "");
        mdns_publish_state(info, state);
    }
}

/* Sleeps on the responder's IPC socket and hands it each reply. select
   with a short timeout, rather than a blocking DNSServiceProcessResult,
   so mdnsUnregister can stop the thread before it tears the ref down
   (deallocating a ref another thread is blocked inside is undefined). */
static void *mdns_thread(void *arg)
{
    mdns_info_t *info = (mdns_info_t *) arg;
    int fd = DNSServiceRefSockFD(info->ref);

    while (!info->stop) {
        fd_set rfds;
        struct timeval tv = { 0, 250000 };
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        int n = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) continue;
        DNSServiceErrorType err = DNSServiceProcessResult(info->ref);
        if (err != kDNSServiceErr_NoError) {
            /* The daemon went away (restart, shutdown). Report and stop;
               a later mdnsRegister starts over against the new daemon. */
            char state[128];
            snprintf(state, sizeof(state), "error %d (responder gone)",
                     (int) err);
            mdns_publish_state(info, state);
            break;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* TXT record                                                                */
/* ------------------------------------------------------------------------ */

static int mdns_build_txt(Tcl_Interp *interp, mdns_info_t *info, Tcl_Obj *dict)
{
    TXTRecordRef txt;
    TXTRecordCreate(&txt, sizeof(info->txt), info->txt);

    if (dict) {
        Tcl_DictSearch search;
        Tcl_Obj *key, *val;
        int done;
        if (Tcl_DictObjFirst(interp, dict, &search, &key, &val, &done)
            != TCL_OK) {
            TXTRecordDeallocate(&txt);
            return TCL_ERROR;
        }
        for (; !done; Tcl_DictObjNext(&search, &key, &val, &done)) {
            Tcl_Size vlen;
            const char *k = Tcl_GetString(key);
            const char *v = Tcl_GetStringFromObj(val, &vlen);
            if (vlen > 255) {
                Tcl_DictObjDone(&search);
                TXTRecordDeallocate(&txt);
                Tcl_SetObjResult(interp, Tcl_ObjPrintf(
                    "mdns: TXT value for \"%s\" exceeds 255 bytes", k));
                return TCL_ERROR;
            }
            DNSServiceErrorType err =
                TXTRecordSetValue(&txt, k, (uint8_t) vlen, v);
            if (err != kDNSServiceErr_NoError) {
                Tcl_DictObjDone(&search);
                TXTRecordDeallocate(&txt);
                Tcl_SetObjResult(interp, Tcl_ObjPrintf(
                    "mdns: TXT record full at \"%s\" (dns_sd error %d)",
                    k, (int) err));
                return TCL_ERROR;
            }
        }
        Tcl_DictObjDone(&search);
    }

    /* TXTRecordCreate was given our own buffer, so the bytes already sit
       in info->txt; only the length needs copying out. */
    info->txt_len = TXTRecordGetLength(&txt);
    TXTRecordDeallocate(&txt);

    if (info->txt_dict) Tcl_DecrRefCount(info->txt_dict);
    info->txt_dict = dict ? dict : Tcl_NewDictObj();
    Tcl_IncrRefCount(info->txt_dict);
    return TCL_OK;
}

/* ------------------------------------------------------------------------ */
/* register / unregister                                                     */
/* ------------------------------------------------------------------------ */

static void mdns_teardown(mdns_info_t *info)
{
    if (info->thread_running) {
        info->stop = 1;
        pthread_join(info->thread, NULL);
        info->thread_running = 0;
    }
    if (info->ref) {
        DNSServiceRefDeallocate(info->ref);
        info->ref = NULL;
    }
    info->registered = 0;
    pthread_mutex_lock(&info->lock);
    info->granted[0] = '\0';
    pthread_mutex_unlock(&info->lock);
}

static int mdns_register_command(ClientData data, Tcl_Interp *interp,
                                 int objc, Tcl_Obj *const objv[])
{
    mdns_info_t *info = (mdns_info_t *) data;
    const char *name = NULL;
    const char *type = MDNS_DEFAULT_TYPE;
    const char *dpoint = MDNS_DEFAULT_DPOINT;
    int port = MDNS_DEFAULT_PORT;
    Tcl_Obj *txt = NULL;

    static const char *opts[] = {
        "-name", "-type", "-port", "-txt", "-dpoint", NULL
    };
    enum { OPT_NAME, OPT_TYPE, OPT_PORT, OPT_TXT, OPT_DPOINT };

    if ((objc - 1) % 2) {
        Tcl_WrongNumArgs(interp, 1, objv,
            "?-name N? ?-type T? ?-port P? ?-txt dict? ?-dpoint D?");
        return TCL_ERROR;
    }
    for (int i = 1; i < objc; i += 2) {
        int idx;
        if (Tcl_GetIndexFromObj(interp, objv[i], opts, "option", 0, &idx)
            != TCL_OK) return TCL_ERROR;
        switch (idx) {
        case OPT_NAME:   name = Tcl_GetString(objv[i + 1]);  break;
        case OPT_TYPE:   type = Tcl_GetString(objv[i + 1]);  break;
        case OPT_DPOINT: dpoint = Tcl_GetString(objv[i + 1]); break;
        case OPT_TXT:    txt = objv[i + 1];                   break;
        case OPT_PORT:
            if (Tcl_GetIntFromObj(interp, objv[i + 1], &port) != TCL_OK)
                return TCL_ERROR;
            if (port <= 0 || port > 65535) {
                Tcl_SetObjResult(interp, Tcl_ObjPrintf(
                    "mdns: port %d out of range", port));
                return TCL_ERROR;
            }
            break;
        }
    }
    if (name && strlen(name) >= MDNS_NAME_MAX) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(
            "mdns: name longer than 63 bytes", -1));
        return TCL_ERROR;
    }
    if (strlen(type) >= MDNS_TYPE_MAX || strlen(dpoint) >= MDNS_DPOINT_MAX) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(
            "mdns: type or dpoint prefix too long", -1));
        return TCL_ERROR;
    }

    /* Re-registering replaces: withdraw first, so a changed port or type
       never leaves two records up. */
    mdns_teardown(info);

    strncpy(info->name, name ? name : "", sizeof(info->name) - 1);
    strncpy(info->type, type, sizeof(info->type) - 1);
    strncpy(info->dpoint, dpoint, sizeof(info->dpoint) - 1);
    info->port = port;
    if (mdns_build_txt(interp, info, txt) != TCL_OK) return TCL_ERROR;

    info->stop = 0;
    DNSServiceErrorType err = DNSServiceRegister(
        &info->ref,
        0,                          /* flags: allow auto-rename on conflict */
        kDNSServiceInterfaceIndexAny,
        name && *name ? name : NULL,
        type,
        NULL,                       /* domain: default (.local) */
        NULL,                       /* host: this machine */
        htons((uint16_t) port),
        info->txt_len, info->txt,
        mdns_register_reply, info);
    if (err != kDNSServiceErr_NoError) {
        info->ref = NULL;
        char state[128];
        snprintf(state, sizeof(state), "error %d", (int) err);
        mdns_publish_state(info, state);
        Tcl_SetObjResult(interp, Tcl_ObjPrintf(
            "mdns: DNSServiceRegister failed (dns_sd error %d)%s", (int) err,
            (err == MDNS_ERR_SERVICE_NOT_RUNNING || err == kDNSServiceErr_Unknown)
                ? " -- is the mDNS responder (avahi-daemon) running?" : ""));
        return TCL_ERROR;
    }
    info->registered = 1;
    mdns_publish_state(info, "pending");

    if (pthread_create(&info->thread, NULL, mdns_thread, info) != 0) {
        mdns_teardown(info);
        Tcl_SetObjResult(interp, Tcl_NewStringObj(
            "mdns: could not start responder thread", -1));
        return TCL_ERROR;
    }
    info->thread_running = 1;

    Tcl_SetObjResult(interp, Tcl_NewStringObj(info->name, -1));
    return TCL_OK;
}

static int mdns_update_command(ClientData data, Tcl_Interp *interp,
                               int objc, Tcl_Obj *const objv[])
{
    mdns_info_t *info = (mdns_info_t *) data;
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "txtdict");
        return TCL_ERROR;
    }
    if (!info->registered) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(
            "mdns: not registered", -1));
        return TCL_ERROR;
    }
    if (mdns_build_txt(interp, info, objv[1]) != TCL_OK) return TCL_ERROR;

    DNSServiceErrorType err = DNSServiceUpdateRecord(
        info->ref, NULL, 0, info->txt_len, info->txt, 0);
    if (err != kDNSServiceErr_NoError) {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf(
            "mdns: DNSServiceUpdateRecord failed (dns_sd error %d)", (int) err));
        return TCL_ERROR;
    }
    return TCL_OK;
}

static int mdns_unregister_command(ClientData data, Tcl_Interp *interp,
                                   int objc, Tcl_Obj *const objv[])
{
    mdns_info_t *info = (mdns_info_t *) data;
    int was = info->registered;
    mdns_teardown(info);
    if (was) mdns_publish_state(info, "unregistered");
    return TCL_OK;
}

static int mdns_info_command(ClientData data, Tcl_Interp *interp,
                             int objc, Tcl_Obj *const objv[])
{
    mdns_info_t *info = (mdns_info_t *) data;
    Tcl_Obj *d = Tcl_NewDictObj();

    pthread_mutex_lock(&info->lock);
    Tcl_Obj *granted = Tcl_NewStringObj(info->granted, -1);
    Tcl_Obj *state = Tcl_NewStringObj(info->state, -1);
    pthread_mutex_unlock(&info->lock);

    Tcl_DictObjPut(interp, d, Tcl_NewStringObj("registered", -1),
                   Tcl_NewBooleanObj(info->registered));
    Tcl_DictObjPut(interp, d, Tcl_NewStringObj("name", -1),
                   Tcl_NewStringObj(info->name, -1));
    Tcl_DictObjPut(interp, d, Tcl_NewStringObj("granted", -1), granted);
    Tcl_DictObjPut(interp, d, Tcl_NewStringObj("type", -1),
                   Tcl_NewStringObj(info->type, -1));
    Tcl_DictObjPut(interp, d, Tcl_NewStringObj("port", -1),
                   Tcl_NewIntObj(info->port));
    Tcl_DictObjPut(interp, d, Tcl_NewStringObj("txt", -1),
                   info->txt_dict ? info->txt_dict : Tcl_NewDictObj());
    Tcl_DictObjPut(interp, d, Tcl_NewStringObj("state", -1), state);
    Tcl_SetObjResult(interp, d);
    return TCL_OK;
}

/* ------------------------------------------------------------------------ */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------ */

static void mdns_cleanup(ClientData data, Tcl_Interp *interp)
{
    mdns_info_t *info = (mdns_info_t *) data;
    mdns_teardown(info);
    if (info->txt_dict) Tcl_DecrRefCount(info->txt_dict);
    pthread_mutex_destroy(&info->lock);
    free(info);
}

#ifdef WIN32
#define EXPORT(a,b) __declspec(dllexport) a b
#else
#define EXPORT(a,b) a b
#endif

#ifdef __cplusplus
extern "C" {
#endif
EXPORT(int,Dserv_mdns_Init) (Tcl_Interp *interp)
#ifdef __cplusplus
}
#endif
{
    if (
#ifdef USE_TCL_STUBS
        Tcl_InitStubs(interp, "9.0-", 0)
#else
        Tcl_PkgRequire(interp, "Tcl", "9.0-", 0)
#endif
        == NULL) {
        return TCL_ERROR;
    }

    /* Avahi's Bonjour shim prints a loud "uses the Apple Bonjour
       compatibility layer" warning to stderr on first use unless told
       not to. Harmless on macOS, where nothing reads it. */
    setenv("AVAHI_COMPAT_NOWARN", "1", 0);

    mdns_info_t *info = (mdns_info_t *) calloc(1, sizeof(mdns_info_t));
    if (!info) return TCL_ERROR;
    info->tclserver = tclserver_get_from_interp(interp);
    pthread_mutex_init(&info->lock, NULL);
    strncpy(info->type, MDNS_DEFAULT_TYPE, sizeof(info->type) - 1);
    strncpy(info->dpoint, MDNS_DEFAULT_DPOINT, sizeof(info->dpoint) - 1);
    strncpy(info->state, "unregistered", sizeof(info->state) - 1);
    info->port = MDNS_DEFAULT_PORT;

    Tcl_CreateObjCommand(interp, "mdnsRegister",
                         (Tcl_ObjCmdProc *) mdns_register_command,
                         (ClientData) info, NULL);
    Tcl_CreateObjCommand(interp, "mdnsUpdate",
                         (Tcl_ObjCmdProc *) mdns_update_command,
                         (ClientData) info, NULL);
    Tcl_CreateObjCommand(interp, "mdnsUnregister",
                         (Tcl_ObjCmdProc *) mdns_unregister_command,
                         (ClientData) info, NULL);
    Tcl_CreateObjCommand(interp, "mdnsInfo",
                         (Tcl_ObjCmdProc *) mdns_info_command,
                         (ClientData) info, NULL);

    Tcl_CallWhenDeleted(interp, mdns_cleanup, (ClientData) info);
    return TCL_OK;
}
