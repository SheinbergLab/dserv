# photo-cmp -- FRDM-MCXN947 with a light sensor on A3, detected by the
# MCU's own comparator: the screen's light as a DIGITAL line for `photo din`,
# with the analog kept alongside (docs/photo_processor.md, "The box's own
# comparator").
#
#   A3  sensor out      box pin 15 in `cmp` mode, label `photopatch`: ain ch 3
#                       (group `photodiode`) AND CMP1_IN2 against the internal
#                       DAC; the comparator's edges are pin 15's DI events
#
# Needs extio firmware with PIN_MODE_CMP (2026-11-24 or later). Older firmware
# reads `cmp` as `off`.
#
# THE THRESHOLD SETS ITSELF. The box watches ch 3 for plateaus (the light
# still for 3 ms), keeps the latest dark and light level, and puts the DAC at
# their midpoint -- moving it only while the light is still. No edges are
# reported until it has seen both levels, so flip the patch a few times
# (`photo_test aim`) after a boot or a config change. Hysteresis is 20 mV.
# Box console `cmp` shows the learned levels, the DAC code and a mismatch
# count (nonzero = the DAC's reference is not the ADC's 3.3 V; try `cmp vref 1`).
# Unlike a pot, the threshold follows room light, so the edge stays at the
# middle of each transition.
#
# Polarity: written for an LM393 module's AO (falls with light), hence
# active_low 1 and `photo invert 1`. The module's own comparator and DO are
# unused -- wire AO only, VCC to 3.3 V. For a sensor that RISES with light
# (an OPT101), set pin/15/active_low 0 and drop `photo invert`.
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
        ain/group/0/channels 3
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
