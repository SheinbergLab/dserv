#ifndef SOCKET_KEEPALIVE_H
#define SOCKET_KEEPALIVE_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/*
 * TCP keepalive for ACCEPTED client sockets, tuned to detect a peer that
 * VANISHED rather than one that is merely idle.
 *
 * WHY THIS EXISTS. Every client connection gets a detached thread that blocks in
 * read() with no timeout. A peer that disappears WITHOUT closing -- an extio box
 * rebooting, being reflashed, losing power, or crashing -- never sends a FIN, so
 * that read() never returns and NOTHING ever reaps the socket or the thread. The
 * leak is permanent for the life of the process and cumulative: one per
 * disappearance.
 *
 * Measured on the rig 2026-07-29, after one evening of development reboots:
 *
 *     40 ESTABLISHED on :4620, peers 192.168.88.10 ... .57
 *     -- a near-contiguous run, one per DHCP lease a SINGLE box had taken that
 *        day; spot-checked five, none answered ping
 *     139 threads in dserv
 *
 * The kernel defaults do detect this eventually -- after about TWO HOURS
 * (TCP_KEEPIDLE 7200 s, then 9 probes at 75 s). That is long enough that the
 * leak looks permanent in practice, and on a rig that reboots a box nightly it
 * accrues faster than it clears. The values below bring detection to ~1 minute.
 *
 * WHAT IT COSTS A HEALTHY CLIENT: nothing observable. Keepalive probes are
 * answered by the peer's TCP stack with no application involvement, and the idle
 * timer is reset by traffic in EITHER direction -- so a box publishing at 1 Hz,
 * or a GUI receiving datapoints, never triggers a probe at all. Only a genuinely
 * silent connection exchanges one small packet per KEEPIDLE seconds. Probes
 * cannot truncate a busy connection, because they only run when it is idle.
 *
 * This does NOT replace closing sockets properly; it is the backstop for peers
 * that were never in a position to close theirs.
 */
static inline void dserv_set_keepalive(int fd)
{
	int on = 1;

	if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on) < 0) {
		return;                 /* nothing else is meaningful without it */
	}

	int idle = 30;              /* silence before the first probe (s) */
	int intvl = 10;             /* between probes (s)                 */
	int cnt = 3;                /* unanswered probes before ETIMEDOUT */

#if defined(TCP_KEEPIDLE)       /* Linux */
	(void) setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
#elif defined(TCP_KEEPALIVE)    /* macOS/BSD spell the same thing differently */
	(void) setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof idle);
#else
	(void) idle;                /* keepalive on, platform defaults for timing */
#endif

#if defined(TCP_KEEPINTVL)
	(void) setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
#else
	(void) intvl;
#endif

#if defined(TCP_KEEPCNT)
	(void) setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
#else
	(void) cnt;
#endif
}

#endif /* SOCKET_KEEPALIVE_H */
