#
# test_ess_live_params.tcl
#
#  Live params that ACTUALLY take effect. add_live_param has always said it
#  is "designed to be twiddled mid-recording", but set_live_param only
#  stored and logged the value -- so a param read fresh each trial worked
#  and a param pushed into a subsystem at init silently did not. The apply
#  hook closes that, and the interesting part is the loop guards.
#
#  A UNIT test: the system object and the event log are STUBBED, so this
#  runs under plain tclsh. The guards are the reason it exists -- a runaway
#  apply chain would wedge the ess interp, and with it the serial queue
#  every subprocess command rides on, which is not something to discover on
#  a rig.
#
#  Run as: tclsh tests/test_ess_live_params.tcl   (or: ctest -R ess_live_params)
#

set ::REPO [file normalize [file join [file dirname [info script]] ..]]

proc now {} { return 1000000 }
array set ::DP {}
proc dservSet { name val } { set ::DP($name) $val }

namespace eval ess {
    variable current
    variable param_types
    array set param_types {VARIABLE 1 STIM 2}
    variable live_apply_inflight
    array set live_apply_inflight {}
    variable live_apply_depth 0
    variable live_apply_max_depth 8
    variable events {}
    proc evt_put { a b t args } { lappend ::ess::events [list $a $b {*}$args] }
    proc get_param_vals {} { return "" }
}

# a stand-in for the system object: just the params dict and the three
# methods set_live_param calls
namespace eval sys {
    variable P
    array set P {}
    proc add_live_param { name val ptype {apply {}} } {
        variable P
        set P($name) [list $val 1 $ptype 1 $apply]
    }
    proc set_param { name val } {
        variable P
        set e $P($name)
        # the real set_param rebuilds the entry -- it must PRESERVE index 4
        set P($name) [list $val [lindex $e 1] [lindex $e 2] [lindex $e 3] \
                          [expr {[llength $e] >= 5 ? [lindex $e 4] : ""}]]
    }
    proc is_live_param { name } {
        variable P
        return [expr {[info exists P($name)] ? [lindex $P($name) 3] : 0}]
    }
    proc live_param_apply { name } {
        variable P
        if { ![info exists P($name)] } { return "" }
        set e $P($name)
        return [expr {[llength $e] >= 5 ? [lindex $e 4] : ""}]
    }
    proc get { name } { variable P; return [lindex $P($name) 0] }
}
proc sysobj { m args } { return [::sys::$m {*}$args] }
set ::ess::current(state_system) sysobj

# pull set_live_param from the real source
set fh [open [file join $::REPO lib ess-2.0.tm]]
set src [read $fh]
close $fh
set start [string first "\n    proc set_live_param " $src]
if { $start < 0 } { puts "FAIL: could not find set_live_param"; exit 1 }
set end [string first "\n    \}\n" $src $start]
set nl  [string first "\n" $src [expr {$start + 1}]]
proc ::ess::set_live_param {param val} \
    [string range $src [expr {$nl + 1}] [expr {$end - 1}]]

set FAIL 0
proc check { label got want } {
    if { $got eq $want } { puts "  ok   $label" } else {
        puts "  FAIL $label: got '$got' want '$want'"; set ::FAIL 1
    }
}

# --- what the handlers do -------------------------------------------------
set ::applied {}
proc record { name val } { lappend ::applied [list $name $val] }

puts "a live param with an apply hook actually applies:"
::sys::add_live_param cursor_accel 0.0 float {record accel}
::ess::set_live_param cursor_accel 16
check "stored"  [::sys::get cursor_accel] 16
check "applied" $::applied {{accel 16}}

puts "\nand the spec survives being set (set_param rebuilds the entry):"
::ess::set_live_param cursor_accel 24
check "still has its hook" [::sys::live_param_apply cursor_accel] {record accel}
check "applied again"      [lindex $::applied end] {accel 24}

puts "\na live param with NO hook still stores and logs, as before:"
set ::applied {}
::sys::add_live_param plain_param 1 int
::ess::set_live_param plain_param 5
check "stored"      [::sys::get plain_param] 5
check "nothing ran" $::applied {}

puts "\na handler may set OTHER params -- that is the useful case:"
set ::applied {}
proc chain { name val } {
    lappend ::applied [list $name $val]
    if { $name eq "a" } { ::ess::set_live_param b [expr {$val * 2}] }
}
::sys::add_live_param a 0 int {chain a}
::sys::add_live_param b 0 int {chain b}
::ess::set_live_param a 3
check "both ran, in order" $::applied {{a 3} {b 6}}
check "derived value stored" [::sys::get b] 6

puts "\nSELF-recursion is skipped rather than spinning:"
set ::applied {}
set ::selfhits 0
proc selfset { name val } {
    incr ::selfhits
    lappend ::applied [list $name $val]
    # a handler setting its own param cannot converge
    ::ess::set_live_param self [expr {$val + 1}]
}
::sys::add_live_param self 0 int {selfset self}
::ess::set_live_param self 1
check "handler ran once, not forever" $::selfhits 1
check "the inner set still STORED the value" [::sys::get self] 2

puts "\na CYCLE across params CONVERGES -- the in-flight guard breaks it:"
proc pingpong { name val } {
    if { $name eq "p" } { ::ess::set_live_param q [expr {$val + 1}] }
    if { $name eq "q" } { ::ess::set_live_param p [expr {$val + 1}] }
}
::sys::add_live_param p 0 int {pingpong p}
::sys::add_live_param q 0 int {pingpong q}
set rc [catch { ::ess::set_live_param p 1 } msg]
# p -> q -> p, and the second p finds itself in flight, so its APPLY is
# skipped. Every finite cycle revisits a param, so every finite cycle
# terminates here rather than at the depth cap.
check "converges without erroring" $rc 0
check "both params still stored"   [list [::sys::get p] [::sys::get q]] {3 2}
check "depth unwound to zero"      $::ess::live_apply_depth 0
check "no param left marked in-flight" \
    [llength [array names ::ess::live_apply_inflight]] 0

puts "\nthe depth cap is the backstop for a long NON-repeating chain:"
# each param sets the NEXT one, so nothing is ever revisited and the
# in-flight guard never fires -- this is what the cap is for
proc cascade { name val } {
    set n [string range $name 1 end]
    set next "c[expr {$n + 1}]"
    if { [::sys::is_live_param $next] } { ::ess::set_live_param $next $val }
}
for { set i 0 } { $i < 12 } { incr i } {
    ::sys::add_live_param c$i 0 int [list cascade c$i]
}
set rc [catch { ::ess::set_live_param c0 1 } msg]
check "it errored rather than running away" $rc 1
check "and said why"                [string match "*cycle*" $msg] 1
check "depth unwound to zero"       $::ess::live_apply_depth 0
check "nothing left marked in-flight" \
    [llength [array names ::ess::live_apply_inflight]] 0

puts "\na FAILING handler: the value is stored and the failure is reported:"
proc boom { name val } { error "handler exploded" }
::sys::add_live_param risky 0 int {boom risky}
set rc [catch { ::ess::set_live_param risky 9 } msg]
check "the caller learns it failed" $rc 1
check "the value is stored anyway"  [::sys::get risky] 9
check "depth unwound"               $::ess::live_apply_depth 0
check "not left in-flight"          [info exists ::ess::live_apply_inflight(risky)] 0

puts "\nnon-live params are still refused:"
check "errors" [catch { ::ess::set_live_param nosuch 1 }] 1

puts ""
if { $FAIL } { puts "FAILED"; exit 1 } else { puts "all checks passed" }
