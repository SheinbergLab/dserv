#
# test_ess_dial_dpad.tcl
#
#  ess_dial dpad source: travel, hold-on-release, direction change through
#  the centre, off-axis gating, homing after a rejected reach, and
#  auto-commit at the target. The timer is stubbed so time is driven
#  explicitly and travel distances are exact rather than flaky.
#
#  A UNIT test: dserv is STUBBED, so this runs under plain tclsh with no
#  server, no rig and no hardware. That is the point -- the geometry and
#  the state machine are the parts worth pinning down, and they are pure
#  Tcl. The integration tests beside it (test_triggers, test_private) run
#  the other way, inside dserv via --tscript.
#
#  Run as: tclsh tests/test_ess_dial_dpad.tcl        (or: ctest -R ess_dial_dpad)
#

# repo root, from this script's location -- so it runs from anywhere
set ::REPO [file normalize [file join [file dirname [info script]] ..]]

namespace eval ess {}
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
proc send { args } {}
proc do_update {} { incr ::UPDATES }
set ::UPDATES 0

# controllable clock, in microseconds
set ::CLOCK 1000000   ;# never 0: dial_armed_time uses 0 as "disarmed"
proc now {} { return $::CLOCK }
proc advance_ms { ms } { incr ::CLOCK [expr {int($ms*1000)}] }

# one-shot timer stub
set ::TIMER ""
proc dservAfter { ms script } { set ::TIMER $script; return "t1" }
proc dservAfterCancel { id } { set ::TIMER "" }

namespace eval ess {
    proc slider_swipe_time {} { return 0 }
    proc joystick_reset {} {}
    proc joystick_response {} { return -1 }
}

source [file join $::REPO lib ess_dial-1.0.tm]

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
proc pointer {} { return $::DP(ess/dial/pointer) }
proc ptr_r {} {
    lassign [split [pointer] ,] x y s b
    return [expr {sqrt($x*$x + $y*$y)}]
}
# deflect to a sector, then let `ms` of holding elapse
proc deflect { sector } { ::ess::dial_dpad_dir ess/joystick/dir $sector }
proc release {} { ::ess::dial_dpad_dir ess/joystick/dir -1 }
proc hold_ms { ms { step 16 } } {
    for { set t 0 } { $t < $ms } { incr t $step } {
        if { $::TIMER eq "" } break
        advance_ms $step
        set s $::TIMER; set ::TIMER ""
        eval $s
    }
}

# ring at 10 deg, band 8..12, whole circle, 8 deg/s -> ~1.25 s to the ring
::ess::dial_init -sources dpad -ring_tolerance 2.0 -dpad_rate 8.0
::ess::dial_set_radius 10.0
::ess::dial_set_arc 0.0 180.0

puts "arm:"
::ess::dial_arm
check "dot starts at the origin" [pointer] "0.0000,0.0000,1,0"
check "no timer running until deflected" $::TIMER ""

puts "\ntravel: hold 'up' (sector 0) for 500 ms at 8 deg/s:"
deflect 0
check "deflection starts the timer" [expr {$::TIMER ne ""}] 1
hold_ms 500
approx "walked ~4 deg out" [ptr_r] 4.0 0.2
lassign [split [pointer] ,] px py
approx "straight up: x ~ 0" $px 0.0 0.05
approx "straight up: y ~ r"  $py 4.0 0.2
check  "not yet in band" [lindex [split [pointer] ,] 3] 0

puts "\nhold on release: progress is kept, travel stops:"
release
check "timer cancelled" $::TIMER ""
set r_at_release [ptr_r]
advance_ms 2000
approx "no drift while centred" [ptr_r] $r_at_release 0.001

puts "\nchanging direction comes back THROUGH the centre, never jumps:"
deflect 2                       ;# ask for right while parked up at ~4 deg
hold_ms 100
lassign [split [pointer] ,] px py
check  "still on the OLD spoke while retracting (x ~ 0)" \
    [expr {abs($px) < 0.01}] 1
check  "still above centre, not teleported across" [expr {$py > 0}] 1
check  "and moving inward" [expr {[ptr_r] < $r_at_release}] 1
hold_ms 600                     ;# through zero and out the new way
lassign [split [pointer] ,] px py
approx "now on the new spoke: y ~ 0" $py 0.0 0.05
check  "heading right" [expr {$px > 0}] 1
set r_out2 [ptr_r]

puts "\n  the crossing itself is continuous:"
::ess::dial_arm
deflect 0 ; hold_ms 400          ;# out to ~3.2 up
lassign [split [pointer] ,] px py
set prev_y $py
set max_step 0.0
deflect 4                        ;# straight DOWN: the opposite spoke
for { set i 0 } { $i < 80 } { incr i } {
    hold_ms 16
    lassign [split [pointer] ,] cx cy
    set d [expr {abs($cy - $prev_y)}]
    if { $d > $max_step } { set max_step $d }
    set prev_y $cy
}
# one tick at 8 deg/s is 0.128 deg; a re-aim would have stepped ~6.4 deg
check "no step bigger than one tick of travel" [expr {$max_step < 0.2}] 1
check "ended up below centre" [expr {[lindex [split [pointer] ,] 1] < 0}] 1

puts "\nauto-commit on reaching the ring:"
deflect 2                        ;# ask for right; comes back through centre first
set before $::UPDATES
hold_ms 5000
check "reached the band and woke the SM" [expr {$::UPDATES > $before}] 1
check "timer stopped at commit" $::TIMER ""
approx "committed angle is the spoke (right = 0 rad)" [::ess::dial_response] 0.0 0.01
check "source recorded" [::ess::dial_source] dpad
approx "committed at the target centre, not the near edge" [ptr_r] 10.0 0.2

puts "\nrejected reach: the return is required, never warped:"
::ess::dial_set_arc 0.0 180.0
::ess::dial_arm
deflect 0
hold_ms 3000                       ;# reach the ring and commit
::ess::dial_response               ;# protocol consumes it...
set r_out [ptr_r]
check "cursor is out at the target" [expr {$r_out > 9.5}] 1
::ess::dial_rearm                  ;# ...and rejects it
approx "rearm does NOT warp the cursor home" [ptr_r] $r_out 0.001
check "armed again" [::ess::dial_armed] 1

puts "\n  holding achieves nothing while homing:"
deflect 2
hold_ms 500
approx "still parked; a held stick does not move it" [ptr_r] $r_out 0.001
set before $::UPDATES
check "and cannot re-commit from out there" $::UPDATES $before

puts "\n  releasing walks it home:"
release
hold_ms 200
check "moving inward" [expr {[ptr_r] < $r_out - 1.0}] 1
hold_ms 3000
approx "arrived home" [ptr_r] 0.0 0.01
check "timer stopped at home" $::TIMER ""

puts "\n  and then a fresh reach works normally:"
set before $::UPDATES
deflect 0
hold_ms 3000
check "commits again after the return" [expr {$::UPDATES > $before}] 1
approx "committed up" [::ess::dial_response] 1.5708 0.01

puts "\nthe arc still gates a dpad commit:"
::ess::dial_set_arc 90.0 30.0    ;# up only, +/-30
::ess::dial_arm
deflect 4                        ;# straight DOWN: outside the arc
set before $::UPDATES
hold_ms 3000
check "no commit outside the arc" $::UPDATES $before
check "not in band despite reaching the radius" [lindex [split [pointer] ,] 3] 0
approx "parked at the band's outer edge" [ptr_r] 12.0 0.2

puts "\nacceleration (the digital-clock idiom):"
::ess::dial_init -sources dpad -ring_tolerance 2.0 \
    -dpad_rate 8.0 -dpad_accel 16.0 -dpad_rate_max 40.0
::ess::dial_set_radius 10.0
::ess::dial_set_arc 0.0 180.0
check "rate at t=0 is the base rate" [::ess::dial_dpad_rate_now 0] 8.0
check "rate after 1 s held"          [::ess::dial_dpad_rate_now 1] 24.0
check "capped"                       [::ess::dial_dpad_rate_now 10] 40.0
::ess::dial_arm
deflect 0
hold_ms 500
check "accelerated travel beats constant rate (4 deg)" \
    [expr {[ptr_r] > 4.5}] 1

puts "\ndisarm stops the walk:"
::ess::dial_arm
deflect 0
hold_ms 100
::ess::dial_disarm
check "timer cancelled by disarm" $::TIMER ""
check "dot hidden" [pointer] "0.0000,0.0000,0,0"

puts "\noff-axis directions do not move the cursor at all:"
::ess::dial_init -sources dpad -ring_tolerance 2.0 -dpad_rate 8.0
::ess::dial_set_radius 10.0
::ess::dial_set_arc 0.0 180.0
check "default is all eight live" [::ess::dial_dpad_sectors] ""
::ess::dial_dpad_sectors {0 2 4 6}          ;# cardinal only
check "reading is non-destructive" [::ess::dial_dpad_sectors] "0 2 4 6"
check "still set after the read"   [::ess::dial_dpad_sectors] "0 2 4 6"
check "live: up"        [::ess::dial_dpad_live 0] 1
check "dead: up_right"  [::ess::dial_dpad_live 1] 0

::ess::dial_arm
deflect 1                       ;# oblique: not on offer
hold_ms 500
check "an off-axis push never sets out" [ptr_r] 0.0
check "no timer left running"           $::TIMER ""

deflect 0                       ;# cardinal: live
hold_ms 300
set r_live [ptr_r]
check "a live direction travels" [expr {$r_live > 2.0}] 1

deflect 3                       ;# oblique again, mid-reach
hold_ms 500
approx "off-axis HOLDS, does not retract" [ptr_r] $r_live 0.001
check  "and does not re-aim" [expr {abs([lindex [split [pointer] ,] 0]) < 0.01}] 1

deflect 0                       ;# back to the live spoke
hold_ms 300
check "resumes from where it held" [expr {[ptr_r] > $r_live + 1.0}] 1

::ess::dial_dpad_sectors {}     ;# restore for later blocks

puts "\njoystick and dpad are mutually exclusive:"
check "both sources refused" \
    [catch { ::ess::dial_init -sources {joystick dpad} }] 1

puts ""
if { $FAIL } { puts "FAILED"; exit 1 } else { puts "all checks passed" }
