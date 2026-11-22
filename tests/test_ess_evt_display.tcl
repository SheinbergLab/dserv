# test_ess_evt_display.tcl -- events linked to the frame that showed them
# (lib/ess-2.0.tm, the evt_display block).
#
#   tclsh tests/test_ess_evt_display.tcl
#
# The block is read out of ess-2.0.tm; evt_put, do_action and the
# stim_cmd_log hook are imitated by the few lines of each that touch it,
# and those lines are checked to be in the source. dservSetData keeps what
# was published. Also times the state machine's paths (printed, not
# judged): an action and an event with nothing to link should cost two
# variable sets and two reads.

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

set ::out {}
proc dservSetData { name ts type data } { lappend ::out [list $name $ts $data] }

set src [slurp [file join $root lib ess-2.0.tm]]
set b [string first "    # BEGIN evt_display\n" $src]
set e [string first "    # END evt_display\n" $src]
if { $b < 0 || $e < 0 } { error "no evt_display block in ess-2.0.tm" }
namespace eval ::ess [string range $src $b $e]

puts "wired in:"
check [string match {*if \{ $::ess::ed_cmd ne "" || $::ess::ed_force \} \{
            ed_after_put $type $subtype $time*} $src] 1 "evt_put calls ed_after_put"
check [string match {*set ::ess::ed_cmd \{\}
	set ::ess::ed_action 1
	set r \[my ${state_name}_a\]
	set ::ess::ed_action 0*} $src] 1 "do_action brackets the action"
check [expr {"::ess::ed_cmd_heard" in $::ess::stim_cmd_hook}] 1 "the stim_cmd_log hook is set"
check [string match {*dservLoggerAddMatch $filename ess/evt_display 0 0 1
        ed_new_file \[dserv_wall_offset\]*} $src] 1 "each data file logs it"

# the lines of evt_put and do_action that matter here
proc ::ess::evt_put { type subtype time args } {
    if { $::ess::ed_cmd ne "" || $::ess::ed_force } {
        ::ess::ed_after_put $type $subtype $time
    }
}
proc action { body } {
    set ::ess::ed_cmd {}
    set ::ess::ed_action 1
    set r [uplevel 1 $body]
    set ::ess::ed_action 0
    set ::ess::ed_cmd {}
    return $r
}
proc rmt { seq cmd swap flip } {
    foreach h $::ess::stim_cmd_hook { {*}$h $seq $cmd $swap 0 $flip }
}
proc recs {} { lmap r $::out { lindex $r 2 } }

::ess::ed_new_file 1000

puts "in an action:"
set ::out {}
action {
    ::ess::evt_put STIMTYPE STIMID 50
    rmt 7 "!sample_on" 120 900000
    ::ess::evt_put SAMPLE ON 60
}
check [llength $::out] 1 "only the event after the '!' command is linked"
check [lindex $::out 0 1] 60 "  stamped at the event's time"
check [recs] {{{"type":"SAMPLE","subtype":"ON","t":60,"seq":7,"swap":120,"flip":901000}}} \
    "  type, subtype, t, the command's seq and swap, the flip on the dserv clock"

set ::out {}
action {
    rmt 8 "!choices_on" 130 910000
    rmt 9 "set x 1" 131 0
    ::ess::evt_put CHOICES ON 70
    rmt 10 "!choices_off" 140 920000
    ::ess::evt_put CHOICES OFF 80
    ::ess::evt_put ENDTRIAL ABORT 81
}
check [lmap j [recs] { regexp -inline {"seq":[0-9]+} $j }] {{{"seq":8}} {{"seq":10}} {{"seq":10}}} \
    "each event to the action's last '!' command; a plain command changes nothing"

puts "not in an action:"
set ::out {}
::ess::evt_put SAMPLE OFF 90
check $::out {} "a callback's event after an action: not linked"
action { ::ess::evt_put SAMPLE OFF 91 }
check $::out {} "an action with no '!' command: not linked"
::ess::evt_put_shown SAMPLE OFF 92
check [regexp -inline {"seq":[0-9]+} [lindex [recs] 0]] {{"seq":10}} \
    "evt_put_shown: the last '!' command anywhere"
set ::out {}
::ess::evt_put SAMPLE OFF 93
check $::out {} "  and only for that event"

puts "no flip time:"
set ::out {}
action { rmt 11 "!x" 150 null ; ::ess::evt_put SAMPLE ON 100 }
check [string match {*"flip":null\}} [lindex [recs] 0]] 1 "stim2 had none: flip null"
set ::ess::ed_off {}
set ::out {}
action { rmt 12 "!x" 151 920000 ; ::ess::evt_put SAMPLE ON 101 }
check [string match {*"flip":null\}} [lindex [recs] 0]] 1 "no clock offset yet: flip null"

puts "cost (us per call, not judged):"
set ::ess::ed_off 1000
proc nop {} {}
proc plain_action {} { nop }
proc ess_action {} {
    set ::ess::ed_cmd {}
    set ::ess::ed_action 1
    set r [nop]
    set ::ess::ed_action 0
    set ::ess::ed_cmd {}
    return $r
}
proc evt_check {} {
    if { $::ess::ed_cmd ne "" || $::ess::ed_force } { ::ess::ed_after_put A B 1 }
}
proc dservSetData { args } {}
set N 200000
foreach p {plain_action ess_action} { plain_action ; ess_action }
set t0 [lindex [time { plain_action } $N] 0]
set t1 [lindex [time { ess_action } $N] 0]
set t2 [lindex [time { evt_check } $N] 0]
set ::ess::ed_cmd {7 120 900000}
set t3 [lindex [time { evt_check } 20000] 0]
set ::ess::ed_cmd {}
puts [format "  an action: %.3f (bare %.3f, +%.3f); an event, nothing linked: %.3f; an event linked: %.3f" \
          $t1 $t0 [expr {$t1 - $t0}] $t2 $t3]

puts [expr {$fails ? "$fails FAILED" : "all checks passed"}]
exit [expr {$fails > 0}]
