# test_ess_stim_evt_time.tcl -- stim-tagged events land on the dserv clock.
#
# stim2 reports each flip's time as flipwall, on the stim host's
# CLOCK_REALTIME.  stim_evt_time used to map it with ptp/d_us, which is
# dserv - PTP hardware clock: TAI, 37 s ahead of the wall clock.  Every
# swap-tagged event on a PTP site landed 37 s before its own obs began and
# its obs-relative time wrapped (officepi -> office-stim, 2026-11-19).  Now
# flipwall is mapped by this host's own (dserv - wall) offset, and the
# result must fall just before the tag's arrival or it is not used.
#
#   dlsh tests/test_ess_stim_evt_time.tcl
#
# The two procs are read out of lib/ess-2.0.tm (the whole module needs a
# dserv); the dserv clock, the datapoints and ess_warning are stand-ins.

set here [file dirname [file normalize [info script]]]
set lib  [file join [file dirname $here] lib ess-2.0.tm]

set fails 0
proc check { got want what } {
    if { $got eq $want } { puts "  ok: $what" } else {
        puts "  FAIL: $what: got {$got} want {$want}"
        incr ::fails
    }
}
# |got - want| <= tol (us): the two clocks are read back to back, not at once
proc near { got want tol what } {
    if { abs($got - $want) <= $tol } { puts "  ok: $what" } else {
        puts "  FAIL: $what: got $got want $want (+/- $tol)"
        incr ::fails
    }
}

proc proc_source { file name } {
    set fd [open $file] ; set src [read $fd] ; close $fd
    set lines [split $src \n]
    set start [lsearch -glob $lines "    proc $name *"]
    if { $start < 0 } { error "no proc $name in $file" }
    set close "    [format %c 125]"
    set end [lsearch -start $start -exact $lines $close]
    return [join [lrange $lines $start $end] \n]
}

proc var_source { file name } {
    set fd [open $file] ; set src [read $fd] ; close $fd
    foreach line [split $src \n] {
        if { [string match "    variable $name *" $line] } { return $line }
    }
    error "no variable $name in $file"
}

# the dserv clock, a fixed offset from this host's wall clock (as on a rig,
# where dserv's clock is its own monotonic origin)
set ::dserv_minus_wall -1795000000000000
set ::dp [dict create]        ;# datapoint -> value
set ::ts [dict create]        ;# datapoint -> dserv receipt time

namespace eval ::ess {
    variable warned {}
    variable stim_evt_anchor {}
    proc now {} { expr {[clock microseconds] + $::dserv_minus_wall} }
    proc ess_warning { msg cat } { variable warned ; lappend warned $msg }
    proc dservGet { name } {
        if { ![dict exists $::dp $name] } { error "no such dpoint $name" }
        dict get $::dp $name
    }
    proc dservTimestamp { name } {
        if { ![dict exists $::ts $name] } { error "no such dpoint $name" }
        dict get $::ts $name
    }
}

foreach v { stim_evt_wall_early_us stim_evt_wall_late_us stim_evt_wall_warned } {
    namespace eval ::ess [var_source $lib $v]
}
namespace eval ::ess [proc_source $lib stim_evt_wall_ok]
namespace eval ::ess [proc_source $lib stim_evt_time]

proc reset {} {
    set ::ess::warned {}
    set ::ess::stim_evt_wall_warned 0
    set ::ess::stim_evt_anchor {}
}

# a flip FLIP_AGO us before now, its tag arriving ARRIVE_AGO us before now
proc flip { flip_ago arrive_ago } {
    set wall [expr {[clock microseconds] - $flip_ago}]
    dict set ::ts stim/tagevt [expr {[::ess::now] - $arrive_ago}]
    return $wall
}

puts "PTP site (ptp/d_us = 37 s of TAI):"
reset
# officepi's D: dserv - PTP hw clock, the PTP clock 37 s ahead of the wall
dict set ::dp ptp/d_us [expr {$::dserv_minus_wall - 37000000}]
set wall [flip 12000 2000]
set want [expr {$wall + $::dserv_minus_wall}]
set t [::ess::stim_evt_time stim/tagevt 10 1000.0 $wall]
near $t $want 2000 "flip mapped by (dserv - wall), not by ptp/d_us"
check [expr {$t < [dict get $::ts stim/tagevt]}] 1 "flip sits before its tag's arrival"
check [expr {abs($t - ($wall + [dict get $::dp ptp/d_us])) > 30000000}] 1 \
    "not 37 s early"
check [llength $::ess::warned] 0 "no warning"

puts "stim host clock disagrees (wall 37 s off):"
reset
set wall [flip 12000 2000]
dict set ::ts stim/tagevt [expr {[::ess::now] - 2000}]
set recv [dict get $::ts stim/tagevt]
set t [::ess::stim_evt_time stim/tagevt 10 1000.0 [expr {$wall - 37000000}]]
check $t $recv "no anchor: falls back to the tag's receipt time"
check [llength $::ess::warned] 1 "warns"
set t [::ess::stim_evt_time stim/tagevt 11 1008.0 [expr {$wall - 37000000}]]
check [llength $::ess::warned] 1 "warns once a minute, not per event"

reset
set ::ess::stim_evt_anchor [list 5000000 10 1000.0]
set t [::ess::stim_evt_time stim/tagevt 12 1016.5 [expr {$wall - 37000000}]]
check $t 5016500 "with an anchor: anchor + delta ticks"

puts "the window:"
reset
set wall [flip 900000 0]
set t [::ess::stim_evt_time stim/tagevt 10 1000.0 $wall]
near $t [expr {$wall + $::dserv_minus_wall}] 2000 "0.9 s before arrival is accepted"
reset
set wall [flip 1500000 0]
set t [::ess::stim_evt_time stim/tagevt 10 1000.0 $wall]
check $t [dict get $::ts stim/tagevt] "1.5 s before arrival is refused"
reset
set wall [flip 0 50000]
set t [::ess::stim_evt_time stim/tagevt 10 1000.0 $wall]
check $t [dict get $::ts stim/tagevt] "50 ms after arrival is refused"
reset
set wall [flip 0 5000]
set t [::ess::stim_evt_time stim/tagevt 10 1000.0 $wall]
near $t [expr {$wall + $::dserv_minus_wall}] 2000 \
    "5 ms after arrival is accepted (clock slack)"

puts "no PTP:"
reset
dict unset ::dp ptp/d_us
set wall [flip 12000 2000]
set t [::ess::stim_evt_time stim/tagevt 10 1000.0 $wall]
check $t [dict get $::ts stim/tagevt] "flipwall unused, receipt time"
set ::ess::stim_evt_anchor [list 5000000 10 1000.0]
set t [::ess::stim_evt_time stim/tagevt 11 1008.0 $wall]
check $t 5008000 "flipwall unused, anchor"

puts "old stim2 (no flipwall):"
reset
dict set ::dp ptp/d_us 0
set t [::ess::stim_evt_time stim/tagevt 10 1000.0 0]
check $t [dict get $::ts stim/tagevt] "flipwall 0 -> receipt time"
check [llength $::ess::warned] 0 "no warning"

if { $fails } {
    puts "$fails FAILURE(S)"
    exit 1
}
puts "all checks passed"
