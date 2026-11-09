#
# test_ess_dial_threshold.tcl
#
#  ess_dial threshold source: the first sample past a fraction of measured
#  throw reports that sample's bearing, and a later sample does not move it.
#
#  A UNIT test: dserv is STUBBED. Run as:
#    tclsh tests/test_ess_dial_threshold.tcl
#

set ::REPO [file normalize [file join [file dirname [info script]] ..]]

namespace eval ess {
    variable screen_halfx 16.0
    variable screen_halfy 9.0
}
array set ::DP {}
set ::DP(ess/screen_halfx) 16.0
set ::DP(ess/screen_halfy) 9.0
set ::TS 0
proc dservSet { name val } { set ::DP($name) $val }
proc dservExists { name } { return [info exists ::DP($name)] }
proc dservGet { name } { return $::DP($name) }
proc dservTimestamp { name } { return $::TS }
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
    proc dial_touch_timestamp {} { return 0 }
}

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
proc approx { label got want { tol 1e-9 } } {
    if { abs($got - $want) <= $tol } {
        puts "  ok   $label ($got)"
    } else {
        puts "  FAIL $label: got $got want ~$want"
        set ::FAIL 1
    }
}
proc approx_ang { label got want { tol 1e-9 } } {
    set d [expr {abs(atan2(sin($got-$want), cos($got-$want)))}]
    if { $d <= $tol } {
        puts "  ok   $label ($got)"
    } else {
        puts "  FAIL $label: got $got want ~$want (off by $d)"
        set ::FAIL 1
    }
}
proc fails { label script } {
    if { [catch { uplevel 1 $script } msg] } {
        puts "  ok   $label"
    } else {
        puts "  FAIL $label: expected an error"
        set ::FAIL 1
    }
}

proc pointer {} { return $::DP(ess/dial/pointer) }
proc feed { x y } {
    set ::DP(slider/position) [list $x $y]
    ::ess::dial_threshold_sample slider/position [list $x $y]
}
proc arm_ready {} {
    # A stick left out by the previous block would commit on arm. Start centered.
    set ::DP(slider/position) {0.0 0.0}
    set ::TS 0
    ::ess::dial_init -sources threshold -threshold_scale 10.0
    ::ess::dial_set_arc 0.0 180.0
}

proc expect_ang { y x } {
    return [::ess::dial_norm2pi [expr {atan2($y, $x)}]]
}

puts "below the fraction:"
arm_ready
set ::TS 5000
::ess::dial_arm
feed 7.0 0.0
check "no commit under the fraction" [::ess::dial_response] -1
check "pointer stays hidden" [pointer] "0.0000,0.0000,0,0"
check "held once past release" [::ess::dial_threshold_held] 1
feed 1.0 0.0
check "not held once back inside" [::ess::dial_threshold_held] 0
check "still no commit" [::ess::dial_response] -1

puts "\ncrossing sample is the answer:"
arm_ready
::ess::dial_set_radius 6.0
::ess::dial_arm
set ::TS 9000000
set before $::UPDATES
feed 10.0 0.0
check "commit wakes the state machine" [expr {$::UPDATES > $before}] 1
approx_ang "bearing is atan2 of that sample" [::ess::dial_response] 0.0
check "report time is the sample's box timestamp" \
    $::ess::dial_report_time 9000000
check "pointer sits on the ring" [pointer] "6.0000,0.0000,1,1"

puts "\na later sample does not move it:"
arm_ready
::ess::dial_set_radius 6.0
::ess::dial_arm
set ::TS 100000
feed 10.0 0.0
set ::TS 200000
feed 0.0 10.0
approx_ang "still the first bearing" [::ess::dial_response] 0.0
check "still the first timestamp" $::ess::dial_report_time 100000

puts "\nthe boundary itself commits:"
arm_ready
::ess::dial_arm
set ::TS 3000
feed 9.5 0.0
approx_ang "the engage fraction of scale is a crossing" [::ess::dial_response] 0.0

puts "\nstraight up:"
arm_ready
::ess::dial_set_radius 6.0
::ess::dial_arm
set ::TS 4000
feed 0.0 10.0
approx_ang "atan2 of +y" [::ess::dial_response] [expect_ang 10.0 0.0]
lassign [split [pointer] ,] px py show band
approx "ring x" $px 0.0
approx "ring y" $py 6.0
check  "shown" $show 1
check  "in band" $band 1

puts "\nno radius: the angle is reported and the pointer stays hidden:"
arm_ready
::ess::dial_arm
set ::TS 4500
feed 0.0 -10.0
approx_ang "atan2 of -y" [::ess::dial_response] [expect_ang -10.0 0.0]
check "pointer hidden" [pointer] "0.0000,0.0000,0,0"

puts "\na sample before the window does not commit:"
arm_ready
set ::TS 50
feed 10.0 0.0
check "held while out, before the window" [::ess::dial_threshold_held] 1
set ::TS 60
feed 1.0 0.0
::ess::dial_arm
check "arming from inside reports nothing" [::ess::dial_response] -1

puts "\narming while already past the fraction:"
arm_ready
::ess::dial_set_radius 6.0
set ::DP(slider/position) {0.0 10.0}
set ::TS 1
::ess::dial_arm
set opened $::ess::dial_armed_time
approx_ang "the held bearing is the answer" [::ess::dial_response] [expect_ang 10.0 0.0]
check "an older sample time becomes the arm time" \
    $::ess::dial_report_time $opened
check "pointer shows that bearing" [pointer] "0.0000,6.0000,1,1"

puts "\na lone engage fraction re-derives release:"
arm_ready
set tuned [::ess::dial_threshold_tune -frac 0.50]
approx "release is 0.6 of the new engage" [dict get $tuned release] 0.30
::ess::dial_init -sources threshold -threshold_scale 10.0 -threshold_frac 0.90 \
    -threshold_release 0.20
set tuned [::ess::dial_threshold_tune]
approx "an explicit release is kept" [dict get $tuned release] 0.20

puts "\nrefused:"
fails "release not below engage" {
    ::ess::dial_init -sources threshold -threshold_scale 10.0 \
        -threshold_frac 0.50 -threshold_release 0.50
}
fails "threshold and rate are one stick read two ways" {
    ::ess::dial_init -sources {threshold rate} -threshold_scale 10.0
}
fails "the astick spelling is the same refusal" {
    ::ess::dial_init -sources {threshold astick} -threshold_scale 10.0
}
::ess::dial_init -sources threshold
fails "missing scale fails at arm" { ::ess::dial_arm }

puts ""
if { $::FAIL } { puts "FAILED" ; exit 1 } else { puts "all checks passed" }
