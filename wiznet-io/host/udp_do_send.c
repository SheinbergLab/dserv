/*
 * udp_do_send.c -- RPi host side of the W6300/Pico2 latency test.
 *
 * Toggles a trigger GPIO high IMMEDIATELY before sendto(), so a scope can
 * measure trigger-rising -> Pico DO-rising. Sends {0x01} to drive the Pico
 * DO high (then optionally {0x00} to reset it).
 *
 * Uses libgpiod v2. Build:
 *   gcc -O2 -o udp_do_send udp_do_send.c -lgpiod
 * (libgpiod v1 has a different API -- see notes at bottom.)
 *
 * Run (needs RT priority + GPIO access):
 *   sudo ./udp_do_send 192.168.11.2 5000 gpiochip0 17 1000
 *      argv: <pico-ip> <port> <gpiochip> <trigger-line> <iterations>
 *
 * Measure the DISTRIBUTION over many iterations (min/median/p99/max), not a
 * single shot -- the RPi network stack is the main jitter source.
 */

#include <arpa/inet.h>
#include <gpiod.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr,
            "usage: %s <ip> <port> <gpiochip> <line> [iters] [gap_ms]\n",
            argv[0]);
        return 1;
    }
    const char *ip       = argv[1];
    int         port     = atoi(argv[2]);
    const char *chippath = argv[3];   /* e.g. /dev/gpiochip0 or gpiochip0 */
    unsigned    line     = atoi(argv[4]);
    int         iters    = (argc > 5) ? atoi(argv[5]) : 1000;
    int         gap_ms   = (argc > 6) ? atoi(argv[6]) : 5;

    char chipfull[64];
    if (strncmp(chippath, "/dev/", 5) != 0)
        snprintf(chipfull, sizeof chipfull, "/dev/%s", chippath);
    else
        snprintf(chipfull, sizeof chipfull, "%s", chippath);

    /* --- lock down jitter: RT scheduling + no paging --- */
    struct sched_param sp = { .sched_priority = 80 };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
        perror("sched_setscheduler (run as root for RT prio)");
    mlockall(MCL_CURRENT | MCL_FUTURE);

    /* --- UDP socket, pre-connected so sendto path is minimal --- */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    inet_pton(AF_INET, ip, &dst.sin_addr);
    if (connect(fd, (struct sockaddr *)&dst, sizeof dst) != 0)
        perror("connect");

    /* --- libgpiod v2: request the trigger line as output --- */
    struct gpiod_chip *chip = gpiod_chip_open(chipfull);
    if (!chip) { perror("gpiod_chip_open"); return 1; }

    struct gpiod_line_settings *ls = gpiod_line_settings_new();
    gpiod_line_settings_set_direction(ls, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(ls, GPIOD_LINE_VALUE_INACTIVE);

    struct gpiod_line_config *lc = gpiod_line_config_new();
    gpiod_line_config_add_line_settings(lc, &line, 1, ls);

    struct gpiod_request_config *rc = gpiod_request_config_new();
    gpiod_request_config_set_consumer(rc, "udp_do_send");

    struct gpiod_line_request *req =
        gpiod_chip_request_lines(chip, rc, lc);
    if (!req) { perror("request_lines"); return 1; }

    const uint8_t hi = 0x01, lo = 0x00;
    struct timespec gap = { .tv_sec = gap_ms / 1000,
                            .tv_nsec = (gap_ms % 1000) * 1000000L };

    for (int i = 0; i < iters; i++) {
        /* reset DO low on the Pico, and trigger line low */
        gpiod_line_request_set_value(req, line, GPIOD_LINE_VALUE_INACTIVE);
        send(fd, &lo, 1, 0);
        nanosleep(&gap, NULL);

        /* THE measured edge: trigger high, then fire the command */
        gpiod_line_request_set_value(req, line, GPIOD_LINE_VALUE_ACTIVE);
        send(fd, &hi, 1, 0);
        nanosleep(&gap, NULL);
    }

    gpiod_line_request_release(req);
    gpiod_request_config_free(rc);
    gpiod_line_config_free(lc);
    gpiod_line_settings_free(ls);
    gpiod_chip_close(chip);
    close(fd);
    return 0;
}

/*
 * libgpiod v1 fallback (older Raspberry Pi OS): replace the request block with
 *   struct gpiod_chip *chip = gpiod_chip_open_by_name(chippath);
 *   struct gpiod_line *l = gpiod_chip_get_line(chip, line);
 *   gpiod_line_request_output(l, "udp_do_send", 0);
 * and use gpiod_line_set_value(l, 1/0). Check with: gpiod --version
 *
 * For the very lowest, most repeatable trigger you can memory-map the BCM
 * GPIO registers instead of libgpiod, but libgpiod is the right starting point.
 */
