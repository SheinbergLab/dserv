/*
 * box_event.h -- "something happened" wakeup for the service loop.
 *
 * The loop used to k_msleep(1) between passes, which put up to a full
 * millisecond of latency on BOTH halves of a round trip: picking up an inbound
 * command, and publishing a DI edge. Measured on a Teensy 4.0 that showed up as
 * a 0.44 ms floor with a ~1.4 ms spread -- pure poll quantization, not transport.
 *
 * Instead, the ISRs that actually produce work (CDC RX, GPIO edge) signal here,
 * and the loop blocks until signalled or until its watchdog tick is due. The
 * publish path stays single-consumer (only the main loop sends), so this adds
 * no concurrency to the uplink.
 */
#ifndef BOX_EVENT_H
#define BOX_EVENT_H

#include <zephyr/kernel.h>

/* Wake the service loop. ISR-safe; cheap and idempotent when already pending. */
void box_event_signal(void);

/* Block until signalled or the timeout expires. */
void box_event_wait(k_timeout_t timeout);

#endif /* BOX_EVENT_H */
