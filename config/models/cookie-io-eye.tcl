# cookie-io-eye -- cookie-io (config/models/cookie-io.tcl) plus a second
# analog stick, no select, standing in for an external eye tracker's
# analog out. A development box: it makes the rig read its EYE from the box.
#
# Everything in cookie-io, and:
#
#   A0/A1  eye v/h       ain ch 0/1, group `eye` -- em's default
#                        `eye ain_group`. The DFRobot stick's connector puts
#                        Y on the left of X, so Y lands on A0 and X on A1.
#
# THE LABELS DO THE SWAP. A block's columns always come in ascending channel
# order (the group's channels are a bit mask, so `channels 1,0` is the same
# group as `0,1`), so the wiring order is carried by the channel labels: em
# takes h and v from the columns labelled h|v (or x|y), see ain_axes_for in
# config/emconf.tcl, and reports its choice on em/ain_axes. Leave em's
# swap_axes OFF -- it applies on top of the labels, for a stick that is
# mounted rotated.
#
# A0/A1 are the right ports for it: dedicated analog pads with no GPIO
# function, so the eye costs no box pin and dodges A4's SJ8 and A5's SW2.
# Power the stick at 3.3 V -- the MCXN947 is not 5 V tolerant. Do not leave
# the adccal D1->A0 jumper on while the eye is on A0.
#
# A SEPARATE model on purpose: its rig half sets `eye source analog`, which
# changes where every protocol's eye comes from. Plain cookie-io must not do
# that to a rig that only wanted the buttons and the stick.
#
# Eye CALIBRATION is not carried here: em keeps it in db/calibration.db on
# the rig (Eye panel: set center, scale, invert/swap), not on the box, so
# there is no `calibrate em` to declare. The pointer stick is calibrated as
# in cookie-io.
#
# batch is left at the box default, 1 (one scan per block, lowest latency
# for an eye). It is not listed because the box's export omits default
# values, so a listed `batch 1` would read as missing on every judged box.
model cookie-io-eye {
    desc  "cookie-io + a second analog stick on A0/A1 as the eye signal (development)"
    board frdm_mcxn947
    match cookie-io-eye*
    box {
        pin/3/mode in
        pin/3/debounce_ms 20
        pin/3/label left_button
        pin/6/mode in
        pin/6/debounce_ms 20
        pin/6/label right_button
        pin/11/mode in
        pin/11/debounce_ms 20
        pin/11/active_low 1
        pin/11/label stick_button
        pin/15/mode ain
        pin/15/label y
        pin/16/mode ain
        pin/16/label x
        group/0/pins 3,6
        group/0/label response
        group/1/pins 11
        group/1/label stick_select
        obs/pin 10
        ain/enable 1
        ain/rate 250
        ain/label/0 v
        ain/label/1 h
        ain/label/3 y
        ain/label/4 x
        ain/group/0/channels 3,4
        ain/group/0/label stick
        ain/group/0/mode continuous
        ain/group/1/channels 0,1
        ain/group/1/label eye
        ain/group/1/mode continuous
    }
    rig {
        slider source extio
        slider ain_group stick
        joystick transport stick
        dial stick_commit stick_select
        button 0 box:*/response/left
        button 1 box:*/response/right
        eye source analog
        eye ain_group eye
    }
    calibrate slider
}
