#
# test_ess_roam.tcl
#
#  ess_roam: free 2-D locomotion in a bounded arena. Covers integration and
#  travel rate, immediate turning (the thing that separates roam from the
#  dial's spoke-bound `sectors` source), hold-on-release, the circular and
#  rectangular walls including SLIDING along a circular one, wall-contact
#  counting, path length, the region tests and the first-entry latch, the
#  state-machine discipline (woken on a region transition and nothing else),
#  and the analog `rate` source.
#
#  A UNIT test: dserv is STUBBED, so this runs under plain tclsh with no
#  server, no rig and no hardware -- the same arrangement as
#  test_ess_dial_dpad.tcl beside it. The timer is stubbed too, so time is
#  driven explicitly and the distances are exact rather than flaky.
#
#  Run as: tclsh tests/test_ess_roam.tcl        (or: ctest -R ess_roam)
#

set ::REPO [file normalize [file join [file dirname [info script]] ..]]

namespace eval ess {}
array set ::DP {}
array set ::DPBIN {}
proc dservSet { name val } { set ::DP($name) $val }
proc dservSetData { name ts type val } {
    set ::DP($name) $val
    set ::DPBIN($name) [list $ts $type $val]
    lappend ::POSE $val
}
set ::POSE {}
proc dservExists { name } { return [info exists ::DP($name)] }
proc dservGet { name } { return $::DP($name) }
proc dservTimestamp { name } { return $::CLOCK }
proc dservAddExactMatch { args } {}
proc dservAddMatch { args } {}
proc dpointAddScript { args } {}
proc dpointRemoveScript { args } {}
proc do_update {} { incr ::UPDATES }
set ::UPDATES 0

set ::CLOCK 1000000
proc now {} { return $::CLOCK }
proc advance_ms { ms } { incr ::CLOCK [expr {int($ms*1000)}] }

set ::TIMER ""
proc dservAfter { ms script } { set ::TIMER $script; return "t1" }
proc dservAfterCancel { id } { set ::TIMER "" }

# ::ess::stick_velocity lives in ess_transports, which drags in the whole
# input-routing file for two arithmetic procs. Copied verbatim rather than
# sourced, so this test stays a unit test -- and if the real one ever
# changes shape, the `rate` section below fails loudly rather than silently
# testing a stale contract.
namespace eval ess {
    proc stick_gain { f deadzone expo } {
        if { $f < $deadzone } { return 0.0 }
        set g [expr {($f - $deadzone)/(1.0 - $deadzone)}]
        if { $g <= 0.0 } { return 0.0 }
        if { $g > 1.0 } { set g 1.0 }
        if { $expo != 1.0 } { set g [expr {pow($g, $expo)}] }
        return $g
    }
    proc stick_velocity { x y scale deadzone expo rate } {
        if { $scale <= 0.0 } { return {0.0 0.0} }
        set mag [expr {sqrt($x*$x + $y*$y)}]
        if { $mag <= 0.0 } { return {0.0 0.0} }
        set g [stick_gain [expr {$mag/$scale}] $deadzone $expo]
        if { $g <= 0.0 } { return {0.0 0.0} }
        set speed [expr {$rate*$g}]
        return [list [expr {$x/$mag*$speed}] [expr {$y/$mag*$speed}]]
    }
}

source [file join $::REPO lib ess_roam-1.0.tm]

set FAIL 0
proc check { label got want } {
    if { $got eq $want } { puts "  ok   $label" } else {
        puts "  FAIL $label: got '$got' want '$want'"; set ::FAIL 1
    }
}
proc approx { label got want { tol 0.05 } } {
    if { abs($got - $want) <= $tol } { puts "  ok   $label ($got)" } else {
        puts "  FAIL $label: got $got want ~$want"; set ::FAIL 1
    }
}
proc fails { label script } {
    if { [catch { uplevel 1 $script } err] } {
        puts "  ok   $label ([string range $err 0 40]...)"
    } else {
        puts "  FAIL $label: did not raise"; set ::FAIL 1
    }
}

proc pos {}   { return [::ess::roam_pos] }
proc posx {}  { return [lindex [::ess::roam_pos] 0] }
proc posy {}  { return [lindex [::ess::roam_pos] 1] }
proc rad {}   { lassign [::ess::roam_pos] x y; return [expr {hypot($x,$y)}] }
proc deflect { sector } { ::ess::roam_dir ess/joystick/dir $sector }
proc release {} { ::ess::roam_dir ess/joystick/dir -1 }
proc hold_ms { ms { step 8 } } {
    for { set t 0 } { $t < $ms } { incr t $step } {
        if { $::TIMER eq "" } break
        advance_ms $step
        set s $::TIMER; set ::TIMER ""
        eval $s
    }
}

###########################################################################
puts "init + arena:"
###########################################################################

fails "a device word is refused with a pointer to the strategy word" {
    ::ess::roam_init -sources dpad -arena {circle 8}
}
fails "two steering sources are refused" {
    ::ess::roam_init -sources {sectors rate} -arena {circle 8}
}
fails "an unimplemented edge rule is refused" {
    ::ess::roam_init -sources sectors -arena {circle 8} -edge bounce
}

::ess::roam_init -sources sectors -arena {circle 8.0} -rate 8.0
check "active"           $::DP(ess/roam_active) 1
check "geometry published" $::DP(ess/roam/geometry) "circle,8.0"
check "pointer hidden at init" $::DP(ess/dial/pointer) "0.0000,0.0000,0,0"

fails "start with no arena refuses" {
    ::ess::roam_set_arena
    set save [::ess::roam_set_arena]
    set ::ess::roam_arena {}
    ::ess::roam_start
}
::ess::roam_set_arena circle 8.0

###########################################################################
puts "\ntravel: hold 'up' (sector 0) for 500 ms at 8 deg/s:"
###########################################################################

::ess::roam_place 0 0
::ess::roam_start
check "starts at the origin"       [pos] {0.0 0.0}
check "no timer until deflected"   $::TIMER ""
check "the start pose is recorded" [llength $::POSE] 1

deflect 0
check "deflection starts the timer" [expr {$::TIMER ne ""}] 1
hold_ms 500
approx "walked ~4 deg"        [rad]  4.0 0.1
approx "straight up: x ~ 0"   [posx] 0.0 0.01
approx "straight up: y ~ 4"   [posy] 4.0 0.1
approx "path length ~ travel" [::ess::roam_path_length] 4.0 0.1

###########################################################################
puts "\nturning is IMMEDIATE -- the whole difference from dial `sectors`:"
###########################################################################

set y_at_turn [posy]
deflect 2                                  ;# right
hold_ms 250
approx "y held where the turn happened" [posy] $y_at_turn 0.05
approx "moved right ~2 deg"             [posx] 2.0 0.1
check  "no retraction through the centre" \
    [expr {[posy] > 3.0}] 1

###########################################################################
puts "\nhold on release: progress is kept, travel stops:"
###########################################################################

release
check "timer cancelled" $::TIMER ""
set held [pos]
advance_ms 2000
check "no drift while centred" [pos] $held

###########################################################################
puts "\ndiagonals travel at the same SPEED, not sqrt(2) faster:"
###########################################################################

::ess::roam_place 0 0
::ess::roam_start
deflect 1                                  ;# up_right
hold_ms 500
approx "diagonal distance is still ~4 deg" [rad] 4.0 0.1
approx "45 degrees: x == y" [expr {[posx]-[posy]}] 0.0 0.01
release

###########################################################################
puts "\nthe circular wall stops the agent and counts the contact:"
###########################################################################

::ess::roam_place 0 0
::ess::roam_start
check "wall count reset by start" [::ess::roam_wall_contacts] 0
deflect 2                                  ;# right, arena radius 8
hold_ms 2000                               ;# 16 deg of travel into an 8 deg wall
approx "parked at the boundary" [posx] 8.0 0.01
approx "still on the x axis"    [posy] 0.0 0.01
check  "wall contact published" $::DP(ess/roam/wall) 1
check  "one contact, not one per tick" [::ess::roam_wall_contacts] 1
approx "path stopped accruing at the wall" [::ess::roam_path_length] 8.0 0.1

puts "\n... and the agent SLIDES along it rather than sticking:"
deflect 0                                  ;# up, while pinned at (8,0)
hold_ms 500
approx "still on the circle" [rad] 8.0 0.01
check  "climbed the wall"    [expr {[posy] > 1.0}] 1
check  "still one contact"   [::ess::roam_wall_contacts] 1

puts "\n... and coming off the wall re-arms the contact count:"
deflect 6                                  ;# left, back into the arena
hold_ms 300
check "off the wall"        $::DP(ess/roam/wall) 0
deflect 4 ; hold_ms 100
release

###########################################################################
puts "\na rectangular arena clamps per-axis:"
###########################################################################

::ess::roam_init -sources sectors -arena {rect -10 10 -5 5} -rate 8.0
::ess::roam_place 0 0
::ess::roam_start
deflect 1                                  ;# up_right, 45 deg
hold_ms 2000
approx "y pinned at the top"   [posy]  5.0 0.01
approx "x kept going to its own wall" [posx] 10.0 0.01
release

###########################################################################
puts "\nregions: entry latches, and ONLY a transition wakes the SM:"
###########################################################################

::ess::roam_init -sources sectors -arena {circle 8.0} -rate 8.0
::ess::roam_regions_clear
::ess::roam_patch_set 0 0.0  6.0 0.75      ;# up
::ess::roam_patch_set 1 6.0  0.0 0.75      ;# right
::ess::roam_place 0 0
::ess::roam_start
check "nothing entered yet" [::ess::roam_entered] -1

set ::UPDATES 0
deflect 2                                  ;# right, toward patch 1
hold_ms 500                                ;# 4 deg -- short of the patch
check "no wake while crossing empty arena" $::UPDATES 0
check "still nothing entered"              [::ess::roam_entered] -1

hold_ms 300                                ;# now ~5.4 deg, inside 6-0.75
check "entered patch 1"          [::ess::roam_entered] 1
check "in_region agrees"         [::ess::roam_in_region 1] 1
check "the other patch is not"   [::ess::roam_in_region 0] 0
check "exactly one wake"         $::UPDATES 1
set t_enter [::ess::roam_entered_time]
check "entry carries a timestamp" [expr {$t_enter > 0}] 1
lassign [split $::DP(ess/dial/pointer) ,] px py ps pb
check "pointer reports in_band"  $pb 1

hold_ms 400                                ;# through and out the far side
check "left the patch"           [::ess::roam_in_region 1] 0
check "leaving wakes too"        $::UPDATES 2
check "the latch keeps the FIRST entry" [::ess::roam_entered] 1
check "and its time"             [::ess::roam_entered_time] $t_enter
release

puts "\n... and starting INSIDE a patch is not an arrival:"
::ess::roam_place 6.0 0.0
set ::UPDATES 0
::ess::roam_start
check "seeded as inside"     [::ess::roam_in_region 1] 1
check "but nothing entered"  [::ess::roam_entered] -1
check "and the SM was not woken" $::UPDATES 0

puts "\n... an inactive region is not tested:"
::ess::roam_region_off 1
::ess::roam_place 0 0
::ess::roam_start
deflect 2 ; hold_ms 900
check "walked through a disabled patch" [::ess::roam_entered] -1
::ess::roam_region_on 1
release

###########################################################################
puts "\na held stick is adopted by roam_start:"
###########################################################################

::ess::roam_init -sources sectors -arena {circle 8.0} -rate 8.0
set ::DP(ess/joystick/dir) 2               ;# already pushed right
::ess::roam_place 0 0
::ess::roam_start
check "start adopts the held direction" [expr {$::TIMER ne ""}] 1
hold_ms 250
approx "and moves" [posx] 2.0 0.1
release
unset ::DP(ess/joystick/dir)

###########################################################################
puts "\nacceleration ramps the rate, and a new heading restarts the ramp:"
###########################################################################

::ess::roam_init -sources sectors -arena {circle 20.0} \
    -rate 4.0 -accel 8.0 -rate_max 12.0
::ess::roam_place 0 0
::ess::roam_start
deflect 2
hold_ms 1000
# rate goes 4 -> 12 over the first second, capped: the integral of
# min(4+8t, 12) over [0,1] is 4+8/2 = 8 (the cap is reached exactly at t=1)
approx "ramped travel, capped at 12 deg/s" [posx] 8.0 0.3
set x_at_turn [posx]
deflect 0
hold_ms 250
approx "the new heading starts back at the base rate" \
    [posy] [expr {4.0*0.25 + 8.0*0.25*0.25/2}] 0.15
release

###########################################################################
puts "\nthe rate source: the deflection vector IS the velocity:"
###########################################################################

fails "rate with no scale refuses to start" {
    ::ess::roam_init -sources rate -arena {circle 8.0}
    ::ess::roam_start
}

::ess::roam_init -sources rate -arena {circle 8.0} \
    -scale 1.0 -rate 10.0 -deadzone 0.0 -expo 1.0
::ess::roam_place 0 0
::ess::roam_start
# 25 ms steps, comfortably under the 50 ms max_dt so nothing is capped yet
::ess::roam_sample slider/position {0.0 0.0}     ;# establishes dt
advance_ms 25
::ess::roam_sample slider/position {1.0 0.0}     ;# full push right
approx "full push travels rate*dt" [posx] 0.25 0.01
advance_ms 25
::ess::roam_sample slider/position {0.5 0.0}     ;# half push
approx "half push, half the distance" [posx] 0.375 0.01
advance_ms 25
::ess::roam_sample slider/position {0.0 0.0}     ;# released
approx "at rest the agent holds" [posx] 0.375 0.01
check "nothing capped yet" [dict get [::ess::roam_tune] gaps] 0

puts "\n... a duplicate stamp is counted, not integrated:"
::ess::roam_sample slider/position {1.0 0.0}     ;# same clock as the last
approx "no travel on a zero dt" [posx] 0.375 0.01
check "duplicate counted" [dict get [::ess::roam_tune] dup_stamps] 1

puts "\n... and a long stall is CAPPED rather than discarded or teleported:"
advance_ms 500                                   ;# a 500 ms gap, max_dt 50 ms
::ess::roam_sample slider/position {1.0 0.0}
approx "caught up by max_dt only" [posx] 0.875 0.01
check "gap counted" [dict get [::ess::roam_tune] gaps] 1

###########################################################################
puts "\ndeinit asserts the gate and drops the wiring:"
###########################################################################

::ess::roam_deinit
check "gate published false" $::DP(ess/roam_active) 0
check "pointer hidden"       $::DP(ess/dial/pointer) "0.0000,0.0000,0,0"
check "not moving"           [::ess::roam_is_moving] 0

puts ""
if { $FAIL } { puts "FAILURES"; exit 1 }
puts "all checks passed"
exit 0
