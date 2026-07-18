/*
 * box_ring.h -- lock-free SPSC byte ring for cross-core streams (log text,
 * console bytes). Exactly ONE producer context and ONE consumer context (a
 * core's mainline; never IRQ handlers). Power-of-2 capacity, free-running
 * 32-bit indices (wrap-safe subtraction), __dmb() pairs so the data is
 * visible before the index that publishes it.
 *
 * Overflow drops the NEWEST bytes (writer-side space check) and counts them:
 * a stalled consumer costs the producer nothing, which is the whole point --
 * printf on the RT core must never block on a sink.
 */
#ifndef BOX_RING_H
#define BOX_RING_H

#include <stdint.h>
#include "hardware/sync.h"      /* __dmb */

typedef struct {
    uint8_t          *buf;
    uint32_t          mask;     /* capacity-1 (capacity is a power of 2)   */
    volatile uint32_t head;     /* written by the producer only            */
    volatile uint32_t tail;     /* written by the consumer only            */
    volatile uint32_t drops;    /* bytes discarded on overflow (producer)  */
} box_ring_t;

static inline void box_ring_init(box_ring_t *r, uint8_t *buf, uint32_t size_pow2)
{ r->buf = buf; r->mask = size_pow2 - 1; r->head = r->tail = r->drops = 0; }

static inline uint32_t box_ring_used(const box_ring_t *r) { return r->head - r->tail; }
static inline uint32_t box_ring_free(const box_ring_t *r) { return (r->mask + 1) - box_ring_used(r); }

/* Producer side. Returns bytes accepted (rest counted in drops). */
static inline uint32_t box_ring_write(box_ring_t *r, const uint8_t *d, uint32_t n)
{
    uint32_t space = box_ring_free(r);
    if (n > space) { r->drops += n - space; n = space; }
    for (uint32_t i = 0; i < n; i++) r->buf[(r->head + i) & r->mask] = d[i];
    __dmb();                    /* data lands before the index that publishes it */
    r->head += n;
    return n;
}

/* Consumer side. Returns bytes copied out (0 = empty). */
static inline uint32_t box_ring_read(box_ring_t *r, uint8_t *d, uint32_t max)
{
    uint32_t avail = box_ring_used(r);
    uint32_t n = avail < max ? avail : max;
    __dmb();                    /* index read before the data it covers          */
    for (uint32_t i = 0; i < n; i++) d[i] = r->buf[(r->tail + i) & r->mask];
    __dmb();                    /* data consumed before the slot is recycled     */
    r->tail += n;
    return n;
}

#endif /* BOX_RING_H */
