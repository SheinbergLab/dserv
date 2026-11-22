# photodiode -- FRDM-MCXN947 with an OPT101 photodiode module on A2, read by
# dserv's `photo` processor (docs/photo_processor.md) for screen light edges.
#
#   A2  OPT101 OUT       ain ch 2 = box pin 14 in ain mode, group `photodiode`
#                        (the photo gear's default `group`)
#
# Wiring (an Elecbee/WCMCU-101 OPT101 board): VCC to 3.3 V -- NOT 5 V; on
# 3.3 V the OPT101 cannot drive past ~2.1 V, so the MCXN947's 3.3 V full
# scale is never exceeded and no divider is needed. GND to GND, OUT to A2.
# The OPT101's internal 1 MOhm feedback works only with its `1M` pin tied to
# OUT (pin 4 to pin 5): check the module's jumper, or bridge them. Tie COM and
# -V to GND. Shield the clear package from room light.
#
# A2 because it is free in both cookie-io models (A0/A1 eye, A3/A4 stick), so
# one box can carry either shield. Not A5: the board's SW2 shorts it.
#
# Sampling: 5 kHz on one channel, batch 20 -> 250 blocks/s, the rate the eye
# feed has proven on this box (the box's ceiling is 2000 blocks/s in total).
# Batching delays when an edge is PUBLISHED by up to 4 ms; it never changes an
# edge's times, which come from the box's per-sample stamps.
#
# Rig: `photo enabled` turns detection and logging on. The patch (`photo
# patch`) is left to the person: it flashes a corner of the SUBJECT's screen,
# and a diode can sit over the stimulus instead.
#
# Switch to and from cookie-io / cookie-io-eye with extio_model_switch (or
# Switch on the box's config page): the stick, buttons and eye group are
# cleared, and cookie-io-eye's `eye source analog` is cleared from the rig.
model photodiode {
    desc  "FRDM-MCXN947 + OPT101 photodiode on A2, for the photo processor"
    board frdm_mcxn947
    match {photodiode photodiode[0-9]* photodiode-*}
    box {
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
        photo group photodiode
    }
}
