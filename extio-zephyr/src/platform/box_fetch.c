/* box_fetch -- see box_fetch.h. One request at a time, one static buffer,
 * one worker thread that spends its whole life asleep between OTAs. */

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <string.h>
#include <errno.h>

#include "box_fetch.h"
#include "box_console.h"

/* Timeouts. CONNECT/HDR are round-trip bounds on a healthy LAN; IDLE is the
 * no-progress bound while streaming, and it must comfortably exceed the
 * longest flash stall the sink can take mid-stream (a sector erase is ~10 ms
 * on the MCXN947, PORTING.md; 4 s also matches the RP2350 original). */
#define FETCH_CONNECT_MS 1000
#define FETCH_HDR_MS     2000
#define FETCH_IDLE_MS    4000

/* One in-flight request; owned by the worker while `busy`. */
static struct {
	uint8_t          ip[4];
	uint16_t         port;
	char             key[64];
	box_fetch_sink_t sink;
	box_fetch_done_t done;
	void            *ud;
} req;

static atomic_t fetch_busy_flag;
static K_SEM_DEFINE(fetch_go, 0, 1);

/* Stream buffer: static, not stack -- one fetch at a time by construction. */
static uint8_t fetch_buf[1024];

int box_fetch_busy(void)
{
	return atomic_get(&fetch_busy_flag) != 0;
}

int box_fetch_start(const uint8_t dserv_ip[4], uint16_t port, const char *key,
		    box_fetch_sink_t sink, box_fetch_done_t done, void *ud)
{
	if (!dserv_ip || !key || !sink || !done ||
	    strlen(key) == 0 || strlen(key) >= sizeof req.key) {
		return -1;
	}
	if (!atomic_cas(&fetch_busy_flag, 0, 1)) {
		return -1;                       /* one at a time */
	}
	memcpy(req.ip, dserv_ip, 4);
	req.port = port;
	strcpy(req.key, key);
	req.sink = sink;
	req.done = done;
	req.ud   = ud;
	k_sem_give(&fetch_go);
	return 0;
}

/* Receive exactly n bytes, polling with an idle bound that resets on progress. */
static int fetch_recv(int s, uint8_t *buf, uint32_t n, int idle_ms)
{
	uint32_t got = 0;

	while (got < n) {
		struct zsock_pollfd pfd = { .fd = s, .events = ZSOCK_POLLIN };

		if (zsock_poll(&pfd, 1, idle_ms) <= 0) {
			return -1;
		}
		int r = zsock_recv(s, buf + got, n - got, 0);

		if (r <= 0) {
			return -1;
		}
		got += (uint32_t) r;
	}
	return 0;
}

/* The '<' get (src/Dataserver.cpp): request '<' + varlen(u16 LE) + varname;
 * reply size(int32 LE), then dpoint_to_binary = varlen(2) + varname + ts(8) +
 * type(4) + datalen(4) + data[datalen]. The DATA span goes to the sink. */
static int fetch_get_binary(void)
{
	uint16_t klen = (uint16_t) strlen(req.key);
	uint8_t  hdr[80];                        /* >= klen(63) + ts+type(12) */
	int      rc = -1;

	int s = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (s < 0) {
		return -1;
	}
	zsock_fcntl(s, ZVFS_F_SETFL, ZVFS_O_NONBLOCK);

	struct sockaddr_in a = { 0 };

	a.sin_family = AF_INET;
	a.sin_port   = htons(req.port);
	memcpy(&a.sin_addr, req.ip, 4);

	if (zsock_connect(s, (struct sockaddr *) &a, sizeof a) < 0 &&
	    errno != EINPROGRESS && errno != EALREADY) {
		goto out;
	}
	struct zsock_pollfd pfd = { .fd = s, .events = ZSOCK_POLLOUT };

	if (zsock_poll(&pfd, 1, FETCH_CONNECT_MS) <= 0) {
		goto out;
	}
	int err = 0;
	socklen_t elen = sizeof err;

	zsock_getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &elen);
	if (err != 0) {
		goto out;
	}

	hdr[0] = '<';
	memcpy(hdr + 1, &klen, sizeof klen);
	memcpy(hdr + 3, req.key, klen);
	if (zsock_send(s, hdr, 3u + klen, 0) != (int) (3u + klen)) {
		goto out;                        /* 66 B; a fresh socket takes it whole */
	}

	int32_t size = 0;

	if (fetch_recv(s, (uint8_t *) &size, 4, FETCH_HDR_MS) != 0 || size <= 0) {
		goto out;                        /* size 0 = key not found (not staged) */
	}

	uint16_t rvarlen = 0;

	if (fetch_recv(s, (uint8_t *) &rvarlen, 2, FETCH_HDR_MS) != 0 ||
	    rvarlen != klen) {
		goto out;                        /* we asked by name; anything else is not our reply */
	}
	if (fetch_recv(s, hdr, (uint32_t) rvarlen + 8u + 4u, FETCH_HDR_MS) != 0) {
		goto out;                        /* name + ts + type: skipped */
	}

	uint32_t datalen = 0;

	if (fetch_recv(s, (uint8_t *) &datalen, 4, FETCH_HDR_MS) != 0) {
		goto out;
	}
	if (datalen != (uint32_t) size - (2u + rvarlen + 8u + 4u + 4u)) {
		goto out;                        /* framing disagrees with itself */
	}

	uint32_t left = datalen;

	while (left) {
		struct zsock_pollfd p2 = { .fd = s, .events = ZSOCK_POLLIN };

		if (zsock_poll(&p2, 1, FETCH_IDLE_MS) <= 0) {
			goto out;
		}
		uint32_t want = left < sizeof fetch_buf ? left : (uint32_t) sizeof fetch_buf;
		int r = zsock_recv(s, fetch_buf, want, 0);

		if (r <= 0) {
			goto out;
		}
		if (req.sink(req.ud, fetch_buf, (uint32_t) r) < 0) {
			goto out;                /* sink abort (flash error, oversize) */
		}
		left -= (uint32_t) r;
	}
	rc = (int) datalen;
out:
	zsock_close(s);
	return rc;
}

static void fetch_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		k_sem_take(&fetch_go, K_FOREVER);

		int rc = fetch_get_binary();

		/* done() is called with busy still held: the transfer owns the
		 * sink until its terminal state is fully published, so nothing
		 * can interleave a new begin/fetch into a half-closed one. */
		req.done(req.ud, rc);
		atomic_set(&fetch_busy_flag, 0);
	}
}

/* Prio 6: below the RX reader (2) and the publisher (4) -- an OTA must never
 * cost the paths that carry real-time data their seat. 3 KB of stack: zsock
 * calls plus the sink (whose page buffer lives in box_ota_t, not here). */
K_THREAD_DEFINE(box_fetch_tid, 3072, fetch_thread_fn, NULL, NULL, NULL, 6, 0, 0);
