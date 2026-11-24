# photo-lm393 -- FRDM-MCXN947 with an LM393 light-comparator module: the
# screen's light as a DIGITAL line, for `photo din` (docs/photo_processor.md,
# "A comparator module").
#
#   D0  module DO        DI pin 0, label `photopatch`: 1 = light (the module
#                        pulls DO low in light, so the pin is active-low)
#   A2  module AO        ain ch 2 = box pin 14, group `photodiode`: the
#                        analog behind the comparator, for checking it
#
# Wiring (the common 4-pin board: a clear photodiode, an LM393, a threshold
# pot and two LEDs): VCC to 3.3 V -- NOT 5 V, DO is pulled up to VCC and the
# MCXN947's pins are 3.3 V. GND to GND, DO to D0, AO to A2. A black-lensed
# diode is an IR part: it sees the rig's illuminator, not the screen.
#
# The threshold: flip the patch (the gear's "Test placement", or
# `photo_test aim`) and turn the pot until the board's DO LED follows the
# patch; then to the middle of the range where it does. The edge is where
# the light crosses that threshold, so the latency it gives includes where
# on the panel's transition the pot sits -- a constant for a given pot,
# panel and room light. Room light shifts it (0.5 ms on an LCD,
# 2026-11-24): shield the detector.
#
# Debounce 2 ms: the LM393 has no hysteresis, so a slow LCD transition
# chatters through the threshold. The box stamps the FIRST edge of a
# debounce window (extio-zephyr box_gpio.c), so the 2 ms delays the report,
# never the time. Two real changes closer than 2 ms merge -- no patch flips
# that fast.
#
# AO falls with light: `photo invert 1` lets the photo processor read it the
# right way up. It is there to check the comparator against the analog
# signal (log the group with `extio record` in a validation session); the
# pairing, verify, the light and the placement test read the line.
#
# A comparator cannot follow a strobed backlight (a CRS Display++): it
# would pulse every frame. Use an analog diode (model photodiode) and
# `photo strobe` there.
model photo-lm393 {
    desc  "FRDM-MCXN947 + LM393 light comparator: DO on D0 (the light line), AO on A2"
    board frdm_mcxn947
    match {photo-sync* photo-lm393*}
    box {
        pin/0/mode in
        pin/0/label photopatch
        pin/0/debounce_ms 2
        pin/0/active_low 1
        pin/14/mode ain
        pin/14/label light
        ain/enable 1
        ain/rate 5000
        ain/label/2 light
        ain/group/0/channels 2
        ain/group/0/label photodiode
        ain/group/0/mode continuous
        ain/group/0/batch 20
    }
    rig {
        photo enabled 1
        photo din photopatch
        photo group photodiode
        photo invert 1
    }
}
