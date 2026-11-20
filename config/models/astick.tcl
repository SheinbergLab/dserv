# astick -- the lab's Teensy 4.0 analog thumbstick.
#
# Stick on A5/A4 (ain ch 5 = x, 6 = y), its push-select on Teensy pin 6
# (box pin 10), wired to ground: input with pullup, active-low, one-pin chord
# group `stick_select` -- the name sliderconf pairs with the `stick` analog
# group (<group>_select) and dial stick_commit presses on.
#
# Box section = extio_cfg_export of a working unit (2026-11-20).
# Rig section = what makes it the rig's joystick. Calibration is NOT here:
# centre, orientation and throw are measured per physical stick.
model astick {
    desc  "Teensy 4.0 analog thumbstick with push-select"
    board teensy40
    match astick*
    box {
        pin/10/mode in_pullup
        pin/10/debounce_ms 10
        pin/10/active_low 1
        pin/10/label select
        group/0/pins 10
        group/0/label stick_select
        ain/enable 1
        ain/rate 100
        ain/oversample 32
        ain/label/5 x
        ain/label/6 y
        ain/group/0/channels 5,6
        ain/group/0/label stick
        ain/group/0/mode continuous
        ain/group/0/batch 2
    }
    rig {
        slider source extio
        slider ain_group stick
        joystick transport stick
        dial stick_commit stick_select
    }
}
