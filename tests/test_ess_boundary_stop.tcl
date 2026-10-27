#
# test_ess_boundary_stop.tcl
#
#  The trial-boundary stop (`setting ess stop_after_trial`) and the juice
#  gate (`setting ess juice_gate`). What matters is WHERE the stop lands:
#  after the action that ended the obs has run to completion, before the
#  next transition can begin another trial -- and that the opt-outs, the
#  second press, and the between-trials case all stay immediate.
#
#  A UNIT test: the real procs and the real State_System start/stop/update
#  methods are pulled from lib/ess-2.0.tm and run against a stub system
#  under plain tclsh.
#
#  Run as: tclsh tests/test_ess_boundary_stop.tcl   (or: ctest -R ess_boundary_stop)
#

set ::REPO [file normalize [file join [file dirname [info script]] ..]]

proc now {} { return 1000000 }
array set ::DP {}
proc dservSet    { name val } { set ::DP($name) $val }
proc dservExists { name } { info exists ::DP($name) }
proc dservGet    { name } { return $::DP($name) }
proc rpioPinOff  { pin } {}
proc print       { args } {}

# the juicer subprocess, as ess::start's juice_check sees it
set ::juicer_reply {unknown {}}
proc send { interp args } {
    if { $interp ne "juicer" } { error "unexpected send to $interp" }
    return $::juicer_reply
}

set fh [open [file join $::REPO lib ess-2.0.tm]]
set ::src [read $fh]
close $fh

# body of `<kind> <name> <args> {` ... the matching 4-space-indented `}`
proc pull_body { kind name } {
    set start [string first "\n    $kind $name " $::src]
    if { $start < 0 } { puts "FAIL: could not find $kind $name"; exit 1 }
    set end [string first "\n    \}\n" $::src $start]
    set nl  [string first "\n" $::src [expr {$start + 1}]]
    return [string range $::src [expr {$nl + 1}] [expr {$end - 1}]]
}

namespace eval ess {
    variable current
    set current(system) stub
    set current(state_system) {}
    variable in_obs 0
    variable obs_pin -1
    variable obs_pending 0
    variable obs_sched_box ""
    variable obs_sched_pin ""
    variable io_class extio
    variable stim_required 0
    variable events {}
    proc evt_put { type sub t args } { lappend ::ess::events "$type $sub" }
    proc ess_log { level msg {cat ""} } { lappend ::ess::events "LOG $level $msg" }
    proc ess_info    { m {c ""} } { ess_log info $m }
    proc ess_warning { m {c ""} } { ess_log warning $m }
    proc ess_error   { m {c ""} } { ess_log error $m }
    proc ess_debug   { m {c ""} } {}
    proc stim_required {} { return 0 }
    proc find_system { name } { return $::sys }
    proc begin_obs {} {
        variable in_obs 1
        evt_put BEGINOBS 0 [now]
    }
}

foreach {name arglist} {
    start {} stop {} request_stop {} trial_boundary {} stop_at_boundary {}
    juice_low {} juice_check {} arm_boundary_stop {reason}
    clear_boundary_stop {} set_hold {msg} clear_hold {}
    set_stop_after_trial {on} set_juice_gate {on} end_obs {{status 1}}
} {
    proc ::ess::$name $arglist [pull_body proc $name]
}
# the namespace variables the section declares
namespace eval ess {
    variable stop_after_trial 0
    variable juice_gate 0
    variable stop_pending ""
    variable boundary_due 0
}

# A stub state system: real start/stop/update/end/set_status methods, a
# trial loop of inter -> start_obs -> wait -> finish -> inter. The wait
# state holds until ::trial_done, so a test can stop mid-trial.
oo::class create Stub {
    variable _status _current_state _start_state _end_state _states \
        _callbacks _systemname
    constructor {} {
        set _status stopped
        set _systemname stub
        set _start_state inter
        set _end_state end
        set _current_state inter
        set _states [dict create inter inter start_obs start_obs \
                         wait wait finish finish end end]
        set _callbacks(quit) quit_cb
    }
    method quit_cb {} { ::ess::end_obs QUIT }
    method do_transition {} {
        switch -- $_current_state {
            inter     {
                if { $::hold_inter } { return "" }   ;# the inter-trial wait
                return [expr {$::trials_left > 0 ? "start_obs" : "end"}]
            }
            start_obs { return wait }
            wait      { return [expr {$::trial_done ? "finish" : ""}] }
            finish    { return inter }
        }
        return ""
    }
    method do_action {} {
        lappend ::visited $_current_state
        switch -- $_current_state {
            start_obs { set ::trial_done 0; ::ess::begin_obs }
            finish {
                ::ess::end_obs COMPLETE
                incr ::trials_left -1
                # the rest of the trial's last action: must still run
                lappend ::ess::events "AFTER_ENDOBS"
            }
        }
    }
    method current {} { return $_current_state }
}
foreach m {start stop update end} {
    oo::define Stub method $m {} [pull_body method $m]
}
oo::define Stub method set_status { status } [pull_body method set_status]
oo::define Stub method status {} { return $_status }

set FAIL 0
proc check { label got want } {
    if { $got eq $want } { puts "  ok   $label" } else {
        puts "  FAIL $label: got '$got' want '$want'"; set ::FAIL 1
    }
}

proc fresh {} {
    catch { $::sys destroy }
    set ::sys [Stub new]
    set ::ess::current(state_system) $::sys
    set ::ess::in_obs 0
    set ::ess::events {}
    set ::visited {}
    set ::trials_left 5
    set ::trial_done 0
    set ::hold_inter 0
    set ::juicer_reply {unknown {}}
    array unset ::DP
    ::ess::clear_boundary_stop
    ::ess::set_hold ""
}
proc finish_trial {} { set ::trial_done 1; $::sys update }
proc evts_matching { pat } { lsearch -all -inline $::ess::events $pat }

# --- opt-out: Stop is immediate, exactly as before -----------------------
puts "without stop_after_trial, request_stop stops mid-trial:"
fresh
::ess::set_stop_after_trial 0
::ess::start
check "in a trial"  $::ess::in_obs 1
::ess::request_stop
check "stopped now" [$::sys status] stopped
check "obs ended as QUIT" [lindex [evts_matching ENDOBS*] end] "ENDOBS QUIT"
check "nothing pending" $::DP(ess/stop_pending) ""

# --- opt-in: the current trial finishes, the next never begins -----------
puts "\nwith stop_after_trial, the trial finishes first:"
fresh
::ess::set_stop_after_trial 1
::ess::start
::ess::request_stop
check "still running"        [$::sys status] running
check "armed"                $::DP(ess/stop_pending) user
check "still in the trial"   $::ess::in_obs 1
set ::visited {}
finish_trial
check "stopped at the boundary" [$::sys status] stopped
check "trial ended COMPLETE" [lindex [evts_matching ENDOBS*] end] "ENDOBS COMPLETE"
check "the ending action ran to completion" \
    [lindex $::ess::events [expr {[lsearch $::ess::events "ENDOBS COMPLETE"] + 1}]] \
    AFTER_ENDOBS
check "no further state ran"  $::visited finish
check "one trial begun"       [llength [evts_matching BEGINOBS*]] 1
check "pending cleared"       $::DP(ess/stop_pending) ""
check "no hold for a user stop" $::DP(ess/hold) ""

puts "\nand Go afterwards runs a fresh trial:"
::ess::start
check "running again"     [$::sys status] running
check "a second trial"    [llength [evts_matching BEGINOBS*]] 2

# --- second press = stop now ---------------------------------------------
puts "\na second Stop while armed stops immediately:"
fresh
::ess::set_stop_after_trial 1
::ess::start
::ess::request_stop
::ess::request_stop
check "stopped"           [$::sys status] stopped
check "obs ended as QUIT" [lindex [evts_matching ENDOBS*] end] "ENDOBS QUIT"
check "pending cleared"   $::DP(ess/stop_pending) ""
check "nothing left due"  $::ess::boundary_due 0

# --- between trials there is nothing to wait for -------------------------
puts "\nbetween trials, request_stop is immediate even when opted in:"
fresh
::ess::set_stop_after_trial 1
set ::hold_inter 1       ;# running, but in the inter-trial interval
::ess::start
check "running, no obs" "[$::sys status] $::ess::in_obs" "running 0"
::ess::request_stop
check "stopped at once" [$::sys status] stopped
check "nothing armed"   $::DP(ess/stop_pending) ""
set ::hold_inter 0

# --- immediate stop from elsewhere clears an armed one -------------------
puts "\nthe plain stop (queues, loads) clears an armed boundary stop:"
fresh
::ess::set_stop_after_trial 1
::ess::start
::ess::request_stop
::ess::stop
check "pending cleared" $::DP(ess/stop_pending) ""
check "nothing due"     $::ess::boundary_due 0
::ess::start
finish_trial
check "the next run is not stopped by the old request" [$::sys status] running

# --- juice gate at Go ----------------------------------------------------
puts "\njuice gate: Go refuses on a fresh low reading, and says why:"
fresh
::ess::set_stop_after_trial 0
::ess::set_juice_gate 1
set ::juicer_reply {low <50mLs}
check "refused"     [::ess::start] 0
check "not running" [$::sys status] stopped
check "hold says why" [string match "*juice low (<50mLs)*" $::DP(ess/hold)] 1
set ::juicer_reply {ok >50mLs}
check "after a refill, Go works" [::ess::start] 1
check "hold cleared" $::DP(ess/hold) ""

puts "\njuice gate: unknown (no usb pump, no juicer) never blocks:"
fresh
::ess::set_juice_gate 1
set ::juicer_reply {unknown {}}
check "started" [::ess::start] 1
fresh
::ess::set_juice_gate 1
rename send send_real
proc send { args } { error "no such interp" }
check "no juicer subprocess: started" [::ess::start] 1
rename send {}
rename send_real send

puts "\njuice gate off: a low reading is ignored:"
fresh
::ess::set_juice_gate 0
set ::juicer_reply {low <50mLs}
check "started" [::ess::start] 1

# --- juice gate mid-session ----------------------------------------------
puts "\njuice gate: a low reading mid-session stops after that trial:"
fresh
::ess::set_juice_gate 1
set ::juicer_reply {ok >50mLs}
::ess::start
set ::DP(juicer/juice_low) 0
finish_trial
check "fine while ok" [$::sys status] running
# this trial's reward reply reports low
set ::DP(juicer/juice_low) 1
set ::DP(juicer/juice_level) <50mLs
finish_trial
check "stopped at the boundary" [$::sys status] stopped
check "trial ended COMPLETE" [lindex [evts_matching ENDOBS*] end] "ENDOBS COMPLETE"
check "hold says paused for juice" \
    [string match "Paused after trial*juice low (<50mLs)*" $::DP(ess/hold)] 1
check "two trials ran" [llength [evts_matching BEGINOBS*]] 2

puts "\na stop's own end_obs QUIT never arms a juice stop:"
fresh
::ess::set_juice_gate 1
set ::juicer_reply {ok >50mLs}
::ess::start
set ::DP(juicer/juice_low) 1
::ess::stop
check "nothing pending" $::DP(ess/stop_pending) ""
check "nothing due"     $::ess::boundary_due 0

puts ""
if { $FAIL } { puts "FAILED"; exit 1 } else { puts "all checks passed" }
