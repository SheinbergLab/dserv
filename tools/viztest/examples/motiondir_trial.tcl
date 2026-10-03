#
# viztest scenario: joystick/motiondir
#
#   tclsh9.0 viztest.tcl -v -scenario examples/motiondir_trial.tcl \
#       -html /tmp/motiondir.html ~/systems/ess/joystick/motiondir/motiondir.tcl
#
# Builds a small stimdg by hand (row 0 canonical with targets up/right,
# row 1 continuous) instead of needing the protocol loaded, then plays
# trials through the events joystick.tcl emits, including the cursor
# feed on ess/dial/pointer. Swap the stimdg block for `-stimdg live`
# when motiondir is what ess has loaded.
#

set pi 3.14159265358979
set g [dg_create stimdg]
dl_set $g:stimtype       [dl_ilist 0 1]
dl_set $g:direction_mode [dl_slist canonical continuous]
dl_set $g:sectors   [dl_llist [dl_ilist 0 2] [dl_ilist -1 -1]]
dl_set $g:target_x  [dl_llist [dl_flist 0 8] [dl_flist [expr {8*cos(0.5)}] [expr {8*cos(3.0)}]]]
dl_set $g:target_y  [dl_llist [dl_flist 8 0] [dl_flist [expr {8*sin(0.5)}] [expr {8*sin(3.0)}]]]
dl_set $g:correct_direction_rad [dl_flist [expr {$pi/2}] 0.5]
dl_set $g:target_scale [dl_flist 2 2]
dl_set $g:target_ecc   [dl_flist 8 8]
dl_set $g:coherence    [dl_flist 0.25 1.0]
dl_set $g:patch_x      [dl_flist 0 0]
dl_set $g:patch_y      [dl_flist 0 0]
dl_set $g:patch_size   [dl_flist 5 5]

setdp ess/params {cursor_scale 0.45}

# one trial: targets on, cursor moves, response, outcome, targets off.
# endtrial subtype is CORRECT or ABORT, as joystick.tcl emits it.
proc trial { row resp outcome } {
    fire BEGINOBS * 0
    fire STIMTYPE STIMID $row
    fire TARGET ON
    dpoint ess/dial/pointer 0.5,2.0,1,0
    dpoint ess/dial/pointer 0.5,6.0,1,1
    if { $resp ne "" } { fire RESP ACT $resp }
    dpoint ess/dial/pointer 0.5,6.0,0,0     ;# cursor hidden on feedback
    fire ENDTRIAL $outcome
    dpoint ess/dial/pointer 0.6,6.1,0,0     ;# a late update must not undo it
    snap "row $row resp $resp $outcome"
}

# filled discs in a frame: 2 targets + the patch, plus 1 per highlight
proc discs {} { regexp -all {fcircle} [summary] }

trial 0 0 CORRECT
check {[set [vns]::outcome] == 1}    "correct canonical trial ends correct"
check {[string match *CORRECT* [summary]]} "status line says CORRECT"
check {[discs] == 4}                 "the chosen target is highlighted"

trial 0 4 ABORT
check {[set [vns]::outcome] == 0}    "strict off-target ends as an abort"
check {[discs] == 3}                 "off-target response highlights no target"

trial 0 "" ABORT
check {[string match {*NO RESPONSE*} [summary]]} "timeout says NO RESPONSE"

trial 1 29 CORRECT
check {[set [vns]::outcome] == 1}    "continuous trial maps the bearing to the correct target"

zoom 1.5
check {[string match *CORRECT* [summary]]} "a zoom redraw keeps the outcome"

fire TARGET OFF
check {[summary] eq ""}              "targets off clears the panel"
