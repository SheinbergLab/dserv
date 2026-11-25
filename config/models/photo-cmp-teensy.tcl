# photo-cmp-teensy -- Teensy 4.0/4.1 with a light sensor on A0, detected by
# the MCU's own comparator: the same job as photo-cmp (FRDM-MCXN947), on a
# USB box. See photo-cmp.tcl for the whole story; what differs is below.
#
#   A0 (pin 14)  sensor out   ain ch 7, sampled on the box, AND CMP1_IN3
#                             against the comparator's internal DAC
#   box pin 15                the comparator's line, mode `cmp`, label
#                             `photopatch`: its edges are pin 15's DI events.
#                             Pin 15 is a pad with no Teensy pin -- on this
#                             board analog pads are not box pins, so the line
#                             lives on one nothing else can use
#
# Wire: sensor AO -> pin 14 (A0), VCC -> 3.3V (NOT 5 V: A0 is not 5 V
# tolerant), GND -> GND. An LM393 module's DO stays unconnected.
#
# The DAC is 6-bit (~52 mV a step), so the box needs dark and light at least
# 192 counts apart to learn (photo-cmp: 100); photo_test knows this from the
# board. Bench, 2026-11-25, Teensy 4.1 + LM393 module held on the stim2 patch:
# dark ~4085, light ~3250, locked at dac 227 (3648 counts), mismatch 0 with
# the default `cmp vref 1`.
#
# Needs extio firmware 0.4.0+115 or later: +113 adds the Teensy comparator,
# +114 lets learning recover from a stale first level (a pin read before the
# sensor was wired), +115 finds the channel when the config is loaded at
# boot (on +113/+114 a saved cmp config reads `no_samples` until re-set).
model photo-cmp-teensy {
    desc  "Teensy 4.x + light sensor on A0, edges from the on-chip comparator"
    board {teensy40 teensy41}
    match {photo-cmp*}
    box {
        pin/15/mode cmp
        pin/15/label photopatch
        pin/15/debounce_ms 2
        pin/15/active_low 1
        ain/enable 1
        ain/rate 5000
        ain/label/7 light
    }
    rig {
        photo enabled 1
        photo din photopatch
    }
}
