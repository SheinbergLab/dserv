/*
 * udp_rtt.c -- portable RTT / latency probe for the W6300/Pico2 UDP fast path.
 *
 * Sends cmd_req_t, waits for cmd_rep_t, and reports the DISTRIBUTION over N
 * iterations. Decomposes each sample using the box's self-timestamps:
 *
 *     rtt          = host stopwatch (CLOCK_MONOTONIC), full round trip
 *     device       = t_tx - t_rx  (box turnaround, host-jitter-free)
 *     net + host   = rtt - device (both host stacks + two wire trips)
 *
 * Builds on macOS and Linux:  cc -O2 -o udp_rtt udp_rtt.c
 * Run:  ./udp_rtt 192.168.11.2 5000 5000     (ip port iters)
 *
 * Notes:
 *  - Take MIN as the latency floor; p99/max is the jitter story.
 *  - First samples include ARP; a warmup burst is discarded automatically.
 *  - Both ends are little-endian; struct is packed. On a big-endian host you'd
 *    need to byteswap (not a concern for x86_64 / Apple silicon / Pi).
 *  - On Linux you can strip host userspace scheduling further with
 *    SO_TIMESTAMPING; this tool uses CLOCK_MONOTONIC for portability.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct __attribute__((packed)) {
    uint16_t seq;
    uint8_t  cmd;
    uint8_t  pin;
} cmd_req_t;

typedef struct __attribute__((packed)) {
    uint16_t seq;
    uint8_t  cmd;
    uint8_t  pin;
    uint32_t t_rx_us;
    uint32_t t_tx_us;
} cmd_rep_t;

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void stats(const char *label, double *v, int n)
{
    qsort(v, n, sizeof *v, cmp_d);
    double sum = 0;
    for (int i = 0; i < n; i++) sum += v[i];
    printf("  %-12s min %8.1f  med %8.1f  p99 %8.1f  max %8.1f  mean %8.1f  (us, n=%d)\n",
           label, v[0], v[n / 2], v[(int)(n * 0.99)], v[n - 1], sum / n, n);
}

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <ip> <port> [iters]\n", argv[0]);
        return 1;
    }
    const char *ip    = argv[1];
    int         port  = atoi(argv[2]);
    int         iters = (argc > 3) ? atoi(argv[3]) : 2000;
    const int   warm  = 20;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &dst.sin_addr) != 1) {
        fprintf(stderr, "bad ip %s\n", ip); return 1;
    }
    if (connect(fd, (struct sockaddr *)&dst, sizeof dst) != 0) {
        perror("connect"); return 1;
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };  /* 200ms */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    double *rtt = malloc(sizeof(double) * iters);
    double *dev = malloc(sizeof(double) * iters);
    double *net = malloc(sizeof(double) * iters);
    int n = 0, lost = 0;

    for (int i = 0; i < iters + warm; i++) {
        cmd_req_t req = { .seq = (uint16_t)i, .cmd = (uint8_t)(i & 1), .pin = 0 };

        double t0 = now_us();
        if (send(fd, &req, sizeof req, 0) != sizeof req) { lost++; continue; }

        cmd_rep_t rep;
        ssize_t r = recv(fd, &rep, sizeof rep, 0);
        double t1 = now_us();

        if (r != (ssize_t)sizeof rep || rep.seq != req.seq) { lost++; continue; }
        if (i < warm) continue;                    /* discard ARP/warmup     */

        rtt[n] = t1 - t0;
        dev[n] = (double)(uint32_t)(rep.t_tx_us - rep.t_rx_us);  /* us wrap-safe */
        net[n] = rtt[n] - dev[n];
        if (net[n] < 0) net[n] = 0;                /* clamp clock-grain noise */
        n++;
    }

    printf("W6300/Pico2 UDP RTT to %s:%d  (%d ok, %d lost)\n", ip, port, n, lost);
    if (n > 0) {
        stats("rtt",        rtt, n);
        stats("device",     dev, n);   /* box turnaround, host-jitter-free   */
        stats("net+host",   net, n);   /* everything outside the box         */
    }
    free(rtt); free(dev); free(net); close(fd);
    return 0;
}
