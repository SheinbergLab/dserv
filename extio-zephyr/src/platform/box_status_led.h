/*
 * box_status_led.h -- the handheld's entire local UI, in one RGB LED.
 *
 * A BLE peripheral has no console anyone will be watching. It is in someone's
 * hand, across the room from the rig, running on a battery; the questions it
 * has to answer without a cable are "is it alive", "can the receiver see it",
 * and "is it actually linked". Those are precisely the states that a receiver
 * CANNOT report, because every one of them is a state in which it hears
 * nothing -- a peripheral that never advertised and a peripheral that is
 * switched off look identical from the other end. This module is the answer to
 * that, and it is why the LED earns a reserved pad rather than staying a
 * configurable output.
 *
 * The Pico handheld (wiznet-io, BLE.md "Status LED") reached the same
 * conclusion first, on a WS2812. THE PALETTE HERE IS DELIBERATELY ITS PALETTE
 * -- red fault, blue advertising, amber linking, green running -- because two
 * handhelds in one room that mean different things by the same colour would be
 * worse than either having no LED at all. What differs is the mechanism (three
 * plain GPIOs, no PIO, no dimming) and one added state this port could observe
 * and that one could not; see below.
 *
 * POWER IS PART OF THE DESIGN, not an afterthought. Without PWM the only lever
 * is duty cycle, so the two states a healthy handheld actually sits in are
 * BLIPS -- a few ms every 1.5-3 s, order 10 uA average against a coin cell's
 * self-discharge. The states that burn real current (solid amber, fast red) are
 * the ones where something is wrong and being noticed matters more than a week
 * of standby.
 */
#ifndef BOX_STATUS_LED_H
#define BOX_STATUS_LED_H

#include <stdint.h>

/* What the box is doing, worst-first. The order IS the priority: decide()
 * returns the first that applies.
 *
 * STALLED has no counterpart on the Pico handheld and is the one state worth
 * arguing for. "Connected" is not the same as "usable": the receiver has to
 * keep running the echo estimator or this box's timestamps decay into
 * meaningless numbers that still LOOK like timestamps (box_ble_periph.h: "a
 * frozen count with a live link is the fingerprint of a peripheral whose
 * timestamps are about to be meaningless"). That failure is otherwise
 * completely silent at both ends -- events keep flowing, nothing errors -- so
 * it is exactly the kind of thing a status light should be spending itself on.
 * It is amber, like the other not-yet-working link state, so anyone who has
 * never heard of echo-sync still reads it correctly as "not green, not right". */
typedef enum {
	BOX_STATUS_FAULT = 0,   /* radio would not start: this box is mute      */
	BOX_STATUS_INVISIBLE,   /* radio up, NOT advertising: nothing can find it */
	BOX_STATUS_ADVERTISING, /* visible, waiting for a receiver              */
	BOX_STATUS_LINKING,     /* connected, not subscribed yet (or MTU short) */
	BOX_STATUS_STALLED,     /* linked, but the clock estimator has stopped  */
	BOX_STATUS_READY,       /* linked, synced, events source-stamped        */
	BOX_STATUS_NSTATES
} box_status_t;

/* Claim the board's status LEDs (see box_gpio_fw_claim) and start rendering.
 * Call once at boot, AFTER box_gpio_init and BEFORE the reserved mask is read.
 * No-op on a board that declares no box-status-rgb. */
void box_status_led_init(void);

/* Re-evaluate and drive. Call once per service pass; cheap (a compare and, only
 * on a change, one GPIO write per colour). */
void box_status_led_service(void);

/* 1 iff this build actually took the board's LEDs. Lets the boot banner and
 * the console describe the pins as they REALLY are on this image rather than
 * telling an operator to configure a pad the firmware owns. */
int box_status_led_claimed(void);

/* The state currently displayed, for the console. */
box_status_t box_status_led_state(void);
const char *box_status_led_state_name(box_status_t st);

/* Bench override: hold a fixed colour (bits 1=R 2=G 4=B, 0 = all off) instead
 * of the state machine, or hand control back with auto=1.
 *
 * Exists because "the LED is not doing what I expect" has two causes that look
 * identical -- the state machine is wrong, or that colour has never lit on this
 * board -- and only one of them is a firmware bug. `led 2` settles it in a
 * second. Runtime only, never persisted: a handheld that boots into a forced
 * colour is a handheld whose status light lies. */
void box_status_led_override(int auto_mode, uint8_t rgb);
int  box_status_led_overridden(void);

#endif /* BOX_STATUS_LED_H */
