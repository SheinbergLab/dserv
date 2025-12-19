/*
 * NAME
 *   mesh.c - UDP mesh broadcast module
 *
 * DESCRIPTION
 *   Broadcasts heartbeat packets for mesh discovery.
 *   Discovery and aggregation handled by dserv-agent.
 *   Timing controlled externally via timer module.
 *
 * AUTHOR
 *   DLS, 12/24
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>

#include <tcl.h>
#include <jansson.h>

#include "Datapoint.h"
#include "tclserver_api.h"

#define MAX_CUSTOM_FIELDS 20
#define MAX_FIELD_KEY_LEN 64
#define MAX_FIELD_VAL_LEN 256
#define MAX_BROADCAST_ADDRS 8
#define NETWORK_SCAN_INTERVAL_SEC 30

typedef struct custom_field_s {
    char key[MAX_FIELD_KEY_LEN];
    char value[MAX_FIELD_VAL_LEN];
} custom_field_t;

typedef struct mesh_info_s {
    tclserver_t *tclserver;
    
    /* Identity */
    char appliance_id[128];
    char appliance_name[128];
    char status[64];
    int web_port;
    int ssl_enabled;
    int discovery_port;
    
    /* Custom fields */
    custom_field_t fields[MAX_CUSTOM_FIELDS];
    int num_fields;
    
    /* Network */
    int udp_socket;
    char broadcast_addrs[MAX_BROADCAST_ADDRS][16];
    int num_broadcast_addrs;
    time_t last_network_scan;
} mesh_info_t;

/*
 * Scan network interfaces for broadcast addresses
 */
static void mesh_scan_broadcast_addrs(mesh_info_t *info)
{
    struct ifaddrs *ifap, *ifa;
    
    info->num_broadcast_addrs = 0;
    
    if (getifaddrs(&ifap) != 0) {
        /* Fallback to global broadcast */
        strncpy(info->broadcast_addrs[0], "255.255.255.255", 16);
        info->num_broadcast_addrs = 1;
        return;
    }
    
    for (ifa = ifap; ifa != NULL && info->num_broadcast_addrs < MAX_BROADCAST_ADDRS; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_BROADCAST)) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        
        struct sockaddr_in *bcast = NULL;
#ifdef __APPLE__
        bcast = (struct sockaddr_in *)ifa->ifa_broadaddr;
#else
        bcast = (struct sockaddr_in *)ifa->ifa_ifu.ifu_broadaddr;
#endif
        if (!bcast) continue;
        
        char *addr_str = inet_ntoa(bcast->sin_addr);
        
        /* Skip invalid and link-local */
        if (strcmp(addr_str, "0.0.0.0") == 0) continue;
        if (strncmp(addr_str, "169.254.", 8) == 0) continue;
        
        /* Check for duplicates */
        int dup = 0;
        for (int i = 0; i < info->num_broadcast_addrs; i++) {
            if (strcmp(info->broadcast_addrs[i], addr_str) == 0) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            strncpy(info->broadcast_addrs[info->num_broadcast_addrs], addr_str, 16);
            info->num_broadcast_addrs++;
        }
    }
    
    freeifaddrs(ifap);
    
    /* Fallback */
    if (info->num_broadcast_addrs == 0) {
        strncpy(info->broadcast_addrs[0], "255.255.255.255", 16);
        info->num_broadcast_addrs = 1;
    }
    
    info->last_network_scan = time(NULL);
}

/*
 * Refresh broadcast addresses if needed
 */
static void mesh_refresh_broadcast_addrs(mesh_info_t *info)
{
    time_t now = time(NULL);
    if (info->num_broadcast_addrs == 0 || 
        (now - info->last_network_scan) > NETWORK_SCAN_INTERVAL_SEC) {
        mesh_scan_broadcast_addrs(info);
    }
}

/*
 * Initialize UDP socket
 */
static int mesh_setup_udp(mesh_info_t *info)
{
    info->udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (info->udp_socket < 0) {
        fprintf(stderr, "Mesh: socket creation failed: %s\n", strerror(errno));
        return -1;
    }
    
    int broadcast = 1;
    if (setsockopt(info->udp_socket, SOL_SOCKET, SO_BROADCAST, 
                   &broadcast, sizeof(broadcast)) < 0) {
        fprintf(stderr, "Mesh: failed to enable broadcast: %s\n", strerror(errno));
        close(info->udp_socket);
        info->udp_socket = -1;
        return -1;
    }
    
    /* Initial network scan */
    mesh_scan_broadcast_addrs(info);
    
    printf("Mesh: UDP socket ready, broadcasting to %d networks\n", 
           info->num_broadcast_addrs);
    
    return 0;
}

/*
 * Send a single heartbeat
 */
static void mesh_send_heartbeat(mesh_info_t *info)
{
    if (info->udp_socket < 0) return;
    
    mesh_refresh_broadcast_addrs(info);
    
    /* Build JSON */
    json_t *heartbeat = json_object();
    json_object_set_new(heartbeat, "type", json_string("heartbeat"));
    json_object_set_new(heartbeat, "applianceId", json_string(info->appliance_id));
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long timestamp_ms = (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    json_object_set_new(heartbeat, "timestamp", json_integer(timestamp_ms));
    
    json_t *data = json_object();
    json_object_set_new(data, "name", json_string(info->appliance_name));
    json_object_set_new(data, "status", json_string(info->status));
    json_object_set_new(data, "webPort", json_integer(info->web_port));
    json_object_set_new(data, "ssl", json_boolean(info->ssl_enabled));
    
    /* Add custom fields */
    for (int i = 0; i < info->num_fields; i++) {
        json_object_set_new(data, info->fields[i].key, 
                           json_string(info->fields[i].value));
    }
    
    json_object_set_new(heartbeat, "data", data);
    
    char *message = json_dumps(heartbeat, JSON_COMPACT);
    size_t msg_len = strlen(message);
    
    /* Send to all broadcast addresses */
    for (int i = 0; i < info->num_broadcast_addrs; i++) {
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(info->discovery_port);
        inet_aton(info->broadcast_addrs[i], &addr.sin_addr);
        
        sendto(info->udp_socket, message, msg_len, 0,
               (struct sockaddr *)&addr, sizeof(addr));
    }
    
    free(message);
    json_decref(heartbeat);
}

/*
 * Tcl Commands
 */

static int mesh_init_command(ClientData data, Tcl_Interp *interp,
                             int objc, Tcl_Obj *objv[])
{
    mesh_info_t *info = (mesh_info_t *)data;
    
    /* Parse options: -id, -name, -port, -webport, -ssl */
    for (int i = 1; i < objc; i += 2) {
        if (i + 1 >= objc) {
            Tcl_WrongNumArgs(interp, 1, objv, "?-id id? ?-name name? ?-port port? ?-webport port? ?-ssl bool?");
            return TCL_ERROR;
        }
        
        const char *opt = Tcl_GetString(objv[i]);
        const char *val = Tcl_GetString(objv[i + 1]);
        
        if (strcmp(opt, "-id") == 0) {
            strncpy(info->appliance_id, val, sizeof(info->appliance_id) - 1);
        } else if (strcmp(opt, "-name") == 0) {
            strncpy(info->appliance_name, val, sizeof(info->appliance_name) - 1);
        } else if (strcmp(opt, "-port") == 0) {
            info->discovery_port = atoi(val);
        } else if (strcmp(opt, "-webport") == 0) {
            info->web_port = atoi(val);
        } else if (strcmp(opt, "-ssl") == 0) {
            info->ssl_enabled = (strcmp(val, "1") == 0 || strcmp(val, "true") == 0);
        }
    }
    
    /* Default appliance_id to hostname if not set */
    if (info->appliance_id[0] == '\0') {
        gethostname(info->appliance_id, sizeof(info->appliance_id) - 1);
    }
    
    /* Default name */
    if (info->appliance_name[0] == '\0') {
        snprintf(info->appliance_name, sizeof(info->appliance_name),
                 "Lab Station %s", info->appliance_id);
    }
    
    /* Setup UDP */
    if (mesh_setup_udp(info) < 0) {
        Tcl_SetResult(interp, "failed to initialize UDP socket", TCL_STATIC);
        return TCL_ERROR;
    }
    
    printf("Mesh broadcaster initialized:\n");
    printf("  Appliance ID: %s\n", info->appliance_id);
    printf("  Name: %s\n", info->appliance_name);
    printf("  Discovery port: %d\n", info->discovery_port);
    printf("  Web port: %d\n", info->web_port);
    
    return TCL_OK;
}

static int mesh_send_heartbeat_command(ClientData data, Tcl_Interp *interp,
                                       int objc, Tcl_Obj *objv[])
{
    mesh_info_t *info = (mesh_info_t *)data;
    mesh_send_heartbeat(info);
    return TCL_OK;
}

static int mesh_update_status_command(ClientData data, Tcl_Interp *interp,
                                      int objc, Tcl_Obj *objv[])
{
    mesh_info_t *info = (mesh_info_t *)data;
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "status");
        return TCL_ERROR;
    }
    
    strncpy(info->status, Tcl_GetString(objv[1]), sizeof(info->status) - 1);
    return TCL_OK;
}

static int mesh_set_field_command(ClientData data, Tcl_Interp *interp,
                                  int objc, Tcl_Obj *objv[])
{
    mesh_info_t *info = (mesh_info_t *)data;
    
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "key value");
        return TCL_ERROR;
    }
    
    const char *key = Tcl_GetString(objv[1]);
    const char *value = Tcl_GetString(objv[2]);
    
    /* Check if key exists */
    for (int i = 0; i < info->num_fields; i++) {
        if (strcmp(info->fields[i].key, key) == 0) {
            strncpy(info->fields[i].value, value, MAX_FIELD_VAL_LEN - 1);
            return TCL_OK;
        }
    }
    
    /* Add new field */
    if (info->num_fields >= MAX_CUSTOM_FIELDS) {
        Tcl_SetResult(interp, "maximum custom fields reached", TCL_STATIC);
        return TCL_ERROR;
    }
    
    strncpy(info->fields[info->num_fields].key, key, MAX_FIELD_KEY_LEN - 1);
    strncpy(info->fields[info->num_fields].value, value, MAX_FIELD_VAL_LEN - 1);
    info->num_fields++;
    
    return TCL_OK;
}

static int mesh_remove_field_command(ClientData data, Tcl_Interp *interp,
                                     int objc, Tcl_Obj *objv[])
{
    mesh_info_t *info = (mesh_info_t *)data;
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "key");
        return TCL_ERROR;
    }
    
    const char *key = Tcl_GetString(objv[1]);
    
    for (int i = 0; i < info->num_fields; i++) {
        if (strcmp(info->fields[i].key, key) == 0) {
            /* Shift remaining fields */
            for (int j = i; j < info->num_fields - 1; j++) {
                info->fields[j] = info->fields[j + 1];
            }
            info->num_fields--;
            return TCL_OK;
        }
    }
    
    return TCL_OK;  /* Key not found is not an error */
}

static int mesh_get_fields_command(ClientData data, Tcl_Interp *interp,
                                   int objc, Tcl_Obj *objv[])
{
    mesh_info_t *info = (mesh_info_t *)data;
    
    Tcl_Obj *dict = Tcl_NewDictObj();
    
    for (int i = 0; i < info->num_fields; i++) {
        Tcl_DictObjPut(interp, dict,
                       Tcl_NewStringObj(info->fields[i].key, -1),
                       Tcl_NewStringObj(info->fields[i].value, -1));
    }
    
    Tcl_SetObjResult(interp, dict);
    return TCL_OK;
}

static int mesh_clear_fields_command(ClientData data, Tcl_Interp *interp,
                                     int objc, Tcl_Obj *objv[])
{
    mesh_info_t *info = (mesh_info_t *)data;
    info->num_fields = 0;
    return TCL_OK;
}

static int mesh_get_appliance_id_command(ClientData data, Tcl_Interp *interp,
                                         int objc, Tcl_Obj *objv[])
{
    mesh_info_t *info = (mesh_info_t *)data;
    Tcl_SetObjResult(interp, Tcl_NewStringObj(info->appliance_id, -1));
    return TCL_OK;
}

static int mesh_info_command(ClientData data, Tcl_Interp *interp,
                             int objc, Tcl_Obj *objv[])
{
    mesh_info_t *info = (mesh_info_t *)data;
    
    Tcl_Obj *dict = Tcl_NewDictObj();
    
    Tcl_DictObjPut(interp, dict,
                   Tcl_NewStringObj("applianceId", -1),
                   Tcl_NewStringObj(info->appliance_id, -1));
    Tcl_DictObjPut(interp, dict,
                   Tcl_NewStringObj("name", -1),
                   Tcl_NewStringObj(info->appliance_name, -1));
    Tcl_DictObjPut(interp, dict,
                   Tcl_NewStringObj("status", -1),
                   Tcl_NewStringObj(info->status, -1));
    Tcl_DictObjPut(interp, dict,
                   Tcl_NewStringObj("discoveryPort", -1),
                   Tcl_NewIntObj(info->discovery_port));
    Tcl_DictObjPut(interp, dict,
                   Tcl_NewStringObj("webPort", -1),
                   Tcl_NewIntObj(info->web_port));
    Tcl_DictObjPut(interp, dict,
                   Tcl_NewStringObj("ssl", -1),
                   Tcl_NewBooleanObj(info->ssl_enabled));
    Tcl_DictObjPut(interp, dict,
                   Tcl_NewStringObj("numBroadcastAddrs", -1),
                   Tcl_NewIntObj(info->num_broadcast_addrs));
    
    Tcl_SetObjResult(interp, dict);
    return TCL_OK;
}

static int mesh_shutdown_command(ClientData data, Tcl_Interp *interp,
                                 int objc, Tcl_Obj *objv[])
{
    mesh_info_t *info = (mesh_info_t *)data;
    
    if (info->udp_socket >= 0) {
        close(info->udp_socket);
        info->udp_socket = -1;
        printf("Mesh broadcaster shutdown\n");
    }
    
    return TCL_OK;
}

/* Cleanup when interpreter is deleted */
static void mesh_cleanup(ClientData data, Tcl_Interp *interp)
{
    mesh_info_t *info = (mesh_info_t *)data;
    
    if (info->udp_socket >= 0) {
        close(info->udp_socket);
    }
    
    free(info);
}

/*****************************************************************************
 * EXPORT
 *****************************************************************************/

#ifdef WIN32
EXPORT(int, Dserv_mesh_Init)(Tcl_Interp *interp)
#else
int Dserv_mesh_Init(Tcl_Interp *interp)
#endif
{
    if (
#ifdef USE_TCL_STUBS
        Tcl_InitStubs(interp, "8.6-", 0)
#else
        Tcl_PkgRequire(interp, "Tcl", "8.6-", 0)
#endif
        == NULL) {
        return TCL_ERROR;
    }
    
    /* Allocate per-interpreter mesh info */
    mesh_info_t *info = (mesh_info_t *)calloc(1, sizeof(mesh_info_t));
    if (!info) {
        return TCL_ERROR;
    }
    
    /* Initialize defaults */
    info->tclserver = tclserver_get_from_interp(interp);
    info->udp_socket = -1;
    info->discovery_port = 12346;
    info->web_port = 2565;
    info->ssl_enabled = 0;
    strncpy(info->status, "idle", sizeof(info->status));
    
    /* Create commands */
    Tcl_CreateObjCommand(interp, "meshInit",
                         (Tcl_ObjCmdProc *)mesh_init_command,
                         (ClientData)info, NULL);
    Tcl_CreateObjCommand(interp, "meshSendHeartbeat",
                         (Tcl_ObjCmdProc *)mesh_send_heartbeat_command,
                         (ClientData)info, NULL);
    Tcl_CreateObjCommand(interp, "meshUpdateStatus",
                         (Tcl_ObjCmdProc *)mesh_update_status_command,
                         (ClientData)info, NULL);
    Tcl_CreateObjCommand(interp, "meshSetField",
                         (Tcl_ObjCmdProc *)mesh_set_field_command,
                         (ClientData)info, NULL);
    Tcl_CreateObjCommand(interp, "meshRemoveField",
                         (Tcl_ObjCmdProc *)mesh_remove_field_command,
                         (ClientData)info, NULL);
    Tcl_CreateObjCommand(interp, "meshGetFields",
                         (Tcl_ObjCmdProc *)mesh_get_fields_command,
                         (ClientData)info, NULL);
    Tcl_CreateObjCommand(interp, "meshClearFields",
                         (Tcl_ObjCmdProc *)mesh_clear_fields_command,
                         (ClientData)info, NULL);
    Tcl_CreateObjCommand(interp, "meshGetApplianceId",
                         (Tcl_ObjCmdProc *)mesh_get_appliance_id_command,
                         (ClientData)info, NULL);
    Tcl_CreateObjCommand(interp, "meshInfo",
                         (Tcl_ObjCmdProc *)mesh_info_command,
                         (ClientData)info, NULL);
    Tcl_CreateObjCommand(interp, "meshShutdown",
                         (Tcl_ObjCmdProc *)mesh_shutdown_command,
                         (ClientData)info, NULL);
    
    /* Register cleanup */
    Tcl_CallWhenDeleted(interp, mesh_cleanup, (ClientData)info);
    
    return TCL_OK;
}