/*
 * box_pub.h -- the publish queues + the one thread that writes the uplink.
 *
 * Producers (the service loop, and in stage 2 the ISR-adjacent event paths)
 * hand 128-byte dserv frames to one of two classes and return in memcpy time:
 *
 *   EVENT -- things that happened and must not be dropped before the wire has
 *            had its chance: DI edges, chords, scheduled/at_abs fires, obs
 *            sync frames, command replies. Drained first, always.
 *   BULK  -- last-value-wins telemetry, announce/manifest frames, OTA status.
 *            Drop-OLDEST on overflow (the old pubq policy, unchanged).
 *
 * The publisher thread drains both through box_uplink_send_stream(), gathering
 * up to `gather` frames per call so the per-send stack traversal (measured
 * 275-864 us on this class of box, CMakeLists.txt) is paid once per BATCH, not
 * once per frame. dserv's '>' reader consumes a byte stream in exact 128-byte
 * records (src/Dataserver.cpp doRead), so batched sends are indistinguishable
 * from singles on the far end -- the win needs no protocol change.
 *
 * WHY A THREAD AT ALL: sends used to run inline in the service loop, so a
 * burst (announce ~dozens of frames, the 1 Hz status block, six sync frames at
 * every obs anchor) held the loop for up to ~9.5 ms while inbound cmd/* waited
 * (see the measurement note above the old pubq in main.c's history: 2 vs 8 per
 * pass vs inline made NO difference to two-box skew -- stamps are ISR-side --
 * but the loop stall is real command latency). Enqueueing costs the loop a
 * memcpy; the stall goes to a thread whose delay hurts nothing.
 *
 * PRIORITY: BELOW main and the analog sampler, above reg/ethrx. A publisher
 * ABOVE main just moves the burst stall from the loop into a higher-priority
 * thread that starves the loop -- same disease, new host. Below them, it runs
 * in the gaps main already spends parked in box_event_wait(), events still
 * reach the wire within ~a pass, and a long bulk drain is preempted by the
 * things whose timing matters.
 */
#ifndef BOX_PUB_H
#define BOX_PUB_H

#include <stdint.h>
#include <zephyr/kernel.h>

/* Frames per gathered send, ceiling and default. 8 x 128 = 1 KB per send call:
 * big enough to flatten the per-send cost, small enough that an event frame
 * behind an in-flight bulk gather waits ~one frame time, not ~one burst. */
#define BOX_PUB_GATHER_MAX 8

/* Bring up the queues and start the publisher thread. Call once, after
 * box_uplink_init() -- the thread checks uplink readiness itself, so frames
 * enqueued before the first connect simply wait. */
void box_pub_init(void);

/* Queue one 128-byte frame. Both return 0 (accepted / bypass-sent) or <0 if
 * the frame was dropped instead (queue full; counted in stats). Safe from any
 * thread; O(memcpy). */
int box_pub_event(const uint8_t *f);
int box_pub_bulk(const uint8_t *f);

/* cmd/pubq/bypass -- send inline from the caller, pre-thread behaviour, so the
 * two arms can be A/B'd at RUNTIME with interleaved reps (reflashing between
 * arms would reset the box, re-anchor PTP and shift thermal state, confounding
 * exactly the difference being measured). */
void box_pub_set_bypass(int on);
int  box_pub_bypass(void);

/* cmd/pubq/gather -- frames per send call, 1..BOX_PUB_GATHER_MAX. 1 = the
 * frame-per-send arm, for measuring what gathering is worth. */
void box_pub_set_gather(int n);
int  box_pub_gather(void);

/* Block until both queues are empty and nothing is mid-send, or the timeout
 * lapses. For the frames that must be SEEN before the box goes away (ota/arm
 * ack before a trial reboot -- stage 2). 0 = drained, -1 = timed out. */
int box_pub_flush(k_timeout_t timeout);

typedef struct {
	uint32_t wait_last_us;    /* enqueue -> handed to the transport, last frame */
	uint32_t wait_max_us;     /* ...and the high-water mark                     */
	uint16_t ev_hwm;          /* deepest either queue has ever been             */
	uint16_t bulk_hwm;
	uint32_t ev_dropped;      /* event frames refused at enqueue (queue full)   */
	uint32_t bulk_dropped;    /* bulk frames displaced by drop-oldest           */
	uint32_t wire_dropped;    /* dequeued frames the transport then refused     */
	uint32_t gathers;         /* send calls issued...                           */
	uint32_t gather_frames;   /* ...and frames they carried (ratio = batching)  */
} box_pub_stats_t;

void box_pub_get_stats(box_pub_stats_t *out);

#endif /* BOX_PUB_H */
