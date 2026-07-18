/*
 * dserv_ble.h -- the extio BLE frame pipe's wire identity (BLE.md "Radio
 * protocol"). FROZEN 2026-07-17: these UUIDs are the pairing contract between
 * every handheld and every receiver ever flashed -- change them and old
 * handhelds go invisible. NUS-shaped (one notify + one write-without-response
 * characteristic) but deliberately NOT the NUS UUIDs: nothing else should
 * ever connect.
 *
 * The unit carried is the unmodified 128-byte dserv_msg frame, ONE per ATT
 * PDU -- no fragmentation layer. That requires ATT_MTU >= 131 (128 + the
 * 3-byte ATT header); both ends verify after the automatic MTU exchange and
 * refuse the pipe below that, loudly (bench boards negotiate 247).
 *
 * Byte order: the arrays below are BIG-ENDIAN (the canonical string order),
 * which is what btstack's uuid128 APIs take. Advertising payloads carry
 * 128-bit UUIDs REVERSED (little-endian) -- reverse_128() before memcmp when
 * matching adv reports.
 *
 * Pure data, no hardware -- but only the pico builds include it.
 */
#ifndef DSERV_BLE_H
#define DSERV_BLE_H

#include <stdint.h>
#include "dserv_msg.h"

/* Service d5e70001-8f2c-4b6a-9ae5-3c7a10a5b2c1 ("d5e7" ~ dserv extio) */
static const uint8_t DSERV_BLE_SVC_UUID[16] =
    { 0xd5,0xe7,0x00,0x01, 0x8f,0x2c, 0x4b,0x6a, 0x9a,0xe5, 0x3c,0x7a,0x10,0xa5,0xb2,0xc1 };
/* TX ...0002...: handheld -> receiver (NOTIFY; events/telemetry frames) */
static const uint8_t DSERV_BLE_TX_UUID[16] =
    { 0xd5,0xe7,0x00,0x02, 0x8f,0x2c, 0x4b,0x6a, 0x9a,0xe5, 0x3c,0x7a,0x10,0xa5,0xb2,0xc1 };
/* RX ...0003...: receiver -> handheld (WRITE_WITHOUT_RESPONSE; config/cmd frames) */
static const uint8_t DSERV_BLE_RX_UUID[16] =
    { 0xd5,0xe7,0x00,0x03, 0x8f,0x2c, 0x4b,0x6a, 0x9a,0xe5, 0x3c,0x7a,0x10,0xa5,0xb2,0xc1 };

#define DSERV_BLE_MTU_MIN  (DSERV_MSG_LEN + 3)   /* 131: one whole frame per ATT PDU */

/* Advertised name = "extio-<cfg name>" (scan response; the adv PDU itself
 * carries the service UUID, which is what the receiver matches on). */
#define DSERV_BLE_NAME_PFX "extio-"

#endif /* DSERV_BLE_H */
