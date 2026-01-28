/*
 * NAME
 *   mesh.c - UDP mesh broadcast module with seed peer support
 *
 * DESCRIPTION
 *   Broadcasts heartbeat packets for mesh discovery.
 *   Supports both local broadcast and unicast to seed peers
 *   for cross-subnet discovery.
 *   Discovery and aggregation handled by dserv-agent.
 *   Timing controlled externally via timer module.
 *
 * AUTHOR
 *   DLS
 *
 * DATE
 *   12/24, 1/26
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
#include <netdb.h>

#include <tcl.h>
#include <jansson.h>

#include "Datapoint.h"
#include "tclserver_api.h"

#define MAX_CUSTOM_FIELDS 20
#define MAX_FIELD_KEY_LEN 64
#define MAX_FIELD_VAL_LEN 256
#define MAX_BROADCAST_ADDRS 8
#define MAX_SEED_PEERS 8
#define NETWORK_SCAN_INTERVAL_SEC 30

typedef struct custom_field_s {
    char key[MAX_FIELD_KEY_LEN];
    char value[MAX_FIELD_VAL_LEN];
} custom_field_t;

typedef struct seed_peer_s {
    char address[128];          /* IP or hostname */
    struct sockaddr_in resolved; /* Resolved address */
    int valid;                   /* Resolution succeeded */
} seed_peer_t;

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
    
    /* Network - broadcast */
    int udp_socket;
    char broadcast_addrs[MAX_BROADCAST_ADDRS][16];
    int num_broadcast_addrs;
    time_t last_network_scan;
    
    /* Network - seed peers */
    seed_peer_t seed_peers[MAX_SEED_PEERS];
    int num_seed_peers;
    
    /* Statistics */
    unsigned long broadcasts_sent;
    unsigned long unicasts_sent;
    unsigned long send_errors;
} mesh_info_t;

/*
 * Resolve a seed peer address (IP or hostname)
 */
static int mesh_resolve_seed(seed_peer_t *seed, int port)
{
    seed->valid = 0;
    
    memset(&seed->resolved, 0, sizeof(seed->resolved));
    seed->resolved.sin_family = AF_INET;
    seed->resolved.sin_port = htons(port);
    
    /* Try as IP address first */
    if (inet_aton(seed->address, &seed->resolved.sin_addr) != 0) {
        seed->valid = 1;
        return 0;
    }
    
    /* Try DNS resolution */
    struct hostent *he = gethostbyname(seed->address);
    if (he && he->h_addr_list[0]) {
        memcpy(&seed->resolved.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
        seed->valid = 1;
        return 0;
    }
    
    fprintf(stderr, "Mesh: failed to resolve seed peer %s\n", seed->address);
    return -1;
}

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
 * Build heartbeat JSON message
 * Returns allocated string that caller must free
 */
static char *mesh_build_heartbeat_json(mesh_info_t *info)
{
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
    json_decref(heartbeat);
    
    return message;
}

/*
 * Send a single heartbeat to all targets
 */
static void mesh_send_heartbeat(mesh_info_t *info)
{
    if (info->udp_socket < 0) return;
    
    mesh_refresh_broadcast_addrs(info);
    
    char *message = mesh_build_heartbeat_json(info);
    if (!message) return;
    
    size_t msg_len = strlen(message);
    
    /* 1. Send to all broadcast addresses (local subnet discovery) */
    for (int i = 0; i < info->num_broadcast_addrs; i++) {
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(info->discovery_port);
        inet_aton(info->broadcast_addrs[i], &addr.sin_addr);
        
        ssize_t result = sendto(info->udp_socket, message, msg_len, 0,
                               (struct sockaddr *)&addr, sizeof(addr));
        if (result >= 0) {
            info->broadcasts_sent++;
        } else {
            info->send_errors++;
        }
    }
    
    /* 2. Send to all seed peers (cross-subnet discovery) */
    for (int i = 0; i < info->num_seed_peers; i++) {
        if (!info->seed_peers[i].valid) continue;
        
        ssize_t result = sendto(info->udp_socket, message, msg_len, 0,
                               (struct sockaddr *)&info->seed_peers[i].resolved,
                               sizeof(info->seed_peers[i].resolved));
        if (result >= 0) {
            info->unicasts_sent++;
        } else {
            info->send_errors++;
        }
    }
    
    free(message);
}

/*
 * Add a seed peer
 */
static int mesh_add_seed_peer(mesh_info_t *info, const char *address)
{
    /* Check for duplicates */
    for (int i = 0; i < info->num_seed_peers; i++) {
        if (strcmp(info->seed_peers[i].address, address) == 0) {
            return 0;  /* Already exists */
        }
    }
    
    if (info->num_seed_peers >= MAX_SEED_PEERS) {
        fprintf(stderr, "Mesh: maximum seed peers (%d) reached\n", MAX_SEED_PEERS);
        return -1;
    }
    
    seed_peer_t *seed = &info->seed_peers[info->num_seed_peers];
    strncpy(seed->address, address, sizeof(seed->address) - 1);
    
    if (mesh_resolve_seed(seed, info->discovery_port) == 0) {
        info->num_seed_peers++;
        printf("Mesh: added seed peer %s\n", address);
        return 0;
    }
    
    return -1;
}

/*
 * Remove a seed peer
 */
static int mesh_remove_seed_peer(mesh_info_t *info, const char *address)
{
    for (int i = 0; i < info->num_seed_peers; i++) {
        if (strcmp(info->seed_peers[i].address, address) == 0) {
            /* Shift remaining seeds */
            for (int j = i; j < info->num_seed_peers - 1; j++) {
                info->seed_peers[j] = info->seed_peers[j + 1];
            }
            info->num_seed_peers--;
            printf("Mesh: removed seed peer %s\n", address);
            return 0;
        }
    }
    return -1;  /* Not found */
}

/*
 * Clear all seed peers
 */
static void mesh_clear_seed_peers(mesh_info_t *info)
{
    info->num_seed_peers = 0;
    printf("Mesh: cleared all seed peers\n");
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
    Tcl_DictObjPut(interp, dict,
                   Tcl_NewStringObj("numSeedPeers", -1),
                   Tcl_NewIntObj(info->num_seed_peers));
    
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

/* Seed peer Tcl commands */

static int mesh_add_seed_command(ClientData data, Tcl_Interp *interp,
                                 int objc, Tcl_Obj *objv[])
{
    mesh_info_t *info = (mesh_info_t *)data;
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "address");
        return TCL_ERROR;
    }
    
    const char *address = Tcl_GetString(objv[1]);
    
    if (mesh_add_seed_peer(info, address) < 0) {
        Tcl_SetResult(interp, "failed to add seed peer", TCL_STATIC);
        return TCL_ERROR;
    }
    
    return TCL_OK;
}

static int mesh_remove_seed_command(ClientData data, Tcl_Interp *interp,
                                    int objc, Tcl_Obj *objv[])
{
    mesh_info_t *info = (mesh_info_t *)data;
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "address");
        return TCL_ERROR;
    }
    
    mesh_remove_seed_peer(info, Tcl_GetString(objv[1]));
    return TCL_OK;
}

static int mesh_get_seeds_command(ClientData data, Tcl_Interp *interp,
                                  int objc, Tcl_Obj *objv[])
{
    mesh_info_t *info = (mesh_info_t *)data;
    
    Tcl_Obj *list = Tcl_NewListObj(0, NULL);
    
    for (int i = 0; i < info->num_seed_peers; i++) {
        Tcl_ListObjAppendElement(interp, list,
                                 Tcl_NewStringObj(info->seed_peers[i].address, -1));
    }
    
    Tcl_SetObjResult(interp, list);
    return TCL_OK;
}

static int mesh_clear_seeds_command(ClientData data, Tcl_Interp *interp,
                                    int objc, Tcl_Obj *objv[])
{
    mesh_info_t *info = (mesh_info_t *)data;
    mesh_clear_seed_peers(info);
    return TCL_OK;
}

static int mesh_stats_command(ClientData data, Tcl_Interp *interp,
                              int objc, Tcl_Obj *objv[])
{
    mesh_info_t *info = (mesh_info_t *)data;
    
    Tcl_Obj *dict = Tcl_NewDictObj();
    
    Tcl_DictObjPut(interp, dict,
                   Tcl_NewStringObj("broadcastsSent", -1),
                   Tcl_NewWideIntObj(info->broadcasts_sent));
    Tcl_DictObjPut(interp, dict,
                   Tcl_NewStringObj("unicastsSent", -1),
                   Tcl_NewWideIntObj(info->unicasts_sent));
    Tcl_DictObjPut(interp, dict,
                   Tcl_NewStringObj("sendErrors", -1),
                   Tcl_NewWideIntObj(info->send_errors));
    
    Tcl_SetObjResult(interp, dict);
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
    
    /* Create commands - original */
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
    
    /* Create commands - seed peers */
    Tcl_CreateObjCommand(interp, "meshAddSeed",
                         (Tcl_ObjCmdProc *)mesh_add_seed_command,
                         (ClientData)info, NULL);
    Tcl_CreateObjCommand(interp, "meshRemoveSeed",
                         (Tcl_ObjCmdProc *)mesh_remove_seed_command,
                         (ClientData)info, NULL);
    Tcl_CreateObjCommand(interp, "meshGetSeeds",
                         (Tcl_ObjCmdProc *)mesh_get_seeds_command,
                         (ClientData)info, NULL);
    Tcl_CreateObjCommand(interp, "meshClearSeeds",
                         (Tcl_ObjCmdProc *)mesh_clear_seeds_command,
                         (ClientData)info, NULL);
    Tcl_CreateObjCommand(interp, "meshStats",
                         (Tcl_ObjCmdProc *)mesh_stats_command,
                         (ClientData)info, NULL);
    
    /* Register cleanup */
    Tcl_CallWhenDeleted(interp, mesh_cleanup, (ClientData)info);
    
    return TCL_OK;
}
