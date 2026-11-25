# photo-cmp -- FRDM-MCXN947 with a light sensor on A3, detected by the
# MCU's own comparator: the screen's light as a DIGITAL line for `photo din`,
# all of its processing on the box (docs/photo_processor.md, "The box's own
# comparator").
#
#   A3  sensor out      box pin 15 in `cmp` mode, label `photopatch`: ain ch 3
#                       sampled on the box AND CMP1_IN2 against the internal
#                       DAC; the comparator's edges are pin 15's DI events
#
# NO ANALOG ON THE WIRE. The box samples ch 3 for the comparator itself and
# sends only the edges, state/cmp, and state/cmp/level (each settled level in
# counts, about one per patch change -- photo_test reads dark/light/step from
# it, and data files log it). To SEE the analog (a transition's shape, an odd
# edge), add a group on ch 3 live from the config page and remove it after.
#
# Needs extio firmware 0.4.0+110 or later (a cmp pin samples its own
# channel; state/cmp/level). On +108/+109 the channel must also be in an ain
# group; before +108 `cmp` reads as `off`.
#
# THE THRESHOLD IS LEARNED, THEN LOCKED. The box watches ch 3 for plateaus
# (the light still for 3 ms): one level, a second >= 100 counts away, and a
# return to the first. Then it writes the DAC once, at their midpoint, arms
# the comparator, and holds that threshold for the session, like a pot. No
# edges are reported before it locks, so flip the patch a few times
# (`photo_test aim`) after a boot or a config change. Hysteresis is 20 mV.
# While locked, state/cmp reports `drift` (% of the step the levels' midpoint
# has moved from the threshold). To learn again: console `cmp relearn`, or
# publish to extio/<box>/cmd/cmp/relearn; it waits for the end of an obs.
# Box console `cmp` shows the levels, the DAC code, the drift and a mismatch
# count (nonzero = the DAC's reference is not the ADC's 3.3 V; try `cmp vref 1`).
#
# Polarity: written for an LM393 module's AO (falls with light), hence
# active_low 1 (the line reads 1 in light). The module's own comparator and DO
# are unused -- wire AO only, VCC to 3.3 V. For a sensor that RISES with light
# (an OPT101), set pin/15/active_low 0. `photo invert` is the processor's,
# for an analog group: a din line does not use it.
#
# Debounce 2 ms, as for the module: the box stamps the FIRST edge of a window.
#
# A3 is the only pin on this board that reaches both the ADC and a
# comparator (A5 does too, but it is SW2). The cookie-io models use A3 for the
# stick, so this is a dedicated photo box, like the other photo models.
# A comparator cannot follow a strobed backlight (Display++): use `photodiode`.
model photo-cmp {
    desc  "FRDM-MCXN947 + light sensor on A3, edges from the on-chip comparator"
    board frdm_mcxn947
    match {photo-cmp*}
    box {
        pin/15/mode cmp
        pin/15/label photopatch
        pin/15/debounce_ms 2
        pin/15/active_low 1
        ain/enable 1
        ain/rate 5000
        ain/label/3 light
    }
    rig {
        photo enabled 1
        photo din photopatch
    }
}
