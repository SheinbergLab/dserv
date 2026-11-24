# test_ess_obs_hold.tcl -- the logged ENDOBS held until the trial's light
# has been judged (lib/ess-2.0.tm, the obs_end_hold block, with the
# photo_verify block it reads).
#
#   tclsh tests/test_ess_obs_hold.tcl
#
# now, dservAfter/dservAfterCancel, dservSetData and evt_put are stand-ins
# writing to one ordered log, so the order of photo/pair records and
# events is what is checked. end_obs, evt_put and unload_system are checked
# to call the block.

set here [file dirname [file normalize [info script]]]
set root [file dirname $here]

set fails 0
proc check { got want what } {
    if { $got eq $want } { puts "  ok: $what" } else {
        puts "  FAIL: $what: got {$got} want {$want}"
        incr ::fails
    }
}
proc slurp { f } { set fd [open $f] ; set s [read $fd] ; close $fd ; return $s }

set ::clk 1000000000
proc now {} { return $::clk }
set ::log {}
proc dservSetData { name ts type data } { lappend ::log [list $name $ts] }
set ::timers [dict create]
set ::tid 0
proc dservAfter { ms script } {
    dict set ::timers [incr ::tid] [list $ms $script]
    return $::tid
}
proc dservAfterCancel { id } {
    set had [dict exists $::timers $id]
    dict unset ::timers $id
    return $had
}
proc fire { id } {
    set s [lindex [dict get $::timers $id] 1]
    dict unset ::timers $id
    uplevel #0 $s
}

# photo_verify is in ess_photo-1.0.tm, obs_end_hold in ess-2.0.tm
set src "[slurp [file join $root lib ess_photo-1.0.tm]][slurp [file join $root lib ess-2.0.tm]]"
foreach blk {photo_verify obs_end_hold} {
    set b [string first "    # BEGIN $blk\n" $src]
    set e [string first "    # END $blk\n" $src]
    if { $b < 0 || $e < 0 } { error "no $blk block in lib" }
    namespace eval ::ess [string range $src $b $e]
}
# evt_put's line that matters here, and the per-obs checks
proc ::ess::evt_put { type subtype time args } {
    if { $::ess::obs_end_held ne "" } { ::ess::obs_end_flush }
    lappend ::log [list $type $subtype $time]
}
set ::checks 0
proc ::ess::obs_end_checks {} { incr ::checks }

puts "wired in:"
check [string match {*if \{ $::ess::obs_end_held ne "" \} \{ obs_end_flush \}
        evtPut*} $src] 1 "evt_put writes a held ENDOBS first"
check [string match {*set t_end \[now\]
        set hold_ms 0
        catch \{ set hold_ms \[obs_end_hold_ms $t_end\] \}
        if \{ $hold_ms > 0 \} \{
            obs_end_hold $status $t_end $hold_ms
        \} else \{
            ::ess::evt_put ENDOBS $status $t_end
        \}*} $src] 1 "end_obs holds or writes it"
check [string match {*proc unload_system \{\} \{
        variable current
        set sname $current(system)
        # a held ENDOBS, while the system that names it is still here
        catch \{ obs_end_flush \}*} $src] 1 "unload_system flushes it"

puts "when to hold:"
::ess::pv_stop
check [::ess::obs_end_hold_ms [now]] 0 "no patch: never"
::ess::pv_start marked 0
check [::ess::obs_end_hold_ms [now]] 0 "nothing pending: no hold"
::ess::pv_cmd 1 "!choices_off" 10 [expr {$::clk - 10000}]
check [::ess::obs_end_hold_ms [now]] 295 "a '!' 10 ms before the end: until it can be judged (300 + 5 - 10)"
set ::clk [expr {$::clk + 400000}]
::ess::pv_resolve $::clk
::ess::pv_cmd 2 "!x" 11 [expr {$::clk - 350000}]
check [::ess::obs_end_hold_ms [now]] 0 "one 350 ms before: already judgeable"
::ess::pv_cmd 3 "!x" 12 [expr {$::clk + 1000}]
check [::ess::obs_end_hold_ms [now]] 0 "  (and one after the end does not count)"
set ::ess::pv_hold_us 900000
::ess::pv_cmd 4 "!x" 13 $::clk
check [::ess::obs_end_hold_ms [now]] 400 "capped at obs_end_hold_cap_ms"
set ::ess::pv_hold_us 300000

puts "the timer writes it:"
::ess::pv_start marked 0
set ::log {}
set ::checks 0
set t0 $::clk
::ess::pv_change [list 10 [expr {$t0 - 20000}] 1 1 0 2 1] $t0      ;# flip 20 ms before the end
::ess::pv_cmd 1 "!choices_off" 10 [expr {$t0 - 15000}]
set ms [::ess::obs_end_hold_ms $t0]
::ess::obs_end_hold COMPLETE $t0 $ms
check [list [llength $::log] [dict size $::timers]] {0 1} "held: nothing written, one timer"
::ess::pv_edge [list 1 [expr {$t0 - 12000}] [expr {$t0 - 13000}] 0 0 0 1 1 0 0]
set ::clk [expr {$t0 + $ms * 1000}]
fire [lindex [dict keys $::timers] 0]
check [lmap r $::log { lindex $r 0 }] {photo/pair ENDOBS} "the light judged and logged, then ENDOBS"
check [lindex $::log end] [list ENDOBS COMPLETE $t0] "  stamped at end_obs's time"
check $::checks 1 "  then the per-obs checks"
check $::ess::obs_end_held {} "  nothing held"

puts "an event meanwhile writes it first:"
set ::log {}
set ::checks 0
set t1 $::clk
::ess::pv_cmd 2 "!x" 20 [expr {$t1 - 1000}]
::ess::obs_end_hold QUIT $t1 [::ess::obs_end_hold_ms $t1]
set id [lindex [dict keys $::timers] 0]
::ess::evt_put SYSTEM_STATE STOPPED [expr {$t1 + 100}]
check [lmap r $::log { lrange $r 0 1 }] {{ENDOBS QUIT} {SYSTEM_STATE STOPPED}} "ENDOBS, then the event"
check [dict exists $::timers $id] 0 "  its timer cancelled"
check $::checks 1 "  the checks once"

puts "a stale timer does nothing:"
set ::log {}
::ess::obs_end_hold COMPLETE $::clk 100
set old [lindex [dict keys $::timers] 0]
set olds [lindex [dict get $::timers $old] 1]
::ess::obs_end_flush
::ess::obs_end_hold COMPLETE [expr {$::clk + 5}] 100
uplevel #0 $olds
check [llength [lsearch -all -index 0 $::log ENDOBS]] 1 "the first's timer, after it was flushed: no second ENDOBS"
check [expr {$::ess::obs_end_held ne ""}] 1 "  the second still held"
::ess::obs_end_flush
check [llength [lsearch -all -index 0 $::log ENDOBS]] 2 "  until its own flush"

puts "no timer available:"
rename dservAfter {}
set ::log {}
::ess::obs_end_hold COMPLETE $::clk 100
check [lmap r $::log { lindex $r 0 }] {ENDOBS} "written at once"

puts [expr {$fails ? "$fails FAILED" : "all checks passed"}]
exit [expr {$fails > 0}]
