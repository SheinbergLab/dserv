#
# test_ess_participant.tcl
#
#  Human subjects run under private ids that are not in the roster. The
#  roster carries the class ("human"); ess::set_participant binds an id over
#  it. What this pins down:
#
#   - no data file opens under the class name (participant_required)
#   - a config load re-selecting "human" keeps the bound id, and does not
#     regenerate the trials
#   - the id cannot change under an open file
#   - the binding lapses after an idle timeout, but never with a file open
#     or ESS running
#   - only a system that declared set_subject_dependent reloads on a bind
#   - a rig id format (h###) rejects the near-miss ids a typo produces
#   - a queue that needs an id waits (needs_subject) and carries on from
#     the same place when one is bound
#
#  A UNIT test: the real procs are pulled from lib/ess-2.0.tm and
#  lib/ess_queues-1.0.tm and run under plain tclsh with the rest stubbed.
#
#  Run as: tclsh tests/test_ess_participant.tcl  (or: ctest -R ess_participant)
#

set ::REPO [file normalize [file join [file dirname [info script]] ..]]

array set ::DP {}
proc dservSet { name val } { set ::DP($name) $val }
proc dservGet { name } { return $::DP($name) }

proc slurp { f } { set fh [open $f]; set s [read $fh]; close $fh; return $s }

set FAIL 0
proc check { label got want } {
    if { $got eq $want } { puts "  ok   $label" } else {
        puts "  FAIL $label: got '$got' want '$want'"; set ::FAIL 1
    }
}
proc check_err { label script pattern } {
    if { [catch { uplevel 1 $script } err] && [string match $pattern $err] } {
        puts "  ok   $label"
    } else {
        puts "  FAIL $label: got '$err' want error matching '$pattern'"; set ::FAIL 1
    }
}

# ---------------------------------------------------------------------------
# ess side
# ---------------------------------------------------------------------------
set ::src [slurp [file join $::REPO lib ess-2.0.tm]]
proc pull_ess { name } {
    set start [string first "\n    proc $name " $::src]
    if { $start < 0 } { puts "FAIL: could not find ess proc $name"; exit 1 }
    set end [string first "\n    \}\n" $::src $start]
    namespace eval ::ess [string range $::src [expr {$start + 1}] [expr {$end + 6}]]
}

# a system object, as far as these procs look at it
set ::sys_status stopped
set ::sys_dependent 0
proc fake_system { cmd args } {
    switch $cmd {
        status            { return $::sys_status }
        subject_dependent { return $::sys_dependent }
    }
}

namespace eval ::ess {
    variable subject_id {}
    variable subject_base {}
    variable participant_id {}
    variable participant_subjects {human}
    variable participant_idle_timeout 2700
    variable participant_idle_since 0
    variable participant_pattern ""
    variable open_datafile {}
    variable current
    array set current { system s protocol p variant v state_system fake_system }
    variable events {}
    variable reloads 0
    proc evt_put { type sub t args } { lappend ::ess::events [list $type $sub {*}$args] }
    proc now {} { return 0 }
    proc ess_info { args } {}
    proc reload_variant {} { incr ::ess::reloads }
}
foreach p { canonical_subject_name set_subject subject_takes_participant
            publish_participant participant_touch participant_reload
            participant_required set_participant clear_participant
            participant_expire set_participant_timeout
            participant_pattern_norm set_participant_pattern
            participant_matches } {
    pull_ess $p
}

puts "The class alone cannot be recorded:"
ess::set_subject human
check "subject"            $ess::subject_id human
check "required"           [ess::participant_required] 1
check "ess/participant"    $::DP(ess/participant) ""
check "ess/subject_base"   $::DP(ess/subject_base) human

puts "Binding an id:"
set ess::reloads 0
check "returns canonical"  [ess::set_participant " P017 "] p017
check "subject is the id"  $ess::subject_id p017
check "base stays human"   $ess::subject_base human
check "required"           [ess::participant_required] 0
check "ess/participant"    $::DP(ess/participant) p017
check "ID/SUBJECT logged"  [lindex $ess::events end] {ID SUBJECT p017}
check "no reload"          $ess::reloads 0
check "expiry is counting" [expr {$::DP(ess/participant_expires) > 0}] 1

puts "A config load re-selecting human keeps it:"
ess::set_subject human
check "subject"            $ess::subject_id p017
check "no reload"          $ess::reloads 0
check "required for a human config" [ess::participant_required human] 0

puts "Bad ids are refused:"
check_err "the class itself" { ess::set_participant human } "*subject class*"
check_err "punctuation"      { ess::set_participant "p 17!" } "*invalid subject id*"
check "still bound"          $ess::participant_id p017

puts "An open file pins the identity:"
set ess::open_datafile /data/p017_x.ess
ess::publish_participant
check "expiry not counting" $::DP(ess/participant_expires) 0
check_err "no rebinding"    { ess::set_participant p018 } "*is open*"
check_err "no ending"       { ess::clear_participant } "*is open*"
set ess::participant_idle_since 1
check "no expiry"           [ess::participant_expire] 0
check "still bound"         $ess::participant_id p017
set ess::open_datafile {}

puts "Idle expiry:"
ess::participant_touch
check "fresh: no expiry"    [ess::participant_expire] 0
set ::sys_status running
set ess::participant_idle_since 1
check "running: no expiry"  [ess::participant_expire] 0
check "running restarts the clock" \
    [expr {$ess::participant_idle_since > 1}] 1
set ::sys_status stopped
set ess::participant_idle_since 1
ess::set_participant_timeout 0
check "timeout 0: never"    [ess::participant_expire] 0
ess::set_participant_timeout 45
check "45 min in seconds"   $ess::participant_idle_timeout 2700
check "idle: expires"       [ess::participant_expire] 1
check "back to the class"   $ess::subject_id human
check "ess/participant"     $::DP(ess/participant) ""
check "ID/SUBJECT logged"   [lindex $ess::events end] {ID SUBJECT human}
check "required again"      [ess::participant_required] 1

puts "A lapsed binding is noticed where it is consulted:"
ess::set_participant p018
set ess::participant_idle_since 1
check "required (lazy expiry)" [ess::participant_required] 1

puts "Another roster subject ends it:"
ess::set_participant p019
ess::set_subject momo
check "subject"             $ess::subject_id momo
check "unbound"             $ess::participant_id ""
check "not required"        [ess::participant_required] 0
check "but a human config would be" [ess::participant_required human] 1

puts "Entering an id selects the human class:"
ess::set_participant p020
check "base"                $ess::subject_base human
check "subject"             $ess::subject_id p020

puts "Only a subject-dependent system reloads:"
set ess::reloads 0
ess::set_participant p021
check "independent: none"   $ess::reloads 0
set ::sys_dependent 1
ess::set_participant p022
check "dependent: reload"   $ess::reloads 1
ess::set_participant p022
check "same id: none"       $ess::reloads 1
ess::clear_participant
check "ending reloads too"  $ess::reloads 2
set ::sys_dependent 0

puts "A rig id format:"
ess::clear_participant
ess::set_participant_pattern " H### "
check "normalized"          $ess::participant_pattern h###
check "published"           $::DP(ess/participant_pattern) h###
check "accepts h017"        [ess::set_participant H017] h017
check_err "too few digits"  { ess::set_participant h17 }   "*does not match*h###*"
check_err "too many digits" { ess::set_participant h0170 } "*does not match*"
check_err "wrong prefix"    { ess::set_participant p017 }  "*does not match*"
check_err "a name"          { ess::set_participant jsmith } "*does not match*"
check "still h017"          $ess::participant_id h017
check_err "bad format"      { ess::set_participant_pattern "h ##!" } "*invalid participant id format*"
check "format unchanged"    $ess::participant_pattern h###
ess::set_participant_pattern "s2-##"
check "literal - and digit" [ess::set_participant s2-07] s2-07
check_err "literal is literal" { ess::set_participant s3-07 } "*does not match*"
ess::set_participant_pattern ""
check "empty: free form"    [ess::set_participant anything_goes] anything_goes
ess::clear_participant

# ---------------------------------------------------------------------------
# queue side
# ---------------------------------------------------------------------------
set ::qsrc [slurp [file join $::REPO lib ess_queues-1.0.tm]]
proc pull_q { name } {
    set start [string first "\nproc ::ess_queues::$name " $::qsrc]
    if { $start < 0 } { puts "FAIL: could not find $name"; exit 1 }
    set end [string first "\n\}\n" $::qsrc $start]
    eval [string range $::qsrc [expr {$start + 1}] [expr {$end + 2}]]
}

# ESS, as seen across `send ess`: the real procs above
set ::ess_starts 0
proc send { interp script } {
    if { $interp ne "ess" } { error "unexpected send to $interp" }
    if { $script eq "ess::start" } { incr ::ess_starts; return 1 }
    return [namespace eval :: $script]
}

namespace eval ess_queues {
    variable flush_delay_ms 0
    variable logged {}
    variable state
    proc log { level msg } { lappend ::ess_queues::logged "$level $msg" }
    proc open_datafile {} { incr ::files_opened; return 1 }
}
set i [string first "    array set state \{" $::qsrc]
namespace eval ess_queues [string range $::qsrc $i [string first "\n    \}\n" $::qsrc $i]]\n\}

# the configs module, as far as load_current_item looks at it
namespace eval ::ess::configs {
    proc get { id } { return [dict create id $id name humancfg subject human] }
    proc load_sync { config } {
        incr ::loads
        ::ess::set_subject [dict get $config subject]
    }
}
# one queued item: config 7, one repeat
namespace eval ess_queues {
    proc dbcmd { sub sql } { return {7 1 0 humancfg} }
    variable db ::ess_queues::dbcmd
}

foreach p { ess_start start_run load_current_item participant_needed
            on_ess_participant publish_state } { pull_q $p }

proc qstatus {} {
    regexp {"status":"([^"]*)"} $::DP(queues/state) -> v
    return $v
}
proc qfresh {} {
    set ::ess_queues::state(status) loading
    set ::ess_queues::state(resume) ""
    set ::ess_queues::state(run_count) 0
    set ::files_opened 0
    set ::ess_starts 0
    set ::loads 0
}

puts "A queue with a human config and no participant waits before loading:"
ess::set_subject human
ess::clear_participant
qfresh
::ess_queues::load_current_item
check "status"         $::ess_queues::state(status) needs_subject
check "published"      [qstatus] needs_subject
check "resume"         $::ess_queues::state(resume) load
check "not loaded"     $::loads 0
check "no file"        $::files_opened 0

puts "An empty ess/participant does not wake it:"
::ess_queues::on_ess_participant ""
check "status"         $::ess_queues::state(status) needs_subject

puts "Binding continues it: load, file, start:"
ess::set_participant p030
::ess_queues::on_ess_participant $::DP(ess/participant)
check "loaded once"    $::loads 1
check "file opened"    $::files_opened 1
check "ESS started"    $::ess_starts 1
check "status"         $::ess_queues::state(status) running
check "subject"        $ess::subject_id p030

puts "A participant already bound: no wait:"
qfresh
::ess_queues::load_current_item
check "status"         $::ess_queues::state(status) running
check "loaded once"    $::loads 1

puts "A binding that lapsed between repeats holds the next run:"
set ess::participant_idle_since 1
set ::ess_queues::state(status) ready
set ::ess_queues::state(run_count) 1
set ::files_opened 0
::ess_queues::start_run
check "status"         $::ess_queues::state(status) needs_subject
check "resume"         $::ess_queues::state(resume) run
check "run not counted" $::ess_queues::state(run_count) 1
check "no file"        $::files_opened 0
set ::loads 0
ess::set_participant p030
::ess_queues::on_ess_participant p030
check "status"         $::ess_queues::state(status) running
check "no reload of the config" $::loads 0
check "run counted"    $::ess_queues::state(run_count) 2
check "file opened"    $::files_opened 1

puts "A bind with no queue waiting changes nothing:"
set ::ess_queues::state(status) idle
::ess_queues::on_ess_participant p031
check "status"         $::ess_queues::state(status) idle

if { $FAIL } { puts "FAILED"; exit 1 }
puts "all checks passed"
exit 0
