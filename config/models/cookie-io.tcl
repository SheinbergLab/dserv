# cookie-io -- FRDM-MCXN947 under a DFRobot cookie I/O shield: an analog
# thumbstick plus a left/right response pair.
#
# The shield breaks out D3 D5 D6 D9 D10 D11 and A0-A5 (plus I2C and reset).
# On this board box pin n IS Arduino Dn and ain channel k IS Ak
# (extio-zephyr/PINMAP.md), so the leaves below read straight off the shield.
#
#   D3  left button      group `response`  -> button 0 (box:*/response/left)
#   D6  right button     group `response`  -> button 1 (box:*/response/right)
#   D11 stick push       group `stick_select` (active-low), the dial's commit
#   A3/A4  stick y/x     ain ch 3/4 = box pins 15/16 in ain mode, labelled
#                        x/y as on the astick
#
# Shield ports this model leaves alone, and why:
#   D5   RESERVED on the MCXN947 -- it is ENET0_MDIO, the PHY's; the box
#        refuses `pin 5`. That cookie port is dead on this board.
#   D10  the board's GREEN LED; the obs mirror is put here so the LED lights
#        for each observation period (PINMAP's intended "box is running"
#        tell). A module plugged into D10 would fight the obs output.
#   D9   the RED LED. Free, but whatever is plugged in lights the LED too.
#        Both LED pads are GPIO_ACTIVE_LOW in the pin map: logical 1 = pad
#        LOW, so a module on either reads (and drives) inverted.
#
# obs here is a MIRROR (the LED follows ess/in_obs as frames arrive), which
# is all a USB box with software sync can do. A box that LEADS obs needs
# Ethernet + a verified PTP lock and a non-LED pin (the LED pads would give
# an inverted TTL) -- that is a different model.
#   A5   is also SW2: pressing the board's user button shorts the A5 port to
#        ground. Use it last.
#   A4   reaches the header through solder jumper SJ8 (1-2 = ADC). A board with
#        SJ8 flipped reads a disconnected stick axis, silently.
#   D6   carries the BLUE LED, so the right button lights it -- harmless.
#
# Labels: button pins token-match, so left_button resolves for `left`; group
# names are exact, hence `response`. Stick axis labels are cosmetic -- the
# slider picks columns by index and calibration decides which is which, then
# stores the result on the unit as meta/cal/slider.
#
# `match` is NOT cookie-io*: that would also claim cookie-io-eye
# (cookie-io-eye.tcl), and a blank box of that name would then be offered
# this model, because fewer missing leaves wins a tie between named models.
model cookie-io {
    desc  "FRDM-MCXN947 + DFRobot cookie I/O: analog stick and left/right buttons"
    board frdm_mcxn947
    match {cookie-io cookie-io[0-9]* cookie-io-[0-9]*}
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
        ain/label/3 y
        ain/label/4 x
        ain/group/0/channels 3,4
        ain/group/0/label stick
        ain/group/0/mode continuous
    }
    rig {
        slider source extio
        slider ain_group stick
        joystick transport stick
        dial stick_commit stick_select
        button 0 box:*/response/left
        button 1 box:*/response/right
    }
    calibrate slider
}
