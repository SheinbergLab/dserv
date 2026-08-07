/*
 * extiodisc.c -- LAN discovery of extio boxes, for the extio subprocess.
 *
 * An extio box broadcasts a UDP beacon on :5011 every 1.5 s carrying its
 * identity and, since beacon v2, its registration health (extio-zephyr
 * src/platform/box_beacon.c; RP2350 wizchip_dserv_config.c beacon_send). This
 * module listens for them and publishes what is live as ONE datapoint, so the
 * fleet page can show boxes that no dserv has adopted yet -- the ones that are
 * otherwise invisible, because a box with no dserv target never registers and
 * so nothing knows it exists.
 *
 * WHY A C MODULE AND NOT TCL. Three independent reasons, and the third is the
 * one that fails silently: core Tcl has no UDP; dserv runs no Tcl event loop,
 * so `fileevent`/`after` are inert rather than erroring; and a blocking read on
 * the interp thread deadlocks the single-thread-per-interp model. usbio.c is
 * the in-tree answer to this exact shape -- worker thread feeding datapoints
 * through tclserver_set_point -- and this follows it deliberately.
 *
 * ---- ONE AGGREGATE DATAPOINT, NOT ONE PER BOX ----
 *
 * `extio/discovered` carries a JSON array of the currently-live boxes, and is
 * republished only when that set CHANGES (a box appearing, a field moving, or a
 * box ageing out).
 *
 * This is the whole design, and it is a direct response to how dserv behaves:
 * datapoints are RETAINED and are never deleted when the thing they describe
 * goes away. Per-box leaves (`extio/discovered/<ip>/link`) would therefore
 * linger after a box is unplugged, and a panel whose entire job is "what is on
 * the LAN right now" would keep confidently listing hardware that is not. That
 * is the same shape as the retained `sync/source` that reported an anchor
 * surviving the reboot which killed it -- a stale value is worse than a missing
 * one, because it answers instead of admitting it does not know.
 *
 * An aggregate is self-cleaning by construction: a box that stops beaconing is
 * simply absent from the next publish. It also gives a page one subscription
 * rather than a wildcard over a namespace that grows with every box ever seen.
 *
 * ---- `via`: WHICH OF OUR ADDRESSES THIS BOX SHOULD BE TOLD ----
 *
 * Each entry carries `via` -- the local address that reaches THAT box, found by
 * connect()ing a throwaway UDP socket to it (which sends nothing) and asking
 * getsockname() what the kernel picked.
 *
 * This is the point of doing discovery at all on a multi-homed host. dserv here
 * answers on several addresses, and a name can always resolve to the wrong one,
 * leaving a box configured with an address it cannot route to while looking
 * perfectly healthy. `via` is the address that is correct BY CONSTRUCTION for
 * the box that just spoke to us, and it is what an adopt step should hand over.
 *
 * ---- WHAT THIS DELIBERATELY DOES NOT DO ----
 *
 * It listens and publishes. It never connects to a box, never writes config,
 * and never adopts anything. Adoption stays an explicit action with a human
 * behind it -- there is more than one dserv on more than one subnet here, and a
 * listener that also adopted would make "who may retarget this box" a question
 * with no good answer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include <tcl.h>
#include "Datapoint.h"
#include "tclserver_api.h"

#define DISC_PORT_DEFAULT 5011
#define DISC_MAX_BOXES    64        /* a rig has a handful; this is a bound, not a target */
#define DISC_TTL_MS       12000     /* 8 missed beacons at 1.5 s -- matches the Go
                                     * listener in tools/extio-setup so the two
                                     * agree about when a box is gone */
#define DISC_TICK_MS      1000      /* recvfrom timeout == how fast a TTL expiry
                                     * is noticed when nothing is arriving */
#define DISC_STRAND_MS    30000     /* how long a box must have been unable to
                                     * reach its dserv before we call it stranded.
                                     *
                                     * NOT zero, which is what this was first. A
                                     * dserv restart drops the box's link for a
                                     * few seconds -- measured at ~4.5 s on the
                                     * rig -- and restarts are ROUTINE here (any
                                     * make install does one). Flagging those
                                     * would put a red badge on the fleet page
                                     * during ordinary work, and a warning that
                                     * cries wolf on maintenance trains people
                                     * to ignore the one that matters.
                                     *
                                     * 30 s is comfortably past any restart while
                                     * still surfacing a real problem within half
                                     * a minute. It is deliberately the same
                                     * order as the box's own MATCH_REFRESH_MS. */
#define DISC_JSON_MAX     (DISC_MAX_BOXES * 512 + 64)

/* One discovered box. Strings are copied out of the beacon and bounded here;
 * a beacon is untrusted input from the network and is never assumed to fit. */
typedef struct {
    char     ip[46];
    char     name[64];
    char     fw[48];
    char     board[48];
    char     build[64];
    char     target[64];
    char     link[8];               /* "up"/"down"; empty for a v1 box */
    char     via[46];               /* our address that reaches this box */
    int      v;
    long     down_ms;
    int      tries;
    int      ever;
    uint64_t last_ms;               /* monotonic-ish arrival, for the TTL */
} disc_box_t;

typedef struct {
    tclserver_t   *tclserver;
    pthread_t      worker;
    volatile int   running;
    int            fd;
    int            port;
    pthread_mutex_t lock;           /* guards boxes/nboxes for extioDiscoverList */
    disc_box_t     boxes[DISC_MAX_BOXES];
    int            nboxes;
    char           last_json[DISC_JSON_MAX];   /* publish only on change */
    uint64_t       rx, bad;
} disc_info_t;

static uint64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ---- a deliberately small JSON reader ----
 *
 * Pulling in a parser for a flat object of known keys would be the larger
 * risk, not the smaller one: this input arrives unauthenticated from any host
 * on the LAN. Scan for "key" and read the scalar after the colon, bounded.
 * Anything unrecognised is ignored rather than rejected, which is what lets a
 * v1 box and a v2 box share one listener.
 */
static int json_str(const char *js, const char *key, char *out, size_t osz)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < osz) {
        if (*p == '\\' && p[1]) p++;      /* no escapes are expected; do not be fooled by one */
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (*p == '"');                   /* unterminated => truncated/hostile => reject */
}

static int json_num(const char *js, const char *key, long *out)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return 0;
    *out = v;
    return 1;
}

/* The local address that reaches `ip`. connect() on a UDP socket sends nothing;
 * it just asks the routing table to pick a source, which getsockname() then
 * reports. Cheaper and far more portable than IP_PKTINFO on the receive path,
 * and it answers the question we actually have ("what should THIS box be told")
 * rather than the one PKTINFO answers ("what address did this arrive on"),
 * which differ when the beacon is a broadcast. */
static void local_addr_for(const char *ip, char *out, size_t osz)
{
    out[0] = '\0';
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port   = htons(9);                 /* discard; nothing is sent */
    if (inet_pton(AF_INET, ip, &to.sin_addr) == 1 &&
        connect(s, (struct sockaddr *) &to, sizeof to) == 0) {
        struct sockaddr_in me;
        socklen_t ml = sizeof me;
        if (getsockname(s, (struct sockaddr *) &me, &ml) == 0) {
            inet_ntop(AF_INET, &me.sin_addr, out, (socklen_t) osz);
        }
    }
    close(s);
}

static void json_escape(const char *in, char *out, size_t osz)
{
    size_t i = 0;
    for (const char *p = in; *p && i + 2 < osz; p++) {
        if (*p == '"' || *p == '\\') { out[i++] = '\\'; out[i++] = *p; }
        else if ((unsigned char) *p < 0x20) { continue; }   /* drop control bytes */
        else out[i++] = *p;
    }
    out[i] = '\0';
}

/* Drop anything past its TTL. Separate from serialisation on purpose: doing
 * both in one pass means an early return on a full buffer leaves the table
 * half-compacted, with entries duplicated behind the write cursor. Caller holds
 * the lock. */
static void expire(disc_info_t *info)
{
    uint64_t now = now_ms();
    int keep = 0;

    for (int i = 0; i < info->nboxes; i++) {
        if (now - info->boxes[i].last_ms > DISC_TTL_MS) continue;
        if (keep != i) info->boxes[keep] = info->boxes[i];
        keep++;
    }
    info->nboxes = keep;
}

/* Serialise the live set. Returns -1 rather than a truncated array: half a
 * fleet list is a wrong answer wearing the shape of a complete one. Caller
 * holds the lock and has already called expire().
 *
 * NO PER-ENTRY AGE FIELD, deliberately, and it was here once. Two reasons, and
 * they reinforce each other. It made the JSON differ on every rebuild, so the
 * change-detection in publish_if_changed never suppressed anything and the
 * aggregate went out ~1.3 times a second on a completely idle fleet -- exactly
 * the churn the change-only design exists to avoid. And under change-only
 * publishing an age is FROZEN at publish time: it would sit there reading "2s"
 * for as long as nothing else moved, which is the stale-field trap this file's
 * header is already about. The 12 s TTL is the freshness guarantee -- every
 * entry present was heard from within it -- and a consumer wanting more can
 * read the datapoint's own timestamp.
 *
 * What DOES still vary is down_ms/tries on a box that cannot reach its dserv.
 * That republishes about twice a second, which is correct: it is proportional
 * to something actually going wrong, and stops the moment it is fixed. */
static int build_json(disc_info_t *info, char *out, size_t osz)
{
    int w = snprintf(out, osz, "[");

    for (int keep = 1; keep <= info->nboxes; keep++) {
        disc_box_t b = info->boxes[keep - 1];

        char en[64], ef[48], eb[48], ebd[64], et[64];
        json_escape(b.name,   en,  sizeof en);
        json_escape(b.fw,     ef,  sizeof ef);
        json_escape(b.board,  eb,  sizeof eb);
        json_escape(b.build,  ebd, sizeof ebd);
        json_escape(b.target, et,  sizeof et);

        /* `configured` and `stranded` are decided HERE, not in the page: they
         * are contract semantics, not presentation, and every consumer must
         * agree on them. In particular a v1 box omits `link` entirely, and
         * reading that silence as "down" would flag the whole RP2350 fleet as
         * broken -- the absence of a health report is not a bad one -- so
         * stranded requires v>=2. */
        int configured = (b.target[0] && strncmp(b.target, "0.0.0.0", 7) != 0);
        int stranded   = (b.v >= 2 && configured && !strcmp(b.link, "down") &&
                          b.down_ms >= DISC_STRAND_MS);

        /* Ownership, which is NOT the same question as health and must not be
         * folded into it. `mine` compares the box's target host against the
         * address that reaches it from here -- so a box pointed at a DIFFERENT
         * dserv on this subnet reads as neither free nor mine, and a page can
         * decline to offer a one-click adopt that would quietly steal it out of
         * a running rig. Every dserv on the LAN sees every box now, so "someone
         * else's" has to be a state we can name. */
        int mine = 0;
        if (configured && b.via[0]) {
            size_t vl = strlen(b.via);
            mine = (strncmp(b.target, b.via, vl) == 0 && b.target[vl] == ':');
        }

        /* snprintf returns what it WOULD have written, so `w` can run past the
         * buffer on a long entry. Bail the moment it does rather than forming
         * `out + w` past the end -- and report failure to the caller instead of
         * publishing a truncated array, because half a fleet list is a wrong
         * answer that looks like a complete one. */
        w += snprintf(out + w, osz - w,
            "%s{\"ip\":\"%s\",\"name\":\"%s\",\"fw\":\"%s\",\"board\":\"%s\","
            "\"build\":\"%s\",\"target\":\"%s\",\"via\":\"%s\",\"v\":%d,"
            "\"configured\":%d,\"mine\":%d,\"stranded\":%d",
            (keep > 1) ? "," : "", b.ip, en, ef, eb, ebd, et, b.via, b.v,
            configured, mine, stranded);
        if (w >= (int) osz) return -1;

        /* Health only from a box that actually reports it. Emitting link:""
         * or down_ms:0 for a v1 box would be inventing an answer it never
         * gave, which is the whole failure mode this file is careful about. */
        if (b.v >= 2 && b.link[0]) {
            w += snprintf(out + w, osz - w,
                ",\"link\":\"%s\",\"down_ms\":%ld,\"tries\":%d,\"ever\":%d",
                b.link, b.down_ms, b.tries, b.ever);
            if (w >= (int) osz) return -1;
        }
        w += snprintf(out + w, osz - w, "}");
        if (w >= (int) osz) return -1;
    }
    w += snprintf(out + w, osz - w, "]");
    return (w < (int) osz) ? w : -1;
}

static void publish_if_changed(disc_info_t *info)
{
    char js[DISC_JSON_MAX];

    pthread_mutex_lock(&info->lock);
    expire(info);
    int n = build_json(info, js, sizeof js);
    int changed = (n > 0) && strcmp(js, info->last_json) != 0;
    if (changed) {
        strncpy(info->last_json, js, sizeof info->last_json - 1);
        info->last_json[sizeof info->last_json - 1] = '\0';
    }
    pthread_mutex_unlock(&info->lock);

    if (!changed) return;
    ds_datapoint_t *dp = dpoint_new("extio/discovered",
                                    tclserver_now(info->tclserver),
                                    DSERV_STRING, (uint32_t) strlen(js),
                                    (unsigned char *) js);
    if (dp) tclserver_set_point(info->tclserver, dp);
}

static void absorb(disc_info_t *info, const char *js, const char *from_ip)
{
    char t[16];
    if (!json_str(js, "t", t, sizeof t) || strcmp(t, "extio") != 0) {
        info->bad++;
        return;                       /* not ours -- 5011 is not exclusively ours */
    }

    disc_box_t nb;
    memset(&nb, 0, sizeof nb);
    /* Prefer the sender's actual source address over the self-reported "ip":
     * they agree in practice, but only one of them cannot be forged into
     * pointing our later adopt step at a third party. */
    snprintf(nb.ip, sizeof nb.ip, "%s", from_ip);
    json_str(js, "name",   nb.name,   sizeof nb.name);
    json_str(js, "fw",     nb.fw,     sizeof nb.fw);
    json_str(js, "board",  nb.board,  sizeof nb.board);
    json_str(js, "build",  nb.build,  sizeof nb.build);
    json_str(js, "target", nb.target, sizeof nb.target);
    json_str(js, "link",   nb.link,   sizeof nb.link);

    long v = 1, dm = 0, tr = 0, ev = 0;
    json_num(js, "v", &v);
    json_num(js, "down_ms", &dm);
    json_num(js, "tries", &tr);
    json_num(js, "ever", &ev);
    nb.v = (int) v; nb.down_ms = dm; nb.tries = (int) tr; nb.ever = (int) ev;
    nb.last_ms = now_ms();

    pthread_mutex_lock(&info->lock);
    int slot = -1;
    for (int i = 0; i < info->nboxes; i++) {
        if (!strcmp(info->boxes[i].ip, nb.ip)) { slot = i; break; }
    }
    if (slot < 0) {
        if (info->nboxes < DISC_MAX_BOXES) slot = info->nboxes++;
    }
    if (slot >= 0) {
        /* `via` costs a syscall trio, so resolve it when the box is new or has
         * moved rather than on every beacon (every 1.5 s per box, forever). */
        if (info->boxes[slot].via[0] == '\0' ||
            strcmp(info->boxes[slot].ip, nb.ip) != 0) {
            local_addr_for(nb.ip, nb.via, sizeof nb.via);
        } else {
            snprintf(nb.via, sizeof nb.via, "%s", info->boxes[slot].via);
        }
        info->boxes[slot] = nb;
    }
    info->rx++;
    pthread_mutex_unlock(&info->lock);
}

static void *worker(void *arg)
{
    disc_info_t *info = (disc_info_t *) arg;
    char buf[2048];

    while (info->running) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(info->fd, buf, sizeof buf - 1, 0,
                             (struct sockaddr *) &from, &fl);
        if (n > 0) {
            buf[n] = '\0';
            char ip[46] = {0};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            absorb(info, buf, ip);
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != EINTR) {
            break;                       /* socket died under us */
        }
        /* Every pass, arriving or not: a TTL expiry is a CHANGE to the live set
         * and has to reach the page, and nothing will arrive to trigger it --
         * the whole point is that the box went away. */
        publish_if_changed(info);
    }
    return NULL;
}

static void disc_stop(disc_info_t *info)
{
    if (info->fd < 0) return;
    info->running = 0;
    /* shutdown() unblocks a recvfrom parked on the timeout boundary; the
     * SO_RCVTIMEO below bounds the join to one tick even without it. */
    shutdown(info->fd, SHUT_RDWR);
    pthread_join(info->worker, NULL);
    close(info->fd);
    info->fd = -1;
}

static int disc_start_cmd(ClientData data, Tcl_Interp *interp,
                          int objc, Tcl_Obj *objv[])
{
    disc_info_t *info = (disc_info_t *) data;
    int port = DISC_PORT_DEFAULT;

    if (objc > 1 && Tcl_GetIntFromObj(interp, objv[1], &port) != TCL_OK) {
        return TCL_ERROR;
    }
    disc_stop(info);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        Tcl_AppendResult(interp, "extioDiscoverStart: socket: ", strerror(errno), NULL);
        return TCL_ERROR;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    /* extio-setup may be listening on the same port on this host; discovery is
     * read-only, so sharing is correct rather than a conflict to lose. */
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    struct timeval tv = { .tv_sec = DISC_TICK_MS / 1000,
                          .tv_usec = (DISC_TICK_MS % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t) port);
    if (bind(fd, (struct sockaddr *) &a, sizeof a) < 0) {
        Tcl_AppendResult(interp, "extioDiscoverStart: bind: ", strerror(errno), NULL);
        close(fd);
        return TCL_ERROR;
    }
    info->fd = fd;
    info->port = port;
    info->running = 1;
    if (pthread_create(&info->worker, NULL, worker, info) != 0) {
        info->running = 0;
        close(fd);
        info->fd = -1;
        Tcl_AppendResult(interp, "extioDiscoverStart: worker thread failed", NULL);
        return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_NewIntObj(port));
    return TCL_OK;
}

static int disc_stop_cmd(ClientData data, Tcl_Interp *interp,
                         int objc, Tcl_Obj *objv[])
{
    (void) interp; (void) objc; (void) objv;
    disc_stop((disc_info_t *) data);
    return TCL_OK;
}

/* The same JSON the datapoint carries, returned synchronously -- for a script
 * that wants to act now rather than subscribe. */
static int disc_list_cmd(ClientData data, Tcl_Interp *interp,
                         int objc, Tcl_Obj *objv[])
{
    (void) objc; (void) objv;
    disc_info_t *info = (disc_info_t *) data;
    char js[DISC_JSON_MAX];

    pthread_mutex_lock(&info->lock);
    expire(info);
    int n = build_json(info, js, sizeof js);
    pthread_mutex_unlock(&info->lock);

    Tcl_SetObjResult(interp, Tcl_NewStringObj(n > 0 ? js : "[]", -1));
    return TCL_OK;
}

static int disc_stats_cmd(ClientData data, Tcl_Interp *interp,
                          int objc, Tcl_Obj *objv[])
{
    (void) objc; (void) objv;
    disc_info_t *info = (disc_info_t *) data;
    char s[160];

    pthread_mutex_lock(&info->lock);
    snprintf(s, sizeof s, "running %d port %d boxes %d rx %llu ignored %llu",
             info->fd >= 0, info->port, info->nboxes,
             (unsigned long long) info->rx, (unsigned long long) info->bad);
    pthread_mutex_unlock(&info->lock);

    Tcl_SetObjResult(interp, Tcl_NewStringObj(s, -1));
    return TCL_OK;
}

#ifdef WIN32
EXPORT(int,Dserv_extiodisc_Init) (Tcl_Interp *interp)
#else
int Dserv_extiodisc_Init(Tcl_Interp *interp)
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
    /* Per-interp, for the same reason usbio is: two subprocesses loading this
     * must not share one listener or one box table. */
    disc_info_t *info = (disc_info_t *) calloc(1, sizeof *info);
    if (!info) return TCL_ERROR;
    info->fd = -1;
    info->tclserver = tclserver_get_from_interp(interp);
    pthread_mutex_init(&info->lock, NULL);

    Tcl_CreateObjCommand(interp, "extioDiscoverStart",
                         (Tcl_ObjCmdProc *) disc_start_cmd, (ClientData) info, NULL);
    Tcl_CreateObjCommand(interp, "extioDiscoverStop",
                         (Tcl_ObjCmdProc *) disc_stop_cmd, (ClientData) info, NULL);
    Tcl_CreateObjCommand(interp, "extioDiscoverList",
                         (Tcl_ObjCmdProc *) disc_list_cmd, (ClientData) info, NULL);
    Tcl_CreateObjCommand(interp, "extioDiscoverStats",
                         (Tcl_ObjCmdProc *) disc_stats_cmd, (ClientData) info, NULL);
    return TCL_OK;
}
