#
# test_ess_dial.tcl
#
#  ess_dial mouse source: scale, the ring band, lock-on across a chord,
#  and the one output contract (nothing writes ess/cursor).
#
#  A UNIT test: dserv is STUBBED, so this runs under plain tclsh with no
#  server, no rig and no hardware. That is the point -- the geometry and
#  the state machine are the parts worth pinning down, and they are pure
#  Tcl. The integration tests beside it (test_triggers, test_private) run
#  the other way, inside dserv via --tscript.
#
#  Run as: tclsh tests/test_ess_dial.tcl        (or: ctest -R ess_dial)
#

# repo root, from this script's location -- so it runs from anywhere
set ::REPO [file normalize [file join [file dirname [info script]] ..]]

namespace eval ess {
    variable screen_halfx 16.0
    variable screen_halfy 9.0
}
array set ::DP {}
set ::DP(ess/screen_halfx) 16.0
set ::DP(ess/screen_halfy) 9.0
proc dservSet { name val } { set ::DP($name) $val }
proc dservExists { name } { return [info exists ::DP($name)] }
proc dservGet { name } { return $::DP($name) }
proc dservTimestamp { name } { return 0 }
proc dservAddExactMatch { args } {}
proc dservAddMatch { args } {}
proc dpointAddScript { args } {}
proc dpointRemoveScript { args } {}
proc now {} { incr ::CLOCK 1000 ; return $::CLOCK }
set ::CLOCK 0
proc do_update {} { incr ::UPDATES }
set ::UPDATES 0
proc send { args } {}
namespace eval ess {
    proc slider_swipe_time {} { return 0 }
    proc joystick_reset {} {}
    proc joystick_response {} { return -1 }
}

# ess_dial declares its routing on the settings API (`setting dial
# sources`). Stub the package: declare is a no-op and get errors, which
# the module treats as its own documented state -- "a bare interp with
# no rig file is a normal way to load this module".
package provide settings 1.0
namespace eval settings {
    proc declare { args } {}
    proc get { args } { error "no settings in a unit test" }
}

source [file join $::REPO lib ess_dial-1.0.tm]

set FAIL 0
proc check { label got want } {
    if { $got eq $want } {
        puts "  ok   $label"
    } else {
        puts "  FAIL $label: got '$got' want '$want'"
        set ::FAIL 1
    }
}
proc approx { label got want { tol 0.05 } } {
    if { abs($got - $want) <= $tol } {
        puts "  ok   $label ($got)"
    } else {
        puts "  FAIL $label: got $got want ~$want"
        set ::FAIL 1
    }
}

# Angles live in [0,2pi), so 6.2815 and 0.0 are the SAME angle -- compare
# them the way the dial does.
proc approx_ang { label got want { tol 0.05 } } {
    set d [expr {abs(atan2(sin($got-$want), cos($got-$want)))}]
    if { $d <= $tol } {
        puts "  ok   $label ($got)"
    } else {
        puts "  FAIL $label: got $got want ~$want (off by $d)"
        set ::FAIL 1
    }
}

# A 1024x600 declared mouse extent on a 32x18 degree screen.
proc feed { x y ev } { ::ess::dial_mouse_sample mouse/event [list $x $y $ev] }
proc pointer {} { return $::DP(ess/dial/pointer) }
proc cursor {} { return $::DP(ess/cursor) }

::ess::dial_init -sources mouse -ring_tolerance 2.0
::ess::dial_mouse_range mouse/event/range {0 1023 0 599}
::ess::dial_set_radius 8.0
::ess::dial_set_arc 0.0 180.0

puts "scale + arm:"
check "pointer hidden before arm" [pointer] "0.0000,0.0000,0,0"
::ess::dial_arm
check "dot at origin on arm" [pointer] "0.0000,0.0000,1,0"

puts "\nfree dot, whole circle:"
feed 800 300 3
lassign [split [pointer] ,] px py show band
approx "x degrees at 800px" $px 9.02
approx "y degrees at centre row" $py 0.0
check  "in band pointing right" $band 1
check "nothing writes ess/cursor any more" [dservExists ess/cursor] 0

puts "\ninside the ring, never engaged:"
::ess::dial_arm       ;# clear the lock the previous block engaged
feed 520 300 3        ;# ~0.27 deg from centre: inside the ring
lassign [split [pointer] ,] px py show band
check "dot still shown" $show 1
check "not in band" $band 0
set before $::UPDATES
feed 520 300 0        ;# press at centre
check "press at centre reports nothing" $::UPDATES $before
check "nothing pending" [set ::ess::dial_pending] ""

puts "\npress on target:"
feed 800 300 3        ;# reach the band: engages
feed 800 300 0
check "press in band woke the SM" [expr {$::UPDATES > $before}] 1
approx_ang "reported angle" [::ess::dial_response] 0.0
check  "source recorded" [::ess::dial_source] mouse

puts "\npast the ring band is rejected too:"
::ess::dial_arm
feed 1010 300 3        ;# ~15.6 deg out: beyond radius+tolerance
check "outside the band not in band" [lindex [split [pointer] ,] 3] 0
set before $::UPDATES
feed 1010 300 0
check "press beyond the ring ignored" $::UPDATES $before

puts "\nrestricted arc rejects, does not clamp:"
::ess::dial_arm
::ess::dial_set_arc 90.0 30.0      ;# straight up, +/-30 deg
feed 800 300 3                     ;# pointing RIGHT: 90 deg out of arc
check "out of arc not in band" [lindex [split [pointer] ,] 3] 0
set before $::UPDATES
feed 800 300 0
check "press out of arc ignored" $::UPDATES $before
check "dial still armed" [::ess::dial_armed] 1
feed 511 50 3                      ;# straight up, r~7.5 deg: inside the band
lassign [split [pointer] ,] px py show band
check  "in band pointing up" $band 1
feed 511 50 0
approx_ang "reported angle up" [::ess::dial_response] 1.5708

puts "\nlock-on: reach the ring once, then the radius is free:"
::ess::dial_arm
::ess::dial_set_arc 0.0 180.0
feed 700 300 3                     ;# r~3.1 deg: inside the ring, not engaged
check "not engaged before touching the band" [lindex [split [pointer] ,] 3] 0
set before $::UPDATES
feed 700 300 0
check "press before engaging is ignored" $::UPDATES $before
feed 800 300 3                     ;# r~9.0: on the band -> engage
check "engaged on reaching the band" [lindex [split [pointer] ,] 3] 1
feed 700 300 3                     ;# the CHORD: back inside the ring
check "stays engaged on the chord" [lindex [split [pointer] ,] 3] 1
feed 700 300 0
approx_ang "commits from off the ring once engaged" [::ess::dial_response] 0.0

puts "\nthe arc still gates after lock-on:"
::ess::dial_arm
::ess::dial_set_arc 270.0 60.0     ;# straight down, +/-60
feed 511 500 3                     ;# straight down, r~6.0: on the band
check "engaged" [lindex [split [pointer] ,] 3] 1
feed 800 300 3                     ;# swing to straight RIGHT: out of arc
check "lock does not survive leaving the arc" [lindex [split [pointer] ,] 3] 0
set before $::UPDATES
feed 800 300 0
check "press outside the arc still ignored" $::UPDATES $before

puts "\na new response window starts unengaged:"
::ess::dial_arm
::ess::dial_set_arc 0.0 180.0
feed 700 300 3
check "lock cleared by dial_arm" [lindex [split [pointer] ,] 3] 0

puts "\ndisarm clears the dot:"
::ess::dial_arm
feed 511 50 3
::ess::dial_disarm
check "dot hidden" [pointer] "0.0000,0.0000,0,0"
check "ess/cursor still never written" [dservExists ess/cursor] 0

puts "\narc mode: hidden at arm, relative from the first movement:"
::ess::dial_init -sources mouse -ring_tolerance 2.0
::ess::dial_mouse_range mouse/event/range {0 1023 0 599}
::ess::dial_set_radius 8.0
::ess::dial_set_arc 270.0 120.0
check "pointer mode by default" [::ess::dial_set_mouse_mode] pointer
check "a bad mode is loud" [catch { ::ess::dial_set_mouse_mode free }] 1
::ess::dial_set_mouse_mode arc
::ess::dial_arm
check "nothing drawn when the window opens" [lindex [split [pointer] ,] 2] 0
set before $::UPDATES
feed 1000 300 0                    ;# click before any movement
check "a click before moving chooses nothing" $::UPDATES $before
check "nothing pending" [set ::ess::dial_pending] ""
# the device cursor sits FAR from centre (a recentre that did not take):
# the first movement must still show the catcher at the arc centre
feed 1000 300 3
lassign [split [pointer] ,] px py show band
check  "first movement shows the catcher" $show 1
check  "in band" $band 1
approx "at the arc centre, x" $px 0.0 0.01
approx "at the arc centre, y" $py -8.0 0.01
feed 900 300 3                     ;# 100 counts LEFT of the first sample
lassign [split [pointer] ,] px py show band
approx_ang "moves by travel: -100/511.5 of the half-arc" \
    [expr {atan2($py, $px)}] [expr {(270.0 - 120.0*100/511.5)*3.14159265358979/180}]
feed 0 300 3                       ;# far left: clamped at the arc end
lassign [split [pointer] ,] px py show band
approx_ang "clamped to the left end (150 deg)" [expr {atan2($py, $px)}] \
    [expr {150.0*3.14159265358979/180}]
feed 0 300 0
approx_ang "a click reports where the catcher is" [::ess::dial_response] \
    [expr {150.0*3.14159265358979/180}]
::ess::dial_arm
check "the next window is hidden again" [lindex [split [pointer] ,] 2] 0
feed 200 300 3
lassign [split [pointer] ,] px py show band
approx "and re-anchors at its own first movement" $px 0.0 0.01
::ess::dial_init -sources mouse -ring_tolerance 2.0
check "dial_init resets to pointer" [::ess::dial_set_mouse_mode] pointer

puts "\nwhy these sources, not just which:"
::ess::dial_bind {}                     ;# no rig declaration
::ess::dial_init -ring_tolerance 2.0
check "default: the module's own" [dservGet ess/dial/sources] "swipe touch"
check "origin says so"            [dservGet ess/dial/source_origin] "default"
check "no binding published"      [dservGet ess/dial/bound] ""

::ess::dial_bind {mouse touch}          ;# the rig declares
check "dial_bind publishes immediately" [dservGet ess/dial/bound] "mouse touch"
::ess::dial_init -ring_tolerance 2.0
check "rig binding wins over the default" [dservGet ess/dial/sources] "mouse touch"
check "origin: rig"                       [dservGet ess/dial/source_origin] "rig"

::ess::dial_init -sources dpad -ring_tolerance 2.0   ;# protocol overrides
check "protocol wins over the rig"  [dservGet ess/dial/sources] "sectors"
check "origin: protocol"            [dservGet ess/dial/source_origin] "protocol"
check "and the rig binding is still visible beside it" \
    [dservGet ess/dial/bound] "mouse touch"
::ess::dial_deinit
check "deinit clears the effective sources" [dservGet ess/dial/sources] ""
check "but the rig binding outlives the system" \
    [dservGet ess/dial/bound] "mouse touch"
::ess::dial_bind {}                     ;# restore for later blocks

puts "\none contract: a steering source places its cursor on the ring:"
::ess::dial_init -sources swipe -ring_tolerance 2.0
::ess::dial_set_radius 8.0
::ess::dial_set_arc 0.0 180.0
::ess::dial_arm
::ess::dial_show [expr {3.14159265358979/2}]     ;# straight up
lassign [split [pointer] ,] px py show band
approx "placed at the ring radius, x" $px 0.0 0.01
approx "placed at the ring radius, y" $py 8.0 0.01
check  "shown" $show 1
check  "a steered angle is always reportable" $band 1
check  "and still no ess/cursor" [dservExists ess/cursor] 0
::ess::dial_hide
check "dial_hide clears the same datapoint" [pointer] "0.0000,0.0000,0,0"

puts "\na dial with no radius cannot place a cursor:"
::ess::dial_init -sources swipe
check "dial_arm refuses" [catch { ::ess::dial_arm } msg2] 1
check "message names the radius" [string match "*ring radius*" $msg2] 1

puts "\na radius with no TOLERANCE is an error, not a silent accept-nothing:"
::ess::dial_init -sources mouse
::ess::dial_mouse_range mouse/event/range {0 1023 0 599}
::ess::dial_set_radius 8.0          ;# radius fine, tolerance still 0
check "dial_arm refuses" [catch { ::ess::dial_arm } msg] 1
check "message names the tolerance" [string match "*tolerance*" $msg] 1

check "-start_radius is loud" \
    [catch { ::ess::dial_init -sources mouse -start_radius 40 }] 1
check "-wedge_inner is gone" \
    [catch { ::ess::dial_init -sources mouse -wedge_inner 2 }] 1

puts ""
if { $::FAIL } { puts "FAILED" ; exit 1 } else { puts "all checks passed" }
