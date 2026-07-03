/*
 * dserv_bench.c -- measure dserv's TCP datapoint pub/sub overhead on localhost.
 *
 * One process, one monotonic clock:
 *   - registers itself as a BINARY send-client (%reg ip port 1) + match bench/[star]
 *   - SET path: writes a '>' binary datapoint frame to dserv:4620 (no ack)
 *   - RECV path: dserv notifies -> send-client thread pushes the datapoint back
 *     to our listen socket; we time set->receive.
 *
 * This isolates dserv's software pub/sub cost (notify + match + send-client
 * thread wakeup + socket). It does NOT include the W6300/wire (measured
 * separately with udp_rtt + scope on real hardware). Take min as the floor,
 * p99/max as the jitter that matters for a control loop.
 *
 *   cc -O2 -Wall -I../common -o dserv_bench dserv_bench.c
 *   ./dserv_bench 127.0.0.1 5020 5000
 */
#include "dserv_msg.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static double now_us(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec*1e6 + t.tv_nsec/1e3; }

static int cmp_d(const void *a, const void *b)
{ double x=*(const double*)a, y=*(const double*)b; return (x>y)-(x<y); }

static void stats(const char *label, double *v, int n)
{
    qsort(v, n, sizeof *v, cmp_d);
    double s=0; for (int i=0;i<n;i++) s+=v[i];
    printf("  %-10s min %7.1f  med %7.1f  p99 %7.1f  max %8.1f  mean %7.1f us (n=%d)\n",
           label, v[0], v[n/2], v[(int)(n*0.99)], v[n-1], s/n, n);
}

static int ctrl_cmd(const char *host, const char *line)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_port=htons(4620);
    inet_pton(AF_INET, host, &a.sin_addr);
    if (connect(s,(struct sockaddr*)&a,sizeof a)) { perror("ctrl connect"); return -1; }
    char buf[128]; snprintf(buf,sizeof buf,"%s\n",line);
    write(s, buf, strlen(buf));
    struct timeval tv={1,0}; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
    char r[64]; ssize_t n=read(s,r,sizeof r-1); if(n>0){r[n]=0;} close(s);
    return 1;
}

/* read exactly 128 bytes (one frame) from a stream fd */
static int recv_frame(int fd, uint8_t *f)
{
    int got=0;
    while (got < DSERV_MSG_LEN) {
        ssize_t r = recv(fd, f+got, DSERV_MSG_LEN-got, 0);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *host = argc>1 ? argv[1] : "127.0.0.1";
    int myport = argc>2 ? atoi(argv[2]) : 5020;
    int iters  = argc>3 ? atoi(argv[3]) : 5000;
    const int warm = 50;

    /* listen for dserv's push connection */
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one=1; setsockopt(ls,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    struct sockaddr_in la={0}; la.sin_family=AF_INET; la.sin_addr.s_addr=INADDR_ANY; la.sin_port=htons(myport);
    if (bind(ls,(struct sockaddr*)&la,sizeof la)||listen(ls,1)){perror("bind/listen");return 1;}

    char m[64];
    printf("registering binary send-client + match bench/[star] ...\n");
    ctrl_cmd(host, (snprintf(m,sizeof m,"%%reg %s %d 1", "127.0.0.1", myport), m));
    ctrl_cmd(host, (snprintf(m,sizeof m,"%%match 127.0.0.1 %d bench/* 1", myport), m));

    int ps = accept(ls, NULL, NULL);          /* dserv -> us (push channel) */
    if (ps < 0){perror("accept");return 1;}
    setsockopt(ps, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    printf("dserv push channel connected\n");

    /* setter connection (we write '>' datapoint frames here) */
    int set_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in da={0}; da.sin_family=AF_INET; da.sin_port=htons(4620); inet_pton(AF_INET,host,&da.sin_addr);
    if (connect(set_fd,(struct sockaddr*)&da,sizeof da)){perror("set connect");return 1;}
    setsockopt(set_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    uint8_t tx[DSERV_MSG_LEN], rx[DSERV_MSG_LEN];
    double *rtt = malloc(sizeof(double)*iters);
    int n=0;

    /* ---- RTT: set -> pushed-back ---- */
    for (int i=0; i<iters+warm; i++) {
        dserv_msg_int(tx, "bench/ping", 0, i);
        double t0 = now_us();
        if (write(set_fd, tx, DSERV_MSG_LEN) != DSERV_MSG_LEN){perror("write");break;}
        if (recv_frame(ps, rx) != 0){fprintf(stderr,"push closed\n");break;}
        double t1 = now_us();
        if (i>=warm) rtt[n++] = t1-t0;
    }
    printf("\ndserv pub/sub RTT (set '>' -> subscription push, localhost):\n");
    if (n) stats("rtt", rtt, n);

    /* ---- throughput: fire K sets, drain K pushes ---- */
    int K = 20000;
    double b0 = now_us();
    for (int i=0;i<K;i++){ dserv_msg_int(tx,"bench/tput",0,i); if(write(set_fd,tx,DSERV_MSG_LEN)!=DSERV_MSG_LEN)break; }
    for (int i=0;i<K;i++){ if(recv_frame(ps,rx)!=0)break; }
    double b1 = now_us();
    printf("\nthroughput: %d datapoints round-tripped in %.1f ms = %.0f dp/s\n",
           K, (b1-b0)/1000.0, K/((b1-b0)/1e6));

    free(rtt); close(ps); close(set_fd); close(ls);
    return 0;
}
