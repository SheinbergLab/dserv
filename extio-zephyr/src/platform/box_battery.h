/*
 * box_battery.h -- what is left in the cell, for a box nobody can reach.
 *
 * WHY THIS EXISTS. The BLE peripherals are the only boxes in this fleet with no
 * cable and no console, and box_ble.c's adaptive-latency windows carry the
 * admission that they "remain UNTUNED here: they are pending a real battery-life
 * measurement, which no bench-powered board can provide." That measurement is
 * this file: LAT_SYNC_MS and LAT_IDLE_MS are trading accuracy for power against
 * numbers nobody has, and they cannot be tuned until a box can say what its own
 * discharge looks like.
 *
 * The estimate they are standing in for is ~1.1 mA at ble_latency 0, or roughly
 * two weeks on a 500 mAh cell, dominated by the 7.5 ms connection interval. That
 * is arithmetic over a datasheet event charge, not a measurement, and the point
 * of publishing state/batt/mv on the ordinary telemetry path is to replace it
 * with a curve.
 *
 * ONE READING PER MINUTE, ON A WORKQUEUE, NOT THE SERVICE LOOP. box_ain_borrow()
 * waits for the sampling thread to actually stop, which can park the caller for
 * a whole sampling period -- up to a second at ain_rate 1. The service loop is
 * the one place that must never block (see the loop_phase breadcrumb in main.c,
 * which exists because of exactly this class of stall), so the read runs on the
 * system workqueue and main.c publishes whatever the last completed read left
 * behind. Changed-only publishing makes that free: a cell moves a few mV an hour.
 *
 * THE CONVERTER IS BORROWED, NEVER JUST TAKEN. box_adc.h is explicit that the
 * sampling thread owns the device and that adc_read() on a suspended converter
 * blocks forever with no error and no log line. box_ain_borrow() is the only
 * sanctioned way in from another thread; it stops the sampler, waits for it to
 * be stopped, and hands the device over powered. A borrow that fails returns
 * -EBUSY and this module does NOTHING -- an unread battery is a missing
 * datapoint, which is recoverable, while a sweep racing the sampler is not.
 *
 * DEGRADES TO STUBS on any board that declares no `vbatt` node, which is every
 * board but the XIAO today. Same pattern as box_adc.c with no box_adc node: the
 * file always compiles, box_battery_present() answers 0, and nothing is
 * published -- rather than a Kconfig that has to be remembered per board.
 */
#ifndef BOX_BATTERY_H
#define BOX_BATTERY_H

#include <stdint.h>

/* Bring the divider up: configure the ADC channel and park the enable pin
 * inactive. Safe to call on a board with no cell. 0 if a battery can be read,
 * -ENODEV if none is declared, or a negative errno from the ADC/GPIO setup. */
int box_battery_init(void);

/* 1 when this board declares a cell AND init() succeeded. */
int box_battery_present(void);

/* Start a reading if one is due. Call from the service loop once a second: it
 * only ever submits work, so it cannot block. */
void box_battery_service(uint32_t now_ms);

/* The most recent completed reading.
 *
 * `mv` is the CELL, already scaled back through the divider; `raw` is the ADC
 * count that produced it. Both are published, and that is deliberate -- the
 * XIAO's divider ratio is copied from Seeed's Arduino variant rather than read
 * off a schematic (see the overlay), so raw is the number that stays true if the
 * ratio turns out to be wrong.
 *
 * Returns 0 when a reading has completed since boot, -EAGAIN before the first
 * one lands, -ENODEV where no cell is declared. */
int box_battery_get(uint32_t *mv, uint16_t *raw);

/* Nominal charge remaining, 0-100, from a piecewise-linear Li-ion curve at light
 * load.
 *
 * AN APPROXIMATION, AND SAY SO WHEREVER IT IS SHOWN. A lithium cell's voltage is
 * nearly flat across the middle of its discharge, so percent derived from it is
 * confident and wrong in the 40-80 band; it is honest near the knees, which is
 * where somebody actually cares. It is here because a fleet page wants one
 * number, not because it is a measurement -- state/batt/mv is the measurement.
 *
 * The table is nominal and should be REPLACED once a real discharge log exists,
 * which is the whole reason this module was built. */
uint8_t box_battery_pct(uint32_t mv);

/* BRING-UP ONLY: convert once with the divider's enable pin ASSERTED and once
 * DEASSERTED, returning both raw counts.
 *
 * The one measurement that separates "the resistor values are wrong" from "the
 * divider was never switched on" -- both of which yield a believable number from
 * a single read, and which need opposite fixes. Differing counts mean the pin
 * gates something and only the ratio is in question; identical counts mean the
 * pin does nothing and the pad is floating.
 *
 * Borrows the converter like an ordinary read, so it can return -EBUSY. Leaves
 * the divider switched off. */
int box_battery_probe(uint16_t *on, uint16_t *off);

/* How many reads have completed, and how many were refused because the sampler
 * would not release the converter. A pct that never moves with `reads` climbing
 * is a stuck divider; `busy` climbing instead is analog contention, and the two
 * want completely different fixes. */
void box_battery_stats(uint32_t *reads, uint32_t *busy);

#endif /* BOX_BATTERY_H */
