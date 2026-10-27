#
# test_ess_queue_hold.tcl
#
#  An unattended queue meets a refused or paused ESS (juice gate, stim
#  required). The queue used to take "the send went through" as "ESS is
#  running", so a refused Go left it running forever against an idle ESS.
#  Now a refusal HOLDS the run: datafile open, status running (the manual-Go
#  state), ess/hold mirrored into queues/state -- and a later Go continues
#  the same run, whose completion advances the queue as usual.
#
#  A UNIT test: the real procs are pulled from lib/ess_queues-1.0.tm and run
#  under plain tclsh with ESS and the datafile stubbed.
#
#  Run as: tclsh tests/test_ess_queue_hold.tcl   (or: ctest -R ess_queue_hold)
#

set ::REPO [file normalize [file join [file dirname [info script]] ..]]

array set ::DP {}
proc dservSet { name val } { set ::DP($name) $val }
proc dservGet { name } { return $::DP($name) }

# ESS, as seen across `send ess`
set ::ess_answer 1
set ::ess_starts 0
proc send { interp script } {
    if { $interp ne "ess" || $script ne "ess::start" } { error "unexpected send" }
    incr ::ess_starts
    if { !$::ess_answer } { set ::DP(ess/hold) "Not started: juicer reports juice low (<50mLs). Refill, then Go." }
    return $::ess_answer
}

set fh [open [file join $::REPO lib ess_queues-1.0.tm]]
set ::src [read $fh]
close $fh
proc pull { name } {
    set start [string first "\nproc ::ess_queues::$name " $::src]
    if { $start < 0 } { puts "FAIL: could not find $name"; exit 1 }
    set end [string first "\n\}\n" $::src $start]
    eval [string range $::src [expr {$start + 1}] [expr {$end + 2}]]
}

namespace eval ess_queues {
    variable flush_delay_ms 0
    variable logged {}
    variable state
    proc log { level msg } { lappend ::ess_queues::logged "$level $msg" }
    proc open_datafile {} { incr ::files_opened; return 1 }
}
# the real initial state block
set i [string first "    array set state \{" $::src]
namespace eval ess_queues [string range $::src $i [string first "\n    \}\n" $::src $i]]\n\}

foreach p {ess_start start_run on_ess_hold on_ess_run_state publish_state} { pull $p }

set FAIL 0
proc check { label got want } {
    if { $got eq $want } { puts "  ok   $label" } else {
        puts "  FAIL $label: got '$got' want '$want'"; set ::FAIL 1
    }
}
proc qjson { key } {
    # good enough for the flat object publish_state writes
    regexp "\"$key\":\"(\[^\"\]*)\"" $::DP(queues/state) -> v
    return $v
}
proc fresh {} {
    set ::ess_queues::state(status) ready
    set ::ess_queues::state(hold) ""
    set ::ess_queues::logged {}
    set ::files_opened 0
    set ::ess_starts 0
    set ::DP(ess/hold) ""
}

puts "ESS starts: the run is running, nothing held:"
fresh
set ::ess_answer 1
::ess_queues::start_run
check "status"        $::ess_queues::state(status) running
check "one start"     $::ess_starts 1
check "no hold"       [qjson hold] ""
check "nothing warned" [lsearch -glob $::ess_queues::logged "warning*"] -1

puts "\nESS refuses (juice low): the run is HELD, not abandoned:"
fresh
set ::ess_answer 0
::ess_queues::start_run
check "still running (waiting for Go)" $::ess_queues::state(status) running
check "datafile opened once"           $::files_opened 1
check "logged why" \
    [string match "*Run held, waiting for Go: Not started: juicer*" \
         [lindex $::ess_queues::logged end]] 1
# the ess/hold change script arrives after the send returns
::ess_queues::on_ess_hold $::DP(ess/hold)
check "queues/state carries the reason" \
    [string match "Not started: juicer reports juice low*" [qjson hold]] 1
check "ess_start reports ESS's refusal" [::ess_queues::ess_start] 0
set ::ess_answer 1
check "and its yes"                     [::ess_queues::ess_start] 1

puts "\na refusal whose send itself fails is a refusal too:"
fresh
rename send send_ok
proc send { args } { error "ess interp gone" }
::ess_queues::start_run
check "still running, held"  $::ess_queues::state(status) running
check "logged generic reason" \
    [string match "*Run held, waiting for Go: ESS refused to start" \
         [lindex $::ess_queues::logged end]] 1
rename send {}
rename send_ok send

puts "\nsomeone refills and presses Go: hold clears, completion advances:"
::ess_queues::on_ess_hold ""
check "hold cleared in queues/state" [qjson hold] ""
::ess_queues::on_ess_run_state complete
check "run completes -> flushing" $::ess_queues::state(status) flushing

puts "\na mid-run pause (juice ran out) shows on the queue too:"
fresh
set ::ess_answer 1
::ess_queues::start_run
::ess_queues::on_ess_hold "Paused after trial: juicer reports juice low (<50mLs). Refill, then Go."
check "still running"        $::ess_queues::state(status) running
check "reason published"     [string match "Paused after trial*" [qjson hold]] 1
check "and logged"           [string match "warning Run held: Paused*" \
                                  [lindex $::ess_queues::logged end]] 1

puts ""
if { $FAIL } { puts "FAILED"; exit 1 } else { puts "all checks passed" }
