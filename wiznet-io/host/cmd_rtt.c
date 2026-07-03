/*
 * cmd_rtt.c -- compare command round-trip latency + jitter to the box, USB vs
 * dserv/TCP. Same action (drive an output), measured host-side over both paths.
 *
 *   USB  : write "do <pin> <v>\n" to the box's USB-CDC, wait for its "OK" reply.
 *          RTT = host -> USB -> box(dispatch) -> USB -> host.
 *   dserv: set <name>/cmd/do/<pin> (dserv pushes it to the box over Ethernet);
 *          the box drives the pin AND publishes <name>/state/do/<pin>, which
 *          dserv pushes back to us. RTT = host -> dserv -> box -> dserv -> host
 *          (two Ethernet hops via the relay -- the real dserv-command path).
 *
 *   cc -O2 -Wall -I../common -o cmd_rtt cmd_rtt.c
 *   ./cmd_rtt usb   /dev/cu.usbmodem1301 5 2000
 *   ./cmd_rtt dserv devpico              5 2000     (dserv on 127.0.0.1:4620)
 *
 * Reports min/med/p99/max -- p99/max is the jitter that matters for a rig.
 */
#include "dserv_msg.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static double now_us(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec*1e6 + t.tv_nsec/1e3; }
static int cmp_d(const void *a, const void *b)
{ double x=*(const double*)a,y=*(const double*)b; return (x>y)-(x<y); }
static void stats(const char *label, double *v, int n)
{
    if (!n) { printf("  %-6s (no samples)\n", label); return; }
    qsort(v, n, sizeof *v, cmp_d);
    double s=0; for (int i=0;i<n;i++) s+=v[i];
    printf("  %-6s min %7.1f  med %7.1f  p99 %8.1f  max %9.1f  mean %7.1f us  (n=%d)\n",
           label, v[0], v[n/2], v[(int)(n*0.99)], v[n-1], s/n, n);
}

/* ---------- USB path ---------- */
static int run_usb(const char *dev, int pin, int iters)
{
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { perror("open serial"); return 1; }
    struct termios t; tcgetattr(fd, &t); cfmakeraw(&t);
    cfsetspeed(&t, B115200); t.c_cc[VMIN]=0; t.c_cc[VTIME]=0;
    tcsetattr(fd, TCSANOW, &t);

    double *rtt = malloc(sizeof(double)*iters); int n=0; const int warm=30;
    for (int i=0;i<iters+warm;i++) {
        tcflush(fd, TCIFLUSH);                      /* drop echo/leftovers */
        char cmd[32]; int cl = snprintf(cmd,sizeof cmd,"do %d %d\n",pin,i&1);
        char buf[512]; int got=0; int ok=0;
        double t0 = now_us();
        write(fd, cmd, cl);
        while (now_us()-t0 < 200000.0) {            /* wait up to 200ms for "OK" */
            struct pollfd p={fd,POLLIN,0};
            if (poll(&p,1,50)<=0) continue;
            int r = read(fd, buf+got, sizeof buf-1-got);
            if (r>0){ got+=r; buf[got]=0; if (strstr(buf,"OK")||strstr(buf,"ERR")){ok=1;break;} }
        }
        double t1 = now_us();
        if (ok && i>=warm) rtt[n++]=t1-t0;
    }
    printf("USB-CDC command RTT (do %d):\n", pin); stats("usb", rtt, n);
    free(rtt); close(fd); return 0;
}

/* ---------- dserv/TCP path ---------- */
static void ctrl(const char *line)
{
    int s=socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_port=htons(4620);
    inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);
    if (!connect(s,(struct sockaddr*)&a,sizeof a)) {
        char b[128]; snprintf(b,sizeof b,"%s\n",line); write(s,b,strlen(b));
        struct timeval tv={1,0}; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
        char r[64]; (void)!read(s,r,sizeof r);
    }
    close(s);
}
static int recv_frame(int fd, uint8_t *f)
{ int g=0; while(g<DSERV_MSG_LEN){ssize_t r=recv(fd,f+g,DSERV_MSG_LEN-g,0); if(r<=0)return -1; g+=r;} return 0; }

static int run_dserv(const char *name, int pin, int iters)
{
    int myport = 5040;
    int ls=socket(AF_INET,SOCK_STREAM,0); int one=1;
    setsockopt(ls,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    struct sockaddr_in la={0}; la.sin_family=AF_INET; la.sin_addr.s_addr=INADDR_ANY; la.sin_port=htons(myport);
    if (bind(ls,(struct sockaddr*)&la,sizeof la)||listen(ls,1)){perror("bind");return 1;}

    char m[96];
    ctrl((snprintf(m,sizeof m,"%%reg 127.0.0.1 %d 1",myport),m));
    ctrl((snprintf(m,sizeof m,"%%match 127.0.0.1 %d %s/state/do/%d 1",myport,name,pin),m));
    int ps=accept(ls,NULL,NULL); if(ps<0){perror("accept");return 1;}
    setsockopt(ps,IPPROTO_TCP,TCP_NODELAY,&one,sizeof one);

    int sf=socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in da={0}; da.sin_family=AF_INET; da.sin_port=htons(4620); inet_pton(AF_INET,"127.0.0.1",&da.sin_addr);
    if (connect(sf,(struct sockaddr*)&da,sizeof da)){perror("set connect");return 1;}
    setsockopt(sf,IPPROTO_TCP,TCP_NODELAY,&one,sizeof one);

    char dp[64]; snprintf(dp,sizeof dp,"%s/cmd/do/%d",name,pin);
    uint8_t tx[DSERV_MSG_LEN], rx[DSERV_MSG_LEN];
    double *rtt=malloc(sizeof(double)*iters); int n=0; const int warm=30;
    for (int i=0;i<iters+warm;i++) {
        dserv_msg_int(tx, dp, 0, i&1);
        double t0=now_us();
        if (write(sf,tx,DSERV_MSG_LEN)!=DSERV_MSG_LEN){perror("write");break;}
        if (recv_frame(ps,rx)!=0){fprintf(stderr,"push closed\n");break;}
        double t1=now_us();
        if (i>=warm) rtt[n++]=t1-t0;
    }
    printf("dserv command RTT (%s <- state/do readback):\n", dp); stats("dserv", rtt, n);
    free(rtt); close(ps); close(sf); close(ls); return 0;
}

int main(int argc, char **argv)
{
    if (argc<4) { fprintf(stderr,
        "usage:\n  %s usb   <serial-dev> <pin> [iters]\n  %s dserv <box-name> <pin> [iters]\n",
        argv[0], argv[0]); return 2; }
    int pin  = atoi(argv[3]);
    int iters = argc>4 ? atoi(argv[4]) : 2000;
    if (!strcmp(argv[1],"usb"))   return run_usb(argv[2], pin, iters);
    if (!strcmp(argv[1],"dserv")) return run_dserv(argv[2], pin, iters);
    fprintf(stderr, "unknown mode '%s'\n", argv[1]); return 2;
}
